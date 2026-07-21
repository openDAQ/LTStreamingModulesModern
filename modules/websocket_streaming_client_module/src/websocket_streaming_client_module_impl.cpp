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

#include <algorithm>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include "boost/algorithm/string/replace.hpp"
#include "websocket_streaming/constants.h"

#include <coretypes/version_info_factory.h>
#include <opendaq/streaming_type_factory.h>
#include <opendaq/device_type_factory.h>
#include <opendaq/address_info_factory.h>

#include <websocket_streaming/ws_streaming.h>
#include <websocket_streaming/ws_streaming_device.h>

#include <websocket_streaming_client_module/common.h>
#include <websocket_streaming_client_module/version.h>
#include <websocket_streaming_client_module/websocket_streaming_client_module_impl.h>

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING_CLIENT_MODULE

static const std::regex RegexIpv6Hostname(R"(^(.+://)?(\[[a-fA-F0-9:]+(?:\%[a-zA-Z0-9_\.-~]+)?\])(?::(\d+))?(/.*)?$)");
static const std::regex RegexIpv4Hostname(R"(^(.+://)?([^:/\s]+)(?::(\d+))?(/.*)?$)");

using namespace discovery;
using namespace websocket_streaming;

WebsocketStreamingClientModule::WebsocketStreamingClientModule(ContextPtr context)
    : Module(
        "OpenDAQWebsocketClientModule",
        daq::VersionInfo(
            WS_STREAM_CL_MODULE_MAJOR_VERSION,
            WS_STREAM_CL_MODULE_MINOR_VERSION,
            WS_STREAM_CL_MODULE_PATCH_VERSION),
        std::move(context),
        "OpenDAQWebsocketClientModule")
    , deviceIndex(0)
    , discoveryClient({"LT"})
{
    discoveryClient.initMdnsClient(List<IString>("_streaming-lt._tcp.local.", "_streaming-ws._tcp.local."));
    loggerComponent = this->context.getLogger().getOrAddComponent("StreamingLTClient");
}

ListPtr<IDeviceInfo> WebsocketStreamingClientModule::onGetAvailableDevices()
{
    auto availableDevices = List<IDeviceInfo>();
    for (const auto& device : discoveryClient.discoverMdnsDevices())
        availableDevices.pushBack(populateDiscoveredDevice(device));
    return availableDevices;
}

DictPtr<IString, IDeviceType> WebsocketStreamingClientModule::onGetAvailableDeviceTypes()
{
    auto result = Dict<IString, IDeviceType>();

    const auto websocketDeviceType = WsStreamingDevice::createNewType();
    const auto oldWebsocketDeviceType = WsStreamingDevice::createOldType();
    const auto secureWebsocketDeviceType = WsStreamingDevice::createNewSecureType();
    const auto oldSecureWebsocketDeviceType = WsStreamingDevice::createOldSecureType();

    result.set(websocketDeviceType.getId(), websocketDeviceType);
    result.set(oldWebsocketDeviceType.getId(), oldWebsocketDeviceType);
    result.set(secureWebsocketDeviceType.getId(), secureWebsocketDeviceType);
    result.set(oldSecureWebsocketDeviceType.getId(), oldSecureWebsocketDeviceType);

    return result;
}

DictPtr<IString, IStreamingType> WebsocketStreamingClientModule::onGetAvailableStreamingTypes()
{
    auto result = Dict<IString, IStreamingType>();

    auto websocketStreamingType = WsStreaming::createType();
    auto secureWebsocketStreamingType = WsStreaming::createSecureType();

    result.set(websocketStreamingType.getId(), websocketStreamingType);
    result.set(secureWebsocketStreamingType.getId(), secureWebsocketStreamingType);

    return result;
}

DevicePtr WebsocketStreamingClientModule::onCreateDevice(const StringPtr& connectionString,
                                                         const ComponentPtr& parent,
                                                         const PropertyObjectPtr& config)
{
    if (!connectionString.assigned())
        DAQ_THROW_EXCEPTION(ArgumentNullException);

    if (!acceptsConnectionParameters(connectionString, config))
        DAQ_THROW_EXCEPTION(InvalidParameterException);

    if (!context.assigned())
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Context is not available.");

    // We don't create any streaming objects here since the
    // internal streaming object is always created within the device

    ConnectionParameters params;
    const StringPtr formedConnectionStr = formConnectionString(connectionString, config, &params);

    PropertyObjectPtr deviceConfig = config;
    if (!deviceConfig.assigned())
        deviceConfig = isSecureConnection(formedConnectionStr) ? WsStreamingDevice::createDefaultSecureConfig()
                                                               : WsStreamingDevice::createDefaultConfig();

    std::scoped_lock lock(sync);

    std::string localId = fmt::format("websocket_pseudo_device{}", deviceIndex++);
    auto deviceType =
        isSecureConnection(formedConnectionStr) ? WsStreamingDevice::createNewSecureType() : WsStreamingDevice::createNewType();
    checkErrorInfo(deviceType.asPtr<IComponentTypePrivate>()->setModuleInfo(moduleInfo));
    auto device = createWithImplementation<IDevice, WsStreamingDevice>(context, parent, localId, formedConnectionStr, deviceType, deviceConfig);

    // Set the connection info for the device
    StreamingTypePtr wsStreamingType =
        (isSecureConnection(formedConnectionStr)) ? WsStreaming::createSecureType() : WsStreaming::createType();

    ServerCapabilityConfigPtr connectionInfo = device.getInfo().getConfigurationConnectionInfo();
    connectionInfo.setProtocolId(wsStreamingType.getId());
    connectionInfo.setProtocolName(wsStreamingType.getId());
    connectionInfo.setProtocolType(ProtocolType::Streaming);
    connectionInfo.setConnectionType("TCP/IP");
    connectionInfo.addAddress(params.host);
    connectionInfo.setPort(params.port);
    connectionInfo.setPrefix(wsStreamingType.getConnectionStringPrefix());
    connectionInfo.setConnectionString(formedConnectionStr);

    return device;
}

bool WebsocketStreamingClientModule::acceptsConnectionParameters(const StringPtr& connectionString, const PropertyObjectPtr& /*config*/)
{
    std::string connStr = connectionString;
    const auto deviceTypes = onGetAvailableDeviceTypes();
    for (const auto& [_, deviceType] : deviceTypes)
    {
        if (connStr.find(deviceType.getConnectionStringPrefix().toStdString() + "://") == 0)
            return true;
    }
    return false;
}

bool WebsocketStreamingClientModule::acceptsStreamingConnectionParameters(const StringPtr& connectionString, const PropertyObjectPtr& config)
{
    if (connectionString.assigned() && connectionString != "")
    {
        return acceptsConnectionParameters(connectionString, config);
    }
    return false;
}

StreamingPtr WebsocketStreamingClientModule::onCreateStreaming(const StringPtr& connectionString, const PropertyObjectPtr& config)
{
    if (!connectionString.assigned())
        DAQ_THROW_EXCEPTION(ArgumentNullException);

    if (!acceptsStreamingConnectionParameters(connectionString, config))
        DAQ_THROW_EXCEPTION(InvalidParameterException);

    PropertyObjectPtr streamingConfig = config;
    if (!streamingConfig.assigned())
        streamingConfig = isSecureConnection(formNewStyleConnectionString(connectionString).toStdString())
                              ? WsStreaming::createDefaultSecureConfig()
                              : WsStreaming::createDefaultConfig();

    const StringPtr str = formConnectionString(connectionString, streamingConfig);
    return createWithImplementation<IStreaming, WsStreaming>(str, context, streamingConfig);
}

Bool WebsocketStreamingClientModule::onCompleteServerCapability(const ServerCapabilityPtr& source, const ServerCapabilityConfigPtr& target)
{
    if (target.getProtocolId() != "OpenDAQLTStreaming")
        return false;

    if (source.getConnectionType() != "TCP/IP")
        return false;

    if (!source.getAddresses().assigned() || !source.getAddresses().getCount())
    {
        LOG_W("Source server capability address is not available when filling in missing LT streaming capability information.")
        return false;
    }

    const auto addrInfos = source.getAddressInfo();
    if (!addrInfos.assigned() || !addrInfos.getCount())
    {
        LOG_W("Source server capability addressInfo is not available when filling in missing LT streaming capability information.")
        return false;
    }

    auto port = target.getPort();
    if (port == -1)
    {
        port = DEFAULT_WS_STREAMING_PORT;
        target.setPort(port);
        LOG_W("LT server capability is missing port. Defaulting to {}", std::to_string(DEFAULT_WS_STREAMING_PORT))
    }

    const auto path = target.hasProperty("Path") ? target.getPropertyValue("Path") : "";
    const auto targetAddress = target.getAddresses();
    for (const auto& addrInfo : addrInfos)
    {
        const auto address = addrInfo.getAddress();
        if (auto it = std::find(targetAddress.begin(), targetAddress.end(), address); it != targetAddress.end())
            continue;

        StringPtr connectionString;
        if (source.getPrefix() == target.getPrefix())
            connectionString = addrInfo.getConnectionString();
        else
            connectionString = createUrlConnectionString(address, port, path);
        const auto targetAddrInfo = AddressInfoBuilder()
                                        .setAddress(address)
                                        .setReachabilityStatus(addrInfo.getReachabilityStatus())
                                        .setType(addrInfo.getType())
                                        .setConnectionString(connectionString)
                                        .build();

        target.addAddressInfo(targetAddrInfo)
              .setConnectionString(connectionString)
              .addAddress(address);
    }

    return true;
}

