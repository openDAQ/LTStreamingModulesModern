#pragma once

#include <cstdint>
#include "common.h"

BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

static constexpr const char* CONST_LT_SERVICE_NAME = "_streaming-lt._tcp.local.";
static constexpr const char* CONST_WS_SERVICE_NAME = "_streaming-ws._tcp.local.";
static constexpr const char* CONST_LTS_SERVICE_NAME = "_streaming-lts._tcp.local.";

static constexpr const char* CONST_LT_STREAMING_PREFIX = "daq.lt";
static constexpr const char* CONST_WS_STREAMING_PREFIX = "daq.ws";
static constexpr const char* CONST_LTS_STREAMING_PREFIX = "daq.lts";

static constexpr const char* CONST_SERVICE_CAPABILITY = "LT";

static constexpr const char* CONST_LT_PROTOCOL_GROUP_ID = "LTStreaming";
static constexpr const char* CONST_LT_STREAMING_ID = "OpenDAQLTStreaming";
static constexpr const char* CONST_LTS_STREAMING_ID = "OpenDAQLTStreamingSecure";
static constexpr const char* CONST_LT_STREAMING_SERVER_ID = "OpenDAQLTStreaming";
static constexpr const char* CONST_LTS_STREAMING_SERVER_ID = "OpenDAQLTStreamingSecure";
static constexpr const int64_t CONST_LT_STREAMING_SECURITY_LVL = 0;
static constexpr const int64_t CONST_LTS_STREAMING_SECURITY_LVL = 10;

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
static constexpr const char* PROPERTY_PATH_SERVER = "Path";

static constexpr const char* PROPERTY_VERIFY_SERVER_CERT_CLIENT = "VerifyServerCertificate";
static constexpr const char* PROPERTY_ENABLE_MTLS_CLIENT = "EnableMutualTls";
static constexpr const char* PROPERTY_WS_STREAMING_PORT_CLIENT = "Port";            // to not break existing configs this property is named "Port" instead of "WebsocketStreamingPort"
static constexpr const char* PROPERTY_WSS_STREAMING_PORT_CLIENT = "Port";
static constexpr const char* PROPERTY_WSS_CERT_FILE_PATH_CLIENT = "CertificateFilePath";
static constexpr const char* PROPERTY_WSS_KEY_FILE_PATH_CLIENT = "KeyFilePath";
static constexpr const char* PROPERTY_WSS_CA_CERT_FILE_PATH_CLIENT = "CaCertificateFilePath";


static constexpr bool DEFAULT_ENABLE_WS_STREAMING_PORT = true;
static constexpr bool DEFAULT_ENABLE_WS_CONTROL_PORT = true;
static constexpr bool DEFAULT_ENABLE_WSS_STREAMING_PORT = false;
static constexpr bool DEFAULT_VERIFY_SERVER_CERT = true;
static constexpr bool DEFAULT_ENABLE_MTLS = true;
static constexpr std::uint16_t DEFAULT_WS_STREAMING_PORT = 7414;
static constexpr std::uint16_t DEFAULT_WS_CONTROL_PORT = 7438;
static constexpr std::uint16_t DEFAULT_WSS_STREAMING_PORT = 7415;
static constexpr const char* DEFAULT_WSS_CERT_FILE_PATH = "";
static constexpr const char* DEFAULT_WSS_KEY_FILE_PATH = "";
static constexpr const char* DEFAULT_WSS_CA_CERT_FILE_PATH = "";


END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING

