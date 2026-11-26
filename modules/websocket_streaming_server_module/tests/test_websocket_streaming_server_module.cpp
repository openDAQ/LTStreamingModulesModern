#include "test_websocket_streaming_server_module.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <opendaq/core_opendaq_event_args_factory.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/device_impl.h>
#include <opendaq/device_info_factory.h>
#include <opendaq/folder_factory.h>
#include <opendaq/signal_factory.h>
#include <opendaq/signal_impl.h>

#include <ws-streaming/client.hpp>
#include <ws-streaming/connection.hpp>
#include <ws-streaming/local_signal.hpp>
#include <ws-streaming/metadata_builder.hpp>

using namespace std::chrono_literals;

namespace
{

    // A signal that fails on request: when its descriptor is read on a given thread (with an error, or as missing), or
    // when a listener connects.
    class FaultySignal : public SignalImpl
    {
    public:
        FaultySignal(const ContextPtr& context,
                     const DataDescriptorPtr& descriptor,
                     const ComponentPtr& parent,
                     const StringPtr& localId)
            : SignalImpl(context, descriptor, parent, localId)
        {
        }

        ErrCode INTERFACE_FUNC getDescriptor(IDataDescriptor** descriptor) override
        {
            OPENDAQ_PARAM_NOT_NULL(descriptor);
            if (std::this_thread::get_id() == descriptorFailsOn)
            {
                ++failures;
                if (!descriptorMissing)
                    return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_GENERALERROR, "The descriptor cannot be read");

                *descriptor = nullptr;
                return OPENDAQ_SUCCESS;
            }

            return SignalImpl::getDescriptor(descriptor);
        }

        ErrCode INTERFACE_FUNC listenerConnected(IConnection* connection) override
        {
            if (listenerConnectedFails)
            {
                ++failures;
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_GENERALERROR, "The listener cannot be connected");
            }

