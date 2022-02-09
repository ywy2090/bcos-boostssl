/*
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  m_limitations under the License.
 *
 * @file WsConnector.cpp
 * @author: octopus
 * @date 2021-08-23
 */

#include <bcos-boostssl/websocket/Common.h>
#include <bcos-boostssl/websocket/WsConnector.h>
#include <bcos-boostssl/websocket/WsTools.h>
#include <boost/asio/error.hpp>
#include <boost/beast/websocket/stream_base.hpp>
#include <boost/thread/thread.hpp>
#include <cstddef>
#include <memory>
#include <utility>

using namespace bcos;
using namespace bcos::boostssl;
using namespace bcos::boostssl::ws;

// TODO: how to set timeout for connect to wsServer ???
void WsConnector::connectToWsServer(const std::string& _host, uint16_t _port, bool _disableSsl,
    std::function<void(boost::beast::error_code, std::shared_ptr<WsStreamDelegate>)> _callback)
{
    auto ioc = m_ioc;
    auto ctx = m_ctx;

    std::string endpoint = _host + ":" + std::to_string(_port);
    // check if last connect opr done
    if (!insertPendingConns(endpoint))
    {
        WEBSOCKET_CONNECTOR(WARNING)
            << LOG_BADGE("connectToWsServer") << LOG_DESC("insertPendingConns")
            << LOG_KV("endpoint", endpoint);
        _callback(boost::beast::error_code(boost::asio::error::would_block), nullptr);
        return;
    }

    auto resolver = m_resolver;
    auto builder = m_builder;
    auto connector = shared_from_this();

    // resolve host
    resolver->async_resolve(_host.c_str(), std::to_string(_port).c_str(),
        [_host, _port, _disableSsl, endpoint, ioc, ctx, connector, builder, _callback](
            boost::beast::error_code _ec, boost::asio::ip::tcp::resolver::results_type _results) {
            if (_ec)
            {
                WEBSOCKET_CONNECTOR(WARNING)
                    << LOG_BADGE("connectToWsServer") << LOG_DESC("async_resolve failed")
                    << LOG_KV("error", _ec) << LOG_KV("errorMessage", _ec.message())
                    << LOG_KV("endpoint", endpoint);
                connector->erasePendingConns(endpoint);
                _callback(_ec, nullptr);
                return;
            }

            WEBSOCKET_CONNECTOR(TRACE)
                << LOG_BADGE("connectToWsServer") << LOG_DESC("async_resolve success")
                << LOG_KV("endPoint", endpoint);

            // create raw tcp stream
            auto rawStream = std::make_shared<boost::beast::tcp_stream>(*ioc);

            // async connect
            rawStream->async_connect(_results,
                [_host, _port, _disableSsl, endpoint, connector, builder, rawStream, _callback](
                    boost::beast::error_code _ec,
                    boost::asio::ip::tcp::resolver::results_type::endpoint_type _ep) mutable {
                    if (_ec)
                    {
                        WEBSOCKET_CONNECTOR(WARNING)
                            << LOG_BADGE("connectToWsServer") << LOG_DESC("async_connect failed")
                            << LOG_KV("error", _ec.message()) << LOG_KV("endpoint", endpoint);
                        connector->erasePendingConns(endpoint);
                        _callback(_ec, nullptr);
                        return;
                    }

                    WEBSOCKET_CONNECTOR(INFO)
                        << LOG_BADGE("connectToWsServer") << LOG_DESC("async_connect success")
                        << LOG_KV("endpoint", endpoint);

                    auto wsStreamDelegate = builder->build(_disableSsl, rawStream);

                    // start ssl handshake
                    wsStreamDelegate->asyncHandshake([wsStreamDelegate, connector, _host, _port,
                                                         endpoint, _ep,
                                                         _callback](boost::beast::error_code _ec) {
                        if (_ec)
                        {
                            WEBSOCKET_CONNECTOR(WARNING)
                                << LOG_BADGE("connectToWsServer")
                                << LOG_DESC("ssl async_handshake failed") << LOG_KV("host", _host)
                                << LOG_KV("port", _port) << LOG_KV("error", _ec.message());
                            connector->erasePendingConns(endpoint);
                            _callback(_ec, nullptr);
                            return;
                        }

                        WEBSOCKET_CONNECTOR(INFO) << LOG_BADGE("connectToWsServer")
                                                  << LOG_DESC("ssl async_handshake success")
                                                  << LOG_KV("host", _host) << LOG_KV("port", _port);

                        // turn off the timeout on the tcp_stream, because
                        // the websocket stream has its own timeout system.
                        wsStreamDelegate->tcpStream().expires_never();

                        std::string tmpHost = _host + ':' + std::to_string(_ep.port());

                        // websocket async handshake
                        wsStreamDelegate->asyncWsHandshake(tmpHost, "/",
                            [connector, _host, _port, endpoint, _callback, wsStreamDelegate](
                                boost::beast::error_code _ec) mutable {
                                if (_ec)
                                {
                                    WEBSOCKET_CONNECTOR(WARNING)
                                        << LOG_BADGE("connectToWsServer")
                                        << LOG_DESC("websocket async_handshake failed")
                                        << LOG_KV("error", _ec.message()) << LOG_KV("host", _host)
                                        << LOG_KV("port", _port);
                                    connector->erasePendingConns(endpoint);
                                    _callback(_ec, nullptr);
                                    return;
                                }

                                WEBSOCKET_CONNECTOR(INFO)
                                    << LOG_BADGE("connectToWsServer")
                                    << LOG_DESC("websocket handshake successfully")
                                    << LOG_KV("host", _host) << LOG_KV("port", _port);

                                connector->erasePendingConns(endpoint);
                                _callback(_ec, wsStreamDelegate);
                            });
                    });
                });
        });
}