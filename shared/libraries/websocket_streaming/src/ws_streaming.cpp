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
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/system/error_code.hpp>

#include <opendaq/opendaq.h>
#include <opendaq/streaming_impl.h>

#include <ws-streaming/connection.hpp>
#include <ws-streaming/remote_signal.hpp>
#include "websocket_streaming/constants.h"

#include <websocket_streaming/common.h>
#include <websocket_streaming/metadata_to_descriptor.h>
#include <websocket_streaming/ws_streaming.h>

using namespace std::placeholders;

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

StreamingTypePtr WsStreaming::createType()
{
    return StreamingTypeBuilder()
        .setId(CONST_LT_STREAMING_ID)
        .setName("openDAQ WebSocket Streaming")
        .setDescription("Streaming from devices using the WebSocket Streaming Protocol")
        .setDefaultConfig(createDefaultConfig())
        .setConnectionStringPrefix(CONST_LT_STREAMING_PREFIX)
        .build();
}

StreamingTypePtr WsStreaming::createSecureType()
{
    return StreamingTypeBuilder()
    .setId(CONST_LTS_STREAMING_ID)
        .setName("openDAQ WebSocket Streaming")
        .setDescription("Streaming from devices using the WebSocket Streaming Protocol and TLS encryption")
        .setDefaultConfig(createDefaultSecureConfig())
        .setConnectionStringPrefix(CONST_LTS_STREAMING_PREFIX)
        .build();
}

WsStreaming::WsStreaming(
        const StringPtr& connectionString,
        const ContextPtr& context,
        const PropertyObjectPtr& config)
    : Streaming(connectionString, context, true)
    , ioContext{1}
    , wsClient(ioContext.get_executor())
{
    // NOTE! The 'port' property is not used there. The formed 'connectionString'
    // must contain the port number.

    // The ws-streaming library wants a URL like ws://1.2.3.4:7418/foo.
    // So we simply need to replace the daq.lt:// prefix with ws://
    // and daq.lts:// with wss:// for secure channel
    auto wsConnectionString = connectionString.toStdString();
    boost::replace_all(wsConnectionString, "daq.lt://", "ws://");
    boost::replace_all(wsConnectionString, "daq.ws://", "ws://");
    boost::replace_all(wsConnectionString, "daq.lts://", "wss://");
    boost::replace_all(wsConnectionString, "daq.wss://", "wss://");
    bool isSecureChannel = wsConnectionString.find("wss://") != std::string::npos;

    if (isSecureChannel)
    {
        LOG_I("Secure channel requested, enabling TLS");

        std::string certFilePath;
        std::string keyFilePath;
        std::string caCertFilePath;
        if (config.hasProperty(PROPERTY_ENABLE_MTLS_CLIENT) && config.getPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT).asPtr<IBoolean>() == True)
        {
            if (!config.hasProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT) || !config.hasProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT))
                DAQ_THROW_EXCEPTION(InvalidParameterException,
                    "Mutual TLS is enabled but the configuration has no {} or {} property",
                    PROPERTY_WSS_CERT_FILE_PATH_CLIENT, PROPERTY_WSS_KEY_FILE_PATH_CLIENT);

            certFilePath = config.getPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_CLIENT).asPtr<IString>().toStdString();
            keyFilePath = config.getPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_CLIENT).asPtr<IString>().toStdString();

            if (certFilePath.empty() || keyFilePath.empty())
                DAQ_THROW_EXCEPTION(InvalidParameterException, "TLS certificate or key file path is not configured");

            LOG_I("mTLS enabled, using cert file: \'{}\' and key file: \'{}\'", certFilePath, keyFilePath);
        }
        else
        {
            LOG_I("mTLS disabled");
        }
        if (!config.hasProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT))
            DAQ_THROW_EXCEPTION(InvalidParameterException,
                "A secure connection requires the {} property in the configuration",
                PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT);

        caCertFilePath = config.getPropertyValue(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT).asPtr<IString>().toStdString();
        if (caCertFilePath.empty())
            DAQ_THROW_EXCEPTION(InvalidParameterException, "TLS CA certificate file path is not configured");

        LOG_I("Using CA certificate file: \'{}\'", caCertFilePath);
        LOG_I("Trying to load TLS secrets...");

        try
        {
            wsClient.enable_tls(caCertFilePath, certFilePath, keyFilePath);
        }
        catch (const std::exception& e)
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Cannot load the TLS secrets: {}", e.what());
        }
    }

    // Start the ws-streaming connection attempt.
    LOG_I("Connecting to {}", wsConnectionString);
    wsClient.async_connect(wsConnectionString,
        std::bind(&WsStreaming::onConnected, this, _1, _2));

    // Start a background thread to pump the Boost.Asio I/O context. The run() function returns
    // when there is no more work: on the failed-connection path below via ioContext.stop(), and
    // during normal teardown once the destructor closes the connection and the work drains.
    thread = std::thread{[this] { ioContext.run(); }};

    // Wait here until the connection is either established or failed.
    auto ec = promise.get_future().get();

    if (ec)
    {
        ioContext.stop();
        thread.join();

        // A failure raised by the TLS layer means the peer was reached but not trusted.
        // That is an authentication problem.
        if (ec.category() == boost::asio::error::get_ssl_category())
            DAQ_THROW_EXCEPTION(AuthenticationFailedException,
                "Failed to connect to {}: {}", connectionString.toStdString(), ec.message());

        DAQ_THROW_EXCEPTION(NotFoundException,
            "Failed to connect to {}: {}", connectionString.toStdString(), ec.message());
    }
}

