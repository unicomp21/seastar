/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2015 Cloudius Systems
 */

#ifndef APPS_HTTPD_HTTPD_HH_
#define APPS_HTTPD_HTTPD_HH_

#include "http/request_parser.hh"
#include "http/request.hh"
#include "core/reactor.hh"
#include "core/sstring.hh"
#include <experimental/string_view>
#include "core/app-template.hh"
#include "core/circular_buffer.hh"
#include "core/distributed.hh"
#include "core/queue.hh"
#include "core/future-util.hh"
#include "core/metrics_registration.hh"
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <bitset>
#include <limits>
#include <cctype>
#include <vector>
#include <boost/intrusive/list.hpp>
#include <boost/variant.hpp>
#include <boost/algorithm/string.hpp>
#include "reply.hh"
#include "http/routes.hh"
#include "http/websocket.hh"

namespace httpd {

    class http_server;
    class http_stats;

    using namespace std::chrono_literals;

    class http_stats {
        seastar::metrics::metric_groups _metric_groups;
    public:
        http_stats(http_server& server, const sstring& name);
    };

    class http_server {
        std::vector<server_socket> _listeners;
        http_stats _stats;
        uint64_t _total_connections = 0;
        uint64_t _current_connections = 0;
        uint64_t _requests_served = 0;
        uint64_t _connections_being_accepted = 0;
        uint64_t _read_errors = 0;
        uint64_t _respond_errors = 0;
        sstring _date = http_date();
        timer<> _date_format_timer { [this] {_date = http_date();} };
        bool _stopping = false;
        promise<> _all_connections_stopped;
        future<> _stopped = _all_connections_stopped.get_future();
    private:
        void maybe_idle() {
            if (_stopping && !_connections_being_accepted && !_current_connections) {
                _all_connections_stopped.set_value();
            }
        }
    public:
        routes _routes;

        explicit http_server(const sstring& name) : _stats(*this, name) {
            _date_format_timer.arm_periodic(1s);
        }
        future<> listen(ipv4_addr addr) {
            listen_options lo;
            lo.reuse_address = true;
            _listeners.push_back(engine().listen(make_ipv4_address(addr), lo));
            _stopped = when_all(std::move(_stopped), do_accepts(_listeners.size() - 1)).discard_result();
            return make_ready_future<>();
        }
        future<> stop() {
            _stopping = true;
            for (auto&& l : _listeners) {
                l.abort_accept();
            }
            for (auto&& c : _connections) {
                c.shutdown();
            }
            return std::move(_stopped);
        }
        future<> do_accepts(int which) {
            ++_connections_being_accepted;
            return _listeners[which].accept().then_wrapped(
                    [this, which] (future<connected_socket, socket_address> f_cs_sa) mutable {
                        --_connections_being_accepted;
                        if (_stopping) {
                            maybe_idle();
                            return;
                        }
                        auto cs_sa = f_cs_sa.get();
                        auto conn = new connection(*this, std::get<0>(std::move(cs_sa)), std::get<1>(std::move(cs_sa)));
                        conn->process().then_wrapped([this, conn] (auto&& f) {
                            delete conn;
                            try {
                                f.get();
                            } catch (std::exception& ex) {
                                std::cerr << "request error " << ex.what() << std::endl;
                            }
                        });
                        do_accepts(which);
                    }).then_wrapped([] (auto f) {
                try {
                    f.get();
                } catch (std::exception& ex) {
                    std::cerr << "accept failed: " << ex.what() << std::endl;
                }
            });
        }
        class connection : public boost::intrusive::list_base_hook<> {
            enum connection_status {
                keep_open = 0,
                close,
                detach
            };

            http_server& _server;
            boost::variant<connected_socket, connected_websocket<websocket_type::SERVER>> _fd;
            input_stream<char> _read_buf;
            output_stream<char> _write_buf;
            static constexpr size_t limit = 4096;
            using tmp_buf = temporary_buffer<char>;
            http_request_parser _parser;
            std::unique_ptr<request> _req;
            std::unique_ptr<reply> _resp;
            socket_address _addr;
            // null element marks eof
            queue<std::unique_ptr<reply>> _replies { 10 };
            connection_status _done = keep_open;
        public:

