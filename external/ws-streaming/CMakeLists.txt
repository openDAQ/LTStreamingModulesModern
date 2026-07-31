set(WS_STREAMING_INSTALL OFF)

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.0.7
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             configurable-buffer-sizes
    EXPECT_TARGET       ws-streaming::ws-streaming
)
