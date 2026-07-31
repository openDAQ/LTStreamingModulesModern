#include <websocket_streaming_server_module/module_dll.h>
#include <websocket_streaming_server_module/ws_streaming_server_module.h>
#include <websocket_streaming_server_module/version.h>
#include <websocket_streaming/constants.h>
#include <websocket_streaming/ws_streaming_server.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_factory.h>
#include <opendaq/context_factory.h>
#include <opendaq/instance_factory.h>
#include <opendaq/instance_ptr.h>
#include <opendaq/module_manager_factory.h>
#include <opendaq/logger_factory.h>
#include <coretypes/type_manager_factory.h>
#include <opendaq/module_ptr.h>
#include <coretypes/common.h>
#include <testutils/testutils.h>

using namespace daq;
using namespace daq::websocket_streaming;

class WsStreamingServerModuleTest : public testing::Test
{
public:
    void TearDown() override
    {
    }

    static ContextPtr CreateContextWithModuleOptions(const DictPtr<IString, IBaseObject>& moduleOptions)
    {
        auto options = Dict<IString, IBaseObject>();
        options.set("Modules", Dict<IString, IBaseObject>({{"StreamingLtServer", moduleOptions}}));

        return NullContext(Logger(), TypeManager(), options);
    }

    static PropertyObjectPtr CreateWsOnlyConfig(const ModulePtr& module, Int wsPort)
    {
        auto config = module.getAvailableServerTypes().get("OpenDAQLTStreaming").createDefaultConfig();
        config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True);
        config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
        config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, False);
        config.setPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER, wsPort);
        return config;
    }
};

static ModulePtr CreateModule(ContextPtr context = NullContext())
{
    ModulePtr module;
    createModule(&module, context);
    return module;
}

static PropertyObjectPtr CreateServerConfig()
{
    auto module = CreateModule();
    auto serverTypes = module.getAvailableServerTypes();
    return serverTypes.get("OpenDAQLTStreaming").createDefaultConfig();
}

TEST_F(WsStreamingServerModuleTest, CreateModule)
{
    IModule* module = nullptr;
    ErrCode errCode = createModule(&module, NullContext());
    ASSERT_TRUE(OPENDAQ_SUCCEEDED(errCode));

    ASSERT_NE(module, nullptr);
    module->releaseRef();
}

TEST_F(WsStreamingServerModuleTest, ModuleName)
{
    auto module = CreateModule();
    ASSERT_EQ(module.getModuleInfo().getName(), "OpenDAQWebSocketStreamingServerModule");
}

TEST_F(WsStreamingServerModuleTest, VersionAvailable)
{
    auto module = CreateModule();
    ASSERT_TRUE(module.getModuleInfo().getVersionInfo().assigned());
}

TEST_F(WsStreamingServerModuleTest, VersionCorrect)
{
    auto module = CreateModule();
    auto version = module.getModuleInfo().getVersionInfo();

    ASSERT_EQ(version.getMajor(), WS_STREAM_SRV_MODULE_MAJOR_VERSION);
    ASSERT_EQ(version.getMinor(), WS_STREAM_SRV_MODULE_MINOR_VERSION);
    ASSERT_EQ(version.getPatch(), WS_STREAM_SRV_MODULE_PATCH_VERSION);
}

TEST_F(WsStreamingServerModuleTest, GetAvailableComponentTypes)
{
    const auto module = CreateModule();

    DictPtr<IString, IFunctionBlockType> functionBlockTypes;
    ASSERT_NO_THROW(functionBlockTypes = module.getAvailableFunctionBlockTypes());
    ASSERT_EQ(functionBlockTypes.getCount(), 1u);

    DictPtr<IString, IDeviceType> deviceTypes;
    ASSERT_NO_THROW(deviceTypes = module.getAvailableDeviceTypes());
    ASSERT_EQ(deviceTypes.getCount(), 0u);

    DictPtr<IString, IServerType> serverTypes;
    ASSERT_NO_THROW(serverTypes = module.getAvailableServerTypes());
    ASSERT_EQ(serverTypes.getCount(), 1u);
    ASSERT_TRUE(serverTypes.hasKey("OpenDAQLTStreaming"));
    ASSERT_EQ(serverTypes.get("OpenDAQLTStreaming").getId(), "OpenDAQLTStreaming");
}

