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

#include <functional>
#include <thread>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <opendaq/connected_client_info.h>
#include <opendaq/network_interface.h>
#include <opendaq/device_info_internal_ptr.h>
#include <opendaq/opendaq.h>
#include <opendaq/server_capability.h>
#include <opendaq/server_impl.h>
#include <opendaq/server_type_factory.h>

#include <ws-streaming/ws-streaming.hpp>

#include <websocket_streaming/constants.h>
#include <websocket_streaming/descriptor_to_metadata.h>
#include <websocket_streaming/ws_streaming_listener.h>
#include <websocket_streaming/ws_streaming_server.h>

using namespace std::placeholders;

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

PropertyObjectPtr WsStreamingServer::createDefaultConfig(const ContextPtr& context)
{
    auto defaultConfig = createDefaultConfig();

    populateDefaultConfigFromProvider(context, defaultConfig);
    return defaultConfig;
}

ServerTypePtr WsStreamingServer::createType(const ContextPtr& context)
{
    return ServerType(
        ID,
        "openDAQ LT Streaming server",
        "Publishes device signals as a flat list and streams data over WebSocketTcp protocol",
        WsStreamingServer::createDefaultConfig(context));
}

void WsStreamingServer::populateDefaultConfigFromProvider(const ContextPtr& context, const PropertyObjectPtr& config)
{
    if (!context.assigned())
        return;
    if (!config.assigned())
        return;

    auto options = context.getModuleOptions("StreamingLtServer");
    for (const auto& [key, value] : options)
    {
        if (config.hasProperty(key))
        {
            config->setPropertyValue(key, value);
        }
    }
}

PropertyObjectPtr WsStreamingServer::createDefaultConfig()
{
    constexpr Int minPortValue = 0;
    constexpr Int maxPortValue = 65535;

    auto defaultConfig = PropertyObject();

    {
        auto builder = BoolPropertyBuilder(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, DEFAULT_ENABLE_WS_STREAMING_PORT);
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = BoolPropertyBuilder(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, DEFAULT_ENABLE_WS_CONTROL_PORT);
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = IntPropertyBuilder(PROPERTY_WS_STREAMING_PORT_SERVER, DEFAULT_WS_STREAMING_PORT)
                           .setMinValue(minPortValue)
                           .setMaxValue(maxPortValue)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = IntPropertyBuilder(PROPERTY_WS_CONTROL_PORT_SERVER, DEFAULT_WS_CONTROL_PORT)
                           .setMinValue(minPortValue)
                           .setMaxValue(maxPortValue)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = BoolPropertyBuilder(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, DEFAULT_ENABLE_WSS_STREAMING_PORT);
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = BoolPropertyBuilder(PROPERTY_ENABLE_MTLS_SERVER, DEFAULT_ENABLE_MTLS)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = IntPropertyBuilder(PROPERTY_WSS_STREAMING_PORT_SERVER, DEFAULT_WSS_STREAMING_PORT)
                           .setMinValue(minPortValue)
                           .setMaxValue(maxPortValue)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = StringPropertyBuilder(PROPERTY_WSS_CERT_FILE_PATH_SERVER, DEFAULT_WSS_CERT_FILE_PATH)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = StringPropertyBuilder(PROPERTY_WSS_KEY_FILE_PATH_SERVER, DEFAULT_WSS_KEY_FILE_PATH)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = StringPropertyBuilder(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER, DEFAULT_WSS_CA_CERT_FILE_PATH)
                           .setVisible(EvalValue(std::string("($") + PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER + " == 1) && ($" +
                                                 PROPERTY_ENABLE_MTLS_SERVER + " == 1)"));
        defaultConfig.addProperty(builder.build());
    }

    defaultConfig.addProperty(StringProperty(PROPERTY_PATH_SERVER, "/"));
    return defaultConfig;
}

void WsStreamingServer::addDefaultConfig(PropertyObjectPtr& config)
{
    if (!config.assigned())
        return;

    const auto defaultConfig = createDefaultConfig();
    for (const auto& prop : defaultConfig.getAllProperties())
    {
        if (config.hasProperty(prop.getName()))
            continue;

        config.addProperty(prop.asPtr<IPropertyInternal>(true).clone());
        config.setPropertyValue(prop.getName(), prop.getValue());
    }
}

WsStreamingServer::WsStreamingServer(
        const InstancePtr& instance)
    : WsStreamingServer(
        instance.getRootDevice(),
        createDefaultConfig(instance.getContext()),
        instance.getContext())
{
}

