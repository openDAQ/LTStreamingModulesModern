#include <testutils/testutils.h>
#include <websocket_streaming_client_module/module_dll.h>
#include <websocket_streaming_client_module/version.h>
#include <websocket_streaming_client_module/websocket_streaming_client_module_impl.h>
#include <websocket_streaming/constants.h>

#include <opendaq/module_ptr.h>
#include <coretypes/common.h>

#include <opendaq/context_factory.h>
#include <opendaq/device_info_factory.h>
#include <opendaq/address_info_factory.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_factory.h>

using namespace daq;
using namespace daq::websocket_streaming;
using namespace daq::modules::websocket_streaming_client_module;

class WebsocketStreamingClientModuleTest : public testing::Test
{
protected:
    using ConnectionParameters = WebsocketStreamingClientModule::ConnectionParameters;

    static StringPtr formConnectionString(const StringPtr& connectionString,
                                          const PropertyObjectPtr& config,
                                          ConnectionParameters* outParams = nullptr)
    {
        return WebsocketStreamingClientModule::formConnectionString(connectionString, config, outParams);
    }

    static StringPtr createUrlConnectionString(const StringPtr& host, const IntegerPtr& port, const StringPtr& path)
    {
        return WebsocketStreamingClientModule::createUrlConnectionString(host, port, path);
    }

    static bool isSecureConnection(const std::string& connectionString)
    {
        return WebsocketStreamingClientModule::isSecureConnection(connectionString);
    }

    static bool acceptsConnectionParameters(const ModulePtr& module, const StringPtr& connectionString, const PropertyObjectPtr& config)
    {
        return reinterpret_cast<WebsocketStreamingClientModule*>(module.getObject())->acceptsConnectionParameters(connectionString, config);
    }

    static bool acceptsStreamingConnectionParameters(const ModulePtr& module, const StringPtr& connectionString, const PropertyObjectPtr& config)
    {
        return reinterpret_cast<WebsocketStreamingClientModule*>(module.getObject())->acceptsStreamingConnectionParameters(connectionString, config);
    }
};

static ModulePtr CreateModule()
{
    ModulePtr module;
    createModule(&module, NullContext());
    return module;
}

TEST_F(WebsocketStreamingClientModuleTest, CreateModule)
{
    IModule* module = nullptr;
    ErrCode errCode = createModule(&module, NullContext());
    ASSERT_TRUE(OPENDAQ_SUCCEEDED(errCode));

    ASSERT_NE(module, nullptr);
    module->releaseRef();
}

TEST_F(WebsocketStreamingClientModuleTest, ModuleName)
{
    auto module = CreateModule();
    ASSERT_EQ(module.getModuleInfo().getName(), "OpenDAQWebsocketClientModule");
}

TEST_F(WebsocketStreamingClientModuleTest, VersionAvailable)
{
    auto module = CreateModule();
    ASSERT_TRUE(module.getModuleInfo().getVersionInfo().assigned());
}

TEST_F(WebsocketStreamingClientModuleTest, VersionCorrect)
{
    auto module = CreateModule();
    auto version = module.getModuleInfo().getVersionInfo();

    ASSERT_EQ(version.getMajor(), WS_STREAM_CL_MODULE_MAJOR_VERSION);
    ASSERT_EQ(version.getMinor(), WS_STREAM_CL_MODULE_MINOR_VERSION);
    ASSERT_EQ(version.getPatch(), WS_STREAM_CL_MODULE_PATCH_VERSION);
}

TEST_F(WebsocketStreamingClientModuleTest, EnumerateDevices)
{
    auto module = CreateModule();

    ListPtr<IDeviceInfo> deviceInfo;
    ASSERT_NO_THROW(deviceInfo = module.getAvailableDevices());
}

