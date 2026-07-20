cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    foreach(VCPKG_ROOT_CANDIDATE IN ITEMS
        "$ENV{VCPKG_ROOT}"
        "$ENV{VCPKG_INSTALLATION_ROOT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg"
        "D:/Dev/vcpkg"
        "C:/vcpkg"
    )
        if(EXISTS "${VCPKG_ROOT_CANDIDATE}/scripts/buildsystems/vcpkg.cmake")
            set(CMAKE_TOOLCHAIN_FILE
                "${VCPKG_ROOT_CANDIDATE}/scripts/buildsystems/vcpkg.cmake"
                CACHE FILEPATH "vcpkg toolchain file"
            )
            message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
            break()
        endif()
    endforeach()
endif()

# CMake Tools may receive absolute compiler paths without adding their directory
# to PATH. vcpkg starts separate compiler-detection processes, so expose the
# selected MinGW toolchain to those child processes before project() runs.
if(CMAKE_HOST_WIN32 AND CMAKE_CXX_COMPILER)
    get_filename_component(MINGW_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(MINGW_COMPILER_DIR)
        set(ENV{PATH} "${MINGW_COMPILER_DIR};$ENV{PATH}")
    endif()
endif()

project(iot-manager CXX)

option(BUILD_FRONTEND "Build and copy frontend" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic "$<$<CONFIG:Release>:-O3>")
if(UNIX AND NOT APPLE)
    add_link_options(-static)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)

file(GLOB_RECURSE SERVER_HEADERS CONFIGURE_DEPENDS
    "server/*.h"
    "server/*.hpp"
)
file(GLOB_RECURSE GB28181_SOURCES CONFIGURE_DEPENDS
    "server/modules/gb28181/*.cpp"
)

add_executable(${PROJECT_NAME}
    server/main.cpp
    ${GB28181_SOURCES}
    ${SERVER_HEADERS}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${PROJECT_SOURCE_DIR}/server"
    "${PROJECT_SOURCE_DIR}/server/modules/gb28181"
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
    "${PROJECT_SOURCE_DIR}/server/pch.hpp"
)

set_source_files_properties(${SERVER_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Drogon::Drogon
    mimalloc-static
    Boost::json
    OpenSSL::Crypto
    pugixml::pugixml
)

# vcpkg's static MinGW PostgreSQL target places pgport before pgcommon.
# Repeat pgport at the end so GNU ld can resolve pgcommon's references.
if(MINGW AND TARGET PostgreSQL::PostgreSQL)
    get_target_property(POSTGRESQL_LIBRARY_DEBUG PostgreSQL::PostgreSQL IMPORTED_LOCATION_DEBUG)
    get_target_property(POSTGRESQL_LIBRARY_RELEASE PostgreSQL::PostgreSQL IMPORTED_LOCATION_RELEASE)
    get_filename_component(POSTGRESQL_DEBUG_LIB_DIR "${POSTGRESQL_LIBRARY_DEBUG}" DIRECTORY)
    get_filename_component(POSTGRESQL_RELEASE_LIB_DIR "${POSTGRESQL_LIBRARY_RELEASE}" DIRECTORY)
    set(POSTGRESQL_PGPORT_LIBRARY
        "$<$<CONFIG:Debug>:${POSTGRESQL_DEBUG_LIB_DIR}/libpgport.a>"
        "$<$<NOT:$<CONFIG:Debug>>:${POSTGRESQL_RELEASE_LIB_DIR}/libpgport.a>"
    )
    set_property(TARGET PostgreSQL::PostgreSQL APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ${POSTGRESQL_PGPORT_LIBRARY}
    )
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 shell32)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/config/config.local.json")
    set(APP_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/config/config.local.json")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/config/config.json")
    set(APP_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/config/config.json")
else()
    set(APP_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/config/config.example.json")
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${APP_CONFIG_SOURCE}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config/config.json"
    COMMENT "Copying config..."
)

if(BUILD_FRONTEND)
    find_program(BUN_EXECUTABLE
        NAMES bun bun.exe
        HINTS "$ENV{BUN_INSTALL}/bin"
        DOC "Path to bun executable"
    )
    if(BUN_EXECUTABLE)
        message(STATUS "Using bun: ${BUN_EXECUTABLE}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env LANG=en_US.UTF-8 ${BUN_EXECUTABLE} run build
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Building frontend..."
        )
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/build/web"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/web"
            COMMENT "Copying frontend..."
        )
    else()
        message(WARNING "bun not found; frontend will not be built.")
    endif()
endif()
