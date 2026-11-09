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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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
#include <opendaq/event_packet_params.h>
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
            bool withholdTimeMetadata = false;      // announce the time signal but send its metadata only once released
            unsigned dropSubscribeRequests = 0;     // leave this many leading value-signal subscribe requests unanswered
                                                    // and without effect (over HTTP: close their connections)
            bool streamData = false;                // stream value-signal data while it is subscribed
            bool reorderRequests = false;           // over HTTP, run the requests in flight together last-first, as the
                                                    // digiBOX-WT did (in-band devices always run requests in order)
            std::chrono::milliseconds requestDelay{0};  // run requests one by one, each this long after the previous
            bool explicitTime = false;              // the time signal carries explicit timestamps instead of a linear rule
            bool rejectValueMetadata = false;       // give the value signal a tick resolution the client rejects
            bool timeSubscribeWithMetadata = false; // first mention the time signal with its metadata, not with the value's subscribe
            std::string relationType = "time";      // "relatedSignals" type of the time signal ("domain" per specification)
            std::optional<std::int64_t> linearStart;  // the time signal's "linear.start"
            bool startInInterpretation = false;     // send the start as an openDAQ server does: also in "interpretation", not in the data
            std::string absoluteReference;          // the time signal's "absoluteReference", if not empty
            bool statusSignal = false;              // also offer a constant-rule status signal on the value's table, as the
                                                    // FusionLN does; while subscribed it gets an entry after each value packet
        };

        explicit FakeLtPeer(Options options)
            : options(options)
            , acceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , httpAcceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
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

        // Requests that reached the device, run or not yet
        unsigned receivedRequestCount()
        {
            std::scoped_lock lock(mutex);
            return receivedRequests;
        }

        unsigned timeMetadataCount()
        {
            std::scoped_lock lock(mutex);
            return timeMetadataSent;
        }

        // The sample index at which the latest value-signal stream started (the sample clock runs regardless)
        std::int64_t lastStreamStart()
        {
            std::scoped_lock lock(mutex);
            return lastStreamStartIndex;
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

        // Sends the withheld time signal's metadata now, and with every later subscribe of the value signal
        void releaseTimeMetadata()
        {
            boost::asio::post(ioc,
                [this]
                {
                    timeMetadataReleased = true;
                    if (peer)
                        sendTimeMetadata();
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
            auto available = nlohmann::json::array({ valueSignalId });
            if (options.statusSignal)
                available.push_back(statusSignalId);
            peer->send_metadata(0, "available", {{ "signalIds", available }});
        }

        // Answers a request over the transport it came in on; a request whose reply is never called stays unanswered
        using Reply = std::function<void(const nlohmann::json& response)>;

        struct Request
        {
            nlohmann::json params;
            Reply reply;
        };

        void handleMetadata(unsigned /*signo*/, const std::string& method, const nlohmann::json& params)
        {
            if (method != "request" || !params.is_object())
                return;

            {
                std::scoped_lock lock(mutex);

                // a device that offers only HTTP does not answer in-band, so a client sending in-band would get nothing
                if (options.transport != CommandTransport::InBand)
                {
                    ++wrongTransportRequests;
                    return;
                }

                ++receivedRequests;
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
                        ++receivedRequests;
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

                    if (!options.reorderRequests)
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

                // run before the unsubscribe it overtook, the subscribe is rejected and the data keeps flowing
                if (valueSubscribed && options.reorderRequests)
                {
                    reply(error(id));
                    return;
                }

                valueSubscribed = true;
                streamStart = sampleIndexNow();
                {
                    std::scoped_lock lock(mutex);
                    lastStreamStartIndex = streamStart;
                }
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

            else if (rpcMethod == "FAKE.subscribe" && signalId == statusSignalId)
            {
                statusSubscribed = true;
                reply(result(id));
                peer->send_metadata(timeSigno, "subscribe", {{ "signalId", timeSignalId }});
                sendTimeMetadata();
                peer->send_metadata(statusSigno, "subscribe", {{ "signalId", statusSignalId }});
                peer->send_metadata(statusSigno, "signal", {
                    { "tableId", tableId },
                    { "relatedSignals", {
                        { { "type", options.relationType }, { "signalId", timeSignalId } },
                    } },
                    { "definition", {
                        { "name", statusSignalId },
                        { "rule", "constant" },
                        { "dataType", "int64" },
                    } },
                });
            }

            else if (rpcMethod == "FAKE.unsubscribe")
            {
                if (signalId == statusSignalId)
                {
                    statusSubscribed = false;
                    reply(result(id));
                    peer->send_metadata(statusSigno, "unsubscribe", nlohmann::json::object());
                    return;
                }

                if (signalId == valueSignalId)
                {
                    {
                        std::scoped_lock lock(mutex);
                        ++valueUnsubscribeRequests;
                    }

                    valueSubscribed = false;
                    reply(result(id));
                    peer->send_metadata(valueSigno, "unsubscribe", nlohmann::json::object());
                    return;
                }

                reply(result(id));
            }
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
                // a peer following the specification sends the absolute start value; an openDAQ server sends the offset only
                const std::int64_t startOffset = options.startInInterpretation ? 0 : options.linearStart.value_or(0);
                timeStart = { streamStart, streamStart + startOffset };
                peer->send_data(timeSigno, boost::asio::buffer(&timeStart, sizeof(timeStart)));
            }

            valueIndex = static_cast<std::uint64_t>(streamStart);
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
            valueIndex += valueSamples.size();

            // a constant-rule entry is the value index it holds from, then the value; this one holds from the next value packet
            if (statusSubscribed)
            {
                auto& entry = statusEntries.emplace_back();
                std::memcpy(entry.data(), &valueIndex, sizeof(valueIndex));
                std::memcpy(entry.data() + sizeof(valueIndex), &statusValue, sizeof(statusValue));
                peer->send_data(statusSigno, boost::asio::buffer(entry));
            }

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
            if (!options.timeSubscribeWithMetadata)
                peer->send_metadata(timeSigno, "subscribe", {{ "signalId", timeSignalId }});

            if (options.timeMetadataBeforeValue)
            {
                sendTimeMetadata();
                sendValueMetadata();
            }
            else
            {
                sendValueMetadata();
                sendTimeMetadata();
            }
        }

        void sendValueMetadata()
        {
            nlohmann::json metadata = {
                { "tableId", tableId },
                { "valueIndex", streamStart },
                { "relatedSignals", {
                    { { "type", options.relationType }, { "signalId", timeSignalId } },
                } },
                { "definition", {
                    { "name", valueSignalId },
                    { "rule", "explicit" },
                    { "dataType", "real32" },
                } },
            };

            if (options.rejectValueMetadata)
                metadata["definition"]["resolution"] = { { "num", 1 }, { "denom", 0 } };

            peer->send_metadata(valueSigno, "signal", metadata);
        }

        void sendTimeMetadata()
        {
            if (options.withholdTimeMetadata && !timeMetadataReleased)
                return;

            if (options.timeSubscribeWithMetadata)
                peer->send_metadata(timeSigno, "subscribe", {{ "signalId", timeSignalId }});

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
            else if (options.linearStart)
                definition["linear"]["start"] = *options.linearStart;

            if (!options.absoluteReference.empty())
                definition["absoluteReference"] = options.absoluteReference;

            nlohmann::json metadata = {
                { "tableId", tableId },
                { "definition", definition },
            };

            if (options.startInInterpretation)
                metadata["interpretation"] = {
                    { "rule", { { "type", 1 }, { "parameters", { { "delta", 1 }, { "start", options.linearStart.value_or(0) } } } } },
                };

            peer->send_metadata(timeSigno, "signal", metadata);

            std::scoped_lock lock(mutex);
            ++timeMetadataSent;
        }

        const std::string tableId = "CH1";
        const std::string valueSignalId = "CH1.value";
        const std::string timeSignalId = "CH1.time";
        const std::string statusSignalId = "CH1.status";
        static constexpr unsigned valueSigno = 1;
        static constexpr unsigned timeSigno = 2;
        static constexpr unsigned statusSigno = 3;

        Options options;

        boost::asio::io_context ioc{1};
        boost::asio::ip::tcp::acceptor acceptor;
        boost::asio::ip::tcp::acceptor httpAcceptor;
        boost::asio::steady_timer dataTimer;
        boost::asio::steady_timer requestTimer;
        boost::asio::steady_timer inFlightTimer;
        std::thread thread;

        // only touched on the ioc thread; an HTTP request's reply holds its connection open
        std::deque<Request> delayedRequests;
        std::vector<Request> inFlight;

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
        bool statusSubscribed = false; // only touched on the ioc thread
        std::uint64_t valueIndex = 0;  // only touched on the ioc thread
        const std::int64_t statusValue = 42;
        std::deque<std::array<std::uint8_t, 16>> statusEntries;  // only touched on the ioc thread; never shrinks
        bool timeMetadataReleased = false;  // only touched on the ioc thread

        std::shared_ptr<wss::detail::peer> peer;
        boost::signals2::scoped_connection onMetadata;

        std::mutex mutex;
        unsigned acceptedConnections = 0;
        unsigned httpRequests = 0;
        unsigned wrongTransportRequests = 0;
        unsigned receivedRequests = 0;
        unsigned valueSubscribeRequests = 0;
        unsigned timeSubscribeRequests = 0;
        unsigned valueUnsubscribeRequests = 0;
        unsigned timeMetadataSent = 0;
        std::int64_t lastStreamStartIndex = 0;
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

// Waits for a condition that raises no event, returning as soon as it holds; false if the timeout elapses first
bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s)
{
    for (const auto deadline = std::chrono::steady_clock::now() + timeout; !condition();)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }

    return true;
}

// Returns the signal the moment it appears, like an application that subscribes every new signal on sight
// (lt_race polls every millisecond, 2026-08-19)
SignalPtr waitForSignal(const DevicePtr& device, const std::string& name)
{
    SignalPtr signal;
    waitUntil([&] { return (signal = findSignalByName(device.getSignals(search::Recursive(search::Any())), name)).assigned(); });
    return signal;
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

// Subscribes the signal and returns the domain value of the first sample it delivers
std::optional<std::int64_t> readFirstDomainValue(const SignalPtr& signal)
{
    auto reader = buildStreamReader(signal);

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (reader.getAvailableCount() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);

    SizeT count = 1;
    double value;
    std::int64_t domain;
    reader.readWithDomain(&value, &domain, &count);
    return count == 1 ? std::optional(domain) : std::nullopt;
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

TEST_P(DeviceCompatibilityTest, HiddenDomainSignalIsLinkedThroughDomainRelation)
{
    auto options = peerOptions();
    options.relationType = "domain";

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_EQ(valueSignal.getDomainSignal(), findSignalByName(signals, "CH1.time"));
}

// The specification's "linear.start" is the absolute value of the next sample
TEST_P(DeviceCompatibilityTest, AbsoluteLinearStartIsNotAddedTwice)
{
    constexpr std::int64_t start = 5003;  // not a multiple of the 100-sample packets

    auto options = peerOptions();
    options.relationType = "domain";
    options.streamData = true;
    options.linearStart = start;
    options.absoluteReference = "1970-01-01T00:00:00.0";

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());

    auto domainDescriptor = valueSignal.getDomainSignal().getDescriptor();
    EXPECT_EQ(static_cast<Int>(domainDescriptor.getRule().getParameters().get("start")), 0);
    EXPECT_EQ(domainDescriptor.getOrigin().toStdString(), "1970-01-01T00:00:00.0");

    // the device's sample clock runs regardless, so its stream starts at the sample index it stands at
    auto domain = readFirstDomainValue(valueSignal);
    ASSERT_TRUE(domain.has_value());
    const std::int64_t streamStart = peer.lastStreamStart();
    EXPECT_GE(*domain, start + streamStart);
    EXPECT_EQ((*domain - start - streamStart) % 100, 0);
}

// openDAQ servers send their rule start in "interpretation" and only the offset in the data
TEST_P(DeviceCompatibilityTest, OpenDaqServerLinearStartIsKept)
{
    constexpr std::int64_t start = 5003;

    auto options = peerOptions();
    options.streamData = true;
    options.linearStart = start;
    options.startInInterpretation = true;

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());

    auto domainDescriptor = valueSignal.getDomainSignal().getDescriptor();
    EXPECT_EQ(static_cast<Int>(domainDescriptor.getRule().getParameters().get("start")), start);

    // the device's sample clock runs regardless, so its stream starts at the sample index it stands at
    auto domain = readFirstDomainValue(valueSignal);
    ASSERT_TRUE(domain.has_value());
    const std::int64_t streamStart = peer.lastStreamStart();
    EXPECT_GE(*domain, start + streamStart);
    EXPECT_EQ((*domain - start - streamStart) % 100, 0);
}

// The value signal publishes without waiting for its time signal's metadata, and is linked to it once it arrives
TEST_P(DeviceCompatibilityTest, DomainMetadataArrivingLateIsStillLinked)
{
    auto options = peerOptions();
    options.timeMetadataBeforeValue = false;
    options.withholdTimeMetadata = true;

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer, 1);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_FALSE(valueSignal.getDomainSignal().assigned());

    // connected before the time signal's metadata arrives, the reader learns the domain from an event
    auto reader = PacketReader(valueSignal);
    peer.releaseTimeMetadata();

    // the link lands after the time signal is added, so the signal count does not show it
    ASSERT_TRUE(waitUntil([&] { return valueSignal.getDomainSignal().assigned(); }));
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

// The value signal publishes before the device mentions its time signal; its next metadata finds the time signal
TEST_P(DeviceCompatibilityTest, HiddenDomainFoundAfterPublicationIsLinked)
{
    auto options = peerOptions();
    options.timeMetadataBeforeValue = false;
    options.withholdTimeMetadata = true;
    options.timeSubscribeWithMetadata = true;
    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer, 1);
    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_FALSE(valueSignal.getDomainSignal().assigned());

    // once the device has mentioned the time signal, a subscribe brings the value signal's metadata again
    peer.releaseTimeMetadata();
    ASSERT_TRUE(waitUntil([&] { return peer.timeMetadataCount() >= 1; }));
    auto reader = PacketReader(valueSignal);

    ASSERT_TRUE(waitUntil([&] { return valueSignal.getDomainSignal().assigned(); }));
    ASSERT_EQ(valueSignal.getDomainSignal(),
        findSignalByName(device.getSignals(search::Recursive(search::Any())), "CH1.time"));
}

