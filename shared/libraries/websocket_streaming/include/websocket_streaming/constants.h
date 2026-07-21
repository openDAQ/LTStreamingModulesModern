#pragma once

#include <cstdint>
#include "common.h"

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

static constexpr const char* PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER = "EnableStreamingPort";
static constexpr const char* PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER = "EnableControlPort";
static constexpr const char* PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER = "EnableTlsStreamingPort";
static constexpr const char* PROPERTY_ENABLE_MTLS_SERVER = "EnableMutualTls";
static constexpr const char* PROPERTY_WS_STREAMING_PORT_SERVER = "WebsocketStreamingPort";
static constexpr const char* PROPERTY_WS_CONTROL_PORT_SERVER = "WebsocketControlPort";
static constexpr const char* PROPERTY_WSS_STREAMING_PORT_SERVER = "TlsWebsocketStreamingPort";
static constexpr const char* PROPERTY_WSS_CERT_FILE_PATH_SERVER = "CertificateFilePath";
static constexpr const char* PROPERTY_WSS_KEY_FILE_PATH_SERVER = "KeyFilePath";
static constexpr const char* PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER = "CaCertificateFilePath";

static constexpr const char* PROPERTY_ENABLE_MTLS_CLIENT = "EnableMutualTls";
static constexpr const char* PROPERTY_WS_STREAMING_PORT_CLIENT = "Port";            // to not break existing configs this property is named "Port" instead of "WebsocketStreamingPort"
static constexpr const char* PROPERTY_WSS_STREAMING_PORT_CLIENT = "Port";
static constexpr const char* PROPERTY_WSS_CERT_FILE_PATH_CLIENT = "CertificateFilePath";
static constexpr const char* PROPERTY_WSS_KEY_FILE_PATH_CLIENT = "KeyFilePath";
static constexpr const char* PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT = "CaCertificateFilePath";


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