TEST_F(WebsocketStreamingClientModuleTest, CreateDeviceConnectionStringNull)
{
    auto module = CreateModule();

    DevicePtr device;
    ASSERT_THROW(device = module.createDevice(nullptr, nullptr), ArgumentNullException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateDeviceConnectionStringEmpty)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createDevice("", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateDeviceConnectionStringInvalid)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createDevice("fdfdfdfdde", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateDeviceConnectionStringInvalidId)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createDevice("daqref://devicett3axxr1", nullptr), InvalidParameterException);
    ASSERT_THROW(module.createDevice("daq.opcua://devicett3axxr1", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateStreamingWithNullArguments)
{
    auto module = CreateModule();

    DevicePtr device;
    ASSERT_THROW(device = module.createStreaming(nullptr, nullptr), ArgumentNullException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateStreamingWithConnectionStringEmpty)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createStreaming("", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateStreamingConnectionStringInvalid)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createStreaming("fdfdfdfdde", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateStreamingConnectionStringInvalidId)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createStreaming("daqref://devicett3axxr1", nullptr), InvalidParameterException);
    ASSERT_THROW(module.createStreaming("daq.opcua://devicett3axxr1", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, GetAvailableComponentTypes)
{
    const auto module = CreateModule();

    DictPtr<IString, IFunctionBlockType> functionBlockTypes;
    ASSERT_NO_THROW(functionBlockTypes = module.getAvailableFunctionBlockTypes());
    ASSERT_EQ(functionBlockTypes.getCount(), 0u);

    DictPtr<IString, IDeviceType> deviceTypes;
    ASSERT_NO_THROW(deviceTypes = module.getAvailableDeviceTypes());
    ASSERT_EQ(deviceTypes.getCount(), 3u);
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreaming"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreaming").getId(), "OpenDAQLTStreaming");
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreamingSecure"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreamingSecure").getId(), "OpenDAQLTStreamingSecure");
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreamingOld"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreamingOld").getId(), "OpenDAQLTStreamingOld");

    DictPtr<IString, IServerType> serverTypes;
    ASSERT_NO_THROW(serverTypes = module.getAvailableServerTypes());
    ASSERT_EQ(serverTypes.getCount(), 0u);
}

TEST_F(WebsocketStreamingClientModuleTest, GetAvailableStreamingTypes)
{
    const auto module = CreateModule();

    DictPtr<IString, IStreamingType> streamingTypes;
    ASSERT_NO_THROW(streamingTypes = module.getAvailableStreamingTypes());
    ASSERT_EQ(streamingTypes.getCount(), 2u);

    ASSERT_TRUE(streamingTypes.hasKey("OpenDAQLTStreaming"));
    ASSERT_EQ(streamingTypes.get("OpenDAQLTStreaming").getId(), "OpenDAQLTStreaming");
    ASSERT_TRUE(streamingTypes.hasKey("OpenDAQLTStreamingSecure"));
    ASSERT_EQ(streamingTypes.get("OpenDAQLTStreamingSecure").getId(), "OpenDAQLTStreamingSecure");
}

TEST_F(WebsocketStreamingClientModuleTest, CreateFunctionBlockIdNull)
{
    auto module = CreateModule();

    FunctionBlockPtr functionBlock;
    ASSERT_THROW(functionBlock = module.createFunctionBlock(nullptr, nullptr, "fb"), ArgumentNullException);
}

