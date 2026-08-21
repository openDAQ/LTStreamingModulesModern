// Tests for the TLS configuration of the LT streaming server module.
// This file is compiled only when DAQMODULES_LT_STREAMING_ENABLE_TLS is on

#include "test_websocket_streaming_server_module.h"

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
