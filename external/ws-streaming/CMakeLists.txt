set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.1
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             d9635cc75a98f5ed3219e8526038037970dac45c
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
