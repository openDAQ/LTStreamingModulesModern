set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.0
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             07b0adb5bd623e0bb85db2873c7ef3ee34241768
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
