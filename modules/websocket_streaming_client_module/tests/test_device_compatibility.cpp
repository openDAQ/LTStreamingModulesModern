/*
 * End-to-end tests for the streaming client against a fake LT peer that mimics devices which
 * do not advertise their time signals ("hidden" domain signals referenced via "relatedSignals"
 * with an abstract table id) and which may drop or reorder command-interface requests.
 *
 * The fake peer accepts a WebSocket upgrade on a raw TCP socket (the client only checks the
 * HTTP status line) and then speaks the LT streaming protocol through the ws-streaming
 * library's own low-level peer class, with full control over metadata content, ordering and
 * request handling.
 *
 * Every test runs against both command interfaces a device can offer: in-band JSON-RPC over the
 * streaming WebSocket, and JSON-RPC over HTTP on a port of its own (the digiBOX-WT has only that).
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
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
#include <opendaq/module_ptr.h>
#include <opendaq/opendaq.h>
#include <opendaq/search_filter_factory.h>

using namespace daq;
using namespace std::chrono_literals;

namespace
{

enum class CommandTransport
{
    InBand,  // JSON-RPC requests and responses as metadata of the streaming WebSocket
    Http,    // JSON-RPC over HTTP, one connection per request
};

const char* transportName(CommandTransport transport)
{
    return transport == CommandTransport::InBand ? "InBand" : "Http";
}

// Lets GoogleTest print the transport of a failed test by name
void PrintTo(CommandTransport transport, std::ostream* os)
{
    *os << transportName(transport);
}

class FakeLtPeer
{
    public:

        struct Options
        {
            CommandTransport transport = CommandTransport::InBand;
            bool timeMetadataBeforeValue = true;    // send the time signal's metadata before the value signal's
            std::chrono::milliseconds timeMetadataDelay{0};  // extra delay before the time signal's metadata
            bool withholdTimeMetadata = false;      // announce the time signal but never send its metadata
            unsigned dropSubscribeRequests = 0;     // leave this many leading value-signal subscribe requests unanswered
                                                    // and without effect (over HTTP: close their connections)
            bool streamData = false;                // stream value-signal data while it is subscribed
            bool outOfOrderUnsubscribe = false;     // run a value-signal unsubscribe after the request that follows it:
                                                    // in-band, defer it behind the next request; over HTTP, run the
                                                    // requests in flight together last-first, as the digiBOX-WT did
            std::chrono::milliseconds requestDelay{0};  // run requests one by one, each this long after the previous
            bool explicitTime = false;              // the time signal carries explicit timestamps instead of a linear rule
        };

        explicit FakeLtPeer(Options options)
            : options(options)
            , acceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , httpAcceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , timer(ioc)
            , dataTimer(ioc)
            , requestTimer(ioc)
            , inFlightTimer(ioc)
        {
            acceptor.async_accept(
                [this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket)
                {
                    if (ec)
                        return;

                    {
                        std::scoped_lock lock(mutex);
                        ++acceptedConnections;
                    }

                    handleAccept(std::move(socket));
                });

            if (options.transport == CommandTransport::Http)
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

        // Counted before the peer answers the WebSocket upgrade, so before the client's connect completes
        unsigned acceptedConnectionCount()
        {
            std::scoped_lock lock(mutex);
            return acceptedConnections;
        }

        unsigned httpRequestCount()
        {
            std::scoped_lock lock(mutex);
            return httpRequests;
        }

        // In-band requests sent to a device that offers only HTTP; it does not answer them
        unsigned wrongTransportRequestCount()
        {
            std::scoped_lock lock(mutex);
            return wrongTransportRequests;
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

            nlohmann::json commandInterfaces = { { "jsonrpc", nlohmann::json::object() } };
            if (options.transport == CommandTransport::Http)
                commandInterfaces = { { "jsonrpc-http", {
                    { "httpMethod", "POST" },
                    { "httpPath", "/" },
                    { "httpVersion", "1.1" },
                    { "port", httpAcceptor.local_endpoint().port() },
                } } };

            peer->send_metadata(0, "apiVersion", {{ "version", "1.0.0" }});
            peer->send_metadata(0, "init", {
                { "streamId", "FAKE" },
                { "commandInterfaces", commandInterfaces },
            });
            peer->send_metadata(0, "available", {{ "signalIds", { valueSignalId } }});
        }

        // Answers a request over the transport it came in on; a request whose reply is never called stays unanswered
        using Reply = std::function<void(const nlohmann::json& response)>;

        struct Request
        {
            nlohmann::json params;
            Reply reply;
        };

        struct PendingUnsubscribe
        {
            nlohmann::json id;
            Reply reply;
        };

        void handleMetadata(unsigned /*signo*/, const std::string& method, const nlohmann::json& params)
        {
            if (method != "request" || !params.is_object())
                return;

            // a device that offers only HTTP does not answer in-band, so a client sending in-band would get nothing
            if (options.transport != CommandTransport::InBand)
            {
                std::scoped_lock lock(mutex);
                ++wrongTransportRequests;
                return;
            }

            handleRequest(params, [this](const nlohmann::json& response) { peer->send_metadata(0, "response", response); });
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

                    auto params = nlohmann::json::parse(request->body(), nullptr, false);
                    if (!params.is_object())
                        return;

                    {
                        std::scoped_lock lock(mutex);
                        ++httpRequests;
                    }

                    // the connection closes unanswered once the last copy of its reply is gone
                    Reply reply = [stream](const nlohmann::json& body)
                    {
                        auto response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(
                            boost::beast::http::status::ok, 11);
                        response->set(boost::beast::http::field::content_type, "application/json");
                        response->body() = body.dump();
                        response->prepare_payload();

                        boost::beast::http::async_write(*stream, *response,
                            [stream, response](const boost::system::error_code&, std::size_t) {});
                    };

                    if (!options.outOfOrderUnsubscribe)
                        return handleRequest(std::move(params), std::move(reply));

                    // requests arriving within 20 ms of each other are in flight together
                    inFlight.push_back({ std::move(params), std::move(reply) });
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
                runRequest(it->params, it->reply);
        }

        void handleRequest(nlohmann::json params, Reply reply)
        {
            if (options.requestDelay.count() == 0)
                return runRequest(params, reply);

            delayedRequests.push_back({ std::move(params), std::move(reply) });
            if (delayedRequests.size() == 1)
                runDelayedRequest();
        }

        void runDelayedRequest()
        {
            requestTimer.expires_after(options.requestDelay);
            requestTimer.async_wait(
                [this](const boost::system::error_code& ec)
                {
                    if (ec)
                        return;

                    auto request = std::move(delayedRequests.front());
                    delayedRequests.pop_front();
                    runRequest(request.params, request.reply);

                    if (!delayedRequests.empty())
                        runDelayedRequest();
                });
        }

        void runRequest(const nlohmann::json& params, const Reply& reply)
        {
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
                    reply(error(id));
                    flushPendingValueUnsubscribe();
                    return;
                }

                valueSubscribed = true;
                streamStart = sampleIndexNow();
                reply(result(id));
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
                    // over HTTP the requests in flight together are reordered instead
                    if (options.outOfOrderUnsubscribe && options.transport == CommandTransport::InBand)
                        pendingValueUnsubscribe = { id, reply };  // sit on it until the next request
                    else
                        executeValueUnsubscribe(id, reply);
                    return;
                }

                reply(result(id));
            }
        }

        void executeValueUnsubscribe(const nlohmann::json& id, const Reply& reply)
        {
            {
                std::scoped_lock lock(mutex);
                ++valueUnsubscribeRequests;
            }

            valueSubscribed = false;
            reply(result(id));
            peer->send_metadata(valueSigno, "unsubscribe", nlohmann::json::object());
        }

        void flushPendingValueUnsubscribe()
        {
            if (!pendingValueUnsubscribe)
                return;

            auto pending = std::move(*pendingValueUnsubscribe);
            pendingValueUnsubscribe.reset();
            executeValueUnsubscribe(pending.id, pending.reply);
        }

        static nlohmann::json result(const nlohmann::json& id)
        {
            return {
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

        // The device's sample clock runs at 5 kHz whether or not anybody is subscribed; a new stream starts where it stands
        std::int64_t sampleIndexNow() const
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - created).count() * 5;
        }

        void startStreamingData()
        {
            // give the linear time table its start point, then pump value samples periodically
            if (!options.explicitTime)
            {
                timeStart = { streamStart, streamStart };
                peer->send_data(timeSigno, boost::asio::buffer(&timeStart, sizeof(timeStart)));
            }

            sendValueData();
        }

        void sendValueData()
        {
            if (!valueSubscribed)
                return;

            // an explicit time signal sends a timestamp for each value sample, ahead of the values
            if (options.explicitTime)
                peer->send_data(timeSigno, boost::asio::buffer(timeSamples));
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
                { "valueIndex", streamStart },
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

            nlohmann::json definition = {
                { "name", timeSignalId },
                { "rule", "linear" },
                { "linear", { { "delta", 1 } } },
                { "dataType", "uint64" },
                { "resolution", { { "num", 1 }, { "denom", 1000 } } },
            };

            if (options.explicitTime)
            {
                definition["rule"] = "explicit";
                definition.erase("linear");
            }

            peer->send_metadata(timeSigno, "signal", {
                { "tableId", tableId },
                { "definition", definition },
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
        boost::asio::steady_timer requestTimer;
        boost::asio::steady_timer inFlightTimer;
        std::thread thread;

        // only touched on the ioc thread; an HTTP request's reply holds its connection open
        std::deque<Request> delayedRequests;
        std::vector<Request> inFlight;
        std::optional<PendingUnsubscribe> pendingValueUnsubscribe;

        const std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
        std::int64_t streamStart = 0;                // only touched on the ioc thread

        // sent by reference from the asynchronous send_data(), so they must outlive the calls
        wss::detail::streaming_protocol::linear_payload timeStart{0, 0};
        const std::vector<float> valueSamples = std::vector<float>(100, 1.0f);
        const std::vector<std::uint64_t> timeSamples = []
        {
            std::vector<std::uint64_t> samples(100);
            std::iota(samples.begin(), samples.end(), 1000);
            return samples;
        }();

        bool valueSubscribed = false;  // only touched on the ioc thread

        std::shared_ptr<wss::detail::peer> peer;
        boost::signals2::scoped_connection onMetadata;

        std::mutex mutex;
        unsigned acceptedConnections = 0;
        unsigned httpRequests = 0;
        unsigned wrongTransportRequests = 0;
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

// Waits up to the timeout for count samples (or a domain gap) and discards what it read; returns how many it read
template <typename Reader>
SizeT readSamples(Reader& reader, SizeT count, std::chrono::milliseconds timeout)
{
    std::vector<double> values(count);
    std::vector<std::int64_t> domain(count);
    reader.readWithDomain(values.data(), domain.data(), &count, static_cast<SizeT>(timeout.count()));
    return count;
}

// Creates the streaming object on its own, without the device that normally owns it
StreamingPtr createStreaming(const FakeLtPeer& peer)
{
    return createWithImplementation<IStreaming, websocket_streaming::WsStreaming>(
        String("daq.lt://127.0.0.1:" + std::to_string(peer.port()) + "/"), NullContext(), nullptr);
}

}  // namespace

class DeviceCompatibilityTest : public testing::TestWithParam<CommandTransport>
{
    protected:

        // The options of a fake device whose command interface uses the transport under test
        FakeLtPeer::Options peerOptions() const
        {
            FakeLtPeer::Options options;
            options.transport = GetParam();
            return options;
        }
};

INSTANTIATE_TEST_SUITE_P(
    CommandTransports,
    DeviceCompatibilityTest,
    testing::Values(CommandTransport::InBand, CommandTransport::Http),
    [](const testing::TestParamInfo<CommandTransport>& info) { return std::string(transportName(info.param)); });

// Every other test relies on this: the client sends its requests over the command interface the device offers
TEST_P(DeviceCompatibilityTest, RequestsUseTheOfferedCommandInterface)
{
    FakeLtPeer peer(peerOptions());
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    EXPECT_EQ(peer.wrongTransportRequestCount(), 0u);
    if (GetParam() == CommandTransport::Http)
        EXPECT_GE(peer.httpRequestCount(), 1u);
    else
        EXPECT_EQ(peer.httpRequestCount(), 0u);
}

TEST_P(DeviceCompatibilityTest, HiddenDomainSignalIsLinked)
{
    FakeLtPeer peer(peerOptions());
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    auto timeSignal = findSignalByName(signals, "CH1.time");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(timeSignal.assigned());

    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
    ASSERT_EQ(valueSignal.getDomainSignal(), timeSignal);
}

TEST_P(DeviceCompatibilityTest, DomainMetadataArrivingLateIsStillLinked)
{
    auto options = peerOptions();
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

TEST_P(DeviceCompatibilityTest, DroppedSubscribeRequestIsRetried)
{
    if (GetParam() == CommandTransport::InBand)
        GTEST_SKIP() << "no device drops in-band requests";

    auto options = peerOptions();
    options.dropSubscribeRequests = 1;

    FakeLtPeer peer(options);

    // ws-streaming resends a request whose connection closes unanswered, and the sweep timer retries after 1.5 s
    auto [instance, device, signals] = connectAndWaitForSignals(peer, 2, 10s);
    ASSERT_EQ(signals.getCount(), 2u);
    ASSERT_GE(peer.subscribeRequestCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
}

TEST_P(DeviceCompatibilityTest, ReadvertisedHiddenDomainSignalStartsNoNewFetch)
{
    FakeLtPeer peer(peerOptions());
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

TEST_P(DeviceCompatibilityTest, ImmediateSubscribeTakesOverFetchSubscription)
{
    FakeLtPeer peer(peerOptions());
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
TEST_P(DeviceCompatibilityTest, DataKeepsFlowingWhenDeviceReordersUnsubscribeAndSubscribe)
{
    auto options = peerOptions();
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

// openDAQ unsubscribes and subscribes again before the device answers; the subscribe must still go out
TEST_P(DeviceCompatibilityTest, ResubscribeWithinOneRoundTripKeepsDataFlowing)
{
    auto options = peerOptions();
    options.streamData = true;
    FakeLtPeer peer(options);

    // outlive the instance, whose I/O thread raises the acks
    std::atomic<unsigned> subscribeAcks{0};
    std::promise<void> resubscribed;

    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    valueSignal.asPtr<IMirroredSignalConfig>().getOnSubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { if (++subscribeAcks == 2) resubscribed.set_value(); };

    auto reader = buildStreamReader(valueSignal);
    ASSERT_EQ(readSamples(reader, 1, 10s), 1u) << "no data before the resubscribe";

    // releasing the reader unsubscribes, and the next reader subscribes within the same round trip
    reader.release();
    reader = buildStreamReader(valueSignal);

    ASSERT_EQ(resubscribed.get_future().wait_for(10s), std::future_status::ready) << "the resubscribe was never acknowledged";
    if (const SizeT count = reader.getAvailableCount(); count > 0)
        readSamples(reader, count, 0ms);

    EXPECT_EQ(readSamples(reader, 1, 10s), 1u) << "no data after the resubscribe";
}

// The old stream flows until the device has run the unsubscribe, and the resubscribe starts a new one where the
// device's clock stands: data of the old stream must not reach the new reader, or it sees a domain gap
TEST_P(DeviceCompatibilityTest, ResubscribeWithinOneRoundTripGetsNoDomainGap)
{
    auto options = peerOptions();
    options.streamData = true;
    options.requestDelay = 200ms;  // the device's pace, not a wait: the old stream flows while it runs the unsubscribe
    FakeLtPeer peer(options);

    // outlive the instance, whose I/O thread raises the acks
    std::atomic<unsigned> subscribeAcks{0};
    std::promise<void> resubscribed;

    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    valueSignal.asPtr<IMirroredSignalConfig>().getOnSubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { if (++subscribeAcks == 2) resubscribed.set_value(); };

    auto reader = buildStreamReader(valueSignal);
    ASSERT_EQ(readSamples(reader, 1, 10s), 1u) << "the old stream does not flow";

    reader.release();
    reader = buildStreamReader(valueSignal);

    // a gap shows where the new stream starts, so read until the new stream has delivered two packets
    auto resubscribedFuture = resubscribed.get_future();
    std::vector<double> values(100);
    std::vector<std::int64_t> domain(100);
    SizeT samples = 0;
    SizeT samplesAfterResubscribe = 0;

    for (const auto deadline = std::chrono::steady_clock::now() + 10s;
         samplesAfterResubscribe < 200 && std::chrono::steady_clock::now() < deadline;)
    {
        const bool resubscribedBefore = resubscribedFuture.wait_for(0s) == std::future_status::ready;
        SizeT count = values.size();
        const auto status = reader.readWithDomain(values.data(), domain.data(), &count, 100);
        samples += count;
        if (resubscribedBefore)
            samplesAfterResubscribe += count;

        if (status.getReadStatus() == ReadStatus::Event)
            ASSERT_NE(status.getEventPacket().getEventId(), event_packet_id::IMPLICIT_DOMAIN_GAP_DETECTED)
                << "after " << samples << " samples";
    }

    EXPECT_GE(samplesAfterResubscribe, 200u);
}

// openDAQ never subscribes a domain signal itself, yet the data of an explicit one must reach the values it times
TEST_P(DeviceCompatibilityTest, ExplicitDomainValuesReachTheReader)
{
    auto options = peerOptions();
    options.streamData = true;
    options.explicitTime = true;
    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());

    auto reader = buildStreamReader(valueSignal);

    std::vector<double> values(100);
    std::vector<std::int64_t> domain(100, -1);
    SizeT count = values.size();
    reader.readWithDomain(values.data(), domain.data(), &count, 10000);

    ASSERT_GT(count, 0u);
    for (SizeT i = 0; i < count; ++i)
        ASSERT_TRUE(domain[i] >= 1000 && domain[i] < 1100) << "sample " << i << " has the domain value " << domain[i];
}

// A remote unsubscribe must end the held fetch subscription; a takeover of it would never get data
TEST_P(DeviceCompatibilityTest, SubscribeAfterRemoteUnsubscribeSendsNewRequest)
{
    auto options = peerOptions();
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

TEST_P(DeviceCompatibilityTest, UnusedFetchSubscriptionIsReleasedBySweep)
{
    FakeLtPeer peer(peerOptions());
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    // nobody subscribes: the sweep must release the held fetch subscription
    std::this_thread::sleep_for(4s);

    EXPECT_EQ(peer.subscribeRequestCount(), 1u);
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);
}

TEST_P(DeviceCompatibilityTest, SignalPublishesWithoutDomainWhenMetadataNeverArrives)
{
    auto options = peerOptions();
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

// The device connects its slots after creating the streaming object; a constructor that connected
// published signals before anyone listened, and each signal is published only once
TEST_P(DeviceCompatibilityTest, ConstructingTheStreamingDoesNotConnect)
{
    FakeLtPeer peer(peerOptions());
    auto streaming = createStreaming(peer);

    // a connecting constructor returns only after the peer has accepted it and answered the upgrade
    EXPECT_EQ(peer.acceptedConnectionCount(), 0u);
}

TEST_P(DeviceCompatibilityTest, NoSignalIsPublishedBeforeTheOwnerListens)
{
    FakeLtPeer peer(peerOptions());

    // declared before the streaming object, whose destructor joins the I/O thread that runs the slot
    std::promise<void> bothPublished;
    unsigned published = 0;

    auto streaming = createStreaming(peer);
    auto& wsStreaming = static_cast<websocket_streaming::WsStreaming&>(*streaming.getObject());

    boost::signals2::scoped_connection slot = wsStreaming.onSignalAvailable.connect(
        [&](wss::remote_signal_ptr, wss::remote_signal_ptr, const DataDescriptorPtr&)
        {
            if (++published == 2)
                bothPublished.set_value();
        });
    wsStreaming.connect();

    // the hidden time signal and the value signal that references it
    ASSERT_EQ(bothPublished.get_future().wait_for(5s), std::future_status::ready);
}

// The metadata fetch arms a 1.5 s sweep timer; closing the connection must cancel it, not wait for it
TEST_P(DeviceCompatibilityTest, ClosingDoesNotWaitForTheFetchSweep)
{
    // a stalled runner slows one teardown; a missing cancel() slows every one by the sweep period
    auto fastest = std::chrono::milliseconds::max();
    for (int run = 0; run < 3 && fastest >= 1000ms; ++run)
    {
        FakeLtPeer peer(peerOptions());
        auto streaming = createStreaming(peer);
        static_cast<websocket_streaming::WsStreaming&>(*streaming.getObject()).connect();

        // the sweep timer is armed in the same I/O handler that sends this request
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (peer.subscribeRequestCount() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        ASSERT_GE(peer.subscribeRequestCount(), 1u)
            << "No subscribe request within 5 s: the metadata fetch never started, so no sweep timer is "
               "armed and the teardown time would prove nothing";

        const auto start = std::chrono::steady_clock::now();
        // the last reference: the destructor runs inside the measured interval
        ASSERT_EQ(streaming.detach()->releaseRef(), 0)
            << "Another reference keeps the streaming alive: its destructor did not run inside the measured "
               "interval, so the teardown time would prove nothing";
        fastest = std::min(fastest,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start));
    }

    // an uncancelled timer holds the destructor until the sweep fires, 1.5 s after the request
    EXPECT_LT(fastest.count(), 1000);
}