// The device may have run a request whose connection closed unanswered, so sending it again could run it twice:
// a dropped metadata fetch is final, and its signal is not published
TEST_P(DeviceCompatibilityTest, DroppedSubscribeRequestIsNotSentAgain)
{
    if (GetParam() == CommandTransport::InBand)
        GTEST_SKIP() << "no device drops in-band requests";

    auto options = peerOptions();
    options.dropSubscribeRequests = 1;
    FakeLtPeer peer(options);

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());
    ASSERT_TRUE(waitUntil([&] { return peer.subscribeRequestCount() >= 1; })) << "the metadata fetch never started";

    // requests go one at a time, so the time signal's fetch, sent from now on, reaches the device after any resend
    peer.advertiseTimeSignal();
    // a resent fetch would have published the value signal and linked the time signal, which then needs no fetch
    ASSERT_TRUE(waitUntil([&] { return peer.timeSubscribeRequestCount() >= 1; }))
        << "the time signal was never fetched; value signal subscribe requests: " << peer.subscribeRequestCount();

    EXPECT_EQ(peer.subscribeRequestCount(), 1u);
    EXPECT_FALSE(findSignalByName(device.getSignals(search::Recursive(search::Any())), "CH1.value").assigned());
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

// The device's ack of the fetch subscription's release must not be taken for an answer to the application
TEST_P(DeviceCompatibilityTest, ImmediateSubscribeGetsOneSubscribeAck)
{
    FakeLtPeer peer(peerOptions());

    // outlive the instance, whose I/O thread raises the acks
    std::atomic<unsigned> subscribeAcks{0};
    std::atomic<unsigned> unsubscribeAcks{0};
    std::promise<void> subscribed;

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    // subscribe the moment the signal appears, while the release of its fetch subscription is in flight
    auto valueSignal = waitForSignal(device, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    auto mirrored = valueSignal.asPtr<IMirroredSignalConfig>();
    mirrored.getOnSubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { if (++subscribeAcks == 1) subscribed.set_value(); };
    mirrored.getOnUnsubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { ++unsubscribeAcks; };

    auto reader = buildStreamReader(valueSignal);

    // the device runs the requests in order, so its ack of the release is in before the one of the subscribe
    ASSERT_EQ(subscribed.get_future().wait_for(10s), std::future_status::ready) << "the subscribe was never acknowledged";
    EXPECT_EQ(unsubscribeAcks.load(), 0u);
    EXPECT_EQ(peer.subscribeRequestCount(), 2u);         // the fetch, then the application
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);  // the release of the fetch subscription
}

