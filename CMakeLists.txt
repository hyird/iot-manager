cmake_minimum_required(VERSION 3.20)

# 自动检测 vcpkg
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# vcpkg triplet - 完全静态链接
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
        set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
    elseif(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
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

# 编译选项
if(MSVC)
    add_compile_options(/W4 /EHsc /bigobj /MP)
    add_compile_options(/wd4068 /wd4996 /wd4244 /wd4267)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    # Linux 完全静态链接
    add_link_options(-static)
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

# 前端构建和复制 (需要 -DBUILD_FRONTEND=ON)
if(BUILD_FRONTEND)
    find_program(BUN_EXECUTABLE bun)
    if(BUN_EXECUTABLE)
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
    else()
        message(WARNING "bun not found, frontend will not be built")
    endif()
endif()
