set(WS_STREAMING_INSTALL OFF)

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.0
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             fd1ebaa7ae542a90ef3c7c06fecffecbf68b9d8d
    GIT_SHALLOW         ON
    EXPECT_TARGET       ws-streaming::ws-streaming
)