            return SignalImpl::listenerConnected(connection);
        }

        std::atomic<std::thread::id> descriptorFailsOn{};
        std::atomic<bool> descriptorMissing{false};
        std::atomic<bool> listenerConnectedFails{false};
        std::atomic<int> failures{0};
    };

    // A signal that calls back the first time its descriptor is read: it lets a test change the tree in the middle of a scan.
    class HookSignal : public SignalImpl
    {
    public:
        using SignalImpl::SignalImpl;

        ErrCode INTERFACE_FUNC getDescriptor(IDataDescriptor** descriptor) override
        {
            if (onFirstRead && !read.exchange(true))
                onFirstRead();
            return SignalImpl::getDescriptor(descriptor);
        }

        std::function<void()> onFirstRead;

    private:
        std::atomic<bool> read{false};
    };

    // A root device whose signals the test adds from its own thread.
    class SignalAddingDevice : public Device
    {
    public:
        explicit SignalAddingDevice(const ContextPtr& context)
            : Device(context, nullptr, "dev")
        {
        }

        std::string addTestSignal(const std::string& localId)
        {
            return createAndAddSignal(localId, DataDescriptorBuilder().setSampleType(SampleType::Float64).build())
            .getGlobalId()
                .toStdString();
        }

        SignalConfigPtr addSignal(const std::string& localId, const DataDescriptorPtr& descriptor)
        {
            return createAndAddSignal(localId, descriptor);
        }

        // A folder of the device that takes any component
        FolderConfigPtr addTestFolder(const std::string& localId)
        {
            return this->addFolder(localId);
        }

        SignalConfigPtr addSignalTo(const FolderConfigPtr& folder, const std::string& localId)
        {
            const auto descriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).build();
            const auto signal = SignalWithDescriptor(context, descriptor, folder, localId);
            folder.addItem(signal);
            return signal;
        }

        // Reports a core event the way a component does
        void reportCoreEvent(const ComponentPtr& component, const CoreEventArgsPtr& eventArgs)
        {
            EventPtr<ComponentPtr, CoreEventArgsPtr> event;
            checkErrorInfo(context->getOnCoreEvent(&event));

            ComponentPtr sender = component;
            CoreEventArgsPtr args = eventArgs;
            event.trigger(sender, args);
        }

        SignalConfigPtr addHookSignal(const std::string& localId, std::function<void()> onFirstRead)
        {
            SignalConfigPtr signal = createWithImplementation<ISignalConfig, HookSignal>(
                context, DataDescriptorBuilder().setSampleType(SampleType::Float64).build(), signals, localId);
            static_cast<HookSignal&>(*signal.getObject()).onFirstRead = std::move(onFirstRead);
            Device::addSignal(signal);
            return signal;
        }

        SignalConfigPtr addFaultySignal(const std::string& localId, std::thread::id descriptorFailsOn = {}, bool descriptorMissing = false)
        {
            return addFaultySignalTo(signals, localId, descriptorFailsOn, descriptorMissing);
        }

        // Set up before it is added, as a scan may read it at once
        SignalConfigPtr addFaultySignalTo(const FolderConfigPtr& folder,
                                          const std::string& localId,
                                          std::thread::id descriptorFailsOn = {},
                                          bool descriptorMissing = false)
        {
            SignalConfigPtr signal = createWithImplementation<ISignalConfig, FaultySignal>(
                context, DataDescriptorBuilder().setSampleType(SampleType::Float64).build(), folder, localId);
            auto& faulty = static_cast<FaultySignal&>(*signal.getObject());
            faulty.descriptorFailsOn = descriptorFailsOn;
            faulty.descriptorMissing = descriptorMissing;
            folder.addItem(signal);
            return signal;
        }

    protected:
        DeviceInfoPtr onGetInfo() override
        {
            return DeviceInfo("daqtest://dev");
        }
    };

    // Ends the process when a test deadlocks, as its threads can then not be joined.
    class Watchdog
    {
    public:
        explicit Watchdog(const char* what)
            : thread([this, what]
              {
                  if (finished.get_future().wait_for(20s) == std::future_status::ready)
                      return;

                  std::cerr << "[ DEADLOCK ] " << what << std::endl;
                  std::_Exit(3);
              })
        {
        }

        ~Watchdog()
        {
            finished.set_value();
            thread.join();
        }

    private:
        std::promise<void> finished;
        std::thread thread;
    };

    // Connects to the LT server and records the ids of the signals it announces and withdraws.
    class AvailableSignalsClient
    {
    public:
        explicit AvailableSignalsClient(const std::string& url)
        {
            client.async_connect(url,
                                 [this](const boost::system::error_code& ec, wss::connection_ptr c)
                                 {
                                     if (ec)
                                         return;
                                     connection = c;
                                     onAvailable = c->on_available.connect(
                                         [this](wss::remote_signal_ptr signal)
                                         {
                                             remoteSignals[signal->id()] = signal;
                                             signal->on_subscribed.connect(
                                                 [this, id = signal->id()]
                                                 {
                                                     std::scoped_lock lock(this->mutex);
                                                     subscribed.insert(id);
                                                     changed.notify_all();
                                                 });

                                             std::scoped_lock lock(this->mutex);
                                             ++announcements[signal->id()];
                                             changed.notify_all();
                                         });
                                     onUnavailable = c->on_unavailable.connect(
                                         [this](wss::remote_signal_ptr signal)
                                         {
                                             std::scoped_lock lock(this->mutex);
                                             ++withdrawals[signal->id()];
                                             changed.notify_all();
                                         });
                                 });
            thread = std::thread{[this] { ioc.run(); }};
        }

        ~AvailableSignalsClient()
        {
            boost::asio::post(ioc,
                              [this]
                              {
                                  onAvailable.disconnect();
                                  onUnavailable.disconnect();
                                  if (connection)
                                      connection->close();
                                  ioc.stop();
                              });
            thread.join();
        }

        bool isAvailable(const std::string& id)
        {
            std::scoped_lock lock(mutex);
            return announcements.count(id) > 0;
        }

        bool waitAvailable(const std::string& id)
        {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, 5s, [&] { return announcements.count(id) > 0; });
        }

        std::size_t announcementCount(const std::string& id)
        {
            std::scoped_lock lock(mutex);
            const auto it = announcements.find(id);
            return it != announcements.end() ? it->second : 0;
        }

        bool waitWithdrawn(const std::string& id)
        {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, 5s, [&] { return withdrawals.count(id) > 0; });
        }

        std::size_t withdrawalCount(const std::string& id)
        {
            std::scoped_lock lock(mutex);
            const auto it = withdrawals.find(id);
            return it != withdrawals.end() ? it->second : 0;
        }

        // Subscribes to an announced signal; true once the server confirms, which it does after its own handling
        bool subscribe(const std::string& id)
        {
            boost::asio::post(ioc,
                              [this, id]
                              {
                                  if (const auto it = remoteSignals.find(id); it != remoteSignals.end())
                                      it->second->subscribe();
                              });

            std::unique_lock lock(mutex);
            return changed.wait_for(lock, 5s, [&] { return subscribed.count(id) > 0; });
        }

    private:
        boost::asio::io_context ioc;
        wss::client client{ioc.get_executor()};
        wss::connection_ptr connection;
        boost::signals2::scoped_connection onAvailable;
        boost::signals2::scoped_connection onUnavailable;
        std::mutex mutex;
        std::condition_variable changed;
        std::map<std::string, std::size_t> announcements;
        std::map<std::string, std::size_t> withdrawals;
        std::set<std::string> subscribed;
        // used on the client thread only
        std::map<std::string, wss::remote_signal_ptr> remoteSignals;
        std::thread thread;
    };

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

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(config.getAllProperties().getCount(), 11u);
#else
    ASSERT_EQ(config.getAllProperties().getCount(), 5u);
