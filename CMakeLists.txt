cmake_minimum_required(VERSION 3.20)

# 自动检测 vcpkg（仅通过环境变量）
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(_vcpkg_toolchain "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${_vcpkg_toolchain}")
        set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_toolchain}" CACHE STRING "vcpkg toolchain file")
        message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

# vcpkg overlay triplets（项目内自定义 triplet 优先）
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")

# vcpkg triplet - 完全静态链接
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
        set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
    elseif(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
    endif()
endif()
if(DEFINED ENV{VCPKG_DEFAULT_HOST_TRIPLET} AND NOT DEFINED VCPKG_HOST_TRIPLET)
    set(VCPKG_HOST_TRIPLET "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" CACHE STRING "")
endif()

project(iot-manager CXX)

# ============== 构建选项 ==============
option(BUILD_FRONTEND "Build and copy frontend" ON)

# C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 运行时：静态链接（/MT），与 x64-windows-static triplet 一致
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 编译选项
if(MSVC)
    add_compile_options(/W4 /EHsc /bigobj /MP)
    add_compile_options(/wd4068 /wd4996 /wd4244 /wd4267)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    # Linux 完全静态链接
    if(UNIX AND NOT APPLE)
        add_link_options(-static)
    endif()
endif()

# Release 优化
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    if(MSVC)
        add_compile_options("$<$<CONFIG:Release>:/O2>")
    else()
        add_compile_options("$<$<CONFIG:Release>:-O3>")
        # LTO (Link-Time Optimization)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
        if(ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
    endif()
endif()

# 查找 Drogon (通过 vcpkg)
find_package(Drogon CONFIG REQUIRED)

# 查找 mimalloc (高性能内存分配器)
find_package(mimalloc CONFIG REQUIRED)

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/server)

# 收集所有头文件
file(GLOB_RECURSE HEADERS "server/*.hpp")

# 添加可执行文件
add_executable(${PROJECT_NAME} server/main.cpp)

# 预编译头文件
target_precompile_headers(${PROJECT_NAME} PRIVATE
    "${PROJECT_SOURCE_DIR}/server/pch.hpp"
)

# ============== 输出目录设置 ==============
# 输出目录为 build/<config> (如 build/release, build/debug)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
)

# 添加头文件到项目 (用于 IDE)
target_sources(${PROJECT_NAME} PRIVATE ${HEADERS})
set_source_files_properties(${HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)

# 链接 Drogon + mimalloc
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon mimalloc-static)

# Agent 子项目
add_subdirectory(agent)

# L2 配置工具（Windows 专用，需要 Npcap SDK，默认关闭）
option(BUILD_L2CONFIG "Build L2 config tool (requires Npcap SDK)" OFF)
if(BUILD_L2CONFIG)
    add_subdirectory(l2config)
endif()

# ============== 构建后操作 ==============

# 优先使用本地配置文件（不提交到仓库），否则回退到默认配置
set(APP_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/config/config.json")
if(EXISTS "${PROJECT_SOURCE_DIR}/config/config.local.json")
    set(APP_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/config/config.local.json")
endif()

# 复制 config 目录到二进制输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${APP_CONFIG_SOURCE}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config/config.json"
    COMMENT "Copying config files..."
)

# 前端构建和复制（默认开启，可用 -DBUILD_FRONTEND=OFF 显式关闭）
if(BUILD_FRONTEND)
    unset(BUN_EXECUTABLE CACHE)
    if(DEFINED ENV{BUN_EXECUTABLE} AND EXISTS "$ENV{BUN_EXECUTABLE}")
        set(BUN_EXECUTABLE "$ENV{BUN_EXECUTABLE}")
    else()
        find_program(BUN_EXECUTABLE
            NAMES bun bun.exe
            HINTS "$ENV{BUN_INSTALL}/bin"
            DOC "Path to bun executable"
        )
    endif()
    if(BUN_EXECUTABLE)
        message(STATUS "Using bun: ${BUN_EXECUTABLE}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env LANG=en_US.UTF-8
                ${BUN_EXECUTABLE} run build
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Building frontend with bun..."
        )
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/build/web"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/web"
            COMMENT "Copying frontend build to output directory..."
        )
    endif()
endif()