WsStreaming::~WsStreaming()
{
    LOG_I("Closing streaming connection and stopping Boost.Asio I/O context thread");

    // Tear the connection down on the I/O context's thread. The ws-streaming peer is not thread-safe,
    // and the connections below and the 'signals' map are only ever touched from that thread, so all
    // of this must run there rather than directly from the destructor.

    // Note we deliberately do NOT call ioContext.stop(): stopping abandons in-flight operations
    // instead of completing them, which corrupts the ssl::stream as it is destroyed
    boost::asio::post(ioContext, [this]
    {
        onAvailableConnection.disconnect();
        onUnavailableConnection.disconnect();

        for (auto& [id, entry] : signals)
        {
            entry->onSubscribed.disconnect();
            entry->onMetadataChanged.disconnect();
            entry->onDataReceived.disconnect();
            entry->onUnsubscribed.disconnect();
        }

        if (wsConnection)
            wsConnection->close();
    });

    thread.join();
}

PropertyObjectPtr WsStreaming::createDefaultConfig()
{
    auto obj = PropertyObject();
    obj.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_CLIENT, DEFAULT_WS_STREAMING_PORT));
    return obj;
}

PropertyObjectPtr WsStreaming::createDefaultSecureConfig()
{
    constexpr Int minPortValue = 0;
    constexpr Int maxPortValue = 65535;

    auto defaultConfig = PropertyObject();

    {
        auto builder = IntPropertyBuilder(PROPERTY_WSS_STREAMING_PORT_CLIENT, DEFAULT_WSS_STREAMING_PORT)
                           .setMinValue(minPortValue)
                           .setMaxValue(maxPortValue);
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = BoolPropertyBuilder(PROPERTY_ENABLE_MTLS_CLIENT, DEFAULT_ENABLE_MTLS);
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = StringPropertyBuilder(PROPERTY_WSS_CERT_FILE_PATH_CLIENT, DEFAULT_WSS_CERT_FILE_PATH)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_MTLS_CLIENT + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = StringPropertyBuilder(PROPERTY_WSS_KEY_FILE_PATH_CLIENT, DEFAULT_WSS_KEY_FILE_PATH)
                           .setVisible(EvalValue(std::string("$") + PROPERTY_ENABLE_MTLS_CLIENT + " == 1"));
        defaultConfig.addProperty(builder.build());
    }

    {
        auto builder = StringPropertyBuilder(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT, DEFAULT_WSS_CA_CERT_FILE_PATH);
        defaultConfig.addProperty(builder.build());
    }

    return defaultConfig;
}

void WsStreaming::onSetActive(bool active)
{
}

void WsStreaming::onAddSignal(const MirroredSignalConfigPtr& signal)
{
}

