#include "test_websocket_streaming_server_module.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <opendaq/data_descriptor_factory.h>
#include <opendaq/device_impl.h>
#include <opendaq/device_info_factory.h>
#include <opendaq/signal_impl.h>

#include <ws-streaming/client.hpp>
#include <ws-streaming/connection.hpp>
#include <ws-streaming/local_signal.hpp>
#include <ws-streaming/metadata_builder.hpp>

using namespace std::chrono_literals;

namespace
{

    // A signal that, once armed, holds the next getDescriptor() call until released: it stops a scan of the tree
    // midway, without holding any lock.
    class GateSignal : public SignalImpl
    {
    public:
        GateSignal(const ContextPtr& context,
                   const DataDescriptorPtr& descriptor,
                   const ComponentPtr& parent,
                   const StringPtr& localId)
            : SignalImpl(context, descriptor, parent, localId)
        {
        }

        std::future<void> arm(std::future<void> releasedFuture)
        {
            released = std::move(releasedFuture);
            entered = std::promise<void>{};
            auto enteredFuture = entered.get_future();
            armed = true;
            return enteredFuture;
        }

        ErrCode INTERFACE_FUNC getDescriptor(IDataDescriptor** descriptor) override
        {
            if (armed.exchange(false))
            {
                entered.set_value();
                released.wait();
            }

            return SignalImpl::getDescriptor(descriptor);
        }

    private:
        std::atomic<bool> armed{false};
        std::promise<void> entered;
        std::future<void> released;
    };

    // A signal that fails on request: when its descriptor is read on a given thread, or when a listener connects.
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
            if (std::this_thread::get_id() == descriptorFailsOn)
            {
                ++failures;
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_GENERALERROR, "The descriptor cannot be read");
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
        std::atomic<bool> listenerConnectedFails{false};
        std::atomic<int> failures{0};
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

        SignalConfigPtr addGateSignal(const std::string& localId)
        {
            SignalConfigPtr signal = createWithImplementation<ISignalConfig, GateSignal>(
                context, DataDescriptorBuilder().setSampleType(SampleType::Float64).build(), signals, localId);
            Device::addSignal(signal);
            return signal;
        }

        SignalConfigPtr addFaultySignal(const std::string& localId, std::thread::id descriptorFailsOn = {})
        {
            SignalConfigPtr signal = createWithImplementation<ISignalConfig, FaultySignal>(
                context, DataDescriptorBuilder().setSampleType(SampleType::Float64).build(), signals, localId);
            static_cast<FaultySignal&>(*signal.getObject()).descriptorFailsOn = descriptorFailsOn;
            Device::addSignal(signal);
            return signal;
        }

    protected:
        DeviceInfoPtr onGetInfo() override
        {
            return DeviceInfo("daqtest://dev");
        }
    };

    // Connects to the LT server and records the ids of the signals it announces.
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
                                 });
            thread = std::thread{[this] { ioc.run(); }};
        }

        ~AvailableSignalsClient()
        {
            boost::asio::post(ioc,
                              [this]
                              {
                                  onAvailable.disconnect();
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
        std::mutex mutex;
        std::condition_variable changed;
        std::map<std::string, std::size_t> announcements;
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
    ASSERT_EQ(cap.getProtocolGroupId(), CONST_LT_PROTOCOL_GROUP_ID);
    ASSERT_EQ(cap.getProtocolSecurityLevel(), CONST_LT_STREAMING_SECURITY_LVL);

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

// The ws-streaming server is not thread-safe: a signal added on another thread is registered on its I/O thread
TEST_F(WsStreamingServerModuleTest, SignalAddedOnAnotherThreadIsAnnouncedFromTheServerThread)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();
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

// The scan no longer builds metadata: a domain signal without a descriptor is skipped instead of throwing
TEST_F(WsStreamingServerModuleTest, DomainSignalWithoutDescriptorDoesNotHideItsValueSignal)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

    const auto domain = deviceImpl.addSignal("domain", nullptr);
    const auto value = deviceImpl.addSignal("value", DataDescriptorBuilder().setSampleType(SampleType::Float64).build());
    value.setDomainSignal(domain);

    ServerPtr server;
    ASSERT_NO_THROW(
        server = (createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7663), context)));

    AvailableSignalsClient client{"ws://127.0.0.1:7663/"};
    EXPECT_TRUE(client.waitAvailable(value.getGlobalId().toStdString()));
    // a domain signal is registered, and so announced, before the signals that use it
    EXPECT_FALSE(client.isAvailable(domain.getGlobalId().toStdString()));

    server.stop();
}