            connection(http_server& server, connected_socket&& fd,
                       socket_address addr)
                    : _server(server), _fd(std::move(fd)), _read_buf(boost::get<connected_socket>(_fd).input()),
                      _write_buf(boost::get<connected_socket>(_fd).output()), _addr(addr) {
                ++_server._total_connections;
                ++_server._current_connections;
                _server._connections.push_back(*this);
            }
            ~connection() {
                --_server._current_connections;
                _server._connections.erase(_server._connections.iterator_to(*this));
                _server.maybe_idle();
            }
            future<> process() {
                // Launch read and write "threads" simultaneously:
                return when_all(read(), respond()).then(
                        [this] (std::tuple<future<>, future<>> joined) {
                            //The connection is now detached. It still exists but outside of read and write fibers
                            if (_done == detach) {
                                sstring url = set_query_param(*_req.get());
                                return _write_buf.flush().then([this, url] {
                                    _fd = std::move(connected_websocket<websocket_type::SERVER>(std::move(boost::get<connected_socket>(_fd)), _addr));
                                    return _server._routes.handle_ws(url, std::move(boost::get<connected_websocket<websocket_type::SERVER>>(_fd)), std::move(_req));
                                }).then_wrapped([] (future<> f) {
                                    if (f.failed()) {
                                        f.get_exception();
                                    }
                                });
                            }
                            // FIXME: notify any exceptions in joined?
                            return make_ready_future<>();
                        });
            }
            void shutdown() {

                if (_fd.which() == 0)
                {
                    boost::get<connected_socket>(_fd).shutdown_input();
                    boost::get<connected_socket>(_fd).shutdown_output();
                }
                else {
                    boost::get<connected_websocket<websocket_type::SERVER>>(_fd).shutdown_input();
                    boost::get<connected_websocket<websocket_type::SERVER>>(_fd).shutdown_output();
                }
            }
            future<> read() {
                return do_until([this] {return _done != keep_open;}, [this] {
                    return read_one();
                }).then_wrapped([this] (future<> f) {
                    // swallow error
                    if (f.failed())
                        _server._read_errors++;
                    f.ignore_ready_future();
                    if (_done == detach)
                        return make_ready_future();
                    return _replies.push_eventually( {});
                }).finally([this] {
                    if (_done == detach)
                        return make_ready_future<>();
                    return _read_buf.close();
                });
            }
            future<> read_one() {
                _parser.init();
                return _read_buf.consume(_parser).then([this] () mutable {
                    if (_parser.eof()) {
                        _done = close;
                        return make_ready_future<>();
                    }
                    ++_server._requests_served;
                    std::unique_ptr<httpd::request> req = _parser.get_parsed_request();

                    return _replies.not_full().then([req = std::move(req), this] () mutable {
                        return generate_reply(std::move(req));
                    }).then([this](connection_status done) {
                        _done = done;
                    });
                });
            }
            future<> respond() {
                return do_response_loop().then_wrapped([this] (future<> f) {
                    // swallow error
                    if (f.failed()) {
                        _server._respond_errors++;
                    }
                    f.ignore_ready_future();
                    if (_done == detach)
                        return make_ready_future<>();
                    return _write_buf.close();
                });
            }
            future<> do_response_loop() {
                return _replies.pop_eventually().then(
                        [this] (std::unique_ptr<reply> resp) {
                            if (!resp) {
                                // eof
                                return make_ready_future<>();
                            }
                            _resp = std::move(resp);
                            return start_response().then([this] {
                                if (_done == keep_open)
                                    return do_response_loop();
                                return make_ready_future<>();
                            });
                        });
            }
            future<> start_response() {
                _resp->_headers["Server"] = "Seastar httpd";
                _resp->_headers["Date"] = _server._date;
                _resp->_headers["Content-Length"] = to_sstring(_resp->_content.size());
                return _write_buf.write(_resp->_response_line.begin(),
                                        _resp->_response_line.size()).then([this] {
                    return write_reply_headers(_resp->_headers.begin());
                }).then([this] {
                    return _write_buf.write("\r\n", 2);
                }).then([this] {
                    return write_body();
                }).then([this] {
                    return _write_buf.flush();
                }).then([this] {
                    _resp.reset();
                });
            }
            future<> write_reply_headers(
                    std::unordered_map<sstring, sstring>::iterator hi) {
                if (hi == _resp->_headers.end()) {
                    return make_ready_future<>();
                }
                return _write_buf.write(hi->first.begin(), hi->first.size()).then(
                        [this] {
                            return _write_buf.write(": ", 2);
                        }).then([hi, this] {
                    return _write_buf.write(hi->second.begin(), hi->second.size());
                }).then([this] {
                    return _write_buf.write("\r\n", 2);
                }).then([hi, this] () mutable {
                    return write_reply_headers(++hi);
                });
            }

