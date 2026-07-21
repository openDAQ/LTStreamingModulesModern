#pragma once

#include <cstdint>
#include "common.h"

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

// static const char* MODULE_NAME = "OpenDAQMQTTModule";
// static const char* MODULE_ID = "OpenDAQMQTTModule";
// static const char* SHORT_MODULE_NAME = "MQTTModule";


static constexpr const char* PROPERTY_ENABLE_WS_STREAMING_PORT = "EnableStreamingPort";
static constexpr const char* PROPERTY_ENABLE_WS_CONTROL_PORT = "EnableControlPort";
static constexpr const char* PROPERTY_ENABLE_WSS_STREAMING_PORT = "EnableTlsStreamingPort";
static constexpr const char* PROPERTY_ENABLE_MTLS = "EnableMutualTls";
static constexpr const char* PROPERTY_WS_STREAMING_PORT = "WebsocketStreamingPort";
static constexpr const char* PROPERTY_WS_CONTROL_PORT = "WebsocketControlPort";
static constexpr const char* PROPERTY_WSS_STREAMING_PORT = "TlsWebsocketStreamingPort";
static constexpr const char* PROPERTY_WSS_CERT_FILE_PATH = "CertificateFilePath";
static constexpr const char* PROPERTY_WSS_KEY_FILE_PATH = "KeyFilePath";
static constexpr const char* PROPERTY_WSS_CA_CERT_FILE_PATH = "CaCertificateFilePath";


static constexpr bool DEFAULT_ENABLE_WS_STREAMING_PORT = true;
static constexpr bool DEFAULT_ENABLE_WS_CONTROL_PORT = true;
static constexpr bool DEFAULT_ENABLE_WSS_STREAMING_PORT = false;
static constexpr bool DEFAULT_ENABLE_MTLS = true;
static constexpr std::uint16_t DEFAULT_WS_STREAMING_PORT = 7414;
static constexpr std::uint16_t DEFAULT_WS_CONTROL_PORT = 7438;
static constexpr std::uint16_t DEFAULT_WSS_STREAMING_PORT = 7415;
static constexpr const char* DEFAULT_WSS_CERT_FILE_PATH = "";
static constexpr const char* DEFAULT_WSS_KEY_FILE_PATH = "";
static constexpr const char* DEFAULT_WSS_CA_CERT_FILE_PATH = "";


END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