// The I/O thread reads the descriptor when it registers a signal: one cleared after the scan is not announced
TEST_F(WsStreamingServerModuleTest, SignalWhoseDescriptorIsClearedBeforeRegistrationIsNotAnnounced)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7664), context);
    auto& wsServer = static_cast<WsStreamingServer&>(*server.getObject()).getWsServer();
    AvailableSignalsClient client{"ws://127.0.0.1:7664/"};

    std::promise<void> entered;
    std::promise<void> release;
    boost::asio::post(wsServer.executor(),
        [&entered, released = release.get_future()]
        {
            entered.set_value();
            released.wait();
        });
    entered.get_future().wait();

    // the scan sees the descriptor; it is gone by the time the I/O thread registers the signal
    const auto cleared = deviceImpl.addSignal("cleared", DataDescriptorBuilder().setSampleType(SampleType::Float64).build());
    cleared.setDescriptor(nullptr);
    release.set_value();

    // signals are registered in order, so the cleared one would be announced before the marker
    const auto markerId = deviceImpl.addTestSignal("marker");
    EXPECT_TRUE(client.waitAvailable(markerId));
    EXPECT_FALSE(client.isAvailable(cleared.getGlobalId().toStdString()));

    server.stop();
}

// Scans from different threads can reach the I/O thread out of order: an older one must not undo a newer one.
// One thread's scan reads the value signal's old domain and is held at the gate signal; meanwhile this thread
// moves the value to a new domain and queues its own scan first. The older scan is then queued last. Each time
// the value is re-registered, the client sees it announced again, so the test counts the announcements.
TEST_F(WsStreamingServerModuleTest, OlderScanQueuedLastDoesNotRestoreTheOldDomain)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

    const auto descriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).build();
    const auto oldDomain = deviceImpl.addSignal("oldDomain", descriptor);
    const auto newDomain = deviceImpl.addSignal("newDomain", descriptor);
    const auto value = deviceImpl.addSignal("value", descriptor);
    value.setDomainSignal(oldDomain);
    // listed after the value signal: a scan held here has already read the value's domain
    const auto gate = deviceImpl.addGateSignal("gate");
    auto& gateImpl = static_cast<GateSignal&>(*gate.getObject());

    // scan 1, from the constructor: the value is announced for the first time, with the old domain
    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7665), context);
    const auto valueId = value.getGlobalId().toStdString();
    AvailableSignalsClient client{"ws://127.0.0.1:7665/"};
    ASSERT_TRUE(client.waitAvailable(valueId));
    // the I/O thread reads the gate's descriptor when it registers it; that must be over before the gate is armed
    ASSERT_TRUE(client.waitAvailable(gate.getGlobalId().toStdString()));

    // scan 2, on another thread: adding a signal starts a rescan that records the value with the old domain and
    // stops at the gate, before it is queued
    std::promise<void> release;
    auto held = gateImpl.arm(release.get_future());
    std::thread olderScan{[&] { deviceImpl.addTestSignal("trigger"); }};
    const bool olderScanHeld = held.wait_for(5s) == std::future_status::ready;

    // scan 3, on this thread: the domain change starts a rescan that records the new domain and is queued first;
    // the gate is no longer armed, so it does not stop. The value is re-registered: second announcement
    if (olderScanHeld)
        value.setDomainSignal(newDomain);

    // scan 2 finishes and is queued last. It must be dropped; applying it would re-register the value with the
    // old domain (third announcement). Released and joined even if the gate was never reached: a joinable
    // std::thread terminates the process when destroyed
    release.set_value();
    olderScan.join();
    ASSERT_TRUE(olderScanHeld) << "the older scan never reached the gate signal";

    // scan 4, queued after both. The client receives messages in order, so once the marker is announced every
    // earlier scan has been applied. Had scan 2 restored the old domain, this scan would find the new one again
    // and re-register the value once more (fourth announcement)
    const auto markerId = deviceImpl.addTestSignal("marker");
    ASSERT_TRUE(client.waitAvailable(markerId));

    // the first announcement and the one for the domain change
    EXPECT_EQ(client.announcementCount(valueId), 2u)
        << "announced again after the domain change: the older scan re-registered the signal with the old domain "
           "and the next scan re-registered it once more";

    server.stop();
}

