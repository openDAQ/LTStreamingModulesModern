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
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/error.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/system/error_code.hpp>

#include <opendaq/opendaq.h>
#include <opendaq/streaming_impl.h>

#include <ws-streaming/connection.hpp>
#include <ws-streaming/remote_signal.hpp>

#include <websocket_streaming/common.h>
#include <websocket_streaming/metadata_to_descriptor.h>
#include <websocket_streaming/ws_streaming.h>

using namespace std::placeholders;

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

// Initial-fetch retry tuning, measured against a real device (burst metadata <= ~1.1 s; >= 50 ms unsub/sub gap keeps ordering).
static constexpr unsigned INITIAL_FETCH_MAX_ATTEMPTS = 3;
static constexpr std::chrono::milliseconds INITIAL_FETCH_TIMEOUT{1500};
static constexpr std::chrono::milliseconds INITIAL_FETCH_RETRY_DELAY{100};

StreamingTypePtr WsStreaming::createType()
{
    return StreamingTypeBuilder()
        .setId("OpenDAQLTStreaming")
        .setName("openDAQ WebSocket Streaming")
        .setDescription("Streaming from devices using the WebSocket Streaming Protocol")
        .setDefaultConfig(createDefaultConfig())
        .setConnectionStringPrefix("daq.lt")
        .build();
}

WsStreaming::WsStreaming(
        const StringPtr& connectionString,
        const ContextPtr& context)
    : Streaming(connectionString, context, true)
    , ioContext{1}
    , wsClient(ioContext.get_executor())
    , initialFetchTimer(ioContext)
{
    // The ws-streaming library wants a URL like ws://1.2.3.4:7418/foo.
    // So we simply need to replace the daq.lt:// prefix with ws://.
    auto wsConnectionString = connectionString.toStdString();
    boost::replace_all(wsConnectionString, "daq.lt://", "ws://");
    boost::replace_all(wsConnectionString, "daq.ws://", "ws://");

    // Start the ws-streaming connection attempt.
    LOG_I("Connecting to {}", wsConnectionString);
    wsClient.async_connect(wsConnectionString,
        std::bind(&WsStreaming::onConnected, this, _1, _2));

    // Start a background thread to pump the Boost.Asio I/O context. The run() function will
    // return when there is no more work or when ioContext.stop() is called in the destructor.
    thread = std::thread{[this] { ioContext.run(); }};

    // Wait here until the connection is either established or failed.
    auto ec = promise.get_future().get();

    if (ec)
    {
        ioContext.stop();
        thread.join();
        throw NotFoundException(
            "Failed to connect to " + connectionString.toStdString() + ": " + std::to_string(ec.value()));
    }
}

WsStreaming::~WsStreaming()
{
    // Stop the Boost.Asio I/O context (which may already have stopped naturally if the connection
    // failed and there is no more scheduled work) so we can join and destroy the thread.
    LOG_I("Stopping Boost.Asio I/O context thread");
    ioContext.stop();
    thread.join();
    wsConnection->close();
}

PropertyObjectPtr WsStreaming::createDefaultConfig()
{
    auto obj = PropertyObject();
    obj.addProperty(IntProperty("Port", 7414));
    return obj;
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

    wsConnection->on_available.connect(
        std::bind(&WsStreaming::onRemoteSignalAvailable, this, _1));
    wsConnection->on_unavailable.connect(
        std::bind(&WsStreaming::onRemoteSignalUnavailable, this, _1));
}

std::shared_ptr<WsStreamingRemoteSignalEntry> WsStreaming::createSignalEntry(wss::remote_signal_ptr signal)
{
    auto entry = std::make_shared<WsStreamingRemoteSignalEntry>();
    entry->ptr = signal;

    std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry = entry;

    entry->onSubscribed         = signal->on_subscribed         .connect(std::bind(&WsStreaming::onRemoteSignalSubscribed,      this, weakEntry));
    entry->onMetadataChanged    = signal->on_metadata_changed   .connect(std::bind(&WsStreaming::onRemoteSignalMetadataChanged, this, weakEntry));
    entry->onDataReceived       = signal->on_data_received      .connect(std::bind(&WsStreaming::onRemoteSignalDataReceived,    this, weakEntry, _1, _2, _3, _4));
    entry->onUnsubscribed       = signal->on_unsubscribed       .connect(std::bind(&WsStreaming::onRemoteSignalUnsubscribed,    this, weakEntry));

    signals[signal->id()] = entry;

    return entry;
}