            static short hex_to_byte(char c) {
                if (c >='a' && c <= 'z') {
                    return c - 'a' + 10;
                } else if (c >='A' && c <= 'Z') {
                    return c - 'A' + 10;
                }
                return c - '0';
            }

            /**
             * Convert a hex encoded 2 bytes substring to char
             */
            static char hexstr_to_char(const std::experimental::string_view& in, size_t from) {
                return static_cast<char>(hex_to_byte(in[from]) * 16 + hex_to_byte(in[from + 1]));
            }

            /**
             * URL_decode a substring and place it in the given out sstring
             */
            static bool url_decode(const std::experimental::string_view& in, sstring& out) {
                size_t pos = 0;
                char buff[in.length()];
                for (size_t i = 0; i < in.length(); ++i) {
                    if (in[i] == '%') {
                        if (i + 3 <= in.size()) {
                            buff[pos++] = hexstr_to_char(in, i + 1);
                            i += 2;
                        } else {
                            return false;
                        }
                    } else if (in[i] == '+') {
                        buff[pos++] = ' ';
                    } else {
                        buff[pos++] = in[i];
                    }
                }
                out = sstring(buff, pos);
                return true;
            }

            /**
             * Add a single query parameter to the parameter list
             */
            static void add_param(request& req, const std::experimental::string_view& param) {
                size_t split = param.find('=');

                if (split >= param.length() - 1) {
                    sstring key;
                    if (url_decode(param.substr(0,split) , key)) {
                        req.query_parameters[key] = "";
                    }
                } else {
                    sstring key;
                    sstring value;
                    if (url_decode(param.substr(0,split), key)
                        && url_decode(param.substr(split + 1), value)) {
                        req.query_parameters[key] = value;
                    }
                }
            }

            /**
             * Set the query parameters in the request objects.
             * query param appear after the question mark and are separated
             * by the ampersand sign
             */
            static sstring set_query_param(request& req) {
                size_t pos = req._url.find('?');
                if (pos == sstring::npos) {
                    return req._url;
                }
                size_t curr = pos + 1;
                size_t end_param;
                std::experimental::string_view url = req._url;
                while ((end_param = req._url.find('&', curr)) != sstring::npos) {
                    add_param(req, url.substr(curr, end_param - curr) );
                    curr = end_param + 1;
                }
                add_param(req, url.substr(curr));
                return req._url.substr(0, pos);
            }