// A signal the I/O thread fails to register is skipped: the signals after it are still registered, and the
// exception does not escape the I/O thread, which would terminate the process.
TEST_F(WsStreamingServerModuleTest, SignalThatFailsToRegisterDoesNotStopTheOthers)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7666), context);
    auto& wsServer = static_cast<WsStreamingServer&>(*server.getObject()).getWsServer();
    AvailableSignalsClient client{"ws://127.0.0.1:7666/"};

    std::promise<std::thread::id> ioThread;
    boost::asio::post(wsServer.executor(), [&ioThread] { ioThread.set_value(std::this_thread::get_id()); });

    // the descriptor can be read during the scan on this thread, but not when the I/O thread registers the signal
    const auto faulty = deviceImpl.addFaultySignal("faulty", ioThread.get_future().get());
    // the next scan lists the healthy signal after the faulty one, so both are registered in the same pass
    const auto healthyId = deviceImpl.addTestSignal("healthy");

    EXPECT_TRUE(client.waitAvailable(healthyId));
    // signals are registered in order, so the faulty one would have been announced before the healthy one
    EXPECT_FALSE(client.isAvailable(faulty.getGlobalId().toStdString()));
    EXPECT_GE(static_cast<FaultySignal&>(*faulty.getObject()).failures.load(), 1)
        << "the I/O thread never failed to read the descriptor, so nothing was tested";

    server.stop();
}

// A listener that cannot connect to its signal leaves that subscription without data, but the exception does not
// escape the I/O thread, which would terminate the process; the server keeps serving the client.
TEST_F(WsStreamingServerModuleTest, SubscriptionWhoseListenerFailsToConnectDoesNotStopTheServer)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

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

// A signal is published only with a descriptor: one added without it is announced once the descriptor is set
TEST_F(WsStreamingServerModuleTest, SignalIsAnnouncedOnceItGetsADescriptor)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

    auto server = createWithImplementation<IServer, WsStreamingServer>(device, CreateWsOnlyConfig(module, 7668), context);
    AvailableSignalsClient client{"ws://127.0.0.1:7668/"};

    const auto late = deviceImpl.addSignal("late", nullptr);
    const auto lateId = late.getGlobalId().toStdString();
    // signals are registered in order, so the late one would be announced before the marker
    const auto markerId = deviceImpl.addTestSignal("marker");
    ASSERT_TRUE(client.waitAvailable(markerId));
    ASSERT_FALSE(client.isAvailable(lateId)) << "the signal was published without a descriptor";

    // the only change: no component is added or removed, no attribute changes
    late.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());
    EXPECT_TRUE(client.waitAvailable(lateId));

    server.stop();
}

// Clients link a signal to its domain signal when it is announced. A value signal published before its domain signal
// is announced again once the domain signal is published, so that clients can link the two.
TEST_F(WsStreamingServerModuleTest, ValueSignalIsAnnouncedAgainOnceItsDomainSignalIsPublished)
{
    const auto context = NullContext();
    const auto module = CreateModule(context);
    auto device = createWithImplementation<IDevice, SignalAddingDevice>(context);
    auto& deviceImpl = static_cast<SignalAddingDevice&>(*device.getObject());
    device.asPtr<IPropertyObjectInternal>().enableCoreEventTrigger();

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
