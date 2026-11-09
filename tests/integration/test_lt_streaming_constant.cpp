/*
 * Streams openDAQ constant-rule signals from the Modern LT server to the Modern LT client over loopback.
 *
 * The server device has a linear time signal, an explicit value signal and a constant-rule status
 * signal, both on the time signal. The server sends the status signal's changes as value index and
 * value entries; the client rebuilds a sample for each sample of the table, on the value signal's
 * domain packets. The tests read the status signal through the client and compare it with what the
 * server device sent.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <testutils/testutils.h>

#include <opendaq/opendaq.h>
#include <opendaq/device_impl.h>
#include <opendaq/packet_factory.h>
#include <opendaq/reader_factory.h>

#include <websocket_streaming/constants.h>
#include <websocket_streaming/ws_streaming_server.h>

using namespace daq;
using namespace daq::websocket_streaming;
using namespace std::chrono_literals;

namespace
{

constexpr SizeT BLOCK = 100;

// A root device with a linear time signal, an explicit value signal and a constant-rule status signal
class ConstantSignalDevice : public Device
{
public:
    ConstantSignalDevice(const ContextPtr& context, SampleType statusType)
        : Device(context, nullptr, "dev")
    {
        time = createAndAddSignal("time", DataDescriptorBuilder()
            .setName("time")
            .setSampleType(SampleType::Int64)
            .setRule(LinearDataRule(1, 0))
            .setTickResolution(Ratio(1, 1000))
            .setOrigin("1970-01-01T00:00:00Z")
            .setUnit(Unit("s", -1, "seconds", "time"))
            .build());

        value = createAndAddSignal("value", DataDescriptorBuilder().setName("value").setSampleType(SampleType::Float64).build());
        value.setDomainSignal(time);

        status = createAndAddSignal("status", DataDescriptorBuilder()
            .setName("status")
            .setSampleType(statusType)
            .setRule(ConstantDataRule())
            .build());
        status.setDomainSignal(time);
    }

    // Sends one block of BLOCK samples at the domain value offset: the status signal first, as the specification sends
    // a constant-rule value before the explicit data of the samples it holds for
    template <typename T>
    void sendBlock(std::int64_t offset, T startValue, const std::vector<ConstantPosAndValue<T>>& changes = {})
    {
        auto domainPacket = DataPacket(time.getDescriptor(), BLOCK, offset);
        auto statusPacket = ConstantDataPacketWithDomain<T>(domainPacket, status.getDescriptor(), BLOCK, startValue, changes);
        auto valuePacket = DataPacketWithDomain(domainPacket, value.getDescriptor(), BLOCK);
        std::fill_n(static_cast<double*>(valuePacket.getRawData()), BLOCK, 1.0);

        status.sendPacket(statusPacket);
        value.sendPacket(valuePacket);
        time.sendPacket(domainPacket);
    }

    SignalConfigPtr time;
    SignalConfigPtr value;
    SignalConfigPtr status;

protected:
    DeviceInfoPtr onGetInfo() override
    {
        return DeviceInfo("daqtest://dev");
    }
};

class LtStreamingConstantTest : public testing::Test
{
protected:
    // Starts a WS-only LT server on the port for a device whose status signal has the sample type
    void startServer(std::uint16_t port, SampleType statusType)
    {
        serverDevice = createWithImplementation<IDevice, ConstantSignalDevice>(serverContext, statusType);
        serverDevice.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

        auto config = WsStreamingServer::createDefaultConfig(serverContext);
        config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True);
        config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
        config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, False);
#endif
        config.setPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER, port);
        server = createWithImplementation<IServer, WsStreamingServer>(serverDevice, config, serverContext);

        clientDevice = client.addDevice("daq.lt://127.0.0.1:" + std::to_string(port) + "/");
    }

    void TearDown() override
    {
        clientDevice.release();
        client.release();
        if (server.assigned())
            server.stop();
    }

    ConstantSignalDevice& device()
    {
        return static_cast<ConstantSignalDevice&>(*serverDevice.getObject());
    }

    // The client's mirror of the server signal, matched by the server signal's global ID
    SignalPtr mirrorOf(const SignalPtr& serverSignal)
    {
        const auto remoteId = serverSignal.getGlobalId();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        do
        {
            for (const auto& signal : clientDevice.getSignals(search::Recursive(search::Any())))
                if (signal.asPtr<IMirroredSignalConfig>().getRemoteId() == remoteId)
                    return signal;
            std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return nullptr;
    }

    // Waits until the server has a listener on the signal, so that packets it sends from now on are streamed
    static bool waitStreamed(const SignalPtr& serverSignal)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (serverSignal.getConnections().getCount() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(20ms);
        return serverSignal.getConnections().getCount() > 0;
    }

    template <typename T>
    static void readSamples(StreamReaderPtr& reader, SizeT count, std::vector<T>& values, std::vector<std::int64_t>& domains)
    {
        values.resize(count);
        domains.resize(count);
        SizeT read = 0;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (read < count && std::chrono::steady_clock::now() < deadline)
        {
            SizeT chunk = count - read;
            reader.readWithDomain(values.data() + read, domains.data() + read, &chunk, 100);
            read += chunk;
        }
        values.resize(read);
        domains.resize(read);
    }

    ContextPtr serverContext = NullContext();
    DevicePtr serverDevice;
    ServerPtr server;
    InstancePtr client = Instance(".");
    DevicePtr clientDevice;
};

}

// Every sample of the status signal reaches the client with the value the server device gave it, changes at their positions
TEST_F(LtStreamingConstantTest, ConstantSignalSamplesMatchTheServerDevice)
{
    startServer(7681, SampleType::Int64);
    auto& dev = device();

    auto clientStatus = mirrorOf(dev.status);
    auto clientValue = mirrorOf(dev.value);
    ASSERT_TRUE(clientStatus.assigned());
    ASSERT_TRUE(clientValue.assigned());

    auto statusReader = StreamReaderBuilder()
        .setSignal(clientStatus)
        .setValueReadType(SampleType::Int64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(true)
        .build();
    auto valueReader = PacketReader(clientValue);
    ASSERT_TRUE(waitStreamed(dev.status));
    ASSERT_TRUE(waitStreamed(dev.value));

    dev.sendBlock<std::int64_t>(0, 3);
    dev.sendBlock<std::int64_t>(100, 3, {{37, 9}});
    dev.sendBlock<std::int64_t>(200, 4);
    dev.sendBlock<std::int64_t>(300, 4, {{99, 5}});

    std::vector<std::int64_t> expected;
    expected.insert(expected.end(), 137, 3);
    expected.insert(expected.end(), 63, 9);
    expected.insert(expected.end(), 199, 4);
    expected.insert(expected.end(), 1, 5);

    std::vector<std::int64_t> values;
    std::vector<std::int64_t> domains;
    readSamples(statusReader, expected.size(), values, domains);

    ASSERT_EQ(values.size(), expected.size());
    EXPECT_EQ(values, expected);
    for (std::size_t i = 0; i < domains.size(); ++i)
        ASSERT_EQ(domains[i], static_cast<std::int64_t>(i)) << "sample " << i;
}

// A client that subscribes the status signal while its value stays the same still gets that value from its first block
TEST_F(LtStreamingConstantTest, LateSubscriberGetsTheCurrentValue)
{
    startServer(7682, SampleType::Int64);
    auto& dev = device();

    auto clientStatus = mirrorOf(dev.status);
    auto clientValue = mirrorOf(dev.value);
    ASSERT_TRUE(clientStatus.assigned());
    ASSERT_TRUE(clientValue.assigned());

    auto valueReader = PacketReader(clientValue);
    ASSERT_TRUE(waitStreamed(dev.value));
    dev.sendBlock<std::int64_t>(0, 6);

    auto statusReader = StreamReaderBuilder()
        .setSignal(clientStatus)
        .setValueReadType(SampleType::Int64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(true)
        .build();
    ASSERT_TRUE(waitStreamed(dev.status));
    dev.sendBlock<std::int64_t>(100, 6);

    std::vector<std::int64_t> values;
    std::vector<std::int64_t> domains;
    readSamples(statusReader, BLOCK, values, domains);

    ASSERT_EQ(values.size(), BLOCK);
    EXPECT_EQ(std::count(values.begin(), values.end(), 6), static_cast<std::ptrdiff_t>(BLOCK));
    EXPECT_EQ(domains.front(), 100);
}

// A floating-point constant-rule signal keeps its values
TEST_F(LtStreamingConstantTest, FloatingPointConstantSignalKeepsItsValues)
{
    startServer(7683, SampleType::Float64);
    auto& dev = device();

    auto clientStatus = mirrorOf(dev.status);
    auto clientValue = mirrorOf(dev.value);
    ASSERT_TRUE(clientStatus.assigned());
    ASSERT_TRUE(clientValue.assigned());

    auto statusReader = StreamReaderBuilder()
        .setSignal(clientStatus)
        .setValueReadType(SampleType::Float64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(true)
        .build();
    auto valueReader = PacketReader(clientValue);
    ASSERT_TRUE(waitStreamed(dev.status));
    ASSERT_TRUE(waitStreamed(dev.value));

    dev.sendBlock<double>(0, 2.5, {{50, -0.25}});

    std::vector<double> values;
    std::vector<std::int64_t> domains;
    readSamples(statusReader, BLOCK, values, domains);

    ASSERT_EQ(values.size(), BLOCK);
    EXPECT_EQ(std::count(values.begin(), values.begin() + 50, 2.5), 50);
    EXPECT_EQ(std::count(values.begin() + 50, values.end(), -0.25), 50);
}