            future<connection_status> generate_reply(std::unique_ptr<request> req) {
                auto resp = std::make_unique<reply>();
                bool conn_keep_alive = false;
                bool conn_close = false;

                auto it = req->_headers.find("Connection");
                if (it != req->_headers.end()) {
                    if (it->second == "Keep-Alive") {
                        conn_keep_alive = true;
                    } else if (it->second == "Close") {
                        conn_close = true;
                    } else if (it->second.find("Upgrade") != std::string::npos) {
                        auto upgrade = req->_headers.find("Upgrade");
                        if (upgrade != req->_headers.end() && boost::iequals(upgrade->second.begin(), "websocket"))
                            return upgrade_websocket(std::move(req)); //websocket upgrade
                    }
                }
                bool should_close;
                // TODO: Handle HTTP/2.0 when it released
                resp->set_version(req->_version);

                if (req->_version == "1.0") {
                    if (conn_keep_alive) {
                        resp->_headers["Connection"] = "Keep-Alive";
                        std::cout << "keep alive" << std::endl;
                    }
                    should_close = !conn_keep_alive;
                } else if (req->_version == "1.1") {
                    should_close = conn_close;
                } else {
                    // HTTP/0.9 goes here
                    should_close = true;
                }
                sstring url = set_query_param(*req.get());
                sstring version = req->_version;

                return _server._routes.handle(url, std::move(req), std::move(resp)).then([this, should_close, version = std::move(version)](std::unique_ptr<reply> rep) {
                    rep->set_version(version).done();
                    this->_replies.push(std::move(rep));
                    if (should_close)
                        return make_ready_future<connection_status>(close);
                    return make_ready_future<connection_status>(keep_open);
                });
            }

            future<connection_status> upgrade_websocket(std::unique_ptr<request> req) {
                connection_status done = detach;

                sstring url = set_query_param(*req.get());
                auto resp = std::make_unique<reply>();
                resp->set_version(req->_version);

                auto it = req->_headers.find("Sec-WebSocket-Key");
                if (it != req->_headers.end() && _server._routes.get_ws_handler(url, _req)) {
                    //Success
                    resp->_headers["Upgrade"] = "websocket";
                    resp->_headers["Connection"] = "Upgrade";

                    resp->_headers["Sec-WebSocket-Accept"] = httpd::generate_websocket_key(it->second);
                    resp->set_status(reply::status_type::switching_protocols).done();
                    _req = std::move(req);
                }
                else {
                    //Refused
                    _done = done = close;
                    resp->set_status(reply::status_type::bad_request);
                }
                resp->done();
                _done = done = detach;
                _replies.push(std::move(resp));
                return make_ready_future<connection_status>(done);
            }

            future<> write_body() {
                return _write_buf.write(_resp->_content.begin(),
                                        _resp->_content.size());
            }
        };
        uint64_t total_connections() const {
            return _total_connections;
        }
        uint64_t current_connections() const {
            return _current_connections;
        }
        uint64_t requests_served() const {
            return _requests_served;
        }
        uint64_t read_errors() const {
            return _read_errors;
        }
        uint64_t reply_errors() const {
            return _respond_errors;
        }
        static sstring http_date() {
            auto t = ::time(nullptr);
            struct tm tm;
            gmtime_r(&t, &tm);
            char tmp[100];
            strftime(tmp, sizeof(tmp), "%d %b %Y %H:%M:%S GMT", &tm);
            return tmp;
        }
    private:
        boost::intrusive::list<connection> _connections;
    };

/*
 * A helper class to start, set and listen an http server
 * typical use would be:
 *
 * auto server = new http_server_control();
 *                 server->start().then([server] {
 *                 server->set_routes(set_routes);
 *              }).then([server, port] {
 *                  server->listen(port);
 *              }).then([port] {
 *                  std::cout << "Seastar HTTP server listening on port " << port << " ...\n";
 *              });
 */
    class http_server_control {
        distributed<http_server>* _server_dist;
    private:
        static sstring generate_server_name();
    public:
        http_server_control() : _server_dist(new distributed<http_server>) {

        }
        future<> start(const sstring& name = generate_server_name()) {
            return _server_dist->start(name);
        }

        future<> stop() {
            return _server_dist->stop();
        }

        future<> set_routes(std::function<void(routes& r)> fun) {
            return _server_dist->invoke_on_all([fun](http_server& server) {
                fun(server._routes);
            });
        }

        future<> listen(ipv4_addr addr) {
            return _server_dist->invoke_on_all(&http_server::listen, addr);
        }

        distributed<http_server>& server() {
            return *_server_dist;
        }
    };

}

#endif /* APPS_HTTPD_HTTPD_HH_ */