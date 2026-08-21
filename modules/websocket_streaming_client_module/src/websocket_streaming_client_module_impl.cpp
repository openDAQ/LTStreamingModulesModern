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
    , discoveryClient({CONST_SERVICE_CAPABILITY})
{
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    discoveryClient.initMdnsClient(List<IString>(CONST_LT_SERVICE_NAME, CONST_LTS_SERVICE_NAME, CONST_WS_SERVICE_NAME));
#else
    discoveryClient.initMdnsClient(List<IString>(CONST_LT_SERVICE_NAME, CONST_WS_SERVICE_NAME));
#endif
    loggerComponent = this->context.getLogger().getOrAddComponent("StreamingLTClient");
}

ListPtr<IDeviceInfo> WebsocketStreamingClientModule::onGetAvailableDevices()
{
    auto availableDevices = List<IDeviceInfo>();
    for (const auto& device : discoveryClient.discoverMdnsDevices())
    {
        if (!isSupportedServiceName(device.serviceName))
        {
            LOG_D("Ignoring discovered service \"{}\": not supported by this module", device.serviceName)
            continue;
        }

        availableDevices.pushBack(populateDiscoveredDevice(device));
    }
    return availableDevices;
}

DictPtr<IString, IDeviceType> WebsocketStreamingClientModule::onGetAvailableDeviceTypes()
{
    auto result = Dict<IString, IDeviceType>();

    const auto websocketDeviceType = WsStreamingDevice::createNewType();
    const auto oldWebsocketDeviceType = WsStreamingDevice::createOldType();

    result.set(websocketDeviceType.getId(), websocketDeviceType);
    result.set(oldWebsocketDeviceType.getId(), oldWebsocketDeviceType);

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    const auto secureWebsocketDeviceType = WsStreamingDevice::createNewSecureType();
    result.set(secureWebsocketDeviceType.getId(), secureWebsocketDeviceType);
#endif

    return result;
}

DictPtr<IString, IStreamingType> WebsocketStreamingClientModule::onGetAvailableStreamingTypes()
{
    auto result = Dict<IString, IStreamingType>();

    auto websocketStreamingType = WsStreaming::createType();
    result.set(websocketStreamingType.getId(), websocketStreamingType);

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    auto secureWebsocketStreamingType = WsStreaming::createSecureType();
    result.set(secureWebsocketStreamingType.getId(), secureWebsocketStreamingType);
#endif

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
        deviceConfig = createDefaultDeviceConfig(formedConnectionStr);

    std::scoped_lock lock(sync);

    std::string localId = fmt::format("websocket_pseudo_device{}", deviceIndex++);
    auto deviceType = createDeviceType(formedConnectionStr);
    checkErrorInfo(deviceType.asPtr<IComponentTypePrivate>()->setModuleInfo(moduleInfo));
    auto device = createWithImplementation<IDevice, WsStreamingDevice>(context, parent, localId, formedConnectionStr, deviceType, deviceConfig);

    // Set the connection info for the device
    const auto wsStreamingType = createStreamingType(formedConnectionStr);

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
    if (connectionString.assigned() && connectionString.getLength() > 0)
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
        streamingConfig = createDefaultStreamingConfig(formNewStyleConnectionString(connectionString));

    const StringPtr str = formConnectionString(connectionString, streamingConfig);
    return createWithImplementation<IStreaming, WsStreaming>(str, context, streamingConfig);
}

Bool WebsocketStreamingClientModule::onCompleteServerCapability(const ServerCapabilityPtr& source, const ServerCapabilityConfigPtr& target)
{
    const auto protoId = target.getProtocolId();
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    if (protoId != CONST_LT_STREAMING_ID && protoId != CONST_LTS_STREAMING_ID)
        return false;
#else
    if (protoId != CONST_LT_STREAMING_ID)
        return false;
#endif

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

    const bool isSecure = (protoId == CONST_LTS_STREAMING_ID);
    auto port = target.getPort();
    if (port == -1)
    {
        port = (isSecure) ? DEFAULT_WSS_STREAMING_PORT : DEFAULT_WS_STREAMING_PORT;
        target.setPort(port);
        LOG_W("LT {}server capability is missing port. Defaulting to {}",
              isSecure ? "secure " : "",
              std::to_string(static_cast<uint16_t>(port)))
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
            connectionString = createUrlConnectionString(isSecure, address, port, path);
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

PropertyObjectPtr WebsocketStreamingClientModule::createDefaultDeviceConfig(const StringPtr& connectionString)
{
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    if (isSecureConnection(connectionString))
        return WsStreamingDevice::createDefaultSecureConfig();
#endif

    return WsStreamingDevice::createDefaultConfig();
}

PropertyObjectPtr WebsocketStreamingClientModule::createDefaultStreamingConfig(const StringPtr& connectionString)
{
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    if (isSecureConnection(connectionString))
        return WsStreaming::createDefaultSecureConfig();
#endif

    return WsStreaming::createDefaultConfig();
}

DeviceTypePtr WebsocketStreamingClientModule::createDeviceType(const StringPtr& connectionString)
{
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    if (isSecureConnection(connectionString))
        return WsStreamingDevice::createNewSecureType();
#endif

    return WsStreamingDevice::createNewType();
}

StreamingTypePtr WebsocketStreamingClientModule::createStreamingType(const StringPtr& connectionString)
{
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    if (isSecureConnection(connectionString))
        return WsStreaming::createSecureType();
#endif

    return WsStreaming::createType();
}

StringPtr WebsocketStreamingClientModule::createUrlConnectionString(bool secureType,
                                                                    const StringPtr& host,
                                                                    const IntegerPtr& port,
                                                                    const StringPtr& path)
{
    const auto prefix = secureType ? CONST_LTS_STREAMING_PREFIX : CONST_LT_STREAMING_PREFIX;
    return String(fmt::format("{}://{}:{}{}", prefix, host, port, path));
}

StringPtr WebsocketStreamingClientModule::formConnectionString(const StringPtr& connectionString,
                                                               const PropertyObjectPtr& config,
                                                               ConnectionParameters* outParams)
{
    std::string urlString = formNewStyleConnectionString(connectionString).toStdString();
    bool isSecure = isSecureConnection(urlString);
    bool portFromConfig = false;
    bool portFromConfigIsDefault = false;
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
            if ((portFromConfig = (isSecure && config.hasProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT))))
            {
                outParams->port = config.getPropertyValue(PROPERTY_WSS_STREAMING_PORT_CLIENT);
                portFromConfigIsDefault = outParams->port == DEFAULT_WSS_STREAMING_PORT;
            }
            else if ((portFromConfig = (!isSecure && config.hasProperty(PROPERTY_WS_STREAMING_PORT_CLIENT))))
            {
                outParams->port = config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_CLIENT);
                portFromConfigIsDefault = outParams->port == DEFAULT_WS_STREAMING_PORT;
            }
        }

    }
    else
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Could not parse connection string: {}", connectionString);
    }
    if (outParams->port == 0)
        outParams->port = isSecure ? DEFAULT_WSS_STREAMING_PORT : DEFAULT_WS_STREAMING_PORT;

    std::string output = outParams->prefix + outParams->host;
    if (match[3].matched || (portFromConfig && !portFromConfigIsDefault))
        output += ":" + std::to_string(outParams->port);
    if (match[4].matched)
        output += outParams->path;
    return output;
}