#endif

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

#if DAQMODULES_LT_STREAMING_ENABLE_TLS

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

#else

    ASSERT_FALSE(config.hasProperty(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER));
    ASSERT_FALSE(config.hasProperty(PROPERTY_ENABLE_MTLS_SERVER));
    ASSERT_FALSE(config.hasProperty(PROPERTY_WSS_STREAMING_PORT_SERVER));
    ASSERT_FALSE(config.hasProperty(PROPERTY_WSS_CERT_FILE_PATH_SERVER));
    ASSERT_FALSE(config.hasProperty(PROPERTY_WSS_KEY_FILE_PATH_SERVER));
    ASSERT_FALSE(config.hasProperty(PROPERTY_WSS_CA_CERT_FILE_PATH_SERVER));

#endif

    ASSERT_TRUE(config.hasProperty(PROPERTY_PATH_SERVER));
    ASSERT_EQ(config.getPropertyValue(PROPERTY_PATH_SERVER), "/");
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

#if DAQMODULES_LT_STREAMING_ENABLE_TLS

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

#endif
}

TEST_F(WsStreamingServerModuleTest, AddCapabilityWsOnly)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());
    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7650));

    const auto info = instance.getRootDevice().getInfo();
    ASSERT_TRUE(info.hasServerCapability(CONST_LT_STREAMING_ID));
    ASSERT_FALSE(info.hasServerCapability(CONST_LTS_STREAMING_ID));

    const auto cap = info.getServerCapability(CONST_LT_STREAMING_ID);
    ASSERT_EQ(cap.getPrefix(), CONST_LT_STREAMING_PREFIX);
    ASSERT_EQ(cap.getPort(), 7650);
    ASSERT_EQ(cap.getConnectionType(), "TCP/IP");

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, RemoveCapabilityOnStop)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());
    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7651));

    const auto info = instance.getRootDevice().getInfo();
    ASSERT_TRUE(info.hasServerCapability(CONST_LT_STREAMING_ID));

    server.stop();
    ASSERT_FALSE(info.hasServerCapability(CONST_LT_STREAMING_ID));
    ASSERT_FALSE(info.hasServerCapability(CONST_LTS_STREAMING_ID));

    // openDAQ may call stop() more than once and the destructor stops the server again
    ASSERT_NO_THROW(server.stop());
}

TEST_F(WsStreamingServerModuleTest, SecondServerOnSameDeviceThrows)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());
    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7652));

    ASSERT_THROW(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7653)),
                 InvalidStateException);

    ASSERT_TRUE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, ServerRestartReAddsCapability)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7654));
    server.stop();

    ASSERT_NO_THROW(server = module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), CreateWsOnlyConfig(module, 7654)));
    ASSERT_TRUE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, PartialConfigIsCompleted)
{
    const auto instance = Instance();

    auto config = PropertyObject();
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True));
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False));

    ServerPtr server;
    ASSERT_NO_THROW((server = createWithImplementation<IServer, websocket_streaming::WsStreamingServer>(
                         instance.getRootDevice(), config, instance.getContext())));

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(config.getAllProperties().getCount(), 11u);
#else
    ASSERT_EQ(config.getAllProperties().getCount(), 5u);
#endif
    ASSERT_TRUE(config.hasProperty(PROPERTY_PATH_SERVER));
    ASSERT_EQ(config.getPropertyValue(PROPERTY_PATH_SERVER), "/");
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), DEFAULT_WS_STREAMING_PORT);

    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER), False);

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, PartialConfigPreservesUserValues)
{
    const auto instance = Instance();

    auto config = PropertyObject();
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True));
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False));
    config.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_SERVER, 7655));

    auto server = createWithImplementation<IServer, websocket_streaming::WsStreamingServer>(
        instance.getRootDevice(), config, instance.getContext());

    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7655);
