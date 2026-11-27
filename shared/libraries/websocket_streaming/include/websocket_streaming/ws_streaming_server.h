/*
 * Copyright 2022-2025 openDAQ d.o.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/system/error_code.hpp>

#include <opendaq/opendaq.h>
#include <opendaq/server_impl.h>

#include <ws-streaming/ws-streaming.hpp>

#include <websocket_streaming/common.h>
#include <websocket_streaming/constants.h>

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

class WsStreamingServer : public Server
{
    public:

        static constexpr const char *ID = CONST_LT_STREAMING_SERVER_ID;

        static PropertyObjectPtr createDefaultConfig(
            const ContextPtr& context);

        static ServerTypePtr createType(
            const ContextPtr& context);

        static PropertyObjectPtr populateDefaultConfig(
            const PropertyObjectPtr& config,
            const ContextPtr& context);

        explicit WsStreamingServer(
            const InstancePtr& instance);

        explicit WsStreamingServer(
            const DevicePtr& rootDevice,
            const PropertyObjectPtr& config,
            const ContextPtr& context);

        ~WsStreamingServer();

        wss::server& getWsServer() noexcept;

    protected:

        ListPtr<IPropertyObject> getDiscoveryConfigs() override;

        void onStopServer() override;

    private:

        static void populateDefaultConfigFromProvider(
            const ContextPtr& context,
            const PropertyObjectPtr& config);

        static PropertyObjectPtr createDefaultConfig();
        static void addDefaultConfig(PropertyObjectPtr& config);

        void addCapability();
        void removeCapability();

        // A signal as read from the device tree, so the I/O thread can register it without the tree's locks
        struct SignalSnapshot
        {
            SignalPtr signal;
            SignalPtr domainSignal;
            std::string id;
        };

        void scan(const ComponentPtr& component);
        std::vector<SignalSnapshot> scanSignals(const ComponentPtr& component);
        bool createListener(const SignalSnapshot& snapshot);

        void onClientConnected(
            const wss::connection_ptr& connection);

        void onClientDisconnected(
            const wss::connection_ptr& connection,
            const boost::system::error_code& ec);

        void onCoreEvent(
            ComponentPtr& component,
            CoreEventArgsPtr& args);

        void applySignals(const std::vector<SignalSnapshot>& snapshots);

    private:

        struct StreamableSignal
        {
            StreamableSignal(
                    const std::string& name,
                    const wss::metadata& metadata,
                    SignalPtr openDaqSignal)
                : localSignal(std::make_shared<wss::local_signal>(name, metadata))
                , openDaqSignal(openDaqSignal)
            {
            }

            std::shared_ptr<wss::local_signal> localSignal;
            SignalPtr openDaqSignal;
            InputPortNotificationsPtr listener;
            SignalPtr domainSignal;
            // The domain signal's registration at the time this signal was registered; empty if the domain
            // signal was not registered then (e.g. it had no descriptor yet).
            std::weak_ptr<wss::local_signal> domainLocalSignal;
            std::string domainId;
        };

        DevicePtr _rootDevice;
        LoggerComponentPtr loggerComponent;
        boost::asio::io_context _ioc;
        wss::server _server;
        std::thread _thread;
        std::map<std::string, StreamableSignal> _localSignals;
        // Reads the device tree: core events are reported under its locks, and the I/O thread is joined under them
        boost::asio::io_context _scanIoc;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _scanWork{_scanIoc.get_executor()};
        std::thread _scanThread;
        Int _ws_port{0};
        Int _wss_port{0};
        std::string _path;
        bool _ws_channel_enabled{false};
        // Stays false in a build without the TLS channel: nothing ever sets it, so every
        // `if (_wss_channel_enabled)` below is dead code the compiler drops.
        bool _wss_channel_enabled{false};
        bool _capability_added{false};

        boost::signals2::scoped_connection _onClientConnected;
        boost::signals2::scoped_connection _onClientDisconnected;
        std::map<wss::connection *, SizeT> _registeredClientIds;
};

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
