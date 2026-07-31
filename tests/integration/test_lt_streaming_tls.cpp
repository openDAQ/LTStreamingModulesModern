#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <testutils/testutils.h>

#include <opendaq/opendaq.h>
#include <opendaq/instance_factory.h>
#include <opendaq/context_factory.h>
#include <opendaq/module_manager_factory.h>
#include <opendaq/scheduler_factory.h>
#include <opendaq/logger_factory.h>
#include <opendaq/reader_factory.h>
#include <coreobjects/authentication_provider_factory.h>
#include <coretypes/type_manager_factory.h>

#include <opendaq/mock/mock_device_module.h>

#include <websocket_streaming/constants.h>

using namespace daq;
using namespace daq::websocket_streaming;
using namespace std::chrono_literals;

namespace
{
constexpr const char* CA_CERT = "secrets/ca.crt";
constexpr const char* OTHER_CA_CERT = "secrets/other-ca.crt";
constexpr const char* SERVER_CERT = "secrets/server.crt";
constexpr const char* SERVER_KEY = "secrets/server.key";
constexpr const char* CLIENT_CERT = "secrets/client.crt";
constexpr const char* CLIENT_KEY = "secrets/client.key";

constexpr const char* SERVER_TYPE_ID = "OpenDAQLTStreaming";
constexpr const char* SECURE_DEVICE_TYPE_ID = "OpenDAQLTStreamingSecure";
constexpr const char* SIGNAL_NAME = "ByteStep";

class CapturingDiscoveryServer : public ImplementationOf<IDiscoveryServer>
{
public:
    ErrCode INTERFACE_FUNC registerService(IString* id, IPropertyObject* config, IDeviceInfo* /*deviceInfo*/) override
    {
        registered.emplace_back(StringPtr(id), PropertyObjectPtr(config));
        return OPENDAQ_SUCCESS;
    }

    ErrCode INTERFACE_FUNC unregisterService(IString* id) override
    {
        unregistered.push_back(StringPtr(id));
        return OPENDAQ_SUCCESS;
    }

    ErrCode INTERFACE_FUNC setRootDevice(IDevice* /*device*/) override
    {
        return OPENDAQ_SUCCESS;
    }

    std::vector<std::pair<StringPtr, PropertyObjectPtr>> registered;
    std::vector<StringPtr> unregistered;
};
}

class LtStreamingTlsTest : public testing::Test
{
protected:
    InstancePtr createServerInstanceWithDevice()
    {
        const auto logger = Logger();
        const auto moduleManager = ModuleManager(".");
        const auto context = Context(Scheduler(logger), logger, TypeManager(), moduleManager, AuthenticationProvider());

        moduleManager.addModule(MockDeviceModule_Create(context));

        auto instance = InstanceCustom(context, "server");
        instance.addDevice("daqmock://phys_device");
        return instance;
    }

    // Instance whose discovery layer is the capturing fake instead of the mDNS server
    InstancePtr createServerInstanceWithDiscovery(CapturingDiscoveryServer** outDiscoveryServer)
    {
        const auto logger = Logger();
        const auto moduleManager = ModuleManager(".");

        auto discoveryServer = createWithImplementation<IDiscoveryServer, CapturingDiscoveryServer>();
        *outDiscoveryServer = static_cast<CapturingDiscoveryServer*>(discoveryServer.getObject());

        const auto context = Context(Scheduler(logger),
                                     logger,
                                     TypeManager(),
                                     moduleManager,
                                     AuthenticationProvider(),
                                     Dict<IString, IBaseObject>(),
                                     Dict<IString, IDiscoveryServer>({{"capturing", discoveryServer}}));

        return InstanceCustom(context, "server");
    }

    static PropertyObjectPtr findDiscoveryConfig(const CapturingDiscoveryServer& discoveryServer, const std::string& serviceName)
    {
        for (const auto& [_, config] : discoveryServer.registered)
            if (config.getPropertyValue("ServiceName") == serviceName)
                return config;
        return nullptr;
    }

    PropertyObjectPtr baseServerConfig(const InstancePtr& instance)
    {
        return instance.getAvailableServerTypes().get(SERVER_TYPE_ID).createDefaultConfig();
    }

