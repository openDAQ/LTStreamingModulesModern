/*
 * End-to-end tests for the streaming client against a fake LT peer that mimics devices which
 * do not advertise their time signals ("hidden" domain signals referenced via "relatedSignals"
 * with an abstract table id) and which may drop or reorder command-interface requests.
 *
 * The fake peer accepts a WebSocket upgrade on a raw TCP socket (the client only checks the
 * HTTP status line) and then speaks the LT streaming protocol through the ws-streaming
 * library's own low-level peer class, with full control over metadata content and ordering.
 * Like the digiBOX-WT, it takes subscribe and unsubscribe requests on an HTTP command interface
 * ("jsonrpc-http"), one request per connection, and runs the requests that are in flight at the
 * same time last-first.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <ws-streaming/detail/peer.hpp>
#include <ws-streaming/detail/streaming_protocol.hpp>

#include <testutils/testutils.h>
#include <websocket_streaming/ws_streaming.h>
#include <websocket_streaming_client_module/module_dll.h>

#include <opendaq/context_factory.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/module_ptr.h>
#include <opendaq/opendaq.h>
#include <opendaq/search_filter_factory.h>

using namespace daq;
using namespace std::chrono_literals;

namespace
{

class FakeLtPeer
{
    public:

        struct Options
        {
            bool timeMetadataBeforeValue = true;    // send the time signal's metadata before the value signal's
            std::chrono::milliseconds timeMetadataDelay{0};  // extra delay before the time signal's metadata
            bool withholdTimeMetadata = false;      // announce the time signal but never send its metadata
            unsigned dropSubscribeRequests = 0;     // close this many leading value-signal subscribe requests unanswered
            bool streamData = false;                // stream value-signal data while it is subscribed
        };

        explicit FakeLtPeer(Options options)
            : options(options)
            , acceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , httpAcceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , timer(ioc)
            , dataTimer(ioc)
            , inFlightTimer(ioc)
        {
            acceptor.async_accept(
                [this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket)
                {
                    if (!ec)
                        handleAccept(std::move(socket));
                });

            acceptHttp();

            thread = std::thread([this] { ioc.run(); });
        }

        ~FakeLtPeer()
        {
            ioc.stop();
            thread.join();
        }

        std::uint16_t port() const
        {
            return acceptor.local_endpoint().port();
        }

        unsigned subscribeRequestCount()
        {
            std::scoped_lock lock(mutex);
            return valueSubscribeRequests;
        }

        unsigned timeSubscribeRequestCount()
        {
            std::scoped_lock lock(mutex);
            return timeSubscribeRequests;
        }

        unsigned valueUnsubscribeRequestCount()
        {
            std::scoped_lock lock(mutex);
            return valueUnsubscribeRequests;
        }

        // Advertises the previously hidden time signal in a second 'available' announcement
        void advertiseTimeSignal()
        {
            boost::asio::post(ioc,
                [this]
                {
                    if (peer)
                        peer->send_metadata(0, "available", {{ "signalIds", { timeSignalId } }});
                });
        }

    private:

        struct InFlightRequest
        {
            std::shared_ptr<boost::beast::tcp_stream> stream;
            nlohmann::json body;
        };

        void handleAccept(boost::asio::ip::tcp::socket socket)
        {
            auto sock = std::make_shared<boost::asio::ip::tcp::socket>(std::move(socket));
            auto buffer = std::make_shared<boost::asio::streambuf>();

            // consume the client's HTTP upgrade request; the client only checks the status line
            boost::asio::async_read_until(*sock, *buffer, "\r\n\r\n",
                [this, sock, buffer](const boost::system::error_code& ec, std::size_t)
                {
                    if (ec)
                        return;

                    auto response = std::make_shared<std::string>(
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: fake\r\n"
                        "\r\n");

                    boost::asio::async_write(*sock, boost::asio::buffer(*response),
                        [this, sock, response](const boost::system::error_code& ec, std::size_t)
                        {
                            if (!ec)
                                startPeer(std::move(*sock));
                        });
                });
        }

        void startPeer(boost::asio::ip::tcp::socket socket)
        {
            peer = std::make_shared<wss::detail::peer>(std::move(socket), false);
            peer->run();

            peer->send_metadata(0, "apiVersion", {{ "version", "1.0.0" }});
            peer->send_metadata(0, "init", {
                { "streamId", "FAKE" },
                { "commandInterfaces", { { "jsonrpc-http", {
                    { "httpMethod", "POST" },
                    { "httpPath", "/" },
                    { "httpVersion", "1.1" },
                    { "port", httpAcceptor.local_endpoint().port() },
                } } } },
            });
            peer->send_metadata(0, "available", {{ "signalIds", { valueSignalId } }});
        }

        void acceptHttp()
        {
            httpAcceptor.async_accept(
                [this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket)
                {
                    if (ec)
                        return;

                    readHttp(std::make_shared<boost::beast::tcp_stream>(std::move(socket)));
                    acceptHttp();
                });
        }

        void readHttp(std::shared_ptr<boost::beast::tcp_stream> stream)
        {
            auto buffer = std::make_shared<boost::beast::flat_buffer>();
            auto request = std::make_shared<boost::beast::http::request<boost::beast::http::string_body>>();

            boost::beast::http::async_read(*stream, *buffer, *request,
                [this, stream, buffer, request](const boost::system::error_code& ec, std::size_t)
                {
                    if (ec)
                        return;

                    // requests arriving within 20 ms of each other are in flight together
                    inFlight.push_back({ stream, nlohmann::json::parse(request->body(), nullptr, false) });
                    inFlightTimer.expires_after(20ms);
                    inFlightTimer.async_wait(
                        [this](const boost::system::error_code& ec)
                        {
                            if (!ec)
                                runInFlightRequests();
                        });
                });
        }

        // Runs the requests in flight last-first, as the digiBOX-WT ran them on 2026-08-14
        void runInFlightRequests()
        {
            auto requests = std::exchange(inFlight, {});

            for (auto it = requests.rbegin(); it != requests.rend(); ++it)
            {
                // a dropped request's connection closes unanswered with its last reference
                auto result = runRequest(it->body);
                if (!result)
                    continue;

                auto response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(
                    boost::beast::http::status::ok, 11);
                response->set(boost::beast::http::field::content_type, "application/json");
                response->body() = result->dump();
                response->prepare_payload();

                boost::beast::http::async_write(*it->stream, *response,
                    [stream = it->stream, response](const boost::system::error_code&, std::size_t) {});
            }
        }

        // Runs a JSON-RPC request before answering it, like the device; nothing means the request is dropped
        std::optional<nlohmann::json> runRequest(const nlohmann::json& request)
        {
            if (!request.is_object())
                return std::nullopt;

            const auto id = request.value<nlohmann::json>("id", nullptr);
            const std::string rpcMethod = request.value("method", std::string());

            std::string signalId;
            if (request.contains("params") && request["params"].is_array()
                    && !request["params"].empty() && request["params"][0].is_string())
                signalId = request["params"][0];

            if (rpcMethod == "FAKE.subscribe" && signalId == valueSignalId)
            {
                {
                    std::scoped_lock lock(mutex);
                    ++valueSubscribeRequests;
                    if (valueSubscribeRequests <= options.dropSubscribeRequests)
                        return std::nullopt;
                }

                // a subscribe of a subscribed signal is rejected, and the data keeps flowing
                if (valueSubscribed)
                    return error(id);

                valueSubscribed = true;
                sendValueSignalFamily();

                if (options.streamData)
                    startStreamingData();
            }

            else if (rpcMethod == "FAKE.subscribe" && signalId == timeSignalId)
            {
                std::scoped_lock lock(mutex);
                ++timeSubscribeRequests;
            }

            else if (rpcMethod == "FAKE.unsubscribe" && signalId == valueSignalId)
            {
                {
                    std::scoped_lock lock(mutex);
                    ++valueUnsubscribeRequests;
                }

                valueSubscribed = false;
                peer->send_metadata(valueSigno, "unsubscribe", nlohmann::json::object());
            }

            return nlohmann::json{
                { "jsonrpc", "2.0" },
                { "id", id },
                { "result", true },
            };
        }

        static nlohmann::json error(const nlohmann::json& id)
        {
            return {
                { "jsonrpc", "2.0" },
                { "id", id },
                { "error", { { "code", -32602 }, { "message", "already subscribed" } } },
            };
        }

        void startStreamingData()
        {
            // give the linear time table its start point, then pump value samples periodically
            peer->send_data(timeSigno, boost::asio::buffer(&timeStart, sizeof(timeStart)));
            sendValueData();
        }

        void sendValueData()
        {
            if (!valueSubscribed)
                return;

            peer->send_data(valueSigno, boost::asio::buffer(valueSamples));

            dataTimer.expires_after(20ms);
            dataTimer.async_wait(
                [this](const boost::system::error_code& ec)
                {
                    if (!ec)
                        sendValueData();
                });
        }

        void sendValueSignalFamily()
        {
            // subscribing the value signal implicitly announces its hidden time signal
            peer->send_metadata(valueSigno, "subscribe", {{ "signalId", valueSignalId }});
            peer->send_metadata(timeSigno, "subscribe", {{ "signalId", timeSignalId }});

            if (options.timeMetadataBeforeValue)
            {
                sendTimeMetadata();
                sendValueMetadata();
            }
            else
            {
                sendValueMetadata();

                if (options.timeMetadataDelay.count() > 0)
                {
                    timer.expires_after(options.timeMetadataDelay);
                    timer.async_wait(
                        [this](const boost::system::error_code& ec)
                        {
                            if (!ec)
                                sendTimeMetadata();
                        });
                }
                else
                {
                    sendTimeMetadata();
                }
            }
        }

        void sendValueMetadata()
        {
            peer->send_metadata(valueSigno, "signal", {
                { "tableId", tableId },
                { "relatedSignals", {
                    { { "type", "time" }, { "signalId", timeSignalId } },
                } },
                { "definition", {
                    { "name", valueSignalId },
                    { "rule", "explicit" },
                    { "dataType", "real32" },
                } },
            });
        }

        void sendTimeMetadata()
        {
            if (options.withholdTimeMetadata)
                return;

            peer->send_metadata(timeSigno, "signal", {
                { "tableId", tableId },
                { "definition", {
                    { "name", timeSignalId },
                    { "rule", "linear" },
                    { "linear", { { "delta", 1 } } },
                    { "dataType", "uint64" },
                    { "resolution", { { "num", 1 }, { "denom", 1000 } } },
                } },
            });
        }

        const std::string tableId = "CH1";
        const std::string valueSignalId = "CH1.value";
        const std::string timeSignalId = "CH1.time";
        static constexpr unsigned valueSigno = 1;
        static constexpr unsigned timeSigno = 2;

        Options options;

        boost::asio::io_context ioc{1};
        boost::asio::ip::tcp::acceptor acceptor;
        boost::asio::ip::tcp::acceptor httpAcceptor;
        boost::asio::steady_timer timer;
        boost::asio::steady_timer dataTimer;
        boost::asio::steady_timer inFlightTimer;
        std::thread thread;

        // sent by reference from the asynchronous send_data(), so they must outlive the calls
        const wss::detail::streaming_protocol::linear_payload timeStart{0, 0};
        const std::vector<float> valueSamples = std::vector<float>(100, 1.0f);

        bool valueSubscribed = false;           // only touched on the ioc thread
        std::vector<InFlightRequest> inFlight;  // only touched on the ioc thread

        std::shared_ptr<wss::detail::peer> peer;

        std::mutex mutex;
        unsigned valueSubscribeRequests = 0;
        unsigned timeSubscribeRequests = 0;
        unsigned valueUnsubscribeRequests = 0;
};

// An Instance owns the device so that teardown runs the device's removal path;
// creating a device directly from the module would leak it (and fail the leak listener)
InstancePtr createClientInstance()
{
    auto instance = Instance("[[none]]");

    ModulePtr module;
    createModule(&module, instance.getContext());
    instance.getModuleManager().addModule(module);

    return instance;
}

DevicePtr connectDevice(const InstancePtr& instance, std::uint16_t port)
{
    return instance.addDevice("daq.lt://127.0.0.1:" + std::to_string(port) + "/");
}

// Polls until the device exposes the expected number of signals or the timeout elapses.
ListPtr<ISignal> waitForSignals(
    const DevicePtr& device,
    size_t expectedCount,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    ListPtr<ISignal> signals;
    do
    {
        signals = device.getSignals(search::Recursive(search::Any()));
        if (signals.getCount() >= expectedCount)
            return signals;
        std::this_thread::sleep_for(50ms);
    } while (std::chrono::steady_clock::now() < deadline);

    return signals;
}

SignalPtr findSignalByName(const ListPtr<ISignal>& signals, const std::string& name)
{
    for (const auto& signal : signals)
        if (signal.getDescriptor().assigned() && signal.getDescriptor().getName() == name)
            return signal;
    return nullptr;
}

// Returns the signal the moment it appears, like an application that subscribes every new signal
// on sight (lt_race polls every millisecond, 2026-08-19)
SignalPtr waitForSignal(const DevicePtr& device, const std::string& name, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    do
    {
        if (auto signal = findSignalByName(device.getSignals(search::Recursive(search::Any())), name); signal.assigned())
            return signal;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);

    return nullptr;
}

// Bundles the connected client so tests keep the owning Instance alive
struct ClientSetup
{
    InstancePtr instance;
    DevicePtr device;
    ListPtr<ISignal> signals;
};

ClientSetup connectAndWaitForSignals(
    const FakeLtPeer& peer,
    size_t expectedCount = 2,
    std::chrono::milliseconds timeout = 5s)
{
    ClientSetup setup;
    setup.instance = createClientInstance();
    setup.device = connectDevice(setup.instance, peer.port());
    setup.signals = waitForSignals(setup.device, expectedCount, timeout);
    return setup;
}

// Builds a Float64/Int64 stream reader on the signal, subscribing it
auto buildStreamReader(const SignalPtr& signal)
{
    return daq::StreamReaderBuilder()
        .setSignal(signal)
        .setValueReadType(daq::SampleType::Float64)
        .setDomainReadType(daq::SampleType::Int64)
        .setSkipEvents(true)
        .build();
}

}  // namespace

using DeviceCompatibilityTest = testing::Test;

TEST_F(DeviceCompatibilityTest, HandlerConnectedLateGetsEverySignal)
{
    FakeLtPeer peer({});

    std::mutex mutex;
    std::vector<std::string> signalIds;

    auto streaming = createWithImplementation<IStreaming, websocket_streaming::WsStreaming>(
        String("daq.lt://127.0.0.1:" + std::to_string(peer.port()) + "/"), NullContext(), nullptr);
    auto& wsStreaming = *reinterpret_cast<websocket_streaming::WsStreaming*>(streaming.getObject());

    // a creator under load connects its handler well after construction
    std::this_thread::sleep_for(200ms);

    boost::signals2::scoped_connection onAvailable = wsStreaming.onSignalAvailable.connect(
        [&](wss::remote_signal_ptr signal, wss::remote_signal_ptr, const DataDescriptorPtr&)
        {
            std::scoped_lock lock(mutex);
            signalIds.push_back(signal->id());
        });

    wsStreaming.connect();

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::scoped_lock lock(mutex);
            if (signalIds.size() >= 2)
                break;
        }
        std::this_thread::sleep_for(50ms);
    }

    std::scoped_lock lock(mutex);
    std::sort(signalIds.begin(), signalIds.end());
    EXPECT_EQ(signalIds, (std::vector<std::string>{ "CH1.time", "CH1.value" }));
}

TEST_F(DeviceCompatibilityTest, HiddenDomainSignalIsLinked)
{
    FakeLtPeer peer({});
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    auto timeSignal = findSignalByName(signals, "CH1.time");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(timeSignal.assigned());

    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
    ASSERT_EQ(valueSignal.getDomainSignal(), timeSignal);
}

TEST_F(DeviceCompatibilityTest, DomainMetadataArrivingLateIsStillLinked)
{
    FakeLtPeer::Options options;
    options.timeMetadataBeforeValue = false;
    options.timeMetadataDelay = 1s;

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer, 1);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    // connected before the time signal's metadata arrives, the reader learns the domain from an event
    auto reader = PacketReader(valueSignal);

    // the link lands after the time signal is added, so the signal count does not show it
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!valueSignal.getDomainSignal().assigned() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(50ms);

    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
    ASSERT_EQ(valueSignal.getDomainSignal(),
        findSignalByName(device.getSignals(search::Recursive(search::Any())), "CH1.time"));

    DataDescriptorPtr domainDescriptor;
    for (const auto& packet : reader.readAll())
        if (const auto event = packet.asPtrOrNull<IEventPacket>();
                event.assigned() && event.getEventId() == event_packet_id::DATA_DESCRIPTOR_CHANGED)
            if (const DataDescriptorPtr descriptor = event.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR);
                    descriptor.assigned())
                domainDescriptor = descriptor;

    ASSERT_TRUE(domainDescriptor.assigned());
    ASSERT_EQ(domainDescriptor.getName(), "CH1.time");
}

TEST_F(DeviceCompatibilityTest, DroppedSubscribeRequestIsRetried)
{
    FakeLtPeer::Options options;
    options.dropSubscribeRequests = 1;

    FakeLtPeer peer(options);

    // ws-streaming sends a request that got no answer once more
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);
    ASSERT_EQ(peer.subscribeRequestCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
}

TEST_F(DeviceCompatibilityTest, ReadvertisedHiddenDomainSignalStartsNoNewFetch)
{
    FakeLtPeer peer({});
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    // the device now advertises the already-published hidden time signal
    peer.advertiseTimeSignal();
    std::this_thread::sleep_for(500ms);

    // no duplicate signal, the domain link is intact, and no fetch subscribe was sent for it
    signals = device.getSignals(search::Recursive(search::Any()));
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());

    ASSERT_EQ(peer.timeSubscribeRequestCount(), 0u);
}

// The device's ack of the fetch subscription's release must not be taken for an answer to the application
TEST_F(DeviceCompatibilityTest, ImmediateSubscribeGetsOneSubscribeAck)
{
    FakeLtPeer peer({});

    std::atomic<unsigned> subscribeAcks{0};
    std::atomic<unsigned> unsubscribeAcks{0};

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    // subscribe the moment the signal appears, while the release of its fetch subscription is in flight
    auto valueSignal = waitForSignal(device, "CH1.value", 5s);
    ASSERT_TRUE(valueSignal.assigned());

    auto mirrored = valueSignal.asPtr<IMirroredSignalConfig>();
    mirrored.getOnSubscribeComplete() +=
        [&subscribeAcks](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { ++subscribeAcks; };
    mirrored.getOnUnsubscribeComplete() +=
        [&unsubscribeAcks](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { ++unsubscribeAcks; };

    auto reader = buildStreamReader(valueSignal);
    std::this_thread::sleep_for(1s);

    EXPECT_EQ(subscribeAcks.load(), 1u);
    EXPECT_EQ(unsubscribeAcks.load(), 0u);
    EXPECT_EQ(peer.subscribeRequestCount(), 2u);         // the fetch, then the application
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);  // the release of the fetch subscription
}

// A device running the fetch subscription's release and an immediate subscribe last-first stops the data
TEST_F(DeviceCompatibilityTest, DataKeepsFlowingWhenDeviceReordersUnsubscribeAndSubscribe)
{
    FakeLtPeer::Options options;
    options.streamData = true;
    FakeLtPeer peer(options);

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    // subscribe the moment the signal appears, while the release of its fetch subscription is in flight
    auto valueSignal = waitForSignal(device, "CH1.value", 5s);
    ASSERT_TRUE(valueSignal.assigned());
    auto reader = buildStreamReader(valueSignal);

    // discard the startup burst; the failure mode goes silent after it
    std::this_thread::sleep_for(1s);

    if (SizeT count = reader.getAvailableCount(); count > 0)
    {
        std::vector<double> values(count);
        std::vector<std::int64_t> domain(count);
        reader.readWithDomain(values.data(), domain.data(), &count);
    }

    std::this_thread::sleep_for(500ms);
    EXPECT_GT(reader.getAvailableCount(), 0u);
}

// openDAQ unsubscribes and subscribes again before the device answers; the subscribe must still go out
TEST_F(DeviceCompatibilityTest, ResubscribeWithinOneRoundTripKeepsDataFlowing)
{
    FakeLtPeer::Options options;
    options.streamData = true;
    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    auto reader = buildStreamReader(valueSignal);
    std::this_thread::sleep_for(500ms);

    // releasing the reader unsubscribes, and the next reader subscribes within the same round trip
    reader.release();
    reader = buildStreamReader(valueSignal);

    std::this_thread::sleep_for(1s);
    if (SizeT count = reader.getAvailableCount(); count > 0)
    {
        std::vector<double> values(count);
        std::vector<std::int64_t> domain(count);
        reader.readWithDomain(values.data(), domain.data(), &count);
    }

    std::this_thread::sleep_for(500ms);
    EXPECT_GT(reader.getAvailableCount(), 0u);
}

TEST_F(DeviceCompatibilityTest, FetchSubscriptionEndsAtPublication)
{
    FakeLtPeer peer({});
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    // nobody subscribes: the fetch subscription ended with the signal's publication
    std::this_thread::sleep_for(500ms);

    EXPECT_EQ(peer.subscribeRequestCount(), 1u);
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);
}

TEST_F(DeviceCompatibilityTest, SignalPublishesWithoutDomainWhenMetadataNeverArrives)
{
    FakeLtPeer::Options options;
    options.timeMetadataBeforeValue = false;
    options.withholdTimeMetadata = true;

    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer, 1);
    ASSERT_EQ(signals.getCount(), 1u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_FALSE(valueSignal.getDomainSignal().assigned());
}