TEST_F(WebsocketStreamingClientModuleTest, CreateFunctionBlockIdEmpty)
{
    auto module = CreateModule();

    ASSERT_THROW(module.createFunctionBlock("", nullptr, "fb"), NotFoundException);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringKeepsPortAndPath)
{
    ConnectionParameters params;
    const auto result = formConnectionString("daq.lt://host:1234/foo", nullptr, &params);

    ASSERT_EQ(result, "daq.lt://host:1234/foo");
    ASSERT_EQ(params.host, "host");
    ASSERT_EQ(params.port, 1234);
    ASSERT_EQ(params.path, "/foo");
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringDefaultsPath)
{
    ConnectionParameters params;
    formConnectionString("daq.lt://host:1234", nullptr, &params);

    ASSERT_EQ(params.path, "");
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringNoPortNoConfig0)
{
    const auto result = formConnectionString("daq.lt://host", nullptr);
    const auto expected = std::string("daq.lt://host");
    ASSERT_EQ(result, expected);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringNoPortNoConfig1)
{
    const auto result = formConnectionString("daq.lts://host", nullptr);
    const auto expected = std::string("daq.lts://host");
    ASSERT_EQ(result, expected);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringTakesPortFromConfig)
{
    auto config = PropertyObject();
    config.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_CLIENT, 7414));

    ConnectionParameters params;
    formConnectionString("daq.lt://host", config, &params);
    ASSERT_EQ(params.port, 7414);

    auto secureConfig = PropertyObject();
    secureConfig.addProperty(IntProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT, 7415));

    ConnectionParameters secureParams;
    formConnectionString("daq.lts://host", secureConfig, &secureParams);
    ASSERT_EQ(secureParams.port, 7415);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringNormalizesOldStylePrefixes)
{
    ASSERT_EQ(formConnectionString("daq.ws://host:1234/", nullptr), "daq.lt://host:1234/");
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringParsesIpv6)
{
    ConnectionParameters params;
    const auto result = formConnectionString("daq.lt://[::1]:1234/", nullptr, &params);

    ASSERT_EQ(result, "daq.lt://[::1]:1234/");
    ASSERT_EQ(params.host, "[::1]");
    ASSERT_EQ(params.port, 1234);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringIpv6TakesPortFromConfig)
{
    auto config = PropertyObject();
    config.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_CLIENT, 7414));

    ConnectionParameters params;
    formConnectionString("daq.lt://[::1]/", config, &params);

    ASSERT_EQ(params.host, "[::1]");
    ASSERT_EQ(params.port, 7414);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringParsesIpv6WithZoneId)
{
    ConnectionParameters params;
    ASSERT_NO_THROW(formConnectionString("daq.lt://[fe80::1%eth0]/", nullptr, &params));
    ASSERT_EQ(params.host, "[fe80::1%eth0]");
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringParsesPrefixless)
{
    ConnectionParameters params;
    formConnectionString("host:1234", nullptr, &params);

    ASSERT_EQ(params.prefix, "");
    ASSERT_EQ(params.host, "host");
    ASSERT_EQ(params.port, 1234);
}

