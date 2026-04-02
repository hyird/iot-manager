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

# 当在非 Visual Studio 开发环境（如普通 CMD/Powershell）调用 MSVC 时，
# 环境变量 INCLUDE 可能不存在，导致 cl 无法找到标准库头文件。这里尝试自动补齐
# MSVC 与 Windows SDK 的默认头文件路径，避免 C1083: string not found。
if(MSVC)
    if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES STREQUAL "")
        string(REGEX REPLACE "[\\/]+bin[\\/].*$" "" _MSVC_TOOL_ROOT "${CMAKE_CXX_COMPILER}")
        set(_IOT_MANAGER_EXTRA_INCLUDE_DIRS "")
        set(_IOT_MANAGER_EXTRA_LIBRARY_DIRS "")

        if(EXISTS "${_MSVC_TOOL_ROOT}/include")
            list(APPEND _IOT_MANAGER_EXTRA_INCLUDE_DIRS "${_MSVC_TOOL_ROOT}/include")
        endif()
        if(EXISTS "${_MSVC_TOOL_ROOT}/lib/x64")
            list(APPEND _IOT_MANAGER_EXTRA_LIBRARY_DIRS "${_MSVC_TOOL_ROOT}/lib/x64")
        endif()
        if(EXISTS "${_MSVC_TOOL_ROOT}/atlmfc/lib/x64")
            list(APPEND _IOT_MANAGER_EXTRA_LIBRARY_DIRS "${_MSVC_TOOL_ROOT}/atlmfc/lib/x64")
        endif()

        set(_IOT_MANAGER_PROGRAM_FILES_ROOT "$ENV{ProgramFiles}")
        if(_IOT_MANAGER_PROGRAM_FILES_ROOT STREQUAL "")
            set(_IOT_MANAGER_PROGRAM_FILES_ROOT "$ENV{ProgramW6432}")
        endif()

        set(_IOT_MANAGER_WINSDK_INCLUDE_BASES
            "${_IOT_MANAGER_PROGRAM_FILES_ROOT}/Windows Kits/10/Include"
            "${_IOT_MANAGER_PROGRAM_FILES_ROOT} (x86)/Windows Kits/10/Include"
        )

        foreach(_IOT_MANAGER_WINSDK_INCLUDE_BASE IN LISTS _IOT_MANAGER_WINSDK_INCLUDE_BASES)
            if(EXISTS "${_IOT_MANAGER_WINSDK_INCLUDE_BASE}")
                file(GLOB _IOT_MANAGER_WINSDK_ROOTS
                    "${_IOT_MANAGER_WINSDK_INCLUDE_BASE}/*"
                )
                if(_IOT_MANAGER_WINSDK_ROOTS)
                    list(SORT _IOT_MANAGER_WINSDK_ROOTS)
                    list(GET _IOT_MANAGER_WINSDK_ROOTS -1 _IOT_MANAGER_WINSDK_ROOT)
                    foreach(_IOT_MANAGER_WINSDK_SUBDIR IN ITEMS ucrt shared um)
                        if(EXISTS "${_IOT_MANAGER_WINSDK_ROOT}/${_IOT_MANAGER_WINSDK_SUBDIR}")
                            list(APPEND _IOT_MANAGER_EXTRA_INCLUDE_DIRS
                                "${_IOT_MANAGER_WINSDK_ROOT}/${_IOT_MANAGER_WINSDK_SUBDIR}"
                            )
                        endif()
                    endforeach()
                    get_filename_component(_IOT_MANAGER_WINSDK_VERSION
                        "${_IOT_MANAGER_WINSDK_ROOT}"
                        NAME
                    )
                    get_filename_component(_IOT_MANAGER_WINSDK_INCLUDE_DIR
                        "${_IOT_MANAGER_WINSDK_ROOT}"
                        DIRECTORY
                    )
                    get_filename_component(_IOT_MANAGER_WINSDK_KITS_ROOT
                        "${_IOT_MANAGER_WINSDK_INCLUDE_DIR}"
                        DIRECTORY
                    )
                    set(_IOT_MANAGER_WINSDK_LIB_DIRS "")
                    foreach(_IOT_MANAGER_WINSDK_LIB_SUBDIR IN ITEMS um ucrt)
                        if(EXISTS "${_IOT_MANAGER_WINSDK_KITS_ROOT}/Lib/${_IOT_MANAGER_WINSDK_VERSION}/${_IOT_MANAGER_WINSDK_LIB_SUBDIR}/x64")
                            list(APPEND _IOT_MANAGER_WINSDK_LIB_DIRS
                                "${_IOT_MANAGER_WINSDK_KITS_ROOT}/Lib/${_IOT_MANAGER_WINSDK_VERSION}/${_IOT_MANAGER_WINSDK_LIB_SUBDIR}/x64"
                            )
                        endif()
                    endforeach()
                    if(_IOT_MANAGER_WINSDK_LIB_DIRS)
                        list(APPEND _IOT_MANAGER_EXTRA_LIBRARY_DIRS ${_IOT_MANAGER_WINSDK_LIB_DIRS})
                    endif()
                    if(_IOT_MANAGER_EXTRA_LIBRARY_DIRS)
                        link_directories(${_IOT_MANAGER_EXTRA_LIBRARY_DIRS})
                        message(STATUS "已自动添加库目录：${_IOT_MANAGER_EXTRA_LIBRARY_DIRS}")
                    endif()
                    break()
                endif()
            endif()
        endforeach()

        if(_IOT_MANAGER_EXTRA_INCLUDE_DIRS)
            include_directories(${_IOT_MANAGER_EXTRA_INCLUDE_DIRS})
            message(STATUS "已自动添加 MSVC/SDK include 目录：${_IOT_MANAGER_EXTRA_INCLUDE_DIRS}")
        else()
            message(WARNING "未能自动定位 MSVC/Windows SDK include 目录，请在 VS Developer Command Prompt 中构建。")
        endif()
        unset(_IOT_MANAGER_WINSDK_ROOTS)
        unset(_IOT_MANAGER_EXTRA_INCLUDE_DIRS)
        unset(_IOT_MANAGER_EXTRA_LIBRARY_DIRS)
        unset(_IOT_MANAGER_PROGRAM_FILES_ROOT)
        unset(_IOT_MANAGER_WINSDK_INCLUDE_BASES)
        unset(_IOT_MANAGER_WINSDK_INCLUDE_DIR)
        unset(_IOT_MANAGER_WINSDK_LIB_DIRS)
        unset(_IOT_MANAGER_WINSDK_KITS_ROOT)
        unset(_MSVC_TOOL_ROOT)
        unset(_IOT_MANAGER_WINSDK_ROOT)
        unset(_IOT_MANAGER_WINSDK_VERSION)
    endif()
endif()

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
        # /Ox = 完整优化（比 /O2 更激进）
        add_compile_options("$<$<CONFIG:Release>:/Ox>")
        # /GL = Whole Program Optimization（开启 LTO）
        add_compile_options("$<$<CONFIG:Release>:/GL>")
        # /ipo = 显式启用链接时优化（/LTCG 已启用，此处显式声明）
        add_link_options("$<$<CONFIG:Release>:/ipo>")
        # /arch:AVX2 = 启用 AVX2 SIMD 指令集（现代 Intel/AMD CPU 支持）
        add_compile_options("$<$<CONFIG:Release>:/arch:AVX2>")
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
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

# EdgeNode 子项目
add_subdirectory(edgenode)

# 独立测试子目录（当前仅保留 S7 连接测试）
add_subdirectory(tests)

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
