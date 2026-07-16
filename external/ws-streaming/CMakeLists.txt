set(WS_STREAMING_INSTALL OFF)

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.0.8
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             tls
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