#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WSS_STREAMING_PORT_SERVER), DEFAULT_ENABLE_WSS_STREAMING_PORT);
#endif
    ASSERT_EQ(instance.getRootDevice().getInfo().getServerCapability(CONST_LT_STREAMING_ID).getPort(), 7655);

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, PartialConfigKeepsForeignProperties)
{
    const auto instance = Instance();

    auto config = PropertyObject();
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, True));
    config.addProperty(BoolProperty(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, False));
    config.addProperty(IntProperty(PROPERTY_WS_STREAMING_PORT_SERVER, 7656));
    config.addProperty(StringProperty("SomeForeignProperty", "keep me"));

    ServerPtr server;
    ASSERT_NO_THROW((server = createWithImplementation<IServer, websocket_streaming::WsStreamingServer>(
                         instance.getRootDevice(), config, instance.getContext())));

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(config.getAllProperties().getCount(), 12u);
#else
    ASSERT_EQ(config.getAllProperties().getCount(), 6u);
#endif
    ASSERT_EQ(config.getPropertyValue("SomeForeignProperty"), "keep me");

    server.stop();
}

TEST_F(WsStreamingServerModuleTest, CreateServerRejectsAllChannelsDisabled)
{
    const auto instance = Instance();
    auto module = CreateModule(instance.getContext());

    auto config = CreateWsOnlyConfig(module, 7657);
    config.setPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER, False);

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "Neither the websocket streaming port nor the TLS streaming port is enabled");
#else
    ASSERT_THROW_MSG(module.createServer("OpenDAQLTStreaming", instance.getRootDevice(), config),
                     InvalidParameterException,
                     "The websocket streaming port is not enabled");
#endif

    ASSERT_FALSE(instance.getRootDevice().getInfo().hasServerCapability(CONST_LT_STREAMING_ID));
}

TEST_F(WsStreamingServerModuleTest, ProviderOptionsOverrideDefaults)
{
    auto moduleOptions = Dict<IString, IBaseObject>();
    moduleOptions.set(PROPERTY_WS_STREAMING_PORT_SERVER, Integer(7660));
    moduleOptions.set(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER, Boolean(False));
    moduleOptions.set(PROPERTY_PATH_SERVER, String("/lt"));

    const auto context = CreateContextWithModuleOptions(moduleOptions);

    auto config = WsStreamingServer::createDefaultConfig(context);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7660);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_CONTROL_PORT_SERVER), False);
    ASSERT_EQ(config.getPropertyValue(PROPERTY_PATH_SERVER), "/lt");
    ASSERT_EQ(config.getPropertyValue(PROPERTY_ENABLE_WS_STREAMING_PORT_SERVER), DEFAULT_ENABLE_WS_STREAMING_PORT);

    auto module = CreateModule(context);
    auto typeConfig = module.getAvailableServerTypes().get("OpenDAQLTStreaming").createDefaultConfig();
    ASSERT_EQ(typeConfig.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7660);
    ASSERT_EQ(typeConfig.getPropertyValue(PROPERTY_PATH_SERVER), "/lt");

}

TEST_F(WsStreamingServerModuleTest, ProviderOptionsIgnoreUnknownKeys)
{
    auto moduleOptions = Dict<IString, IBaseObject>();
    moduleOptions.set("NoSuchProperty", String("ignored"));
    moduleOptions.set(PROPERTY_WS_STREAMING_PORT_SERVER, Integer(7661));

    const auto context = CreateContextWithModuleOptions(moduleOptions);

    PropertyObjectPtr config;
    ASSERT_NO_THROW(config = WsStreamingServer::createDefaultConfig(context));

#if DAQMODULES_LT_STREAMING_ENABLE_TLS
    ASSERT_EQ(config.getAllProperties().getCount(), 11u);
#else
    ASSERT_EQ(config.getAllProperties().getCount(), 5u);
#endif
    ASSERT_FALSE(config.hasProperty("NoSuchProperty"));
    ASSERT_EQ(config.getPropertyValue(PROPERTY_WS_STREAMING_PORT_SERVER), 7661);
}

// A server on a device whose tree the test changes, which reports core events as an instance does
class WsStreamingServerTreeTest : public WsStreamingServerModuleTest
{
protected:
    WsStreamingServerTreeTest()
    {
        device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();
    }