    PropertyObjectPtr insecureServerConfig(const InstancePtr& instance, int wsPort)
    {
        auto cfg = baseServerConfig(instance);
        cfg.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True);
        cfg.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
        cfg.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, False);
        cfg.setPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER, wsPort);
        return cfg;
    }

    PropertyObjectPtr secureServerConfig(const InstancePtr& instance, int wssPort, bool mtls)
    {
        auto cfg = baseServerConfig(instance);
        cfg.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);
        cfg.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
        cfg.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);
        cfg.setPropertyValue(PROPERTY_WSS_STREAMING_PORT_SERVER, wssPort);
        cfg.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, mtls);
        cfg.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_SERVER, SERVER_CERT);
        cfg.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_SERVER, SERVER_KEY);
        if (mtls)
            cfg.setPropertyValue(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER, CA_CERT);
        return cfg;
    }

    PropertyObjectPtr bothChannelsServerConfig(const InstancePtr& instance, int wsPort, int wssPort)
    {
        auto cfg = baseServerConfig(instance);
        cfg.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True);
        cfg.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
        cfg.setPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER, wsPort);
        cfg.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);
        cfg.setPropertyValue(PROPERTY_WSS_STREAMING_PORT_SERVER, wssPort);
        cfg.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, False);
        cfg.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_SERVER, SERVER_CERT);
        cfg.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_SERVER, SERVER_KEY);
        return cfg;
    }

    InstancePtr createClientInstance()
    {
        return Instance(".");
    }

    PropertyObjectPtr secureDeviceConfig(const InstancePtr& instance, bool mtls, const std::string& caFile)
    {
        auto cfg = instance.getAvailableDeviceTypes().get(SECURE_DEVICE_TYPE_ID).createDefaultConfig();
        cfg.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, mtls);
        cfg.setPropertyValue(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT, String(caFile));
        if (mtls)
        {
            cfg.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_CLIENT, CLIENT_CERT);
            cfg.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_CLIENT, CLIENT_KEY);
        }
        return cfg;
    }

    static SignalPtr findSignal(const DevicePtr& device, const std::string& name)
    {
        for (const auto& signal : device.getSignals(search::Recursive(search::Visible())))
        {
            const auto descriptor = signal.getDescriptor();
            if (descriptor.assigned() && descriptor.getName() == name)
                return signal;
        }
        return nullptr;
    }

    // LT client mirrors remote signals asynchronously
    // (an initial subscribe round-trip is needed to learn their metadata)
    static SignalPtr getSignal(const DevicePtr& device, const std::string& name, std::chrono::seconds timeout = 10s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do
        {
            if (auto signal = findSignal(device, name); signal.assigned())
                return signal;
            std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw NotFoundException("signal '" + name + "' not found");
    }

    static std::vector<int8_t> readSamples(const StreamReaderPtr& reader, SizeT count, std::chrono::seconds timeout)
    {
        std::vector<int8_t> out;
        std::vector<int8_t> chunk(count);
        out.reserve(count);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (out.size() < count && std::chrono::steady_clock::now() < deadline)
        {
            SizeT toRead = count - out.size();
            reader.read(chunk.data(), &toRead, 200);
            if (toRead > 0)
                out.insert(out.end(), chunk.begin(), chunk.begin() + toRead);
        }
        return out;
    }

    void generatePackets(const InstancePtr& serverInstance, size_t packetCount)
    {
        for (const auto& device : serverInstance.getDevices())
            if (device.getName() == "MockPhysicalDevice")
                device.setPropertyValue("GeneratePackets", static_cast<Int>(packetCount));
    }

    void runStreamingExchange(const InstancePtr& serverInstance, const DevicePtr& clientDevice)
    {
        auto serverSignal = getSignal(serverInstance, SIGNAL_NAME);
        auto clientSignal = getSignal(clientDevice, SIGNAL_NAME);

        auto mirrored = clientSignal.asPtr<IMirroredSignalConfig>();
        std::promise<bool> subscribePromise;
        std::future<bool> subscribeFuture = subscribePromise.get_future();
        mirrored.getOnSubscribeComplete() += [&subscribePromise](MirroredSignalConfigPtr&, SubscriptionEventArgsPtr&)
        {
            try { subscribePromise.set_value(true); }
            catch (const std::future_error&) { }
        };

        auto serverReader = StreamReader<int8_t, int64_t>(serverSignal);
        auto clientReader = StreamReader<int8_t, int64_t>(clientSignal);

        ASSERT_EQ(subscribeFuture.wait_for(10s), std::future_status::ready)
            << "client did not receive a subscribe acknowledgement";

        constexpr size_t packetsToGenerate = 20;
        constexpr SizeT samplesToRead = 10;
        generatePackets(serverInstance, packetsToGenerate);

        auto serverSamples = readSamples(serverReader, samplesToRead, 15s);
        auto clientSamples = readSamples(clientReader, samplesToRead, 15s);

        ASSERT_EQ(clientSamples.size(), samplesToRead) << "client did not receive the expected number of samples";
        ASSERT_EQ(serverSamples.size(), samplesToRead) << "server did not produce the expected number of samples";
        EXPECT_EQ(clientSamples, serverSamples) << "client samples do not match server samples";
    }
};