void WsStreaming::onRemoteSignalAvailable(wss::remote_signal_ptr signal)
{
    LOG_I("Signal available: {}", signal->id());

    auto entry = createSignalEntry(signal);

    // Do not immediately register the new signal with openDAQ. We need its metadata first so
    // we can make an openDAQ descriptor. Do an initial subscribe to get that metadata.
    entry->initialFetchAttempts = 1;
    entry->initialFetchActive = true;
    signal->subscribe();

    armInitialFetchSweep();
}

void WsStreaming::armInitialFetchSweep()
{
    if (initialFetchSweepArmed)
        return;

    initialFetchSweepArmed = true;
    initialFetchTimer.expires_after(INITIAL_FETCH_TIMEOUT);
    initialFetchTimer.async_wait(std::bind(&WsStreaming::onInitialFetchSweep, this, _1));
}

void WsStreaming::onInitialFetchSweep(const boost::system::error_code& ec)
{
    if (ec == boost::asio::error::operation_aborted)
        return;

    bool retrying = false;

    for (const auto& [id, entry] : signals)
    {
        if (entry->isPublished)
            continue;

        // deferred entry: metadata arrived but the domain signal hasn't published;
        // grant at least one full sweep period before dropping the domain link
        if (entry->descriptor.assigned())
        {
            if (++entry->deferredSweeps >= 2)
                publishSignalEntry(entry);
            continue;
        }

        if (!entry->initialFetchActive)
            continue;

        entry->initialFetchActive = false;
        entry->ptr->unsubscribe();

        if (entry->initialFetchAttempts >= INITIAL_FETCH_MAX_ATTEMPTS)
        {
            LOG_W("No metadata received for signal {}; giving up (without a descriptor the signal cannot be added to openDAQ)", id);
            entry->initialFetchAttempts = 0;
        }
        else
        {
            ++entry->initialFetchAttempts;
            retrying = true;
        }
    }

    if (retrying)
    {
        // stay marked busy so a new arrival cannot re-arm the timer and cancel this resubscribe
        initialFetchTimer.expires_after(INITIAL_FETCH_RETRY_DELAY);
        initialFetchTimer.async_wait(std::bind(&WsStreaming::onInitialFetchResubscribe, this, _1));
    }

    else
    {
        initialFetchSweepArmed = false;

        // signals may still be awaiting metadata or deferred on a domain signal
        if (anyInitialFetchPending())
            armInitialFetchSweep();
    }
}

void WsStreaming::onInitialFetchResubscribe(const boost::system::error_code& ec)
{
    if (ec == boost::asio::error::operation_aborted)
        return;

    initialFetchSweepArmed = false;
    bool pending = false;

    for (const auto& [id, entry] : signals)
    {
        // attempts >= 2 selects signals the sweep unsubscribed (fresh are at 1, given-up at 0);
        // an assigned descriptor means metadata already arrived (entry is deferred, not lost)
        if (entry->isPublished || entry->initialFetchActive
                || entry->descriptor.assigned() || entry->initialFetchAttempts < 2)
            continue;

        entry->initialFetchActive = true;
        entry->ptr->subscribe();
        pending = true;
    }

    // also stay armed for fetches that arrived during the retry delay and deferred entries
    if (pending || anyInitialFetchPending())
        armInitialFetchSweep();
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
        entry->domainEntry = resolveDomainEntry(entry);

        if (entry->domainEntry)
        {
            LOG_D("Signal {} domain now points to {}", entry->ptr->id(), entry->domainEntry->ptr->id());
        }
        else
        {
            LOG_D("Signal {} domain now points to nullptr", entry->ptr->id());
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

        // propagate to signals that use this signal as their domain
        packet = DataDescriptorChangedEventPacket(nullptr, entry->descriptor);
        for (const auto& [id, dataSignalEntry] : signals)
        {
            if (dataSignalEntry != entry &&
                dataSignalEntry->domainEntry == entry &&
                dataSignalEntry->isPublished)
                onPacket(dataSignalEntry->ptr->id(), packet);
        }
    }
    if (entry->descriptor.assigned() && !entry->isPublished)
    {
        // defer until the domain publishes: publishSignalEntry() then publishes this signal too;
        // if the domain never publishes, the sweep publishes this signal without the domain link
        if (entry->domainEntry && !entry->domainEntry->isPublished)
        {
            LOG_D("Deferring signal {} until its domain signal {} is published",
                entry->ptr->id(), entry->domainEntry->ptr->id());
            armInitialFetchSweep();
        }
        else
        {
            publishSignalEntry(entry);
        }
    }
}