    // The I/O thread of the ws-streaming server, read from a handler it runs
    static std::thread::id ioThreadOf(const ServerPtr& server)
    {
        std::promise<std::thread::id> ioThread;
        auto id = ioThread.get_future();
        boost::asio::post(static_cast<WsStreamingServer&>(*server.getObject()).getWsServer().executor(),
                          [ioThread = std::move(ioThread)]() mutable { ioThread.set_value(std::this_thread::get_id()); });
        return id.get();
    }

    ContextPtr context = NullContext();
    ModulePtr module = CreateModule(context);
    DevicePtr device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    SignalAddingDevice& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
};

// The ws-streaming server is not thread-safe: a signal added on another thread is registered on its I/O thread
TEST_F(WsStreamingServerTreeTest, SignalAddedOnAnotherThreadIsAnnouncedFromTheServerThread)
{
    const auto initialId = deviceImpl.addTestSignal("initial");

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7662), context);
    auto& wsServer = static_cast<WsStreamingServer&>(*server.getObject()).getWsServer();

    AvailableSignalsClient client{"ws://127.0.0.1:7662/"};
    ASSERT_TRUE(client.waitAvailable(initialId));

    std::promise<void> entered;
    std::promise<void> release;
    boost::asio::post(wsServer.executor(),
        [&entered, released = release.get_future()]
        {
            entered.set_value();
            released.wait();
        });
    entered.get_future().wait();

    const auto addedId = deviceImpl.addTestSignal("added");

    // With its I/O thread blocked the server is this thread's alone: the marker goes out on the socket now,
    // after anything that adding the signal wrote from this thread
    wss::local_signal marker{"marker", wss::metadata_builder{"marker"}.build()};
    wsServer.add_local_signal(marker);
    const bool markerAnnounced = client.waitAvailable("marker");
    const bool announcedWhileServerThreadBlocked = client.isAvailable(addedId);
    wsServer.remove_local_signal(marker);
    release.set_value();

    ASSERT_TRUE(markerAnnounced) << "the marker written from the test thread never reached the client";
    EXPECT_FALSE(announcedWhileServerThreadBlocked) << "the signal was announced from the thread that added it";
    EXPECT_TRUE(client.waitAvailable(addedId));

    server.stop();
}

// The I/O thread reads the descriptor again when it registers a signal: one that is gone by then is not announced
TEST_F(WsStreamingServerTreeTest, SignalWithoutADescriptorAtRegistrationIsNotAnnounced)
{
    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7664), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7664/"};

    // the scan sees the descriptor, the I/O thread does not, as if it was cleared between the two
    const auto missing = deviceImpl.addFaultySignal("missing", ioThreadOf(server), true);

    // signals are registered in order, so the missing one would be announced before the marker
    const auto markerId = deviceImpl.addTestSignal("marker");
    EXPECT_TRUE(client.waitAvailable(markerId));
    EXPECT_FALSE(client.isAvailable(missing.getGlobalId().toStdString()));
    EXPECT_GE(static_cast<FaultySignal&>(*missing.getObject()).failures.load(), 1)
        << "the I/O thread never read the descriptor, so nothing was tested";

    server.stop();
}

// A signal the I/O thread fails to register is skipped: the signals after it are still registered, and the
// exception does not escape the I/O thread, which would terminate the process.
TEST_F(WsStreamingServerTreeTest, SignalThatFailsToRegisterDoesNotStopTheOthers)
{
    const auto tree = deviceImpl.addTestFolder("tree");
    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7666), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7666/"};

    // built before it is part of the tree, so that one event adds both signals and one pass registers them in order
    const auto group = Folder(context, tree, "group");
    // the descriptor can be read during the scan, but not when the I/O thread registers the signal
    const auto faulty = deviceImpl.addFaultySignalTo(group, "faulty", ioThreadOf(server));
    const auto healthyId = deviceImpl.addSignalTo(group, "healthy").getGlobalId().toStdString();
    tree.addItem(group);

    EXPECT_TRUE(client.waitAvailable(healthyId));
    // signals are registered in order, so the faulty one would have been announced before the healthy one
    EXPECT_FALSE(client.isAvailable(faulty.getGlobalId().toStdString()));
    EXPECT_GE(static_cast<FaultySignal&>(*faulty.getObject()).failures.load(), 1)
        << "the I/O thread never failed to read the descriptor, so nothing was tested";

    server.stop();
}

