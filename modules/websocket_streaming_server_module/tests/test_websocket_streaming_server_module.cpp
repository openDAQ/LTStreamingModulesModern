#include <websocket_streaming_server_module/module_dll.h>
#include <websocket_streaming_server_module/ws_streaming_server_module.h>
#include <websocket_streaming_server_module/version.h>
#include <websocket_streaming/constants.h>
#include <opendaq/context_factory.h>
#include <opendaq/instance_factory.h>
#include <opendaq/instance_ptr.h>
#include <opendaq/module_manager_factory.h>
#include <opendaq/module_ptr.h>
#include <coretypes/common.h>
#include <testutils/testutils.h>

class WsStreamingServerModuleTest : public testing::Test
{
public:
    void TearDown() override
    {
    }
};

using namespace daq;
using namespace daq::websocket_streaming;

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

    ASSERT_TRUE(config.hasProperty("Path"));
    ASSERT_EQ(config.getPropertyValue("Path"), "/");
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
