#include "test_websocket_streaming_client_module.h"

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
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreaming"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreaming").getId(), "OpenDAQLTStreaming");
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreamingOld"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreamingOld").getId(), "OpenDAQLTStreamingOld");
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(deviceTypes.getCount(), 3u);
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreamingSecure"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreamingSecure").getId(), "OpenDAQLTStreamingSecure");
#else
    ASSERT_EQ(deviceTypes.getCount(), 2u);
    ASSERT_FALSE(deviceTypes.hasKey("OpenDAQLTStreamingSecure"));
#endif

    DictPtr<IString, IServerType> serverTypes;
    ASSERT_NO_THROW(serverTypes = module.getAvailableServerTypes());
    ASSERT_EQ(serverTypes.getCount(), 0u);
}

TEST_F(WebsocketStreamingClientModuleTest, GetAvailableStreamingTypes)
{
    const auto module = CreateModule();

    DictPtr<IString, IStreamingType> streamingTypes;
    ASSERT_NO_THROW(streamingTypes = module.getAvailableStreamingTypes());

    ASSERT_TRUE(streamingTypes.hasKey("OpenDAQLTStreaming"));
    ASSERT_EQ(streamingTypes.get("OpenDAQLTStreaming").getId(), "OpenDAQLTStreaming");
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(streamingTypes.getCount(), 2u);
    ASSERT_TRUE(streamingTypes.hasKey("OpenDAQLTStreamingSecure"));
    ASSERT_EQ(streamingTypes.get("OpenDAQLTStreamingSecure").getId(), "OpenDAQLTStreamingSecure");
#else
    ASSERT_EQ(streamingTypes.getCount(), 1u);
    ASSERT_FALSE(streamingTypes.hasKey("OpenDAQLTStreamingSecure"));
#endif
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
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_TRUE(acceptsConnectionParameters(module, "daq.lts://h", nullptr));
#else
    ASSERT_FALSE(acceptsConnectionParameters(module, "daq.lts://h", nullptr));
#endif
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
    ASSERT_TRUE(acceptsStreamingConnectionParameters(module, "daq.lt://h", nullptr));
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_TRUE(acceptsStreamingConnectionParameters(module, "daq.lts://h", nullptr));
#else
    ASSERT_FALSE(acceptsStreamingConnectionParameters(module, "daq.lts://h", nullptr));
#endif
}