// A listener that cannot connect to its signal leaves that subscription without data, but the exception does not
// escape the I/O thread, which would terminate the process; the server keeps serving the client.
TEST_F(WsStreamingServerTreeTest, SubscriptionWhoseListenerFailsToConnectDoesNotStopTheServer)
{
    const auto faulty = deviceImpl.addFaultySignal("faulty");
    auto& faultyImpl = static_cast<FaultySignal&>(*faulty.getObject());
    faultyImpl.listenerConnectedFails = true;

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7667), context);
    const auto faultyId = faulty.getGlobalId().toStdString();
    AvailableSignalsClient client{"ws://127.0.0.1:7667/"};
    ASSERT_TRUE(client.waitAvailable(faultyId));

    // the server creates the listener when the subscription arrives and confirms it only afterwards
    EXPECT_TRUE(client.subscribe(faultyId));
    EXPECT_EQ(faultyImpl.failures.load(), 1) << "the listener never tried to connect, so nothing was tested";

    const auto markerId = deviceImpl.addTestSignal("marker");
    EXPECT_TRUE(client.waitAvailable(markerId));

    server.stop();
}

// Clients link a signal to its domain signal when it is announced. A value signal published before its domain signal
// is announced again once the domain signal is published, so that clients can link the two.
TEST_F(WsStreamingServerTreeTest, ValueSignalIsAnnouncedAgainOnceItsDomainSignalIsPublished)
{
    // without a descriptor the domain signal is not published, but the value signal is
    const auto domain = deviceImpl.addSignal("domain", nullptr);
    const auto value = deviceImpl.addSignal("value", DataDescriptorBuilder().setSampleType(SampleType::Float64).build());
    value.setDomainSignal(domain);
    const auto domainId = domain.getGlobalId().toStdString();
    const auto valueId = value.getGlobalId().toStdString();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7669), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7669/"};
    ASSERT_TRUE(client.waitAvailable(valueId));
    // a domain signal is registered, and so announced, before the signals that use it
    ASSERT_FALSE(client.isAvailable(domainId));

    // while the domain signal stays unpublished, further scans leave the value signal alone
    const auto unchangedId = deviceImpl.addTestSignal("unchanged");
    ASSERT_TRUE(client.waitAvailable(unchangedId));
    EXPECT_EQ(client.announcementCount(valueId), 1u) << "the value signal was announced again on an unrelated scan";

    domain.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Int64).build());
    ASSERT_TRUE(client.waitAvailable(domainId));

    // the value signal is announced again right after its domain signal; the marker is announced after both
    const auto markerId = deviceImpl.addTestSignal("marker");
    ASSERT_TRUE(client.waitAvailable(markerId));
    EXPECT_EQ(client.announcementCount(valueId), 2u)
        << "the value signal was not announced again, so clients keep it without its domain signal";

    server.stop();
}

// One event reports a folder that is added with its signals, and one that it is removed with them.
TEST_F(WsStreamingServerTreeTest, SignalsOfAFolderAreAnnouncedWhenItIsAddedAndWithdrawnWhenItIsRemoved)
{
    const auto tree = deviceImpl.addTestFolder("tree");

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7665), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7665/"};

    // built before it is part of the tree, so that the event of the folder is the only one
    const auto group = Folder(context, tree, "group");
    const auto nested = Folder(context, group, "nested");
    group.addItem(nested);
    const auto aId = deviceImpl.addSignalTo(group, "a").getGlobalId().toStdString();
    const auto bId = deviceImpl.addSignalTo(nested, "b").getGlobalId().toStdString();
    tree.addItem(group);

    ASSERT_TRUE(client.waitAvailable(aId));
    ASSERT_TRUE(client.waitAvailable(bId));

    tree.removeItem(group);
    ASSERT_TRUE(client.waitWithdrawn(aId));
    ASSERT_TRUE(client.waitWithdrawn(bId));

    server.stop();
}

