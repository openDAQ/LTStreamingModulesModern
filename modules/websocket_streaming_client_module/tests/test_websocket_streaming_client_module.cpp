#include <testutils/testutils.h>
#include <websocket_streaming_client_module/module_dll.h>
#include <websocket_streaming_client_module/version.h>

#include <opendaq/module_ptr.h>
#include <coretypes/common.h>

#include <opendaq/context_factory.h>
#include <opendaq/device_info_factory.h>
#include <coreobjects/property_factory.h>

using WebsocketStreamingClientModuleTest = testing::Test;
using namespace daq;

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
    ASSERT_EQ(deviceTypes.getCount(), 2u);
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreaming"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreaming").getId(), "OpenDAQLTStreaming");
    ASSERT_TRUE(deviceTypes.hasKey("OpenDAQLTStreamingOld"));
    ASSERT_EQ(deviceTypes.get("OpenDAQLTStreamingOld").getId(), "OpenDAQLTStreamingOld");

    DictPtr<IString, IServerType> serverTypes;
    ASSERT_NO_THROW(serverTypes = module.getAvailableServerTypes());
    ASSERT_EQ(serverTypes.getCount(), 0u);
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