WsStreamingServer::WsStreamingServer(
        const DevicePtr& rootDevice,
        const PropertyObjectPtr& config,
        const ContextPtr& context)
    : Server{ID, config, rootDevice, context}
    , _rootDevice{rootDevice}
    , _ioc{1}
    , _server{_ioc.get_executor()}
{
    // if the config does not contain all the default properties, add them to the config
    addDefaultConfig(this->config);

    _ws_channel_enabled = (this->config.getPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER).asPtr<IBoolean>().getValue(False) == True);
    _wss_channel_enabled = (this->config.getPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER).asPtr<IBoolean>().getValue(False) == True);
    const bool control_channel_enabled =
        (this->config.getPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER).asPtr<IBoolean>().getValue(False) == True);

    if (!_ws_channel_enabled && !_wss_channel_enabled)
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException,
                            "Neither the websocket streaming port nor the TLS streaming port is enabled");
    }

    if (control_channel_enabled && !_ws_channel_enabled)
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException,
                            "The control port cannot be enabled without the websocket streaming port");
    }

    if (_ws_channel_enabled)
    {
        _ws_port = config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER);
        _server.add_listener(_ws_port);
    }
    if (control_channel_enabled)
    {
        _server.add_listener(config.getPropertyValue(PROPERTY_WS_CONTROL_PORT_SERVER), true);
    }

    if (_wss_channel_enabled)
    {
        std::string ca_cert;
        if (this->config.getPropertyValue(PROPERTY_ENABLE_MTLS_SERVER).asPtr<IBoolean>().getValue(False) == True)
        {
            ca_cert = this->config.getPropertyValue(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER).asPtr<IString>().toStdString();
            if (ca_cert.empty())
            {
                DAQ_THROW_EXCEPTION(InvalidParameterException, "Mutual TLS is enabled but no CA certificate file path is configured");
            }
        }
        std::string server_cert = this->config.getPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_SERVER).asPtr<IString>().toStdString();
        std::string server_key = this->config.getPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_SERVER).asPtr<IString>().toStdString();

        if (server_cert.empty() || server_key.empty())
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "TLS certificate or key file path is not configured");
        }

        _wss_port = this->config.getPropertyValue(PROPERTY_WSS_STREAMING_PORT_SERVER);

        try
        {
            _server.add_tls_listener(_wss_port, server_cert, server_key, ca_cert);
        }
        catch (const std::exception& e)
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Cannot load the TLS secrets: {}", e.what());
        }
    }
    _path = config.getPropertyValue(PROPERTY_PATH_SERVER).asPtr<IString>().toStdString();

    _onClientConnected = _server.on_client_connected.connect(
        std::bind(&WsStreamingServer::onClientConnected, this, _1));
    _onClientDisconnected = _server.on_client_disconnected.connect(
        std::bind(&WsStreamingServer::onClientDisconnected, this, _1, _2));

    _server.run();

    rescan();

    // No destructor runs for a constructor that throws, so everything below must unwind on its own.
    addCapability();

    try
    {
        context.getOnCoreEvent() += event(&WsStreamingServer::onCoreEvent);
        _thread = std::thread{[this]() { _ioc.run(); }};
    }
    catch (...)
    {
        context.getOnCoreEvent() -= event(&WsStreamingServer::onCoreEvent);
        removeCapability();
        throw;
    }
}

WsStreamingServer::~WsStreamingServer()
{
    try
    {
        onStopServer();
    }
    catch (...)
    {
    }
}

wss::server& WsStreamingServer::getWsServer() noexcept
{
    return _server;
}

ListPtr<IPropertyObject> WsStreamingServer::getDiscoveryConfigs()
{
    auto discoveryConfigs = List<IPropertyObject>();
    if (_ws_channel_enabled)
    {
        auto discoveryConfig = PropertyObject();
        discoveryConfig.addProperty(StringProperty("ServiceName", CONST_LT_SERVICE_NAME));
        discoveryConfig.addProperty(StringProperty("ServiceCap", CONST_SERVICE_CAPABILITY));
        discoveryConfig.addProperty(StringProperty("Path", String(_path)));
        discoveryConfig.addProperty(IntProperty("Port", Integer(_ws_port)));
        discoveryConfig.addProperty(StringProperty("ProtocolVersion", ""));

        discoveryConfigs.pushBack(std::move(discoveryConfig));
    }

    if (_wss_channel_enabled)
    {
        auto discoveryConfig = PropertyObject();
        discoveryConfig.addProperty(StringProperty("ServiceName", CONST_LTS_SERVICE_NAME));
        discoveryConfig.addProperty(StringProperty("ServiceCap", CONST_SERVICE_CAPABILITY));
        discoveryConfig.addProperty(StringProperty("Path", String(_path)));
        discoveryConfig.addProperty(IntProperty("Port", Integer(_wss_port)));
        discoveryConfig.addProperty(StringProperty("ProtocolVersion", ""));

        discoveryConfigs.pushBack(std::move(discoveryConfig));
    }
    return discoveryConfigs;
}