void WsStreaming::onRemoveSignal(const MirroredSignalConfigPtr& signal)
{
}

void WsStreaming::onSubscribeSignal(const StringPtr& signalId)
{
    LOG_I("Asked to subscribe signal {}", signalId);

    if (auto signalIt = signals.find(signalId); signalIt != signals.end())
    {
        // Don't subscribe a signal if we haven't actually registered it yet (because we're
        // waiting for its initial metadata). This should not happen.
        if (!signalIt->second->isPublished)
        {
            LOG_W("Found signal, but refusing to subscribe it because it's not published yet");
            return;
        }

        // Don't subscribe a signal if it's already subscribed.
        if (signalIt->second->isSubscribed)
        {
            LOG_I("Found signal, but refusing to subscribe it because it's already subscribed");
            return;
        }

        LOG_I("Found signal, subscribing");
        signalIt->second->ptr->subscribe();
        signalIt->second->isSubscribed = true;
    }

    else
    {
        LOG_E("No such signal");
    }
}

void WsStreaming::onUnsubscribeSignal(const StringPtr& signalId)
{
    LOG_I("Asked to unsubscribe signal {}", signalId);

    if (auto signalIt = signals.find(signalId); signalIt != signals.end())
    {   
        // Don't unsubscribe a signal if it's not actually subscribed.
        if (!signalIt->second->isSubscribed)
        {
            LOG_W("Found signal, but refusing to unsubscribe it because it's not subscribed");
            return;
        }

        LOG_I("Found signal, unsubscribing");
        signalIt->second->ptr->unsubscribe();
    }

    else
    {
        LOG_E("No such signal");
    }
}

void WsStreaming::onConnected(
    const boost::system::error_code& ec,
    wss::connection_ptr connection)
{
    promise.set_value(ec);

    if (ec)
        return;

    LOG_I("Connected to remote peer");
    wsConnection = connection;

    onAvailableConnection = wsConnection->on_available.connect(
        std::bind(&WsStreaming::onRemoteSignalAvailable, this, _1));
    onUnavailableConnection = wsConnection->on_unavailable.connect(
        std::bind(&WsStreaming::onRemoteSignalUnavailable, this, _1));
}

void WsStreaming::onRemoteSignalAvailable(wss::remote_signal_ptr signal)
{
    LOG_I("Signal available: {}", signal->id());

    auto entry = std::make_shared<WsStreamingRemoteSignalEntry>();
    entry->ptr = signal;

    std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry = entry;

    entry->onSubscribed         = signal->on_subscribed         .connect(std::bind(&WsStreaming::onRemoteSignalSubscribed,      this, weakEntry));
    entry->onMetadataChanged    = signal->on_metadata_changed   .connect(std::bind(&WsStreaming::onRemoteSignalMetadataChanged, this, weakEntry));
    entry->onDataReceived       = signal->on_data_received      .connect(std::bind(&WsStreaming::onRemoteSignalDataReceived,    this, weakEntry, _1, _2, _3, _4));
    entry->onUnsubscribed       = signal->on_unsubscribed       .connect(std::bind(&WsStreaming::onRemoteSignalUnsubscribed,    this, weakEntry));

    signals[signal->id()] = std::move(entry);

    // Do not immediately register the new signal with openDAQ. We need its metadata first so
    // we can make an openDAQ descriptor. Do an initial subscribe to get that metadata.
    signal->subscribe();
}

void WsStreaming::onRemoteSignalSubscribed(std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry)
{
    auto entry = weakEntry.lock();
    if (!entry)
        return;

    LOG_I("Signal subscribed: {}", entry->ptr->id());

    if (entry->isPublished)
    {
        entry->isSubscribed = true;
        triggerSubscribeAck(entry->ptr->id(), true);
    }
}

