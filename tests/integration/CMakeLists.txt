set(TEST_APP test_lt_streaming_tls_integration)

set(TEST_SOURCES test_lt_streaming_tls.cpp
                 test_app.cpp
)

add_executable(${TEST_APP} ${TEST_SOURCES}
)

target_link_libraries(${TEST_APP} PRIVATE daq::opendaq_test_utils gtest
                                          daq::opendaq
                                          daq::opendaq_mocks
                                          daq::websocket_streaming
)

add_dependencies(${TEST_APP} daq::ws_stream_cl_module daq::ws_stream_srv_module)

# Copy the pre-generated TLS fixtures next to the test binary
add_custom_command(TARGET ${TEST_APP} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/secrets
            $<TARGET_FILE_DIR:${TEST_APP}>/secrets)

add_test(NAME ${TEST_APP}
         COMMAND $<TARGET_FILE_NAME:${TEST_APP}>
         WORKING_DIRECTORY $<TARGET_FILE_DIR:${TEST_APP}>)

if (COMMAND setup_target_for_coverage AND OPENDAQ_ENABLE_COVERAGE)
    setup_target_for_coverage(${TEST_APP}coverage ${TEST_APP} ${TEST_APP}coverage)
endif()