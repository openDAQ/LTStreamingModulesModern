set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.1
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             1fded35484ae043c54410022b0a884e0cd57564a
    GIT_SHALLOW         ON
    EXPECT_TARGET       ws-streaming::ws-streaming
)
