// Tests for the daq.lts:// (TLS) streaming channel of the LT streaming client module.
// This file is compiled only when DAQMODULES_LT_STREAMING_ENABLE_TLS is on

#include "test_websocket_streaming_client_module.h"

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

TEST_F(WebsocketStreamingClientModuleTest, SecureStreamingCompletesNullConfig)
{
    // A missing configuration is completed with the secure defaults, so the constructor reaches
    // the certificate check instead of dereferencing a null configuration object.
    ASSERT_THROW_MSG((createWithImplementation<IStreaming, WsStreaming>(
                          String("daq.lts://127.0.0.1:1/"), NullContext(), nullptr)),
                     InvalidParameterException,
                     "TLS certificate or key file path is not configured");
}

TEST_F(WebsocketStreamingClientModuleTest, SecureStreamingCompletesPartialConfig)
{
    // A configuration carrying only the port is completed with the missing TLS properties.
    auto module = CreateModule();
    auto config = PropertyObject();
    config.addProperty(IntProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT, DEFAULT_WSS_STREAMING_PORT));

    ASSERT_THROW_MSG(module.createStreaming("daq.lts://127.0.0.1:1/", config),
                     InvalidParameterException,
                     "TLS certificate or key file path is not configured");
}

TEST_F(WebsocketStreamingClientModuleTest, DefaultSecureStreamingConfig)
{
    auto module = CreateModule();
    auto config = module.getAvailableStreamingTypes().get("OpenDAQLTStreamingSecure").createDefaultConfig();
    ASSERT_TRUE(config.assigned());

    ASSERT_EQ(config.getAllProperties().getCount(), 6u);

    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_VERIFY_SERVER_CERT_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_ENABLE_MTLS_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT));
    ASSERT_TRUE(config.hasProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT));

    ASSERT_EQ(config.getProperty(PROPERTY_WSS_STREAMING_PORT_CLIENT).getValueType(), CoreType::ctInt);
    ASSERT_EQ(config.getProperty(PROPERTY_VERIFY_SERVER_CERT_CLIENT).getValueType(), CoreType::ctBool);
    ASSERT_EQ(config.getProperty(PROPERTY_ENABLE_MTLS_CLIENT).getValueType(), CoreType::ctBool);

    ASSERT_EQ(config.getPropertyValue(PROPERTY_WSS_STREAMING_PORT_CLIENT), DEFAULT_WSS_STREAMING_PORT);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_VERIFY_SERVER_CERT_CLIENT), DEFAULT_VERIFY_SERVER_CERT);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT), DEFAULT_ENABLE_MTLS);

    config.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, True);
    ASSERT_TRUE(config.getProperty(PROPERTY_ENABLE_MTLS_CLIENT).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT).getVisible());
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, False);
    ASSERT_TRUE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT).getVisible());

    // Nothing below the verification switch is meaningful once it is off
    config.setPropertyValue(PROPERTY_ENABLE_MTLS_CLIENT, True);
    config.setPropertyValue(PROPERTY_VERIFY_SERVER_CERT_CLIENT, False);
    ASSERT_TRUE(config.getProperty(PROPERTY_VERIFY_SERVER_CERT_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_ENABLE_MTLS_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_CERT_FILE_PATH_CLIENT).getVisible());
    ASSERT_FALSE(config.getProperty(PROPERTY_WSS_KEY_FILE_PATH_CLIENT).getVisible());
}

TEST_F(WebsocketStreamingClientModuleTest, SecureStreamingWithoutVerificationNeedsNoCa)
{
    auto module = CreateModule();
    auto config = module.getAvailableStreamingTypes().get("OpenDAQLTStreamingSecure").createDefaultConfig();
    config.setPropertyValue(PROPERTY_VERIFY_SERVER_CERT_CLIENT, False);

    ASSERT_THROW(module.createStreaming("daq.lts://127.0.0.1:1/", config), NotFoundException);
}

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceLtsService)
{
    const auto info = populateDiscoveredDevice(makeDiscoveredDevice(CONST_LTS_SERVICE_NAME, 7435));
    const auto cap = firstCapability(info);

    ASSERT_EQ(cap.getProtocolId(), CONST_LTS_STREAMING_ID);
    ASSERT_EQ(cap.getProtocolName(), CONST_LTS_STREAMING_ID);
    ASSERT_EQ(cap.getPrefix(), CONST_LTS_STREAMING_PREFIX);
    ASSERT_EQ(cap.getProtocolGroupId(), CONST_LT_PROTOCOL_GROUP_ID);
    ASSERT_EQ(cap.getProtocolType(), ProtocolType::Streaming);
    ASSERT_EQ(cap.getPort(), 7435);
    ASSERT_EQ(info.getDeviceType().getId(), CONST_LTS_STREAMING_ID);
    ASSERT_EQ(cap.getProtocolSecurityLevel(), CONST_LTS_STREAMING_SECURITY_LVL);
}

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceSecurityLevel)
{
    const auto insecure = populateDiscoveredDevice(makeDiscoveredDevice(CONST_LT_SERVICE_NAME));
    const auto secure = populateDiscoveredDevice(makeDiscoveredDevice(CONST_LTS_SERVICE_NAME, 7415));

    ASSERT_EQ(firstCapability(insecure).getProtocolSecurityLevel(), CONST_LT_STREAMING_SECURITY_LVL);
    ASSERT_EQ(firstCapability(secure).getProtocolSecurityLevel(), CONST_LTS_STREAMING_SECURITY_LVL);
    ASSERT_GT(firstCapability(secure).getProtocolSecurityLevel(), firstCapability(insecure).getProtocolSecurityLevel());
}

TEST_F(WebsocketStreamingClientModuleTest, CompleteServerCapabilityBuildsString)
{
    auto module = CreateModule();

    auto target = ServerCapability(CONST_LTS_STREAMING_ID, CONST_LTS_STREAMING_ID, ProtocolType::Streaming);
    target.setPrefix(CONST_LTS_STREAMING_PREFIX);
    target.setPort(7435);

    ASSERT_TRUE(completeServerCapability(module, makeSourceCapability("192.168.1.10", "daq.opcua"), target));

    ASSERT_EQ(target.getConnectionString(), "daq.lts://192.168.1.10:7435");
    ASSERT_EQ(target.getAddresses().getCount(), 1u);
    ASSERT_EQ(target.getAddresses()[0], "192.168.1.10");
}