void WsStreaming::onRemoteSignalMetadataChanged(std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry)
{
    auto entry = weakEntry.lock();
    if (!entry)
        return;

    LOG_I("Signal metadata changed: {}", entry->ptr->id());

    try
    {
        entry->lastPacket = nullptr;
        entry->descriptor = metadataToDescriptor(entry->ptr->metadata());

        std::string tableId = entry->ptr->metadata().table_id();

        if (auto it = signals.find(tableId); it != signals.end()
                && it->second != entry)
        {
            entry->domainEntry = it->second;
            LOG_I("Signal {} domain now points to {}", entry->ptr->id(), entry->domainEntry->ptr->id());
        }
        else
        {
            LOG_I("Signal {} domain now points to nullptr", entry->ptr->id());
            entry->domainEntry = nullptr;
        }
    }

    catch (const std::exception& ex)
    {
        LOG_E("Cannot understand new metadata for signal {}: {}: {}",
            entry->ptr->id(), ex.what(), entry->ptr->metadata().json().dump());
        entry->descriptor = nullptr;
        entry->domainEntry = nullptr;
    }

    if (entry->descriptor.assigned() && entry->isPublished)
    {
        auto packet = DataDescriptorChangedEventPacket(entry->descriptor, nullptr);
        onPacket(entry->ptr->id(), packet);

        // changed signal is time signal
        if (entry->ptr->id() == entry->ptr->metadata().table_id())
        {
            packet = DataDescriptorChangedEventPacket(nullptr, entry->descriptor);
            for (const auto& [id, dataSignalEntry] : signals)
            {
                if (dataSignalEntry->ptr->metadata().table_id() == entry->ptr->id() &&
                    dataSignalEntry != entry)
                    onPacket(dataSignalEntry->ptr->id(), packet);
            }
        }
    }
    if (entry->descriptor.assigned() && !entry->isPublished)
    {
        LOG_I("Signal {} is now ready, publishing it", entry->ptr->id());
        entry->isPublished = true;
        addToAvailableSignals(entry->ptr->id());
        onSignalAvailable(
            entry->ptr,
            entry->domainEntry ? entry->domainEntry->ptr : nullptr,
            entry->descriptor);
        entry->ptr->unsubscribe();
    }
}

void WsStreaming::onRemoteSignalDataReceived(
    std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry,
    std::int64_t domainValue,
    std::size_t sampleCount,
    const void *data,
    std::size_t size)
{
    auto entry = weakEntry.lock();
    if (!entry)
        return;

    if (!entry->isSubscribed)
        return;

    DataPacketPtr domainPacket;
    DataPacketPtr packet;

    if (entry->domainEntry
        && entry->domainEntry->descriptor.assigned())
    {
        if (entry->domainEntry->descriptor.getRule().assigned()
            && entry->domainEntry->descriptor.getRule().getType() == DataRuleType::Linear
            && (
                !entry->domainEntry->lastPacket.assigned()
                || !entry->domainEntry->lastPacket.getOffset().assigned()
                || entry->domainEntry->lastPacket.getOffset() != domainValue))
        {
            domainPacket = DataPacket(
                entry->domainEntry->descriptor,
                sampleCount,
                domainValue);

            entry->domainEntry->lastPacket = domainPacket;
            onPacket(entry->domainEntry->ptr->id(), domainPacket);
        }

        else
        {
            domainPacket = entry->domainEntry->lastPacket;
        }
    }

    if (domainPacket.assigned())
        packet = DataPacketWithDomain(
            domainPacket,
            entry->descriptor,
            sampleCount);
    else
        packet = DataPacket(entry->descriptor, sampleCount);

    std::memcpy(
        packet.getRawData(),
        data,
        std::min(
            size,
            packet.getRawDataSize()));

    entry->lastPacket = packet;
    onPacket(entry->ptr->id(), packet);
}

void WsStreaming::onRemoteSignalUnsubscribed(std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry)
{
    auto entry = weakEntry.lock();
    if (!entry)
        return;

    LOG_I("Signal unsubscribed: {}", entry->ptr->id());

    if (entry->isSubscribed)
    {
        entry->isSubscribed = false;
        triggerSubscribeAck(entry->ptr->id(), false);
    }
}

void WsStreaming::onRemoteSignalUnavailable(wss::remote_signal_ptr signal)
{
    if (auto signalIt = signals.find(signal->id()); signalIt != signals.end())
    {
        bool wasPublished = signalIt->second->isPublished;
        signals.erase(signalIt);
        if (wasPublished)
            removeFromAvailableSignals(signal->id());
    }

    onSignalUnavailable(signal);
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
