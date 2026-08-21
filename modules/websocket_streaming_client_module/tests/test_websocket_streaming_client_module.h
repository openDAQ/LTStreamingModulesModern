#pragma once

#include <testutils/testutils.h>
#include <websocket_streaming_client_module/module_dll.h>
#include <websocket_streaming_client_module/version.h>
#include <websocket_streaming_client_module/websocket_streaming_client_module_impl.h>
#include <websocket_streaming/constants.h>
#include <websocket_streaming/ws_streaming.h>

#include <opendaq/module_ptr.h>
#include <coretypes/common.h>

#include <algorithm>
#include <string>
#include <vector>

#include <opendaq/context_factory.h>
#include <opendaq/device_info_factory.h>
#include <opendaq/address_info_factory.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_factory.h>

using namespace daq;
using namespace daq::websocket_streaming;
using namespace daq::modules::websocket_streaming_client_module;

class WebsocketStreamingClientModuleTest : public testing::Test
{
protected:
    using ConnectionParameters = WebsocketStreamingClientModule::ConnectionParameters;

    static StringPtr formConnectionString(const StringPtr& connectionString,
                                          const PropertyObjectPtr& config,
                                          ConnectionParameters* outParams = nullptr)
    {
        return WebsocketStreamingClientModule::formConnectionString(connectionString, config, outParams);
    }

    static StringPtr createUrlConnectionString(const StringPtr& host, const IntegerPtr& port, const StringPtr& path)
    {
        return WebsocketStreamingClientModule::createUrlConnectionString(false, host, port, path);
    }

    static StringPtr createUrlConnectionString(bool secureType, const StringPtr& host, const IntegerPtr& port, const StringPtr& path)
    {
        return WebsocketStreamingClientModule::createUrlConnectionString(secureType, host, port, path);
    }

    static bool isSecureConnection(const std::string& connectionString)
    {
        return WebsocketStreamingClientModule::isSecureConnection(connectionString);
    }

    static bool acceptsConnectionParameters(const ModulePtr& module, const StringPtr& connectionString, const PropertyObjectPtr& config)
    {
        return reinterpret_cast<WebsocketStreamingClientModule*>(module.getObject())->acceptsConnectionParameters(connectionString, config);
    }

    static bool acceptsStreamingConnectionParameters(const ModulePtr& module, const StringPtr& connectionString, const PropertyObjectPtr& config)
    {
        return reinterpret_cast<WebsocketStreamingClientModule*>(module.getObject())->acceptsStreamingConnectionParameters(connectionString, config);
    }

    static DeviceInfoPtr populateDiscoveredDevice(const discovery::MdnsDiscoveredDevice& discoveredDevice)
    {
        return WebsocketStreamingClientModule::populateDiscoveredDevice(discoveredDevice);
    }

    static Bool completeServerCapability(const ModulePtr& module,
                                         const ServerCapabilityPtr& source,
                                         const ServerCapabilityConfigPtr& target)
    {
        return module.completeServerCapability(source, target);
    }

    static discovery::MdnsDiscoveredDevice makeDiscoveredDevice(const std::string& serviceName,
                                                                uint32_t servicePort = 7414,
                                                                const std::unordered_set<std::string>& ipv4 = {"192.168.1.10"},
                                                                const std::unordered_set<std::string>& ipv6 = {},
                                                                const std::unordered_map<std::string, std::string>& properties = {})
    {
        discovery::MdnsDiscoveredDevice device{};
        device.serviceName = serviceName;
        device.servicePort = servicePort;
        device.ipv4Addresses = ipv4;
        device.ipv6Addresses = ipv6;
        device.properties = properties;
        return device;
    }

    static ServerCapabilityPtr firstCapability(const DeviceInfoPtr& info)
    {
        const auto caps = info.getServerCapabilities();
        EXPECT_EQ(caps.getCount(), 1u);
        return caps[0];
    }

    static ServerCapabilityConfigPtr makeSourceCapability(const std::string& address = "192.168.1.10",
                                                          const std::string& prefix = "daq.opcua")
    {
        auto source = ServerCapability("OpenDAQOPCUA", "OpenDAQOPCUA", ProtocolType::Configuration);
        source.setConnectionType("TCP/IP");
        source.setPrefix(String(prefix));
        source.addAddress(String(address));
        source.addAddressInfo(AddressInfoBuilder()
                                  .setAddress(String(address))
                                  .setReachabilityStatus(AddressReachabilityStatus::Reachable)
                                  .setType("IPv4")
                                  .setConnectionString(String(prefix + "://" + address + ":4840"))
                                  .build());
        return source;
    }
};

inline ModulePtr CreateModule()
{
    ModulePtr module;
    createModule(&module, NullContext());
    return module;
}
