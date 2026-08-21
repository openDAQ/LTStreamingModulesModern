#pragma once

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
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
        config.setPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER, False);
#endif
        config.setPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER, wsPort);
        return config;
    }
};

inline ModulePtr CreateModule(ContextPtr context = NullContext())
{
    ModulePtr module;
    createModule(&module, context);
    return module;
}

inline PropertyObjectPtr CreateServerConfig()
{
    auto module = CreateModule();
    auto serverTypes = module.getAvailableServerTypes();
    return serverTypes.get("OpenDAQLTStreaming").createDefaultConfig();
}
