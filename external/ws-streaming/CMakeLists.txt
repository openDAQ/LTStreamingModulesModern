set(WS_STREAMING_INSTALL OFF)

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.1.1
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             v3.1.1
    EXPECT_TARGET       ws-streaming::ws-streaming
)
