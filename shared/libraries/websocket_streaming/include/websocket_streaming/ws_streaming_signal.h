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

#include <opendaq/mirrored_signal_impl.h>
#include <opendaq/opendaq.h>

#include <websocket_streaming/common.h>

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

class WsStreamingSignal : public MirroredSignal
{
    public:

        static StringPtr createLocalId(const StringPtr& streamingId);

        explicit WsStreamingSignal(
            const ContextPtr& context,
            const ComponentPtr& parent,
            const StringPtr& streamingId);

        StringPtr onGetRemoteId() const override;

        Bool onTriggerEvent(const EventPacketPtr& eventPacket) override;

        /*!
         * @brief Links the signal to a domain signal that became available after it was added.
         *
         * Raises the core event a local signal raises when its domain signal is set: AttributeChanged
         * with "AttributeName" set to "DomainSignal". Must not be called while holding a component lock.
         *
         * @param domainSignal The mirrored domain signal.
         */
        void linkDomainSignal(const MirroredSignalConfigPtr& domainSignal);

    protected:

        SignalPtr onGetDomainSignal() override;

        DataDescriptorPtr onGetDescriptor() override;
    
    private:
    
        StringPtr streamingId;
};

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
