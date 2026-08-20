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

#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/signals2/signal.hpp>
#include <boost/system/error_code.hpp>

#include <opendaq/opendaq.h>
#include <opendaq/streaming_impl.h>

#include <ws-streaming/client.hpp>
#include <ws-streaming/connection.hpp>
#include <ws-streaming/remote_signal.hpp>

#include <websocket_streaming/common.h>
#include <websocket_streaming/ws_streaming_remote_signal_entry.h>

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

/*!
 * @brief An openDAQ streaming object based on the WebSocket Streaming Protocol.
 *
 * This object uses the ws-streaming library to establish a WebSocket streaming connection to a
 * remote peer.
 *
 * openDAQ signals need a valid descriptor, but the WebSocket Streaming protocol only provides
 * metadata once a signal is subscribed. New signals are therefore first subscribed to fetch
 * their metadata, and registered via addToAvailableSignals() once a descriptor can be built.
 * The fetch subscription is briefly kept afterwards so an immediate application subscribe can
 * take it over without wire traffic (devices may process a back-to-back unsubscribe/subscribe
 * pair out of order); a sweep timer releases it if unused. A takeover replays the cached
 * descriptor to openDAQ.
 *
 * Once registered with openDAQ, the onAddSignal() and onRemoveSignal() functions are implemented
 * to manage the subscription state of each known signal. When data is received for an active
 * signal, openDAQ packets are created and passed into openDAQ via onPacket().
 *
 * This object also adapts the WebSocket Streaming Protocol linear domain concept to openDAQ's.
 * Specifically, in the WebSocket Streaming Protocol, data is not normally transmitted for a
 * linear-rule domain signal. Instead the domain value is implicitly calculated based on a sample
 * counter and the linear rule parameters. The streaming object understands this and handles the
 * creation of synthetic domain packets.
 *
 * This object creates a Boost.Asio I/O context (which is required by the ws-streaming library),
 * and manages a std::thread to pump it. The thread is automatically stopped and destroyed when
 * the streaming object is destroyed.
 */
class WsStreaming : public Streaming
{
    public:

        /*!
         * @brief Creates an openDAQ streaming type object using the `daq.lt://` prefix.
         *
         * @return An openDAQ streaming type object using the `daq.lt://` prefix.
         */
        static StreamingTypePtr createType();

    public:

        /*!
         * @brief Constructs a streaming object and initiates a connection to the remote peer.
         *
         * @param connectionString The openDAQ connection string, which must use the `daq.lt://`
         *     prefix. The remote peer address and TCP port number are parsed from the connection
         *     string.
         * @param context The openDAQ context object.
         */
        explicit WsStreaming(
            const StringPtr& connectionString,
            const ContextPtr& context);

        /*!
         * @brief Destroys a streaming object and stops the Boost.Asio I/O context's thread.
         */
        ~WsStreaming();

        /*!
         * @brief An event raised when a signal becomes available.
         *
         * This event is only raised after an initial subscribe has acquired the metadata for a
         * signal. Refer to the class documentation for details.
         *
         * @param signal The ws-streaming library's remote signal object.
         * @param domainSignal The ws-streaming library's remote signal object for the descriptor,
         *     or nullptr if there is no associated domain signal.
         * @param descriptor The openDAQ descriptor for the signal.
         */
        boost::signals2::signal<
            void(wss::remote_signal_ptr signal,
                wss::remote_signal_ptr domainSignal,
                const DataDescriptorPtr& descriptor)
        > onSignalAvailable;

        /**
         * @brief An event raised when a signal is no longer available.
         *
         * @param signal The ws-streaming library's remote signal object.
         */
        boost::signals2::signal<
            void(wss::remote_signal_ptr signal)
        > onSignalUnavailable;

    protected:

        static PropertyObjectPtr createDefaultConfig();

        void onSetActive(bool active) override;
        void onAddSignal(const MirroredSignalConfigPtr& signal) override;
        void onRemoveSignal(const MirroredSignalConfigPtr& signal) override;
        void onSubscribeSignal(const StringPtr& signalId) override;
        void onUnsubscribeSignal(const StringPtr& signalId) override;

    private:

        void onConnected(
            const boost::system::error_code& ec,
            wss::connection_ptr connection);

        /*! @brief I/O-thread implementation of onSubscribeSignal(). */
        void subscribeRemoteSignal(const std::string& signalId);

        /*! @brief I/O-thread implementation of onUnsubscribeSignal(). */
        void unsubscribeRemoteSignal(const std::string& signalId);

        /*! @brief Creates a tracking entry for a remote signal and connects its event slots. */
        std::shared_ptr<WsStreamingRemoteSignalEntry> createSignalEntry(wss::remote_signal_ptr signal);

        void onRemoteSignalAvailable(wss::remote_signal_ptr signal);

        void onRemoteSignalSubscribed(std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry);

        void onRemoteSignalMetadataChanged(std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry);

        void onRemoteSignalDataReceived(
            std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry,
            std::int64_t domain_value,
            std::size_t sample_count,
            const void *data,
            std::size_t size);

        void onRemoteSignalUnsubscribed(std::weak_ptr<WsStreamingRemoteSignalEntry> weakEntry);

        void onRemoteSignalUnavailable(wss::remote_signal_ptr signal);

        /*! @brief Finds a signal's domain entry by table ID or via "relatedSignals", discovering hidden domain signals on demand. */
        std::shared_ptr<WsStreamingRemoteSignalEntry> resolveDomainEntry(
            const std::shared_ptr<WsStreamingRemoteSignalEntry>& entry);

        /*! @brief Registers a signal with openDAQ, marks its initial-fetch subscription as held for takeover and publishes signals deferred on it. */
        void publishSignalEntry(const std::shared_ptr<WsStreamingRemoteSignalEntry>& entry);

        /*! @brief Pushes the entry's cached descriptor into openDAQ as descriptor-changed events, propagating to signals that use it as their domain. */
        void emitDescriptorChangedEvents(const std::shared_ptr<WsStreamingRemoteSignalEntry>& entry);

        /*! @brief Checks whether any signal is still awaiting initial metadata or deferred on an unpublished domain signal. */
        bool anyInitialFetchPending() const;

        void armInitialFetchSweep();
        void onInitialFetchSweep(const boost::system::error_code& ec);
        void onInitialFetchResubscribe(const boost::system::error_code& ec);

        boost::asio::io_context ioContext;
        std::thread thread;

        wss::client wsClient;
        wss::connection_ptr wsConnection;

        std::map<std::string, std::shared_ptr<WsStreamingRemoteSignalEntry>> signals;

        /** Re-subscribes signals whose initial metadata fetch was dropped (some devices drop concurrent requests). */
        boost::asio::steady_timer initialFetchTimer;
        bool initialFetchSweepArmed = false;

        std::promise<boost::system::error_code> promise;
};

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