TEST_F(WsStreamingServerModuleTest, ServerConfig)
{
    auto config = CreateServerConfig();
    ASSERT_TRUE(config.assigned());

    ASSERT_EQ(config.getAllProperties().getCount(), 11u);

    ASSERT_TRUE(config.hasProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER).getValueType(), CoreType::ctBool);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER), DEFAULT_ENABLE_WS_STREAMING_PORT);

    ASSERT_TRUE(config.hasProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER).getValueType(), CoreType::ctBool);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER), DEFAULT_ENABLE_WS_CONTROL_PORT);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WS_STREAMING_PORT_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_WS_STREAMING_PORT_SERVER).getValueType(), CoreType::ctInt);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), DEFAULT_WS_STREAMING_PORT);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WS_CONTROL_PORT_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_WS_CONTROL_PORT_SERVER).getValueType(), CoreType::ctInt);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_CONTROL_PORT_SERVER), DEFAULT_WS_CONTROL_PORT);

    ASSERT_TRUE(config.hasProperty(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER).getValueType(), CoreType::ctBool);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER), DEFAULT_ENABLE_WSS_STREAMING_PORT);

    ASSERT_TRUE(config.hasProperty(PROPERTY_ENABLE_MTLS_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_ENABLE_MTLS_SERVER).getValueType(), CoreType::ctBool);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_MTLS_SERVER), DEFAULT_ENABLE_MTLS);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_STREAMING_PORT_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_WSS_STREAMING_PORT_SERVER).getValueType(), CoreType::ctInt);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WSS_STREAMING_PORT_SERVER), DEFAULT_WSS_STREAMING_PORT);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_CERT_FILE_PATH_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_SERVER).getValueType(), CoreType::ctString);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_KEY_FILE_PATH_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_SERVER).getValueType(), CoreType::ctString);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER));
    ASSERT_EQ(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER).getValueType(), CoreType::ctString);

    ASSERT_TRUE(config.hasProperty(PROPERTY_PATH_SERVER));
    ASSERT_EQ(config.getPropertyValue(PROPERTY_PATH_SERVER), "/");
}

TEST_F(WsStreamingServerModuleTest, ServerConfigVisibility)
{
    auto config = CreateServerConfig();

    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True);
    ASSERT_TRUE(config.getProperty(PROPERTY_WS_STREAMING_PORT_SERVER).getVisible());
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);
    ASSERT_FALSE(config.getProperty(PROPERTY_WS_STREAMING_PORT_SERVER).getVisible());

    config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, True);
    ASSERT_TRUE(config.getProperty(PROPERTY_WS_CONTROL_PORT_SERVER).getVisible());
    config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
    ASSERT_FALSE(config.getProperty(PROPERTY_WS_CONTROL_PORT_SERVER).getVisible());

    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, False);
    ASSERT_FALSE(config.getProperty(PROPERTY_ENABLE_MTLS_SERVER).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_STREAMING_PORT_SERVER).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_SERVER).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_SERVER).getVisible());

    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);
    ASSERT_TRUE(config.getProperty(PROPERTY_ENABLE_MTLS_SERVER).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_STREAMING_PORT_SERVER).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_SERVER).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_SERVER).getVisible());

    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, True);
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER).getVisible());

    config.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, False);
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER).getVisible());

    config.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, True);
    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, False);
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER).getVisible());
}

TEST_F(WsStreamingServerModuleTest, CreateServerRejectsMtlsWithoutCa)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto config = module.getAvailableServerTypes().get("OpenDAQLTStreaming").createDefaultConfig();
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);

    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "Mutual TLS is enabled but no CA certificate file path is configured");
}

TEST_F(WsStreamingServerModuleTest, CreateServerRejectsEmptyCertificatePath)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto config = module.getAvailableServerTypes().get("OpenDAQLTStreaming").createDefaultConfig();
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, False);
    config.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_SERVER, "some-key.pem");

    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "TLS certificate or key file path is not configured");
}

TEST_F(WsStreamingServerModuleTest, CreateServerRejectsEmptyKeyPath)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto config = module.getAvailableServerTypes().get("OpenDAQLTStreaming").createDefaultConfig();
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_SERVER, False);
    config.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_SERVER, "some-cert.pem");

    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "TLS certificate or key file path is not configured");
}

TEST_F(WsStreamingServerModuleTest, AddCapabilityWsOnly)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());
    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7650));

    const auto info = instance.getRootDevice().getInfo();
    ASSERT_TRUE(info.hasServerCapability(CONST_LT_STREAMING_ID));
    ASSERT_FALSE(info.hasServerCapability(CONST_LTS_STREAMING_ID));

    const auto cap = info.getServerCapability(CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getPrefix(), CONST_LT_STREAMING_PREFIX);
    ASSERT_EQ(cap.getPort(), 7650);
    ASSERT_EQ(cap.getConnectionType(), "TCP/IP");
    ASSERT_EQ(cap.getProtocolGroupId(), CONST_LT_PROTOCOL_GROUP_ID);
    ASSERT_EQ(cap.getProtocolSecurityLevel(), CONST_LT_STREAMING_SECURITY_LVL);

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, RemoveCapabilityOnStop)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());
    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7651));

    const auto info = instance.getRootDevice().getInfo();
    ASSERT_TRUE(info.hasServerCapability(CONST_LT_STREAMING_ID));

    server.stop();
    ASSERT_FALSE(info.hasServerCapability(CONST_LT_STREAMING_ID));
    ASSERT_FALSE(info.hasServerCapability(CONST_LTS_STREAMING_ID));

    // openDAQ may call stop() more than once and the destructor stops the server again
    ASSERT_NO_THROW(server.stop());
}

