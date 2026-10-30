set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.0
    GIT_REPOSITORY      https://github.com/AfonsoCoxito/ws-streaming
    GIT_REF             91ab8240cb2f972ff0e6f8c9b00f7349b1d2f0e8
    GIT_SHALLOW         OFF
    EXPECT_TARGET       ws-streaming::ws-streaming
)