// The events of a component are muted while it is updated; one event reports the end of the update.
TEST_F(WsStreamingServerTreeTest, UpdatedFolderKeepsItsUnchangedSignals)
{
    const auto tree = deviceImpl.addTestFolder("tree");

    const auto keptId = deviceImpl.addSignalTo(tree, "kept").getGlobalId().toStdString();
    const auto removed = deviceImpl.addSignalTo(tree, "removed");
    const auto replaced = deviceImpl.addSignalTo(tree, "replaced");
    const auto replacedId = replaced.getGlobalId().toStdString();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7670), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7670/"};
    ASSERT_TRUE(client.waitAvailable(keptId));
    ASSERT_TRUE(client.waitAvailable(replacedId));

    tree.asPtr<IPropertyObjectInternal>().disableCoreEventTrigger();
    tree.removeItem(removed);
    tree.removeItem(replaced);
    deviceImpl.addSignalTo(tree, "replaced");
    const auto addedId = deviceImpl.addSignalTo(tree, "added").getGlobalId().toStdString();
    tree.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();
    deviceImpl.reportCoreEvent(tree, CoreEventArgsComponentUpdateEnd());

    ASSERT_TRUE(client.waitAvailable(addedId));
    EXPECT_EQ(client.withdrawalCount(removed.getGlobalId().toStdString()), 1u);
    EXPECT_EQ(client.announcementCount(replacedId), 2u) << "the signal that took the place of a removed one was not announced";
    EXPECT_EQ(client.withdrawalCount(keptId), 0u) << "a signal the update left alone was withdrawn, which ends its subscriptions";

    server.stop();
}

// A signal is announced again when its domain signal changes, and only then.
TEST_F(WsStreamingServerTreeTest, SignalIsAnnouncedAgainOnlyWhenItsDomainSignalChanges)
{
    const auto descriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).build();
    const auto domain = deviceImpl.addSignal("domain", descriptor);
    const auto otherDomain = deviceImpl.addSignal("otherDomain", descriptor);
    const auto value = deviceImpl.addSignal("value", descriptor);
    value.setDomainSignal(domain);
    const auto valueId = value.getGlobalId().toStdString();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7671), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7671/"};
    ASSERT_TRUE(client.waitAvailable(valueId));

    // changes that do not affect what is published
    value.setName("renamed");
    value.setActive(false);
    value.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float32).build());
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addTestSignal("first")));
    EXPECT_EQ(client.announcementCount(valueId), 1u);

    value.setDomainSignal(otherDomain);
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addTestSignal("second")));
    EXPECT_EQ(client.announcementCount(valueId), 2u);

    server.stop();
}

// openDAQ clears a removed domain signal from the signals that use it and reports no event for them. They are announced
// again, without it: clients would otherwise keep them linked to a signal that is gone, or to a later one with its id.
TEST_F(WsStreamingServerTreeTest, ValueSignalIsAnnouncedAgainWhenItsDomainSignalIsRemoved)
{
    const auto tree = deviceImpl.addTestFolder("tree");
    const auto domain = deviceImpl.addSignalTo(tree, "domain");
    const auto value = deviceImpl.addSignalTo(tree, "value");
    value.setDomainSignal(domain);
    const auto valueId = value.getGlobalId().toStdString();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7682), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7682/"};
    ASSERT_TRUE(client.waitAvailable(valueId));

    tree.removeItem(domain);
    ASSERT_TRUE(client.waitWithdrawn(domain.getGlobalId().toStdString()));
    ASSERT_FALSE(value.getDomainSignal().assigned()) << "openDAQ did not clear the removed domain signal, so nothing was tested";

    // the value signal is announced again by the pass that withdraws its domain signal; the marker is announced after it
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addTestSignal("marker")));
    EXPECT_EQ(client.announcementCount(valueId), 2u) << "the value signal is still published with the removed domain signal";

    // a signal that takes the id of the removed one is not the domain signal of the value signal
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addSignalTo(tree, "domain").getGlobalId().toStdString()));
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addTestSignal("secondMarker")));
    EXPECT_EQ(client.announcementCount(valueId), 2u);

    server.stop();
}

// A thread holds a lock of the tree until a core event that another thread reports has returned. Reading the tree on
// the reporting thread would wait for that lock and never return.
TEST_F(WsStreamingServerTreeTest, CoreEventDoesNotWaitForALockOfTheTreeHeldByAnotherThread)
{
    const auto other = deviceImpl.addTestFolder("other");
    const auto descriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).build();
    const auto domain = deviceImpl.addSignal("domain", descriptor);
    const auto value = deviceImpl.addSignal("value", descriptor);
    const auto valueId = value.getGlobalId().toStdString();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7672), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7672/"};
    ASSERT_TRUE(client.waitAvailable(valueId));

    Watchdog watchdog{"A core event waits for a lock of the tree that another thread holds until the event is reported"};
    std::promise<void> otherLocked;
    std::promise<void> reported;
    std::thread otherThread{[&]
    {
        const auto otherLock = other.asPtr<IPropertyObjectInternal>().getRecursiveLockGuard();
        otherLocked.set_value();
        reported.get_future().wait();
    }};

    otherLocked.get_future().wait();
    value.setDomainSignal(domain);
    reported.set_value();
    otherThread.join();

    // the event was not lost: the signal is announced again, now with its domain signal
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addTestSignal("marker")));
    EXPECT_EQ(client.announcementCount(valueId), 2u);

    server.stop();
}

