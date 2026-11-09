set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.0
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             ade9ffbd1a54fbfe4bd21769d64332c26b6abcf5
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