StringPtr WebsocketStreamingClientModule::createUrlConnectionString(const StringPtr& host,
                                                                    const IntegerPtr& port,
                                                                    const StringPtr& path)
{
    return String(WsStreaming::createType().getConnectionStringPrefix().toStdString()
        + fmt::format("://{}:{}{}", host, port, path));
}

StringPtr WebsocketStreamingClientModule::formConnectionString(const StringPtr& connectionString,
                                                               const PropertyObjectPtr& config,
                                                               ConnectionParameters* outParams)
{
    std::string urlString = formNewStyleConnectionString(connectionString).toStdString();
    bool isSecure = isSecureConnection(urlString);
    bool portFromConfig = false;
    std::smatch match;
    ConnectionParameters localParams;
    if (outParams == nullptr)
        outParams = &localParams;

    bool parsed = std::regex_search(urlString, match, RegexIpv6Hostname);
    if (!parsed)
    {
        parsed = std::regex_search(urlString, match, RegexIpv4Hostname);
    }

    if (parsed)
    {
        outParams->prefix = match[1];
        outParams->host = match[2];

        if (match[3].matched)
            outParams->port = std::stoi(match[3]);

        if (match[4].matched)
            outParams->path = match[4];

        if (outParams->port == 0 && config.assigned())
        {
            if (portFromConfig = (isSecure && config.hasProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT)))
                outParams->port = config.getPropertyValue(PROPERTY_WSS_STREAMING_PORT_CLIENT);
            else if (portFromConfig = (!isSecure && config.hasProperty(PROPERTY_WS_STREAMING_PORT_CLIENT)))
                outParams->port = config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_CLIENT);
        }

    }
    else
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Could not parse connection string: {}", connectionString);
    }
    if (outParams->port == 0)
        outParams->port = isSecure ? DEFAULT_WSS_STREAMING_PORT : DEFAULT_WS_STREAMING_PORT;

    std::string output = outParams->prefix + outParams->host;
    if (match[3].matched || portFromConfig)
        output += ":" + std::to_string(outParams->port);
    if (match[4].matched)
        output += outParams->path;
    return output;
}

StringPtr WebsocketStreamingClientModule::formNewStyleConnectionString(const StringPtr& connectionString)
{
    auto wsConnectionString = connectionString.toStdString();
    boost::replace_all(wsConnectionString, "daq.ws://", "daq.lt://");
    boost::replace_all(wsConnectionString, "daq.wss://", "daq.lts://");
    return wsConnectionString;
}

DeviceInfoPtr WebsocketStreamingClientModule::populateDiscoveredDevice(const MdnsDiscoveredDevice& discoveredDevice)
{
    auto cap = ServerCapability(
        WsStreamingDevice::createNewType().getId(),
        "OpenDAQLTStreaming",
        ProtocolType::Streaming);

    for (const auto& ipAddress : discoveredDevice.ipv4Addresses)
    {
        auto connectionStringIpv4 = WebsocketStreamingClientModule::createUrlConnectionString(
            ipAddress,
            discoveredDevice.servicePort,
            discoveredDevice.getPropertyOrDefault("path", "/")
            );
        cap.addConnectionString(connectionStringIpv4);
        cap.addAddress(ipAddress);
        const auto addressInfo = AddressInfoBuilder().setAddress(ipAddress)
                                     .setReachabilityStatus(AddressReachabilityStatus::Unknown)
                                     .setType("IPv4")
                                     .setConnectionString(connectionStringIpv4)
                                     .build();
        cap.addAddressInfo(addressInfo);
    }

    for (const auto& ipAddress : discoveredDevice.ipv6Addresses)
    {
        auto connectionStringIpv6 = WebsocketStreamingClientModule::createUrlConnectionString(
            ipAddress,
            discoveredDevice.servicePort,
            discoveredDevice.getPropertyOrDefault("path", "/")
            );
        cap.addConnectionString(connectionStringIpv6);
        cap.addAddress(ipAddress);

        const auto addressInfo = AddressInfoBuilder().setAddress(ipAddress)
                                     .setReachabilityStatus(AddressReachabilityStatus::Unknown)
                                     .setType("IPv6")
                                     .setConnectionString(connectionStringIpv6)
                                     .build();
        cap.addAddressInfo(addressInfo);
    }

    cap.setConnectionType("TCP/IP");
    cap.setPrefix("daq.lt");
    cap.setProtocolVersion(discoveredDevice.getPropertyOrDefault("protocolVersion", ""));
    if (discoveredDevice.servicePort > 0)
        cap.setPort(discoveredDevice.servicePort);

    return populateDiscoveredDeviceInfo(
        DiscoveryClient::populateDiscoveredInfoProperties,
        discoveredDevice,
        cap,
        WsStreamingDevice::createNewType());
}

bool WebsocketStreamingClientModule::isSecureConnection(const std::string& connectionString)
{
    const auto securePrefix = WsStreaming::createSecureType().getConnectionStringPrefix().toStdString() + "://";
    return connectionString.find(securePrefix) != std::string::npos;
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING_CLIENT_MODULE
