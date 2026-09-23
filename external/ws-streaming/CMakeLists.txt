set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.1
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             ea0055263ac9760a2270a7ee6a19413f5a1cde54
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
