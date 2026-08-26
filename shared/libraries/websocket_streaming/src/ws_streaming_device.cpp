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
#include "websocket_streaming/constants.h"

#include <opendaq/device_impl.h>
#include <opendaq/opendaq.h>

#include <websocket_streaming/common.h>
#include <websocket_streaming/ws_streaming.h>
#include <websocket_streaming/ws_streaming_device.h>
#include <websocket_streaming/ws_streaming_signal.h>

using namespace std::placeholders;

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

DeviceTypePtr WsStreamingDevice::createOldType()
{
    return DeviceTypeBuilder()
    .setId("OpenDAQLTStreamingOld")
        .setName("Streaming LT enabled pseudo-device")
        .setDescription("Exposes signals from devices streamed using the WebSocket Streaming Protocol")
        .setDefaultConfig(createDefaultConfig())
        .setConnectionStringPrefix("daq.ws")
        .build();
}

DeviceTypePtr WsStreamingDevice::createNewType()
{
    return DeviceTypeBuilder()
    .setId("OpenDAQLTStreaming")
        .setName("Streaming LT enabled pseudo-device")
        .setDescription("Exposes signals from devices streamed using the WebSocket Streaming Protocol")
        .setDefaultConfig(createDefaultConfig())
        .setConnectionStringPrefix("daq.lt")
        .build();
}

#if DAQMODULES_LT_STREAMING_ENABLE_TLS

DeviceTypePtr WsStreamingDevice::createNewSecureType()
{
    return DeviceTypeBuilder()
    .setId("OpenDAQLTStreamingSecure")
        .setName("Secure streaming LT enabled pseudo-device")
        .setDescription("Exposes signals from devices streamed using the WebSocket Streaming Protocol and TLS encryption")
        .setDefaultConfig(createDefaultSecureConfig())
        .setConnectionStringPrefix("daq.lts")
        .build();
}

#endif

WsStreamingDevice::WsStreamingDevice(
        const ContextPtr& context,
        const ComponentPtr& parent,
        const StringPtr& localId,
        const StringPtr& connectionString,
        const DeviceTypePtr& type,
        const PropertyObjectPtr& config)
    : Device(context, parent, localId)
    , connectionString(connectionString)
    , deviceType(type)
{
    if (!connectionString.assigned())
        DAQ_THROW_EXCEPTION(ArgumentNullException, "connectionString cannot be null");
    name = "WebsocketClientPseudoDevice";

    streaming = createWithImplementation<IStreaming, WsStreaming>(connectionString, context, config);
    streaming.setActive(true);

    auto& wsStreaming = *reinterpret_cast<WsStreaming *>(streaming.getObject());

    streamingEvents.emplace_back(wsStreaming.onSignalAvailable.connect(std::bind(&WsStreamingDevice::onSignalAvailable, this, _1, _2, _3)));
    streamingEvents.emplace_back(wsStreaming.onSignalUnavailable.connect(std::bind(&WsStreamingDevice::onSignalUnavailable, this, _1)));
}

PropertyObjectPtr WsStreamingDevice::createDefaultConfig()
{
    return WsStreaming::createDefaultConfig();
}

#if DAQMODULES_LT_STREAMING_ENABLE_TLS

PropertyObjectPtr WsStreamingDevice::createDefaultSecureConfig()
{
    return WsStreaming::createDefaultSecureConfig();
}

#endif

void WsStreamingDevice::removed()
{
    streamingEvents.clear();
}

void WsStreamingDevice::removedNoLock()
{
    streaming.release();
    auto lock = getRecursiveConfigLock2();
    Device::removed();
}

DeviceInfoPtr WsStreamingDevice::onGetInfo()
{
    auto info = DeviceInfo(connectionString, "WebsocketClientPseudoDevice");
    info.setDeviceType(deviceType);
    return info;
}

void WsStreamingDevice::onSignalAvailable(
    wss::remote_signal_ptr signal,
    wss::remote_signal_ptr domainSignal,
    const DataDescriptorPtr& descriptor)
{
    auto lock = getRecursiveConfigLock2();
    if (this->objPtr.template asPtr<IRemovable>(true).isRemoved() || !streaming.assigned())
        return;
    daq::MirroredSignalConfigPtr openDaqDomainSignal;

    if (domainSignal)
    {
        auto localId = WsStreamingSignal::createLocalId(domainSignal->id());
        for (const auto& s : thisPtr<daq::DevicePtr>().getSignals())
            if (s.getLocalId() == localId)
                openDaqDomainSignal = s;
        if (!openDaqDomainSignal.assigned())
            DAQ_THROW_EXCEPTION(NotFoundException,
                "Streaming signal '{}' refers to unregistered domain signal '{}'", signal->id(), domainSignal->id());
    }

    auto openDaqSignal = createWithImplementation<IMirroredSignalPrivate, WsStreamingSignal>(
        context,
        signals,
        signal->id());
    openDaqSignal.setMirroredDataDescriptor(descriptor);

    if (openDaqDomainSignal.assigned())
        openDaqSignal.setMirroredDomainSignal(openDaqDomainSignal);

    streaming.addSignals({openDaqSignal});

    auto mirroredSignalConfigPtr = openDaqSignal.asPtr<IMirroredSignalConfig>();
    mirroredSignalConfigPtr.setActiveStreamingSource(streaming.getConnectionString());

    addSignal(openDaqSignal);
    streamingSignals[signal->id()] = openDaqSignal;
}

void WsStreamingDevice::onSignalUnavailable(wss::remote_signal_ptr signal)
{
    auto lock = getRecursiveConfigLock2();
    if (this->objPtr.template asPtr<IRemovable>(true).isRemoved() || !streaming.assigned())
        return;

    auto it = streamingSignals.find(signal->id());
    if (it == streamingSignals.end())
        return;

    removeSignal(it->second);
    streamingSignals.erase(it);
}

END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING
