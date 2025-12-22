opendaq_set_cmake_folder_context(TARGET_FOLDER_NAME)

if (MSVC)
    add_compile_options(/wd4100)
endif()

if (${REPO_OPTION_PREFIX}_ENABLE_CLIENT)
    add_subdirectory(websocket_streaming_client_module)
endif()

if (${REPO_OPTION_PREFIX}_ENABLE_SERVER)
    add_subdirectory(websocket_streaming_server_module)
endif()