StringPtr WebsocketStreamingClientModule::formNewStyleConnectionString(const StringPtr& connectionString)
{
    auto wsConnectionString = connectionString.toStdString();
    boost::replace_all(wsConnectionString, std::string(CONST_WS_STREAMING_PREFIX) + "://", std::string(CONST_LT_STREAMING_PREFIX) + "://");
    return wsConnectionString;
}

DeviceInfoPtr WebsocketStreamingClientModule::populateDiscoveredDevice(const MdnsDiscoveredDevice& discoveredDevice)
{
    StreamingTypePtr streamingType;
    DeviceTypePtr deviceType;
    bool isSecure = false;
    if (discoveredDevice.serviceName == CONST_LT_SERVICE_NAME || discoveredDevice.serviceName == CONST_WS_SERVICE_NAME)
    {
        isSecure = false;
        streamingType = WsStreaming::createType();
        deviceType = WsStreamingDevice::createNewType();
    }
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    else if (discoveredDevice.serviceName == CONST_LTS_SERVICE_NAME)
    {
        isSecure = true;
        streamingType = WsStreaming::createSecureType();
        deviceType = WsStreamingDevice::createNewSecureType();
    }
#endif
    else
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException,
                            "Discovered device service name \"{}\" is not supported by the WebsocketStreamingClientModule.",
                            discoveredDevice.serviceName);
    }

    auto cap = ServerCapability(streamingType.getId(), streamingType.getId(), ProtocolType::Streaming);

    auto addAddressInfo = [&cap, &discoveredDevice, isSecure](const std::unordered_set<std::string>& ipAddresses, const std::string& type)
    {
        for (const auto& ipAddr : ipAddresses)
        {
            auto connectionStr = createUrlConnectionString(
                isSecure, ipAddr, discoveredDevice.servicePort, discoveredDevice.getPropertyOrDefault("path", "/"));
            cap.addConnectionString(connectionStr);
            cap.addAddress(ipAddr);
            const auto addressInfo = AddressInfoBuilder()
                                         .setAddress(ipAddr)
                                         .setReachabilityStatus(AddressReachabilityStatus::Unknown)
                                         .setType(type)
                                         .setConnectionString(connectionStr)
                                         .build();
            cap.addAddressInfo(addressInfo);
        }
    };

    addAddressInfo(discoveredDevice.ipv4Addresses, "IPv4");
    addAddressInfo(discoveredDevice.ipv6Addresses, "IPv6");

    cap.setConnectionType("TCP/IP");
    cap.setProtocolGroupId(CONST_LT_PROTOCOL_GROUP_ID);
    cap.setProtocolSecurityLevel(isSecure ? CONST_LTS_STREAMING_SECURITY_LVL : CONST_LT_STREAMING_SECURITY_LVL);
    cap.setPrefix(streamingType.getConnectionStringPrefix());
    cap.setProtocolVersion(discoveredDevice.getPropertyOrDefault("protocolVersion", ""));
    if (discoveredDevice.servicePort > 0)
        cap.setPort(discoveredDevice.servicePort);

    return populateDiscoveredDeviceInfo(
        DiscoveryClient::populateDiscoveredInfoProperties,
        discoveredDevice,
        cap,
        deviceType);
}

bool WebsocketStreamingClientModule::isSecureConnection(const std::string& connectionString)
{
    const auto securePrefix = std::string(CONST_LTS_STREAMING_PREFIX) + "://";
    return connectionString.find(securePrefix) != std::string::npos;
}

bool WebsocketStreamingClientModule::isSupportedServiceName(const std::string& serviceName)
{
    if (serviceName == CONST_LT_SERVICE_NAME || serviceName == CONST_WS_SERVICE_NAME)
        return true;

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    if (serviceName == CONST_LTS_SERVICE_NAME)
        return true;
#endif

    return false;
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING_CLIENT_MODULE