TEST_F(WebsocketStreamingClientModuleTest, InsecureStreamingCompletesNullConfig)
{
    // The plain channel completes a missing configuration too, and so fails on the unreachable
    // peer rather than on the configuration.
    ASSERT_THROW((createWithImplementation<IStreaming, WsStreaming>(
                      String("daq.lt://127.0.0.1:1/"), NullContext(), nullptr)),
                 NotFoundException);
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

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceLtService)
{
    const auto info = populateDiscoveredDevice(makeDiscoveredDevice(CONST_LT_SERVICE_NAME, 7425));
    const auto cap = firstCapability(info);

    ASSERT_EQ(cap.getProtocolId(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getProtocolName(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getPrefix(), CONST_LT_STREAMING_PREFIX);
    ASSERT_EQ(cap.getProtocolGroupId(), CONST_LT_PROTOCOL_GROUP_ID);
    ASSERT_EQ(cap.getProtocolType(), ProtocolType::Streaming);
    ASSERT_EQ(cap.getConnectionType(), "TCP/IP");
    ASSERT_EQ(cap.getPort(), 7425);
    ASSERT_EQ(info.getDeviceType().getId(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getProtocolSecurityLevel(), CONST_LT_STREAMING_SECURITY_LVL);
}

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceLegacyWsService)
{
    const auto info = populateDiscoveredDevice(makeDiscoveredDevice(CONST_WS_SERVICE_NAME, 7425));
    const auto cap = firstCapability(info);

    ASSERT_EQ(cap.getProtocolId(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getProtocolName(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getPrefix(), CONST_LT_STREAMING_PREFIX);
    ASSERT_EQ(cap.getProtocolGroupId(), CONST_LT_PROTOCOL_GROUP_ID);
    ASSERT_EQ(cap.getProtocolType(), ProtocolType::Streaming);
    ASSERT_EQ(cap.getConnectionType(), "TCP/IP");
    ASSERT_EQ(cap.getPort(), 7425);
    ASSERT_EQ(info.getDeviceType().getId(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getProtocolSecurityLevel(), CONST_LT_STREAMING_SECURITY_LVL);
}

TEST_F(WebsocketStreamingClientModuleTest, CompleteServerCapabilityDefaultPortPerScheme)
{
    auto module = CreateModule();

    auto target = ServerCapability(CONST_LT_STREAMING_ID, CONST_LT_STREAMING_ID, ProtocolType::Streaming);
    target.setPrefix(CONST_LT_STREAMING_PREFIX);
    ASSERT_TRUE(completeServerCapability(module, makeSourceCapability(), target));
    ASSERT_EQ(target.getPort(), DEFAULT_WS_STREAMING_PORT);

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    auto secureTarget = ServerCapability(CONST_LTS_STREAMING_ID, CONST_LTS_STREAMING_ID, ProtocolType::Streaming);
    secureTarget.setPrefix(CONST_LTS_STREAMING_PREFIX);
    ASSERT_TRUE(completeServerCapability(module, makeSourceCapability(), secureTarget));
    ASSERT_EQ(secureTarget.getPort(), DEFAULT_WSS_STREAMING_PORT);
#endif
}

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceUnknownServiceThrows)
{
    ASSERT_THROW(populateDiscoveredDevice(makeDiscoveredDevice("_streaming-xx._tcp.local.")), InvalidParameterException);
}

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceConnectionStrings)
{
    const auto info = populateDiscoveredDevice(makeDiscoveredDevice(CONST_LT_SERVICE_NAME,
                                                                    7425,
                                                                    {"192.168.1.10", "192.168.1.11"},
                                                                    {"[fe80::1]"},
                                                                    {{"path", "/foo"}}));
    const auto cap = firstCapability(info);

    const auto connectionStrings = cap.getConnectionStrings();
    ASSERT_EQ(connectionStrings.getCount(), 3u);
    ASSERT_EQ(cap.getAddresses().getCount(), 3u);

    std::vector<std::string> actual;
    for (const auto& connectionString : connectionStrings)
        actual.push_back(connectionString.toStdString());
    std::sort(actual.begin(), actual.end());

    const std::vector<std::string> expected{"daq.lt://192.168.1.10:7425/foo",
                                            "daq.lt://192.168.1.11:7425/foo",
                                            "daq.lt://[fe80::1]:7425/foo"};
    ASSERT_EQ(actual, expected);

    for (const auto& addressInfo : cap.getAddressInfo())
    {
        ASSERT_EQ(addressInfo.getReachabilityStatus(), AddressReachabilityStatus::Unknown);
        const std::string address = addressInfo.getAddress().toStdString();
        ASSERT_EQ(addressInfo.getType(), address.front() == '[' ? "IPv6" : "IPv4");
    }
}

TEST_F(WebsocketStreamingClientModuleTest, PopulateDiscoveredDeviceDefaultsPath)
{
    const auto cap = firstCapability(populateDiscoveredDevice(makeDiscoveredDevice(CONST_LT_SERVICE_NAME, 7425)));
    ASSERT_EQ(cap.getConnectionStrings()[0], "daq.lt://192.168.1.10:7425/");
}

TEST_F(WebsocketStreamingClientModuleTest, CreateUrlConnectionStringSecure)
{
    ASSERT_EQ(createUrlConnectionString(true, "192.168.1.10", 7435, "/"), "daq.lts://192.168.1.10:7435/");
    ASSERT_EQ(createUrlConnectionString(false, "192.168.1.10", 7425, "/"), "daq.lt://192.168.1.10:7425/");
    ASSERT_EQ(createUrlConnectionString(true, "192.168.1.10", 7435, ""), "daq.lts://192.168.1.10:7435");
}

TEST_F(WebsocketStreamingClientModuleTest, CreateUrlConnectionStringIpv6)
{
    ASSERT_EQ(createUrlConnectionString(false, "[::1]", 7425, "/"), "daq.lt://[::1]:7425/");
    ASSERT_EQ(createUrlConnectionString(true, "[::1]", 7435, "/"), "daq.lts://[::1]:7435/");
}

TEST_F(WebsocketStreamingClientModuleTest, CompleteServerCapabilityAcceptsBothIds)
{
    auto module = CreateModule();

    {
        auto target = ServerCapability(CONST_LT_STREAMING_ID, CONST_LT_STREAMING_ID, ProtocolType::Streaming);
        ASSERT_TRUE(completeServerCapability(module, makeSourceCapability(), target));
    }

    {
        auto target = ServerCapability(CONST_LTS_STREAMING_ID, CONST_LTS_STREAMING_ID, ProtocolType::Streaming);
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
        ASSERT_TRUE(completeServerCapability(module, makeSourceCapability(), target));
#else
        ASSERT_FALSE(completeServerCapability(module, makeSourceCapability(), target));
#endif
    }

    auto foreign = ServerCapability("OpenDAQOPCUA", "OpenDAQOPCUA", ProtocolType::Configuration);
    ASSERT_FALSE(completeServerCapability(module, makeSourceCapability(), foreign));
}

TEST_F(WebsocketStreamingClientModuleTest, ConstantsMatchPublicIds)
{
    ASSERT_EQ(WsStreaming::createType().getId(), CONST_LT_STREAMING_ID);
    ASSERT_EQ(WsStreaming::createType().getConnectionStringPrefix(), CONST_LT_STREAMING_PREFIX);

    const auto deviceTypes = CreateModule().getAvailableDeviceTypes();
    ASSERT_EQ(deviceTypes.get(CONST_LT_STREAMING_ID).getConnectionStringPrefix(), CONST_LT_STREAMING_PREFIX);

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(WsStreaming::createSecureType().getId(), CONST_LTS_STREAMING_ID);
    ASSERT_EQ(WsStreaming::createSecureType().getConnectionStringPrefix(), CONST_LTS_STREAMING_PREFIX);
    ASSERT_EQ(deviceTypes.get(CONST_LTS_STREAMING_ID).getConnectionStringPrefix(), CONST_LTS_STREAMING_PREFIX);
#endif
}