void WsStreamingServer::onStopServer()
{
    _onClientConnected.disconnect();
    _onClientDisconnected.disconnect();

    context.getOnCoreEvent() -= event(&WsStreamingServer::onCoreEvent);

    // openDAQ can (but probably should not) call onStopServer() more than once.
    if (_thread.joinable())
    {
        // Close the ws-streaming server on its own I/O thread (it is not thread-safe): this cancels
        // the listeners' pending accepts and closes every client connection, letting _ioc.run()
        // drain and return so the thread can be joined.

        boost::asio::post(_ioc, [this] { _server.close(); });
        _thread.join();
    }
    removeCapability();
}

PropertyObjectPtr WsStreamingServer::populateDefaultConfig(
    const PropertyObjectPtr& config,
    const ContextPtr& context)
{
    const auto defConfig = createDefaultConfig(context);
    for (const auto& prop : defConfig.getAllProperties())
    {
        const auto name = prop.getName();
        if (config.hasProperty(name))
            defConfig.setPropertyValue(name, config.getPropertyValue(name));
    }

    return defConfig;
}

void WsStreamingServer::addCapability()
{
    auto info = _rootDevice.getInfo();

    {
        // If the device already has a ws-streaming or wss-streaming capability, throw an exception.
        // It is not allowed adding the second instance of ws(s)-streaming server to the same device.
        // If the device already has a ws-streaming and does not have wss-streaming capability (or vice versa),
        // when first remove the existing server, then add the new one with both channels enabled.
        if (info.hasServerCapability(CONST_LT_STREAMING_ID))
            DAQ_THROW_EXCEPTION(InvalidStateException,
                                fmt::format("Device \"{}\" already has an {} server capability.", info.getName(), CONST_LT_STREAMING_ID));

        if (info.hasServerCapability(CONST_LTS_STREAMING_ID))
            DAQ_THROW_EXCEPTION(InvalidStateException,
                                fmt::format("Device \"{}\" already has an {} server capability.", info.getName(), CONST_LTS_STREAMING_ID));

    }

    if (_ws_channel_enabled)
    {
        auto cap = ServerCapability(CONST_LT_STREAMING_ID, CONST_LT_STREAMING_ID, ProtocolType::Streaming);
        cap.setProtocolGroupId(CONST_LT_PROTOCOL_GROUP_ID);
        cap.setProtocolSecurityLevel(CONST_LT_STREAMING_SECURITY_LVL);
        cap.setPrefix(CONST_LT_STREAMING_PREFIX);
        cap.setPort(_ws_port);
        cap.setConnectionType("TCP/IP");
        info.asPtr<IDeviceInfoInternal>(true).addServerCapability(cap);
    }

    if (_wss_channel_enabled)
    {
        auto cap = ServerCapability(CONST_LTS_STREAMING_ID, CONST_LTS_STREAMING_ID, ProtocolType::Streaming);
        cap.setProtocolGroupId(CONST_LT_PROTOCOL_GROUP_ID);
        cap.setProtocolSecurityLevel(CONST_LTS_STREAMING_SECURITY_LVL);
        cap.setPrefix(CONST_LTS_STREAMING_PREFIX);
        cap.setPort(_wss_port);
        cap.setConnectionType("TCP/IP");
        info.asPtr<IDeviceInfoInternal>(true).addServerCapability(cap);
    }

    _capability_added = _ws_channel_enabled || _wss_channel_enabled;
}

void WsStreamingServer::removeCapability()
{
    if (!_capability_added)
        return;

    _capability_added = false;

    if (!_rootDevice.assigned() || _rootDevice.isRemoved())
        return;

    auto info = _rootDevice.getInfo();
    if (!info.assigned())
        return;

    if (info.hasServerCapability(CONST_LT_STREAMING_ID))
        info.asPtr<IDeviceInfoInternal>(true).removeServerCapability(CONST_LT_STREAMING_ID);
    if (info.hasServerCapability(CONST_LTS_STREAMING_ID))
        info.asPtr<IDeviceInfoInternal>(true).removeServerCapability(CONST_LTS_STREAMING_ID);
}