void WsStreaming::publishSignalEntry(const std::shared_ptr<WsStreamingRemoteSignalEntry>& entry)
{
    LOG_I("Signal {} is now ready, publishing it{}",
        entry->ptr->id(), entry->isHiddenDomain ? " (hidden domain signal)" : "");

    if (entry->domainEntry && !entry->domainEntry->isPublished)
    {
        LOG_W("Signal {} is published without a link to its domain signal {}, which was never published",
            entry->ptr->id(), entry->domainEntry->ptr->id());
    }

    entry->isPublished = true;

    addToAvailableSignals(entry->ptr->id());
    onSignalAvailable(
        entry->ptr,
        entry->domainEntry && entry->domainEntry->isPublished ? entry->domainEntry->ptr : nullptr,
        entry->descriptor);

    // release the initial metadata-fetch subscription, if one is active
    if (entry->initialFetchActive)
    {
        entry->initialFetchActive = false;
        entry->ptr->unsubscribe();
    }
    entry->initialFetchAttempts = 0;

    // publish signals that were deferred waiting for this signal as their domain
    for (const auto& [id, dependent] : signals)
        if (dependent != entry && dependent->domainEntry == entry
                && !dependent->isPublished && dependent->descriptor.assigned())
            publishSignalEntry(dependent);
}

bool WsStreaming::anyInitialFetchPending() const
{
    // an entry with a descriptor but no publication is deferred, waiting for its domain signal
    for (const auto& [id, entry] : signals)
        if (!entry->isPublished && (entry->initialFetchActive || entry->descriptor.assigned()))
            return true;

    return false;
}

std::shared_ptr<WsStreamingRemoteSignalEntry> WsStreaming::resolveDomainEntry(
    const std::shared_ptr<WsStreamingRemoteSignalEntry>& entry)
{
    std::string tableId = entry->ptr->metadata().table_id();

    if (tableId.empty() || tableId == entry->ptr->id())
        return nullptr;

    // openDAQ servers advertise domain signals and use the domain signal's ID as the table ID
    if (auto it = signals.find(tableId); it != signals.end() && it->second != entry)
        return it->second;

    // other devices reference a hidden domain signal via "relatedSignals"
    std::string domainSignalId;
    const auto& metadataJson = entry->ptr->metadata().json();

    if (auto relatedIt = metadataJson.find("relatedSignals");
            relatedIt != metadataJson.end() && relatedIt->is_array())
        for (const auto& related : *relatedIt)
            if (related.is_object()
                    && related.value("type", std::string()) == "time"
                    && related.contains("signalId")
                    && related["signalId"].is_string())
                domainSignalId = related["signalId"];

    if (domainSignalId.empty() || domainSignalId == entry->ptr->id())
        return nullptr;

    if (auto it = signals.find(domainSignalId); it != signals.end() && it->second != entry)
        return it->second;

    if (!wsConnection)
        return nullptr;

    // the connection knows hidden signals: the peer's acks/metadata create remote signal objects
    auto domainSignal = wsConnection->find_remote_signal(domainSignalId);
    if (!domainSignal)
        return nullptr;

    LOG_D("Discovered hidden domain signal {} of signal {}", domainSignalId, entry->ptr->id());

    auto domainEntry = createSignalEntry(domainSignal);
    domainEntry->isHiddenDomain = true;

    // process its already-received metadata now so it publishes before the referencing signal
    onRemoteSignalMetadataChanged(domainEntry);

    return domainEntry;
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