// Several threads change the tree at once: what the server publishes in the end is what the tree holds.
TEST_F(WsStreamingServerTreeTest, ChangesFromSeveralThreadsAreAllPublished)
{
    constexpr int threadCount = 4;
    constexpr int rounds = 100;
    std::vector<FolderConfigPtr> folders;
    for (int t = 0; t < threadCount; ++t)
        folders.push_back(deviceImpl.addTestFolder("t" + std::to_string(t)));

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7674), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7674/"};

    // every round leaves one more signal in the folder of the thread, and removes another one right after adding it
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t)
        threads.emplace_back([&, t]
        {
            for (int i = 0; i < rounds; ++i)
            {
                deviceImpl.addSignalTo(folders[t], "kept" + std::to_string(i));
                folders[t].removeItem(deviceImpl.addSignalTo(folders[t], "removed"));
            }
        });
    for (auto& thread : threads)
        thread.join();

    // changes are published in the order they were reported, so the marker is announced last
    ASSERT_TRUE(client.waitAvailable(deviceImpl.addTestSignal("marker")));

    for (const auto& folder : folders)
    {
        const auto folderId = folder.getGlobalId().toStdString();
        for (int i = 0; i < rounds; ++i)
            EXPECT_EQ(client.announcementCount(folderId + "/kept" + std::to_string(i)), 1u) << folderId << " kept" << i;

        EXPECT_EQ(client.announcementCount(folderId + "/removed"), client.withdrawalCount(folderId + "/removed"))
            << "a removed signal is still published";
    }

    server.stop();
}

// Core events keep arriving while the server stops and is destroyed.
TEST_F(WsStreamingServerTreeTest, ServerStopsWhileTheTreeChanges)
{
    const auto tree = deviceImpl.addTestFolder("tree");

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7675), context);
    Watchdog watchdog{"Stopping the server while the tree changes does not return"};

    std::atomic<bool> stop{false};
    std::thread changer{[&]
    {
        for (int i = 0; !stop; ++i)
            tree.removeItem(deviceImpl.addSignalTo(tree, "s" + std::to_string(i % 16)));
    }};

    std::this_thread::sleep_for(100ms);
    server.stop();
    server.release();

    stop = true;
    changer.join();
}

// The event that reports the server as added names the server. A scan of it that is still queued when the server
// stops would keep the server alive for good, and one that is running would destroy the server on its own scan thread.
TEST_F(WsStreamingServerTreeTest, ServerIsNotKeptAliveByTheEventOfItsOwnAddition)
{
    const auto tree = deviceImpl.addTestFolder("tree");

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7678), context);
    const WeakRefPtr<IServer> weakServer = server;
    Watchdog watchdog{"A server that was reported as added does not stop"};

    {
        const auto treeLock = tree.asPtr<IPropertyObjectInternal>().getRecursiveLockGuard();

        // a read of the tree that waits for the lock held here, and the event of the server queued behind it
        deviceImpl.reportCoreEvent(device, CoreEventArgsComponentUpdateEnd());
        deviceImpl.reportCoreEvent(device, CoreEventArgsComponentAdded(server));
        std::this_thread::sleep_for(200ms);
        server.stop();
    }

    server.release();
    ASSERT_FALSE(weakServer.getRef().assigned()) << "the server is still referenced after its last reference was released";
}

// A signal that is added while the server reads the tree for the first time is reported by an event, not found by that read.
TEST_F(WsStreamingServerTreeTest, SignalAddedWhileTheServerStartsIsAnnounced)
{
    std::string addedId;
    deviceImpl.addHookSignal("hook", [&] { addedId = deviceImpl.addTestSignal("added"); });
    ASSERT_TRUE(addedId.empty()) << "the descriptor of the hook signal was read before the server started";

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7679), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7679/"};
    ASSERT_FALSE(addedId.empty()) << "the first read of the tree did not reach the signal that adds another one";
    ASSERT_TRUE(client.waitAvailable(addedId));

    server.stop();
}
