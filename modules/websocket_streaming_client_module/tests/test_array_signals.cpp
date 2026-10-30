/*
 * Tests for array (dimensioned) signals such as FFT spectra: each sample carries a vector of
 * values. The dimensions must survive the descriptor <-> metadata conversion; otherwise the
 * client splits every spectrum into scalar samples and its timestamps run ahead of real time.
 */

#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>

#include <ws-streaming/ws-streaming.hpp>

#include <testutils/testutils.h>
#include <websocket_streaming/descriptor_to_metadata.h>
#include <websocket_streaming/metadata_to_descriptor.h>
#include <websocket_streaming_client_module/module_dll.h>

#include <opendaq/context_factory.h>
#include <opendaq/mirrored_signal_config_ptr.h>
#include <opendaq/module_ptr.h>
#include <opendaq/opendaq.h>
#include <opendaq/search_filter_factory.h>

using namespace daq;
using namespace daq::websocket_streaming;
using namespace std::chrono_literals;

namespace
{

constexpr SizeT BIN_COUNT = 25;

DataDescriptorPtr spectrumDescriptor(const NumberPtr& binWidth)
{
    return DataDescriptorBuilder()
        .setName("Spectrum")
        .setSampleType(SampleType::Float64)
        .setDimensions(List<IDimension>(
            Dimension(LinearDimensionRule(binWidth, 0, BIN_COUNT), Unit("Hz"), "Frequency")))
        .build();
}

wss::metadata toMetadata(const DataDescriptorPtr& descriptor)
{
    const auto signal = Signal(NullContext(), nullptr, "sig");
    return descriptorToMetadata(signal, descriptor);
}

std::uint16_t findFreePort()
{
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor(
        ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

// An Instance owns the device so that teardown runs the device's removal path
InstancePtr createClientInstance()
{
    auto instance = Instance("[[none]]");

    ModulePtr module;
    createModule(&module, instance.getContext());
    instance.getModuleManager().addModule(module);

    return instance;
}

SignalPtr waitForSignal(const DevicePtr& device, const std::string& name, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    do
    {
        for (const auto& signal : device.getSignals(search::Recursive(search::Any())))
            if (signal.getDescriptor().assigned() && signal.getDescriptor().getName() == name)
                return signal;
        std::this_thread::sleep_for(50ms);
    } while (std::chrono::steady_clock::now() < deadline);

    return nullptr;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

}  // namespace

using ArraySignalTest = testing::Test;

TEST_F(ArraySignalTest, ScalarSignalHasNoDimensions)
{
    const auto metadata = toMetadata(
        DataDescriptorBuilder().setName("Scalar").setSampleType(SampleType::Float64).build());

    EXPECT_TRUE(metadata.dimensions().empty());
    EXPECT_EQ(metadata.sample_size(), sizeof(double));

    const auto descriptor = metadataToDescriptor(metadata);
    EXPECT_EQ(descriptor.getDimensions().getCount(), 0u);
}

TEST_F(ArraySignalTest, DimensionsRoundTrip)
{
    const auto metadata = toMetadata(spectrumDescriptor(Integer(20)));

    ASSERT_EQ(metadata.dimensions().size(), 1u);
    EXPECT_EQ(metadata.sample_size(), BIN_COUNT * sizeof(double));

    const auto descriptor = metadataToDescriptor(metadata);
    ASSERT_EQ(descriptor.getDimensions().getCount(), 1u);

    const auto dimension = descriptor.getDimensions()[0];
    EXPECT_EQ(dimension.getName(), "Frequency");
    EXPECT_EQ(dimension.getSize(), BIN_COUNT);
    ASSERT_EQ(dimension.getRule().getType(), DimensionRuleType::Linear);

    const NumberPtr delta = dimension.getRule().getParameters().get("delta");
    const NumberPtr start = dimension.getRule().getParameters().get("start");
    EXPECT_EQ(delta.getCoreType(), ctInt);
    EXPECT_EQ(delta.getIntValue(), 20);
    EXPECT_EQ(start.getIntValue(), 0);

    // The client allocates packets from the descriptor, so its sample size must match the wire
    EXPECT_EQ(descriptor.getSampleSize(), metadata.sample_size());
}

TEST_F(ArraySignalTest, RealValuedBinWidthIsPreserved)
{
    // e.g. 51200 Hz / 1000-sample window
    const auto descriptor = metadataToDescriptor(toMetadata(spectrumDescriptor(Floating(51.2))));

    ASSERT_EQ(descriptor.getDimensions().getCount(), 1u);
    const NumberPtr delta = descriptor.getDimensions()[0].getRule().getParameters().get("delta");
    EXPECT_DOUBLE_EQ(delta.getFloatValue(), 51.2);
}

TEST_F(ArraySignalTest, NonLinearDimensionKeepsElementCount)
{
    const auto descriptor = DataDescriptorBuilder()
        .setName("Bands")
        .setSampleType(SampleType::Float32)
        .setDimensions(List<IDimension>(
            Dimension(ListDimensionRule(List<INumber>(31.5, 63.0, 125.0)), Unit("Hz"), "Band")))
        .build();

    const auto metadata = toMetadata(descriptor);
    EXPECT_EQ(metadata.sample_size(), 3 * sizeof(float));
    EXPECT_EQ(metadataToDescriptor(metadata).getDimensions()[0].getSize(), 3u);
}

// One spectrum per sample, as sent by an FFT function block
TEST_F(ArraySignalTest, ClientReceivesOneSamplePerSpectrum)
{
    constexpr std::int64_t timeDelta = 50000;
    constexpr std::int64_t timeStart = 1000000;
    constexpr int spectrumCount = 10;

    wss::local_signal timeSignal{
        "/Spectrum/Time",
        wss::metadata_builder{"Time"}
            .data_type(wss::data_types::int64_t)
            .unit(wss::unit::seconds)
            .linear_rule(0, timeDelta)
            .tick_resolution(1, 1000000)
            .origin(wss::metadata::unix_epoch)
            .table("/Spectrum/Time")
            .build()};

    wss::local_signal spectrumSignal{
        "/Spectrum/Value",
        toMetadata(spectrumDescriptor(Integer(20)))};
    spectrumSignal.set_metadata(
        wss::metadata_builder{wss::from_json, spectrumSignal.metadata().json()}
            .table(timeSignal.id())
            .build());

    const auto port = findFreePort();

    boost::asio::io_context ioc{1};
    wss::server server{ioc.get_executor()};
    server.add_listener(port);
    server.add_local_signal(timeSignal);
    server.add_local_signal(spectrumSignal);
    server.run();
    std::thread ioThread([&ioc] { ioc.run(); });

    {
        auto instance = createClientInstance();
        auto device = instance.addDevice("daq.lt://127.0.0.1:" + std::to_string(port) + "/");

        auto signal = waitForSignal(device, "Spectrum", 5s);
        ASSERT_TRUE(signal.assigned());

        // The client's metadata fetch also subscribes, so wait for the reader's own subscription
        std::promise<void> subscribed;
        auto subscribedFuture = subscribed.get_future();
        signal.asPtr<IMirroredSignalConfig>().getOnSubscribeComplete() +=
            [&subscribed](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&)
            {
                try { subscribed.set_value(); } catch (const std::future_error&) {}
            };

        auto reader = PacketReader(signal);
        ASSERT_EQ(subscribedFuture.wait_for(5s), std::future_status::ready);

        std::vector<double> bins(BIN_COUNT);
        for (int i = 0; i < spectrumCount; ++i)
        {
            for (SizeT bin = 0; bin < BIN_COUNT; ++bin)
                bins[bin] = i * 100.0 + static_cast<double>(bin);

            boost::asio::post(ioc, [&spectrumSignal, bins, i]
            {
                spectrumSignal.publish_data(
                    timeStart + i * timeDelta,
                    1,
                    bins.data(),
                    bins.size() * sizeof(double));
            });
        }

        std::vector<DataPacketPtr> packets;
        waitUntil([&]
        {
            for (auto packet = reader.read(); packet.assigned(); packet = reader.read())
                if (packet.getType() == PacketType::Data)
                    packets.push_back(packet);
            return packets.size() >= spectrumCount;
        }, 5s);

        ASSERT_EQ(packets.size(), static_cast<size_t>(spectrumCount));

        for (int i = 0; i < spectrumCount; ++i)
        {
            const auto& packet = packets[i];
            ASSERT_EQ(packet.getSampleCount(), 1u);
            ASSERT_EQ(packet.getDataDescriptor().getDimensions().getCount(), 1u);
            ASSERT_EQ(packet.getDataDescriptor().getDimensions()[0].getSize(), BIN_COUNT);

            const auto* data = static_cast<const double*>(packet.getData());
            EXPECT_EQ(data[0], i * 100.0);
            EXPECT_EQ(data[BIN_COUNT - 1], i * 100.0 + (BIN_COUNT - 1));

            const auto* time = static_cast<const std::int64_t*>(packet.getDomainPacket().getData());
            EXPECT_EQ(time[0], timeStart + i * timeDelta) << "spectrum " << i;
        }
    }

    boost::asio::post(ioc, [&server] { server.close(); });
    ioThread.join();
}