// Non-secure ws://
TEST_F(LtStreamingTlsTest, WsBaselineStreaming)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, insecureServerConfig(server, 7610));

    auto client = createClientInstance();
    auto device = client.addDevice("daq.lt://127.0.0.1:7610/");
    runStreamingExchange(server, device);
}

// Secure (no mutual TLS) wss:// streaming
TEST_F(LtStreamingTlsTest, WssServerAuthStreaming)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, secureServerConfig(server, 7611, /*mtls*/ false));

    auto client = createClientInstance();
    auto device = client.addDevice("daq.lts://127.0.0.1:7611/", secureDeviceConfig(client, /*mtls*/ false, CA_CERT));
    runStreamingExchange(server, device);
}

// Secure mutual-TLS wss:// streaming
TEST_F(LtStreamingTlsTest, WssMutualTlsStreaming)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, secureServerConfig(server, 7612, /*mtls*/ true));

    auto client = createClientInstance();
    auto device = client.addDevice("daq.lts://127.0.0.1:7612/", secureDeviceConfig(client, /*mtls*/ true, CA_CERT));
    runStreamingExchange(server, device);
}

// A single server exposing both ws:// and wss:// listeners; the client connects over the secure one
TEST_F(LtStreamingTlsTest, BothChannelsSecureClient)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, bothChannelsServerConfig(server, 7615, 7616));

    auto client = createClientInstance();
    auto device = client.addDevice("daq.lts://127.0.0.1:7616/", secureDeviceConfig(client, /*mtls*/ false, CA_CERT));
    runStreamingExchange(server, device);
}

// A single server exposing both ws:// and wss:// listeners; the client connects over the plaintext one
TEST_F(LtStreamingTlsTest, BothChannelsInsecureClient)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, bothChannelsServerConfig(server, 7617, 7618));

    auto client = createClientInstance();
    auto device = client.addDevice("daq.lt://127.0.0.1:7617/");
    runStreamingExchange(server, device);
}

// A client that trusts the wrong CA must fail to connect to the secure server
TEST_F(LtStreamingTlsTest, WssUntrustedCaRejected)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, secureServerConfig(server, 7613, /*mtls*/ false));

    auto client = createClientInstance();

    ASSERT_THROW(client.addDevice("daq.lts://127.0.0.1:7613/", secureDeviceConfig(client, /*mtls*/ false, OTHER_CA_CERT)),
                 AuthenticationFailedException);
}

// A secure connection with no CA configured must be rejected before any connection is attempted
TEST_F(LtStreamingTlsTest, WssMissingCaRejected)
{
    auto server = createServerInstanceWithDevice();
    server.addServer(SERVER_TYPE_ID, secureServerConfig(server, 7614, /*mtls*/ false));

    auto client = createClientInstance();
    ASSERT_THROW(client.addDevice("daq.lts://127.0.0.1:7614/", secureDeviceConfig(client, /*mtls*/ false, "")),
                 InvalidParameterException);
}

TEST_F(LtStreamingTlsTest, WssUnreachableServerReportedAsNotFound)
{
    auto client = createClientInstance();
    ASSERT_THROW(client.addDevice("daq.lts://127.0.0.1:7623/", secureDeviceConfig(client, /*mtls*/ false, CA_CERT)),
                 NotFoundException);
}

TEST_F(LtStreamingTlsTest, ServerRejectsMissingCertificateFile)
{
    auto instance = createServerInstanceWithDevice();
    auto config = secureServerConfig(instance, 7619, /*mtls*/ false);
    config.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_SERVER, "secrets/does-not-exist.crt");

    ASSERT_THROW_MSG(instance.addServer(SERVER_TYPE_ID, config),
                     InvalidParameterException,
                     "Cannot load the TLS secrets");
}

