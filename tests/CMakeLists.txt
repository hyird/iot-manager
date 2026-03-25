cmake_minimum_required(VERSION 3.20)

# Standalone S7-200 connection test.
# Default target connects to 192.168.1.12 using TSAP mode.

add_executable(iot_s7_test
    s7_test.cpp
)

target_include_directories(iot_s7_test PRIVATE
    ${PROJECT_SOURCE_DIR}/server
)

target_link_libraries(iot_s7_test PRIVATE
    snap7::snap7
)

set_target_properties(iot_s7_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>/tests"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>/tests"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>/tests"
)