TEST_F(WebsocketStreamingClientModuleTest, FormConnectionStringThrowsOnUnparsable)
{
    ASSERT_THROW(formConnectionString("", nullptr), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, IsSecureConnection)
{
    ASSERT_TRUE(isSecureConnection("daq.lts://h"));
    ASSERT_FALSE(isSecureConnection("daq.lt://h"));
    ASSERT_FALSE(isSecureConnection("daq.ws://h"));
    ASSERT_FALSE(isSecureConnection("daq.wss://h"));
}

TEST_F(WebsocketStreamingClientModuleTest, AcceptsAllPrefixes)
{
    auto module = CreateModule();

    ASSERT_TRUE(acceptsConnectionParameters(module, "daq.lt://h", nullptr));
    ASSERT_TRUE(acceptsConnectionParameters(module, "daq.ws://h", nullptr));
    ASSERT_TRUE(acceptsConnectionParameters(module, "daq.lts://h", nullptr));
}

TEST_F(WebsocketStreamingClientModuleTest, RejectsForeignPrefixes)
{
    auto module = CreateModule();

    ASSERT_FALSE(acceptsConnectionParameters(module, "daq.opcua://h", nullptr));
    ASSERT_FALSE(acceptsConnectionParameters(module, "daqref://h", nullptr));
    ASSERT_FALSE(acceptsConnectionParameters(module, "daq.nd://h", nullptr));
    ASSERT_FALSE(acceptsConnectionParameters(module, "garbage", nullptr));
    ASSERT_FALSE(acceptsConnectionParameters(module, "x daq.lt://h", nullptr));
}

TEST_F(WebsocketStreamingClientModuleTest, AcceptsStreamingRejectsEmpty)
{
    auto module = CreateModule();

    ASSERT_FALSE(acceptsStreamingConnectionParameters(module, "", nullptr));
    ASSERT_FALSE(acceptsStreamingConnectionParameters(module, nullptr, nullptr));
    ASSERT_TRUE(acceptsStreamingConnectionParameters(module, "daq.lts://h", nullptr));
}

TEST_F(WebsocketStreamingClientModuleTest, SecureStreamingDefaultConfigRejectsEmptyCertKey)
{
    auto module = CreateModule();
    ASSERT_THROW_MSG(module.createStreaming("daq.lts://127.0.0.1:1/", nullptr),
                     InvalidParameterException,
                     "TLS certificate or key file path is not configured");
}

TEST_F(WebsocketStreamingClientModuleTest, SecureStreamingRejectsEmptyCaWithMtlsDisabled)
{
    auto module = CreateModule();
    auto config = module.getAvailableStreamingTypes().get("OpenDAQLTStreamingSecure").createDefaultConfig();
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, False);

    ASSERT_THROW_MSG(module.createStreaming("daq.lts://127.0.0.1:1/", config),
                     InvalidParameterException,
                     "TLS CA certificate file path is not configured");
}

TEST_F(WebsocketStreamingClientModuleTest, SecureStreamingRejectsEmptyCaWithMtlsEnabled)
{
    auto module = CreateModule();
    auto config = module.getAvailableStreamingTypes().get("OpenDAQLTStreamingSecure").createDefaultConfig();
    config.setPropertyValue(PROPERTY_WSS_CERT_FILE_PATH_CLIENT, "/tmp/cert.pem");
    config.setPropertyValue(PROPERTY_WSS_KEY_FILE_PATH_CLIENT, "/tmp/key.pem");

    ASSERT_THROW_MSG(module.createStreaming("daq.lts://127.0.0.1:1/", config),
                     InvalidParameterException,
                     "TLS CA certificate file path is not configured");
}

TEST_F(WebsocketStreamingClientModuleTest, DefaultInsecureStreamingConfig)
{
    auto module = CreateModule();
    auto config = module.getAvailableStreamingTypes().get("OpenDAQLTStreaming").createDefaultConfig();
    ASSERT_TRUE(config.assigned());

    ASSERT_EQ(config.getAllProperties().getCount(), 1u);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WS_STREAMING_PORT_CLIENT));
    ASSERT_EQ(config.getProperty(PROPERTY_WS_STREAMING_PORT_CLIENT).getValueType(), CoreType::ctInt);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_CLIENT), DEFAULT_WS_STREAMING_PORT);
}

TEST_F(WebsocketStreamingClientModuleTest, DefaultSecureStreamingConfig)
{
    auto module = CreateModule();
    auto config = module.getAvailableStreamingTypes().get("OpenDAQLTStreamingSecure").createDefaultConfig();
    ASSERT_TRUE(config.assigned());

    ASSERT_EQ(config.getAllProperties().getCount(), 5u);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_ENABLE_MTLS_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT));

    ASSERT_EQ(config.getProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT).getValueType(), CoreType::ctInt);
    ASSERT_EQ(config.getProperty(PROPERTY_ENABLE_MTLS_CLIENT).getValueType(), CoreType::ctBool);

    ASSERT_EQ(config.getPropertyValue(PROPERTY_WSS_STREAMING_PORT_CLIENT), DEFAULT_WSS_STREAMING_PORT);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT), DEFAULT_ENABLE_MTLS);

    config.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, True);
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT).getVisible());
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, False);
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT).getVisible());
}