set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.0
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             c1e274c40e6a47f706c8a596133887910c01f749
    GIT_SHALLOW         ON
    EXPECT_TARGET       ws-streaming::ws-streaming
)