// A device running the fetch subscription's release and an immediate subscribe last-first would stop the data
TEST_P(DeviceCompatibilityTest, DataKeepsFlowingWhenDeviceReordersUnsubscribeAndSubscribe)
{
    if (GetParam() == CommandTransport::InBand)
        GTEST_SKIP() << "in-band devices run requests in order";

    auto options = peerOptions();
    options.streamData = true;
    options.reorderRequests = true;
    FakeLtPeer peer(options);

    // outlives the instance, whose I/O thread raises the ack
    std::promise<void> subscribed;

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    // subscribe the moment the signal appears, while the release of its fetch subscription is in flight
    auto valueSignal = waitForSignal(device, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    valueSignal.asPtr<IMirroredSignalConfig>().getOnSubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { subscribed.set_value(); };

    auto reader = buildStreamReader(valueSignal);

    // a reordered subscribe is rejected by the device and never acknowledged
    ASSERT_EQ(subscribed.get_future().wait_for(10s), std::future_status::ready) << "the subscribe was never acknowledged";
    if (const SizeT count = reader.getAvailableCount(); count > 0)
        readSamples(reader, count, 0ms);

    EXPECT_EQ(readSamples(reader, 1, 10s), 1u) << "no data after the subscribe";
}

// openDAQ unsubscribes and subscribes again before the device answers; the subscribe must still go out
TEST_P(DeviceCompatibilityTest, ResubscribeWithinOneRoundTripKeepsDataFlowing)
{
    auto options = peerOptions();
    options.streamData = true;
    options.requestDelay = 100ms;  // the device's pace, not a wait: the round trip of the unsubscribe stays open
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

    // Releasing the reader unsubscribes once the reader is gone, which a packet in delivery can delay; a reader
    // connected before that would share the subscription. So the next reader subscribes once the unsubscribe
    // reached the device, which has not answered it yet.
    const auto requestsBefore = peer.receivedRequestCount();
    reader.release();
    ASSERT_TRUE(waitUntil([&] { return peer.receivedRequestCount() > requestsBefore; })) << "the unsubscribe never went out";
    reader = buildStreamReader(valueSignal);

    ASSERT_EQ(resubscribed.get_future().wait_for(10s), std::future_status::ready) << "the resubscribe was never acknowledged";
    if (const SizeT count = reader.getAvailableCount(); count > 0)
        readSamples(reader, count, 0ms);

    EXPECT_EQ(readSamples(reader, 1, 10s), 1u) << "no data after the resubscribe";
}

// A reader destroyed on the I/O thread (its last reference held by a packet in delivery) unsubscribes from there, and
// a reader connected right after subscribes from another thread: the subscribe must not run first, or it is refused
// as a duplicate and the unsubscribe that follows ends the new reader's data
TEST_P(DeviceCompatibilityTest, UnsubscribeFromTheIoThreadAndSubscribeElsewhereRunInOrder)
{
    auto options = peerOptions();
    options.streamData = true;
    FakeLtPeer peer(options);

    // outlive the instance, whose I/O thread raises the acks
    std::mutex readerMutex;
    std::optional<decltype(buildStreamReader(SignalPtr()))> firstReader;
    std::promise<void> firstReleased;
    std::promise<void> secondConnected;
    auto secondConnectedFuture = secondConnected.get_future();
    std::promise<void> resubscribed;
    std::atomic<unsigned> subscribeAcks{0};

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());
    auto valueSignal = waitForSignal(device, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    valueSignal.asPtr<IMirroredSignalConfig>().getOnSubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&)
        {
            const unsigned ack = ++subscribeAcks;
            if (ack == 1)
            {
                // on the I/O thread: the first reader unsubscribes from here, and the second subscribes meanwhile
                {
                    std::scoped_lock lock(readerMutex);
                    firstReader.reset();
                }
                firstReleased.set_value();
                secondConnectedFuture.wait_for(10s);
            }
            else if (ack == 2)
            {
                resubscribed.set_value();
            }
        };

    {
        std::scoped_lock lock(readerMutex);
        firstReader = buildStreamReader(valueSignal);
    }

    ASSERT_EQ(firstReleased.get_future().wait_for(10s), std::future_status::ready) << "the first subscribe was never acknowledged";
    auto secondReader = buildStreamReader(valueSignal);
    secondConnected.set_value();

    ASSERT_EQ(resubscribed.get_future().wait_for(10s), std::future_status::ready) << "the second subscribe was never acknowledged";

    // a read stops at a domain gap, which the old stream's last packets can leave in front of the new stream
    SizeT samples = 0;
    for (const auto deadline = std::chrono::steady_clock::now() + 10s; samples == 0 && std::chrono::steady_clock::now() < deadline;)
        samples = readSamples(secondReader, 1, 100ms);
    EXPECT_EQ(samples, 1u) << "no data for the second reader";
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

    // the next reader subscribes once the unsubscribe reached the device (see ResubscribeWithinOneRoundTripKeepsDataFlowing)
    const auto requestsBefore = peer.receivedRequestCount();
    reader.release();
    ASSERT_TRUE(waitUntil([&] { return peer.receivedRequestCount() > requestsBefore; })) << "the unsubscribe never went out";
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

// The fetch subscription's stream flows until the device has run its release, and the application's subscribe starts
// a new stream where the device's clock stands: the fetch's data must not reach the reader, or it sees a domain gap
TEST_P(DeviceCompatibilityTest, ReaderConnectedAtPublicationGetsNoDomainGap)
{
    auto options = peerOptions();
    options.streamData = true;
    options.requestDelay = 200ms;  // the device's pace, not a wait: the fetch's stream flows while it runs the release
    FakeLtPeer peer(options);

    // outlives the instance, whose I/O thread raises the ack
    std::promise<void> subscribed;

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    auto valueSignal = waitForSignal(device, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    valueSignal.asPtr<IMirroredSignalConfig>().getOnSubscribeComplete() +=
        [&](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&) { subscribed.set_value(); };

    auto reader = buildStreamReader(valueSignal);

    // a gap shows where the application's stream starts, so read until it has delivered two packets
    auto subscribedFuture = subscribed.get_future();
    std::vector<double> values(100);
    std::vector<std::int64_t> domain(100);
    SizeT samples = 0;
    SizeT samplesAfterSubscribe = 0;

    for (const auto deadline = std::chrono::steady_clock::now() + 10s;
         samplesAfterSubscribe < 200 && std::chrono::steady_clock::now() < deadline;)
    {
        const bool subscribedBefore = subscribedFuture.wait_for(0s) == std::future_status::ready;
        SizeT count = values.size();
        const auto status = reader.readWithDomain(values.data(), domain.data(), &count, 100);
        samples += count;
        if (subscribedBefore)
            samplesAfterSubscribe += count;

        if (status.getReadStatus() == ReadStatus::Event)
            ASSERT_NE(status.getEventPacket().getEventId(), event_packet_id::IMPLICIT_DOMAIN_GAP_DETECTED)
                << "after " << samples << " samples";
    }

    EXPECT_GE(samplesAfterSubscribe, 200u);
}

// Nobody subscribes: the fetch subscription is released, once, and nothing else is sent
TEST_P(DeviceCompatibilityTest, UnusedFetchSubscriptionIsReleased)
{
    FakeLtPeer peer(peerOptions());
    auto [instance, device, signals] = connectAndWaitForSignals(peer);
    ASSERT_EQ(signals.getCount(), 2u);

    ASSERT_TRUE(waitUntil([&] { return peer.valueUnsubscribeRequestCount() >= 1; }));
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);
    EXPECT_EQ(peer.subscribeRequestCount(), 1u);
}

TEST_P(DeviceCompatibilityTest, SignalWithRejectedMetadataReleasesItsFetch)
{
    auto options = peerOptions();
    options.rejectValueMetadata = true;
    options.streamData = true;  // data for a signal without a descriptor must not end the process
    FakeLtPeer peer(options);

    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    ASSERT_TRUE(waitUntil([&] { return peer.valueUnsubscribeRequestCount() >= 1; })) << "the fetch subscription is kept";
    EXPECT_EQ(peer.valueUnsubscribeRequestCount(), 1u);
    EXPECT_EQ(device.getSignals(search::Recursive(search::Any())).getCount(), 0u);
}

TEST_P(DeviceCompatibilityTest, SignalPublishesWithoutDomainWhenMetadataNeverArrives)
{
    auto options = peerOptions();
    options.timeMetadataBeforeValue = false;
    options.withholdTimeMetadata = true;

    FakeLtPeer peer(options);

    auto [instance, device, signals] = connectAndWaitForSignals(peer, 1);
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

// Closing the connection must cancel a request the device has not answered yet, not wait for it
TEST_P(DeviceCompatibilityTest, ClosingDoesNotWaitForAnUnansweredRequest)
{
    auto options = peerOptions();
    options.requestDelay = 1h;  // the device never gets round to the metadata fetch's subscribe

    // a stalled runner slows one teardown; a missing cancel holds every one until the HTTP client's 30 s deadline
    auto fastest = std::chrono::milliseconds::max();
    for (int run = 0; run < 3 && fastest >= 1000ms; ++run)
    {
        FakeLtPeer peer(options);
        auto streaming = createStreaming(peer);
        static_cast<websocket_streaming::WsStreaming&>(*streaming.getObject()).connect();

        ASSERT_TRUE(waitUntil([&] { return peer.receivedRequestCount() >= 1; }))
            << "the metadata fetch's subscribe never reached the device, so the teardown time would prove nothing";

        const auto start = std::chrono::steady_clock::now();
        // the last reference: the destructor runs inside the measured interval
        ASSERT_EQ(streaming.detach()->releaseRef(), 0)
            << "Another reference keeps the streaming alive: its destructor did not run inside the measured "
               "interval, so the teardown time would prove nothing";
        fastest = std::min(fastest,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start));
    }

    EXPECT_LT(fastest.count(), 1000);
}

// A constant-rule entry that holds from the next value packet's first sample must not lend that packet its domain packet
TEST_P(DeviceCompatibilityTest, ConstantSignalOnTheSameTableLeavesValueDomainPacketsIntact)
{
    auto options = peerOptions();
    options.streamData = true;
    options.statusSignal = true;

    FakeLtPeer peer(options);
    auto [instance, device, signals] = connectAndWaitForSignals(peer, 3);
    auto valueSignal = findSignalByName(signals, "CH1.value");
    auto statusSignal = findSignalByName(signals, "CH1.status");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(statusSignal.assigned());

    auto statusReader = PacketReader(statusSignal);
    auto valueReader = PacketReader(valueSignal);

    std::size_t valuePackets = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (valuePackets < 20 && std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& packet : valueReader.readAll())
        {
            if (packet.getType() != PacketType::Data)
                continue;

            const auto data = packet.asPtr<IDataPacket>(true);
            ASSERT_TRUE(data.getDomainPacket().assigned());
            ASSERT_EQ(data.getDomainPacket().getSampleCount(), data.getSampleCount()) << "value packet " << valuePackets;
            ++valuePackets;
        }
        std::this_thread::sleep_for(20ms);
    }

    ASSERT_GE(valuePackets, 20u);
}
