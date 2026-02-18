cmake_minimum_required(VERSION 3.10)
opendaq_set_cmake_folder_context(TARGET_FOLDER_NAME ${OPENDAQ_SDK_TARGET_NAMESPACE}_websocket_streaming)
project(OpenDaqStreaming
    VERSION   4.0.0
    LANGUAGES CXX
)

add_subdirectory(src)
