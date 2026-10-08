set(WS_STREAMING_INSTALL OFF)

set(WS_STREAMING_ENABLE_TLS ${${REPO_OPTION_PREFIX}_ENABLE_TLS})

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.2.0
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             999e27684beb15f7e900571d9b2f72fa76680b15
    GIT_SHALLOW         ON
    EXPECT_TARGET       ws-streaming::ws-streaming
)
