set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.1
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             bb9ad72a69a8d5e6572bffa5fff3e7f1035d785b
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
