file(GLOB_RECURSE AGENT_HEADERS "*.hpp")
file(GLOB_RECURSE SERVER_HEADERS "${CMAKE_SOURCE_DIR}/server/*.hpp")

add_executable(iot-agent
    main.cpp
    ${AGENT_HEADERS}
    ${SERVER_HEADERS}
)

set_source_files_properties(${AGENT_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(${SERVER_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    target_compile_options(iot-agent PRIVATE -Wno-psabi)
endif()

target_include_directories(iot-agent PRIVATE
    "${CMAKE_SOURCE_DIR}/server"
)

target_precompile_headers(iot-agent PRIVATE
    "${CMAKE_SOURCE_DIR}/server/pch.hpp"
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(iot-agent PRIVATE Drogon::Drogon mimalloc-static unofficial::sqlite3::sqlite3)

set_target_properties(iot-agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
)

set(AGENT_CONFIG_SOURCE "${CMAKE_SOURCE_DIR}/config/agent.example.json")
if(EXISTS "${CMAKE_SOURCE_DIR}/config/agent.local.json")
    set(AGENT_CONFIG_SOURCE "${CMAKE_SOURCE_DIR}/config/agent.local.json")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/config/agent.json")
    set(AGENT_CONFIG_SOURCE "${CMAKE_SOURCE_DIR}/config/agent.json")
endif()

add_custom_command(TARGET iot-agent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:iot-agent>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AGENT_CONFIG_SOURCE}"
        "$<TARGET_FILE_DIR:iot-agent>/config/agent.json"
    COMMENT "Copying agent config..."
)