void WsStreamingServer::createListener(const SignalPtr& signal)
{
    SignalPtr domainSignal = signal.getDomainSignal();

    if (domainSignal.assigned())
        createListener(domainSignal);

    auto it = _localSignals.find(signal.getGlobalId());
    if (it != _localSignals.end())
    {
        // Check if the signal has acquired a new/different domain signal since it was added. If
        // so, we need to unregister the local_signal from ws-streaming and re-register it so that
        // the domain tab table is linked correctly.

        if (domainSignal != it->second.domainSignal)
        {
            _server.remove_local_signal(*it->second.localSignal);
            _localSignals.erase(it);
        }

        else
        {
            return;
        }
    }

    auto& streamableSignal = _localSignals.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(
                signal.getGlobalId()),
            std::forward_as_tuple(
                signal.getGlobalId(),
                daq::websocket_streaming::descriptorToMetadata(signal, signal.getDescriptor()),
                signal))
        .first->second;

    streamableSignal.domainSignal = domainSignal;

    streamableSignal.localSignal->on_subscribed.connect([
        =,
        &streamableSignal,
        signal_id = signal.getGlobalId().toStdString()
    ]()
    {
        streamableSignal.listener = createWithImplementation<IInputPortNotifications, WsStreamingListener>(
            this->template thisPtr<ComponentPtr>().getContext(),
            signal,
            streamableSignal.localSignal,
            _server.executor());
        reinterpret_cast<WsStreamingListener *>(streamableSignal.listener.getObject())->start();
    });

    streamableSignal.localSignal->on_unsubscribed.connect([
        =,
        &streamableSignal,
        signal_id = signal.getGlobalId().toStdString()
    ]()
    {
        streamableSignal.listener.release();
    });

    _server.add_local_signal(*streamableSignal.localSignal);
}

void WsStreamingServer::onClientConnected(
    const wss::connection_ptr& connection)
{
    SizeT clientNumber = 0;
    if (_rootDevice.assigned() && !_rootDevice.isRemoved())
    {
        std::string endpointAddress;
        bool secureChannel = false;
        try
        {
            const auto& socket = connection->socket();
            endpointAddress = socket.remote_endpoint().address().to_string();
            secureChannel = _wss_channel_enabled && socket.local_endpoint().port() == _wss_port;
        }
        catch (const std::exception& /*e*/)
        {
            return;
        }
        _rootDevice.getInfo().asPtr<IDeviceInfoInternal>(true).addConnectedClient(
            &clientNumber,
            ConnectedClientInfo(endpointAddress,
                ProtocolType::Streaming,
                secureChannel ? CONST_LTS_STREAMING_ID : CONST_LT_STREAMING_ID,
                "",
                ""));
    }
    _registeredClientIds.insert({connection.get(), clientNumber});
}

void WsStreamingServer::onClientDisconnected(
    const wss::connection_ptr& connection,
    const boost::system::error_code& ec)
{
    if (auto it = _registeredClientIds.find(connection.get()); it != _registeredClientIds.end())
    {
        if (_rootDevice.assigned() && !_rootDevice.isRemoved() && it->second != 0)
            _rootDevice.getInfo().asPtr<IDeviceInfoInternal>(true).removeConnectedClient(it->second);
        _registeredClientIds.erase(it);
    }
}

void WsStreamingServer::onCoreEvent(
    ComponentPtr& component,
    CoreEventArgsPtr& args)
{
    switch (static_cast<CoreEventId>(args.getEventId()))
    {
        case CoreEventId::ComponentAdded:       return onComponentAdded(component, args);
        case CoreEventId::ComponentRemoved:     return onComponentRemoved(component, args);
        case CoreEventId::ComponentUpdateEnd:   return onComponentUpdateEnd(component, args);
        case CoreEventId::AttributeChanged:     return onAttributeChanged(component, args);
        default: break;
    }
}

void WsStreamingServer::onComponentAdded(
    ComponentPtr& component,
    CoreEventArgsPtr& args)
{
    rescan();
}

void WsStreamingServer::onComponentRemoved(
    ComponentPtr& component,
    CoreEventArgsPtr& args)
{
    rescan();
}

void WsStreamingServer::onComponentUpdateEnd(
    ComponentPtr& component,
    CoreEventArgsPtr& args)
{
    rescan();
}

void WsStreamingServer::onAttributeChanged(
    ComponentPtr& component,
    CoreEventArgsPtr& args)
{
    rescan();
}

void WsStreamingServer::rescan()
{
    auto it = _localSignals.begin();
    while (it != _localSignals.end())
    {
        if (it->second.openDaqSignal.isRemoved())
        {
            auto jt = it++;
            _server.remove_local_signal(*jt->second.localSignal);
            _localSignals.erase(jt);
        }

        else
            ++it;
    }

    auto items = _rootDevice.getItems(search::Recursive(search::Any()));
    for (const auto& item : items)
        if (auto signal = item.asPtrOrNull<daq::ISignal>(); signal.assigned() && signal.getDescriptor().assigned())
            createListener(signal);
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
