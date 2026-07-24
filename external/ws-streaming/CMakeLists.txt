set(WS_STREAMING_INSTALL OFF)

opendaq_dependency(
    NAME                ws-streaming
    REQUIRED_VERSION    3.0.7
    GIT_REPOSITORY      https://github.com/openDAQ/ws-streaming
    GIT_REF             6dcb8666f0f9b291bd790fbef85de3e1e13a31ef
    EXPECT_TARGET       ws-streaming::ws-streaming
    PATCH_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/patches/0001-Fixes-plus.patch
)
