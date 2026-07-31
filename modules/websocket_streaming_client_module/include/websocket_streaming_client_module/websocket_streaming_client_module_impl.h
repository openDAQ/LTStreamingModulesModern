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
#include <websocket_streaming_client_module/common.h>
#include <opendaq/module_impl.h>
#include <daq_discovery/daq_discovery_client.h>

class WebsocketStreamingClientModuleTest;

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING_CLIENT_MODULE

class WebsocketStreamingClientModule final : public Module
{
public:
    WebsocketStreamingClientModule(ContextPtr context);

    ListPtr<IDeviceInfo> onGetAvailableDevices() override;
    DictPtr<IString, IDeviceType> onGetAvailableDeviceTypes() override;
    DictPtr<IString, IStreamingType> onGetAvailableStreamingTypes() override;
    DevicePtr onCreateDevice(const StringPtr& connectionString,
                             const ComponentPtr& parent,
                             const PropertyObjectPtr& config) override;
    DAQ_WS_STREAM_CL_MODULE_API bool acceptsConnectionParameters(const StringPtr& connectionString, const PropertyObjectPtr& config);
    DAQ_WS_STREAM_CL_MODULE_API bool acceptsStreamingConnectionParameters(const StringPtr& connectionString, const PropertyObjectPtr& config);
    StreamingPtr onCreateStreaming(const StringPtr& connectionString, const PropertyObjectPtr& config) override;
    Bool onCompleteServerCapability(const ServerCapabilityPtr& source, const ServerCapabilityConfigPtr& target) override;

private:
    friend class ::WebsocketStreamingClientModuleTest;

    struct ConnectionParameters
    {
        std::string host;
        std::uint16_t port = 0;
        std::string prefix;
        std::string path;
    };

    DAQ_WS_STREAM_CL_MODULE_API static StringPtr createUrlConnectionString(bool secureType,
                                                                           const StringPtr& host,
                                                                           const IntegerPtr& port,
                                                                           const StringPtr& path);
    DAQ_WS_STREAM_CL_MODULE_API static StringPtr formConnectionString(const StringPtr& connectionString,
                                                                      const PropertyObjectPtr& config,
                                                                      ConnectionParameters* outParams = nullptr);
    DAQ_WS_STREAM_CL_MODULE_API static StringPtr formNewStyleConnectionString(const StringPtr& connectionString);
    DAQ_WS_STREAM_CL_MODULE_API static DeviceInfoPtr populateDiscoveredDevice(const discovery::MdnsDiscoveredDevice& discoveredDevice);
    DAQ_WS_STREAM_CL_MODULE_API static bool isSecureConnection(const std::string& connectionString);

    std::mutex sync;
    size_t deviceIndex;
    discovery::DiscoveryClient discoveryClient;
};

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING_CLIENT_MODULE
