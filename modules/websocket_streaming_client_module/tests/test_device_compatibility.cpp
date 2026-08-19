/*
 * End-to-end tests for the streaming client against a fake LT peer that mimics devices which
 * do not advertise their time signals ("hidden" domain signals referenced via "relatedSignals"
 * with an abstract table id) and which may drop or reorder command-interface requests.
 *
 * The fake peer accepts a WebSocket upgrade on a raw TCP socket (the client only checks the
 * HTTP status line) and then speaks the LT streaming protocol through the ws-streaming
 * library's own low-level peer class, with full control over metadata content, ordering and
 * request handling.
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <nlohmann/json.hpp>

#include <ws-streaming/detail/peer.hpp>
#include <ws-streaming/detail/streaming_protocol.hpp>

#include <testutils/testutils.h>
#include <websocket_streaming_client_module/module_dll.h>

#include <opendaq/context_factory.h>
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
            unsigned dropSubscribeRequests = 0;     // ignore this many leading value-signal subscribe requests
            bool streamData = false;                // stream value-signal data while it is subscribed
            bool outOfOrderUnsubscribe = false;     // defer a value-signal unsubscribe behind the next request
        };

        explicit FakeLtPeer(Options options)
            : options(options)
            , acceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , timer(ioc)
            , dataTimer(ioc)
        {
            acceptor.async_accept(
                [this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket)
                {
                    if (!ec)
                        handleAccept(std::move(socket));
                });

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

        // Unilaterally ends the value-signal subscription, like a device dropping it on its own
        void unsubscribeValueSignal()
        {
            boost::asio::post(ioc,
                [this]
                {
                    if (!peer)
                        return;
                    valueSubscribed = false;
                    peer->send_metadata(valueSigno, "unsubscribe", nlohmann::json::object());
                });
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

            onMetadata = peer->on_metadata_received.connect(
                [this](unsigned signo, const std::string& method, const nlohmann::json& params)
                {
                    handleMetadata(signo, method, params);
                });

            peer->run();

            peer->send_metadata(0, "apiVersion", {{ "version", "1.0.0" }});
            peer->send_metadata(0, "init", {
                { "streamId", "FAKE" },
                { "commandInterfaces", { { "jsonrpc", nlohmann::json::object() } } },
            });
            peer->send_metadata(0, "available", {{ "signalIds", { valueSignalId } }});
        }

        void handleMetadata(unsigned /*signo*/, const std::string& method, const nlohmann::json& params)
        {
            if (method != "request" || !params.is_object())
                return;

            const auto id = params.value<nlohmann::json>("id", nullptr);
            const std::string rpcMethod = params.value("method", std::string());

            std::string signalId;
            if (params.contains("params") && params["params"].is_array()
                    && !params["params"].empty() && params["params"][0].is_string())
                signalId = params["params"][0];

            if (rpcMethod == "FAKE.subscribe" && signalId == valueSignalId)
            {
                {
                    std::scoped_lock lock(mutex);
                    ++valueSubscribeRequests;
                    if (valueSubscribeRequests <= options.dropSubscribeRequests)
                        return;  // simulate a dropped request: no response, no effect
                }

                // a subscribe processed while still subscribed is rejected, then the deferred unsubscribe runs
                if (valueSubscribed && options.outOfOrderUnsubscribe)
                {
                    respondError(id);
                    flushPendingValueUnsubscribe();
                    return;
                }

                valueSubscribed = true;
                respond(id, true);
                sendValueSignalFamily();

                if (options.streamData)
                    startStreamingData();
            }

            else if (rpcMethod == "FAKE.subscribe" && signalId == timeSignalId)
            {
                std::scoped_lock lock(mutex);
                ++timeSubscribeRequests;
            }

            else if (rpcMethod == "FAKE.unsubscribe")
            {
                if (signalId == valueSignalId)
                {
                    if (options.outOfOrderUnsubscribe)
                        pendingValueUnsubscribeId = id;  // sit on it until the next request
                    else
                        executeValueUnsubscribe(id);
                    return;
                }

                respond(id, true);
            }
        }

        void executeValueUnsubscribe(const nlohmann::json& id)
        {
            {
                std::scoped_lock lock(mutex);
                ++valueUnsubscribeRequests;
            }

            valueSubscribed = false;
            respond(id, true);
            peer->send_metadata(valueSigno, "unsubscribe", nlohmann::json::object());
        }

        void flushPendingValueUnsubscribe()
        {
            if (!pendingValueUnsubscribeId)
                return;

            executeValueUnsubscribe(*pendingValueUnsubscribeId);
            pendingValueUnsubscribeId.reset();
        }

        void respond(const nlohmann::json& id, bool result)
        {
            peer->send_metadata(0, "response", {
                { "jsonrpc", "2.0" },
                { "id", id },
                { "result", result },
            });
        }

        void respondError(const nlohmann::json& id)
        {
            peer->send_metadata(0, "response", {
                { "jsonrpc", "2.0" },
                { "id", id },
                { "error", { { "code", -32602 }, { "message", "already subscribed" } } },
            });
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
        boost::asio::steady_timer timer;
        boost::asio::steady_timer dataTimer;
        std::thread thread;

        // sent by reference from the asynchronous send_data(), so they must outlive the calls
        const wss::detail::streaming_protocol::linear_payload timeStart{0, 0};
        const std::vector<float> valueSamples = std::vector<float>(100, 1.0f);

        bool valueSubscribed = false;  // only touched on the ioc thread
        std::optional<nlohmann::json> pendingValueUnsubscribeId;

        std::shared_ptr<wss::detail::peer> peer;
        boost::signals2::scoped_connection onMetadata;

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
    options.timeMetadataDelay = 300ms;

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    // the value signal must have been deferred until the time signal published
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
}