TEST_F(WsStreamingServerModuleTest, SecondServerOnSameDeviceThrows)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());
    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7652));

    ASSERT_THROW(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7653)),
                 InvalidStateException);

    ASSERT_TRUE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, ServerRestartReAddsCapability)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7654));
    server.stop();

    ASSERT_NO_THROW(server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7654)));
    ASSERT_TRUE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, PartialConfigIsCompleted)
{
    const auto instance = Instance();

    auto config = PropertyObject();
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True));
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False));

    ServerPtr server;
    ASSERT_NO_THROW((server = createWithImplementation<IServer, websocket_streaming::WsStreamingServer>(
                         instance.getRootDevice(), config, instance.getContext())));

    ASSERT_EQ(config.getAllProperties().getCount(), 11u);
    ASSERT_TRUE(config.hasProperty(PROPERTY_PATH_SERVER));
    ASSERT_EQ(config.getPropertyValue(PROPERTY_PATH_SERVER), "/");
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), DEFAULT_WS_STREAMING_PORT);

    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER), False);

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, PartialConfigPreservesUserValues)
{
    const auto instance = Instance();

    auto config = PropertyObject();
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True));
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False));
    config.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_SERVER, 7655));

    auto server = createWithImplementation<IServer, websocket_streaming::WsStreamingServer>(
        instance.getRootDevice(), config, instance.getContext());

    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7655);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER), DEFAULT_ENABLE_WSS_STREAMING_PORT);
    ASSERT_EQ(instance.getRootDevice().getInfo().getServerCapability(CONST_LT_STREAMING_ID).getPort(), 7655);

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, PartialConfigKeepsForeignProperties)
{
    const auto instance = Instance();

    auto config = PropertyObject();
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True));
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False));
    config.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_SERVER, 7656));
    config.addProperty(StringProperty("SomeForeignProperty", "keep me"));

    ServerPtr server;
    ASSERT_NO_THROW((server = createWithImplementation<IServer, websocket_streaming::WsStreamingServer>(
                         instance.getRootDevice(), config, instance.getContext())));

    ASSERT_EQ(config.getAllProperties().getCount(), 12u);
    ASSERT_EQ(config.getPropertyValue("SomeForeignProperty"), "keep me");

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, CreateServerRejectsAllChannelsDisabled)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto config = CreateWsOnlyConfig(module, 7657);
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);

    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "Neither the websocket streaming port nor the TLS streaming port is enabled");

    ASSERT_FALSE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));
}

TEST_F(WsStreamingServerModuleTest, CreateServerRejectsControlPortWithoutStreamingPort)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto config = CreateWsOnlyConfig(module, 7658);
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);
    config.setPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, True);
    config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, True);

    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "The control port cannot be enabled without the websocket streaming port");
}

TEST_F(WsStreamingServerModuleTest, ProviderOptionsOverrideDefaults)
{
    auto moduleOptions = Dict<IString, IBaseObject>();
    moduleOptions.set(PROPERTY_WS_STREAMING_PORT_SERVER, Integer(7660));
    moduleOptions.set(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, Boolean(False));
    moduleOptions.set(PROPERTY_PATH_SERVER, String("/lt"));

    const auto context = CreateContextWithModuleOptions(moduleOptions);

    auto config = WsStreamingServer::createDefaultConfig(context);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7660);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER), False);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_PATH_SERVER), "/lt");
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER), DEFAULT_ENABLE_WS_STREAMING_PORT);

    auto module = CreateModule(context);
    auto typeConfig = module.getAvailableServerTypes().get("OpenDAQLTStreaming").createDefaultConfig();
    ASSERT_EQ(typeConfig.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7660);
    ASSERT_EQ(typeConfig.getPropertyValue(PROPERTY_PATH_SERVER), "/lt");

}

TEST_F(WsStreamingServerModuleTest, ProviderOptionsIgnoreUnknownKeys)
{
    auto moduleOptions = Dict<IString, IBaseObject>();
    moduleOptions.set("NoSuchProperty", String("ignored"));
    moduleOptions.set(PROPERTY_WS_STREAMING_PORT_SERVER, Integer(7661));

    const auto context = CreateContextWithModuleOptions(moduleOptions);

    PropertyObjectPtr config;
    ASSERT_NO_THROW(config = WsStreamingServer::createDefaultConfig(context));

    ASSERT_EQ(config.getAllProperties().getCount(), 11u);
    ASSERT_FALSE(config.hasProperty("NoSuchProperty"));
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7661);
}
