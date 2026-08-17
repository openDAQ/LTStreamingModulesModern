/*
 * End-to-end tests for the streaming client against a fake LT peer that mimics devices which
 * do not advertise their time signals ("hidden" domain signals referenced via "relatedSignals"
 * with an abstract table id) and which may drop command-interface requests.
 *
 * The fake peer accepts a WebSocket upgrade on a raw TCP socket (the client only checks the
 * HTTP status line) and then speaks the LT streaming protocol through the ws-streaming
 * library's own low-level peer class, with full control over metadata content, ordering and
 * request handling.
 */

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <nlohmann/json.hpp>

#include <ws-streaming/detail/peer.hpp>

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
        };

        explicit FakeLtPeer(Options options)
            : options(options)
            , acceptor(ioc, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0))
            , timer(ioc)
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

                respond(id, true);
                sendValueSignalFamily();
            }

            else if (rpcMethod == "FAKE.subscribe" && signalId == timeSignalId)
            {
                std::scoped_lock lock(mutex);
                ++timeSubscribeRequests;
            }

            else if (rpcMethod == "FAKE.unsubscribe")
            {
                respond(id, true);
                if (signalId == valueSignalId)
                    peer->send_metadata(valueSigno, "unsubscribe", nlohmann::json::object());
            }
        }

        void respond(const nlohmann::json& id, bool result)
        {
            peer->send_metadata(0, "response", {
                { "jsonrpc", "2.0" },
                { "id", id },
                { "result", result },
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
        std::thread thread;

        std::shared_ptr<wss::detail::peer> peer;
        boost::signals2::scoped_connection onMetadata;

        std::mutex mutex;
        unsigned valueSubscribeRequests = 0;
        unsigned timeSubscribeRequests = 0;
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

}  // namespace

using HiddenDomainSignalsTest = testing::Test;

TEST_F(HiddenDomainSignalsTest, HiddenDomainSignalIsLinked)
{
    FakeLtPeer peer({});
    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    auto signals = waitForSignals(device, 2, 5s);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    auto timeSignal = findSignalByName(signals, "CH1.time");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(timeSignal.assigned());

    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
    ASSERT_EQ(valueSignal.getDomainSignal(), timeSignal);
}

TEST_F(HiddenDomainSignalsTest, DomainMetadataArrivingLateIsStillLinked)
{
    FakeLtPeer::Options options;
    options.timeMetadataBeforeValue = false;
    options.timeMetadataDelay = 300ms;

    FakeLtPeer peer(options);
    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    auto signals = waitForSignals(device, 2, 5s);
    ASSERT_EQ(signals.getCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());

    // the value signal must have been deferred until the time signal published
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
}

TEST_F(HiddenDomainSignalsTest, DroppedSubscribeRequestIsRetried)
{
    FakeLtPeer::Options options;
    options.dropSubscribeRequests = 1;

    FakeLtPeer peer(options);
    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    // the sweep timer retries after 1.5 s + 100 ms; allow generous margin
    auto signals = waitForSignals(device, 2, 10s);
    ASSERT_EQ(signals.getCount(), 2u);
    ASSERT_GE(peer.subscribeRequestCount(), 2u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_TRUE(valueSignal.getDomainSignal().assigned());
}

TEST_F(HiddenDomainSignalsTest, ReadvertisedHiddenDomainSignalStartsNoNewFetch)
{
    FakeLtPeer peer({});
    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    auto signals = waitForSignals(device, 2, 5s);
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

TEST_F(HiddenDomainSignalsTest, SignalPublishesWithoutDomainWhenMetadataNeverArrives)
{
    FakeLtPeer::Options options;
    options.timeMetadataBeforeValue = false;
    options.withholdTimeMetadata = true;

    FakeLtPeer peer(options);
    auto instance = createClientInstance();
    auto device = connectDevice(instance, peer.port());

    // deferral gives up after two sweep periods (~3 s); allow generous margin
    auto signals = waitForSignals(device, 1, 10s);
    ASSERT_EQ(signals.getCount(), 1u);

    auto valueSignal = findSignalByName(signals, "CH1.value");
    ASSERT_TRUE(valueSignal.assigned());
    ASSERT_FALSE(valueSignal.getDomainSignal().assigned());
}