TEST_F(DeviceCompatibilityTest, DroppedSubscribeRequestIsRetried)
{
    FakeLtPeer::Options options;
    options.dropSubscribeRequests = 1;

    FakeLtPeer peer(options);

    // the sweep timer retries after 1.5 s + 100 ms; allow generous margin
    auto [instance, device, signals] = connectAndWaitForSignals(peer, 2, 10s);
    ASSERT_EQ(signals.getCount(), 2u);
    ASSERT_GE(peer.subscribeRequestCount(), 2u);

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

TEST_F(DeviceCompatibilityTest, ImmediateSubscribeTakesOverFetchSubscription)
{
    FakeLtPeer peer({});
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    // subscribe immediately after the signal appeared, like an auto-subscribing application
    auto mirrored = valueSignal.asPtr<IMirroredSignalConfig>();
    std::promise<void> ackPromise;
    auto ackFuture = ackPromise.get_future();
    mirrored.getOnSubscribeComplete() +=
        [&ackPromise](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { ackPromise.set_value(); };

    auto reader = buildStreamReader(valueSignal);

    // the takeover must acknowledge the subscription without any wire traffic
    ASSERT_EQ(ackFuture.wait_for(3s), std::future_status::ready);

    // give the sweep time to have (wrongly) released the fetch subscription
    std::this_thread::sleep_for(4s);

    EXPECT_EQ(peer.subscribeRequestCount(), 1u);         // only the initial fetch subscribed
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 0u);  // never released: taken over
}

// Without the takeover, a device swapping the release unsubscribe with the app subscribe stops the data
TEST_F(DeviceCompatibilityTest, DataKeepsFlowingWhenDeviceReordersUnsubscribeAndSubscribe)
{
    FakeLtPeer::Options options;
    options.streamData = true;
    options.outOfOrderUnsubscribe = true;
    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    // subscribe immediately after the signal appeared, like an auto-subscribing application
    auto reader = buildStreamReader(valueSignal);

    // wait past the sweep window and discard the startup burst (the failure mode goes silent after it)
    std::this_thread::sleep_for(4s);

    if (SizeT count = reader.getAvailableCount(); count > 0)
    {
        std::vector<double> values(count);
        std::vector<std::int64_t> domain(count);
        reader.readWithDomain(values.data(), domain.data(), &count);
    }

    std::this_thread::sleep_for(500ms);
    EXPECT_GT(reader.getAvailableCount(), 0u);
}

// A remote unsubscribe must end the held fetch subscription; a takeover of it would never get data
TEST_F(DeviceCompatibilityTest, SubscribeAfterRemoteUnsubscribeSendsNewRequest)
{
    FakeLtPeer::Options options;
    options.streamData = true;
    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    // the device drops the subscription on its own while it is held for takeover
    peer.unsubscribeValueSignal();
    std::this_thread::sleep_for(500ms);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    auto reader = buildStreamReader(valueSignal);

    std::this_thread::sleep_for(1s);
    EXPECT_EQ(peer.subscribeRequestCount(), 2u);  // a real second subscribe request was sent
    EXPECT_GT(reader.getAvailableCount(), 0u);    // and data flows again
}

TEST_F(DeviceCompatibilityTest, UnusedFetchSubscriptionIsReleasedBySweep)
{
    FakeLtPeer peer({});
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    // nobody subscribes: the sweep must release the held fetch subscription
    std::this_thread::sleep_for(4s);

    EXPECT_EQ(peer.subscribeRequestCount(), 1u);
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);
}

TEST_F(DeviceCompatibilityTest, SignalPublishesWithoutDomainWhenMetadataNeverArrives)
{
    FakeLtPeer::Options options;
    options.timeMetadataBeforeValue = false;
    options.withholdTimeMetadata = true;

    FakeLtPeer peer(options);

    // deferral gives up after two sweep periods (~3 s); allow generous margin
    auto [instance, device, signals] = connectAndWaitForSignals(peer, 1, 10s);
    ASSERT_EQ(signals.getCount(), 1u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_FALSE(valueSignal.getDomainSignal().assigned());
}