TEST_F(LtStreamingTlsTest, ServerRejectsMismatchedKey)
{
    auto instance = createServerInstanceWithDevice();
    auto config = secureServerConfig(instance, 7620, /*mtls*/ false);
    config.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_SERVER, CLIENT_KEY);

    ASSERT_THROW_MSG(instance.addServer(SERVER_TYPE_ID, config),
                     InvalidParameterException,
                     "Cannot load the TLS secrets");
}

TEST_F(LtStreamingTlsTest, ClientRejectsMissingKeyFile)
{
    auto client = createClientInstance();
    auto config = secureDeviceConfig(client, /*mtls*/ true, CA_CERT);
    config.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_CLIENT, "secrets/does-not-exist.key");

    ASSERT_THROW(client.addDevice("daq.lts://127.0.0.1:7621/", config), InvalidParameterException);
}

TEST_F(LtStreamingTlsTest, ClientRejectsMismatchedKey)
{
    auto client = createClientInstance();
    auto config = secureDeviceConfig(client, /*mtls*/ true, CA_CERT);
    config.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_CLIENT, SERVER_KEY);

    ASSERT_THROW(client.addDevice("daq.lts://127.0.0.1:7622/", config), InvalidParameterException);
}

TEST_F(LtStreamingTlsTest, DiscoveryRegistersBothChannels)
{
    auto instance = InstanceBuilder().setModulePath(".").addDiscoveryServer("mdns").build();
    auto server = instance.addServer(SERVER_TYPE_ID, bothChannelsServerConfig(instance, 7630, 7631));

    ASSERT_NO_THROW(server.enableDiscovery());
    ASSERT_NO_THROW(server.disableDiscovery());
    ASSERT_NO_THROW(server.enableDiscovery());
}

TEST_F(LtStreamingTlsTest, AddCapabilityWssOnly)
{
    auto instance = createServerInstanceWithDevice();
    instance.addServer(SERVER_TYPE_ID, secureServerConfig(instance, 7632, /*mtls*/ false));

    const auto info = instance.getRootDevice().getInfo();
    ASSERT_TRUE(info.hasServerCapability(CONST_LTS_STREAMING_ID));
    ASSERT_FALSE(info.hasServerCapability(CONST_LT_STREAMING_ID));

    const auto cap = info.getServerCapability(CONST_LTS_STREAMING_ID);
    ASSERT_EQ(cap.getPrefix(), CONST_LTS_STREAMING_PREFIX);
    ASSERT_EQ(cap.getPort(), 7632);
    ASSERT_EQ(cap.getProtocolGroupId(), CONST_LT_PROTOCOL_GROUP_ID);
    ASSERT_EQ(cap.getProtocolSecurityLevel(), CONST_LTS_STREAMING_SECURITY_LVL);
}

TEST_F(LtStreamingTlsTest, AddCapabilityBothChannels)
{
    auto instance = createServerInstanceWithDevice();
    instance.addServer(SERVER_TYPE_ID, bothChannelsServerConfig(instance, 7633, 7634));

    const auto info = instance.getRootDevice().getInfo();
    ASSERT_TRUE(info.hasServerCapability(CONST_LT_STREAMING_ID));
    ASSERT_TRUE(info.hasServerCapability(CONST_LTS_STREAMING_ID));

    const auto plain = info.getServerCapability(CONST_LT_STREAMING_ID);
    const auto secure = info.getServerCapability(CONST_LTS_STREAMING_ID);

    ASSERT_EQ(plain.getPort(), 7633);
    ASSERT_EQ(secure.getPort(), 7634);
    ASSERT_EQ(plain.getProtocolGroupId(), secure.getProtocolGroupId());
    ASSERT_GT(secure.getProtocolSecurityLevel(), plain.getProtocolSecurityLevel());
}

TEST_F(LtStreamingTlsTest, FailedSecondServerLeavesFirstIntact)
{
    auto instance = createServerInstanceWithDevice();
    instance.addServer(SERVER_TYPE_ID, insecureServerConfig(instance, 7635));

    auto badConfig = secureServerConfig(instance, 7636, /*mtls*/ false);
    badConfig.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_SERVER, "secrets/does-not-exist.crt");
    ASSERT_THROW(instance.addServer(SERVER_TYPE_ID, badConfig), InvalidParameterException);

    // rejected because the device already carries an LT capability, not because of the config
    ASSERT_THROW(instance.addServer(SERVER_TYPE_ID, insecureServerConfig(instance, 7637)), InvalidStateException);

    ASSERT_TRUE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));

    auto client = createClientInstance();
    auto device = client.addDevice("daq.lt://127.0.0.1:7635/");
    runStreamingExchange(instance, device);
}

