file(GLOB_RECURSE AGENT_HEADERS "*.hpp")
file(GLOB_RECURSE SERVER_HEADERS "${CMAKE_SOURCE_DIR}/server/*.hpp")

add_executable(iot-edgenode
    main.cpp
    ${AGENT_HEADERS}
    ${SERVER_HEADERS}
)

set_source_files_properties(${AGENT_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(${SERVER_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    target_compile_options(iot-edgenode PRIVATE -Wno-psabi)
endif()

target_include_directories(iot-edgenode PRIVATE
    "${CMAKE_SOURCE_DIR}/server"
)

target_precompile_headers(iot-edgenode PRIVATE
    "${CMAKE_SOURCE_DIR}/server/pch.hpp"
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(iot-edgenode PRIVATE Drogon::Drogon mimalloc-static unofficial::sqlite3::sqlite3)

set_target_properties(iot-edgenode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
)

