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

#include <cstdint>
#include <utility>

#include <boost/asio/post.hpp>

#include <opendaq/opendaq.h>

#include <ws-streaming/local_signal.hpp>
#include <opendaq/event_packet_utils.h>
#include <opendaq/event_packet_params.h>

#include <websocket_streaming/descriptor_to_metadata.h>

#include <websocket_streaming/common.h>
#include <websocket_streaming/ws_streaming_listener.h>

using namespace daq::websocket_streaming;

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

WsStreamingListener::WsStreamingListener(
        IContext *context,
        ISignal *signal,
        std::shared_ptr<wss::local_signal> localSignal,
        boost::asio::any_io_executor executor)
    : _signal(signal)
    , _port(
        InputPort(
            context,
            nullptr,
            String("ws-streaming")))
    , _lastDescriptor(_signal.getDescriptor())
    , _localSignal(std::move(localSignal))
    , _executor(std::move(executor))
{
    // The listener is constructed on the streaming endpoint's strand (inside the on_subscribed handler)
    // so touching _localSignal directly here is safe. All later access happens from
    // packetReceived() on the acquisition thread and is send to _executor instead.
    _localSignal->set_metadata(
        descriptorToMetadata(
            signal,
            _lastDescriptor));
}

WsStreamingListener::~WsStreamingListener()
{
    _port.disconnect();
}

void WsStreamingListener::start()
{
    _port.setListener(this->template thisPtr<InputPortNotificationsPtr>());
    _port.setNotificationMethod(PacketReadyNotification::SameThread);
    _port.connect(_signal);
}

ErrCode WsStreamingListener::acceptsSignal(
    IInputPort *port,
    ISignal *signal,
    Bool *accept)
{
    *accept = true;

    return OPENDAQ_SUCCESS;
};

ErrCode WsStreamingListener::connected(IInputPort *port)
{
    return OPENDAQ_SUCCESS;
}

ErrCode WsStreamingListener::disconnected(IInputPort *port)
{
    return OPENDAQ_SUCCESS;
}

ErrCode WsStreamingListener::packetReceived(IInputPort *port)
{
    while (true)
    {
        auto packet = _port.getConnection().dequeue();
        if (!packet.assigned())
            break;

        if (packet.getType() == PacketType::Data)
            onDataPacketReceived(packet);
        else if (packet.getType() == PacketType::Event)
            onEventPacketReceived(packet.asPtr<IEventPacket>(true));
    }

    return OPENDAQ_SUCCESS;
}

void WsStreamingListener::onDataPacketReceived(DataPacketPtr packet)
{
    std::int64_t offset = 0;

    if (auto domainPacket = packet.getDomainPacket(); domainPacket.assigned())
        if (auto offsetPtr = domainPacket.getOffset(); offsetPtr.assigned())
            offset = offsetPtr;

    auto descriptor = packet.getDataDescriptor();

    // wss::local_signal/connection/peer are not thread-safe
    // packetReceived() runs on the acquisition thread
    // so marshal every local_signal call onto _executor
    if (descriptor != _lastDescriptor)
    {
        auto metadata = descriptorToMetadata(_signal, descriptor);
        boost::asio::post(
            _executor,
            [localSignal = _localSignal, metadata = std::move(metadata)]()
            {
                localSignal->set_metadata(metadata);
            });

        _lastDescriptor = descriptor;
    }

    if (packet.getRawDataSize())
        // Capture the packet by value so its raw buffer stays alive until the handler runs
        boost::asio::post(
            _executor,
            [localSignal = _localSignal, packet, offset]()
            {
                localSignal->publish_data(
                    offset,
                    packet.getSampleCount(),
                    packet.getRawData(),
                    packet.getRawDataSize());
            });
}

void WsStreamingListener::onEventPacketReceived(EventPacketPtr packet)
{
    if (packet.getEventId() == event_packet_id::DATA_DESCRIPTOR_CHANGED)
    {
        bool valueDescriptorChanged;
        DataDescriptorPtr newValueDescriptor;
        std::tie(valueDescriptorChanged, std::ignore, newValueDescriptor, std::ignore) =
            parseDataDescriptorEventPacket(packet);

        if (valueDescriptorChanged && newValueDescriptor != _lastDescriptor)
        {
            // Marshal onto the streaming endpoint's strand (see onDataPacketReceived).
            auto metadata = descriptorToMetadata(_signal, newValueDescriptor);
            boost::asio::post(
                _executor,
                [localSignal = _localSignal, metadata = std::move(metadata)]()
                {
                    localSignal->set_metadata(metadata);
                });

            _lastDescriptor = newValueDescriptor;
        }
    }
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