TEST_F(LtStreamingTlsTest, DiscoveryConfigsWsOnly)
{
    CapturingDiscoveryServer* discoveryServer = nullptr;
    auto instance = createServerInstanceWithDiscovery(&discoveryServer);
    auto server = instance.addServer(SERVER_TYPE_ID, insecureServerConfig(instance, 7640));
    server.enableDiscovery();

    ASSERT_EQ(discoveryServer->registered.size(), 1u);
    ASSERT_EQ(discoveryServer->registered[0].first, SERVER_TYPE_ID);

    const auto config = discoveryServer->registered[0].second;
    ASSERT_EQ(config.getPropertyValue("ServiceName"), CONST_LT_SERVICE_NAME);
    ASSERT_EQ(config.getPropertyValue("ServiceCap"), CONST_SERVICE_CAPABILITY);
    ASSERT_EQ(config.getPropertyValue("Port"), 7640);
    ASSERT_EQ(config.getPropertyValue("Path"), "/");
}

TEST_F(LtStreamingTlsTest, DiscoveryConfigsWssOnly)
{
    CapturingDiscoveryServer* discoveryServer = nullptr;
    auto instance = createServerInstanceWithDiscovery(&discoveryServer);
    auto server = instance.addServer(SERVER_TYPE_ID, secureServerConfig(instance, 7641, /*mtls*/ false));
    server.enableDiscovery();

    ASSERT_EQ(discoveryServer->registered.size(), 1u);

    const auto config = discoveryServer->registered[0].second;
    ASSERT_EQ(config.getPropertyValue("ServiceName"), CONST_LTS_SERVICE_NAME);
    ASSERT_EQ(config.getPropertyValue("ServiceCap"), CONST_SERVICE_CAPABILITY);
    ASSERT_EQ(config.getPropertyValue("Port"), 7641);
}

TEST_F(LtStreamingTlsTest, DiscoveryConfigsUseConfiguredPathAndPorts)
{
    CapturingDiscoveryServer* discoveryServer = nullptr;
    auto instance = createServerInstanceWithDiscovery(&discoveryServer);

    auto config = bothChannelsServerConfig(instance, 7642, 7643);
    config.setPropertyValue(PROPERTY_PATH_SERVER, "/lt");

    auto server = instance.addServer(SERVER_TYPE_ID, config);
    server.enableDiscovery();

    ASSERT_EQ(discoveryServer->registered.size(), 2u);

    const auto plain = findDiscoveryConfig(*discoveryServer, CONST_LT_SERVICE_NAME);
    const auto secure = findDiscoveryConfig(*discoveryServer, CONST_LTS_SERVICE_NAME);
    ASSERT_TRUE(plain.assigned());
    ASSERT_TRUE(secure.assigned());

    ASSERT_EQ(plain.getPropertyValue("Port"), 7642);
    ASSERT_EQ(secure.getPropertyValue("Port"), 7643);
    ASSERT_EQ(plain.getPropertyValue("Path"), "/lt");
    ASSERT_EQ(secure.getPropertyValue("Path"), "/lt");

    server.disableDiscovery();
    ASSERT_EQ(discoveryServer->unregistered.size(), 1u);
    ASSERT_EQ(discoveryServer->unregistered[0], SERVER_TYPE_ID);
}

TEST_F(LtStreamingTlsTest, DiscoveryConfigIgnoresDisabledChannelPort)
{
    CapturingDiscoveryServer* discoveryServer = nullptr;
    auto instance = createServerInstanceWithDiscovery(&discoveryServer);

    auto config = insecureServerConfig(instance, 7644);
    config.setPropertyValue(PROPERTY_WSS_STREAMING_PORT_SERVER, 7645);

    auto server = instance.addServer(SERVER_TYPE_ID, config);
    server.enableDiscovery();

    ASSERT_EQ(discoveryServer->registered.size(), 1u);
    ASSERT_EQ(discoveryServer->registered[0].second.getPropertyValue("Port"), 7644);
}

TEST_F(LtStreamingTlsTest, StopAfterRootDeviceRemovedDoesNotThrow)
{
    auto instance = createServerInstanceWithDevice();
    auto server = instance.addServer(SERVER_TYPE_ID, insecureServerConfig(instance, 7646));

    instance.getRootDevice().remove();
    ASSERT_NO_THROW(server.stop());
}
