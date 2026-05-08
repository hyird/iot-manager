cmake_minimum_required(VERSION 3.20)

# Auto-detect vcpkg before project() so IDE configure runs can find packages.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
    set(_IOT_MANAGER_VCPKG_ROOT_CANDIDATES "")

    foreach(_IOT_MANAGER_VCPKG_ENV IN ITEMS VCPKG_ROOT VCPKG_INSTALLATION_ROOT)
        if(DEFINED ENV{${_IOT_MANAGER_VCPKG_ENV}} AND NOT "$ENV{${_IOT_MANAGER_VCPKG_ENV}}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{${_IOT_MANAGER_VCPKG_ENV}}" _IOT_MANAGER_VCPKG_ROOT)
            list(APPEND _IOT_MANAGER_VCPKG_ROOT_CANDIDATES "${_IOT_MANAGER_VCPKG_ROOT}")
        endif()
    endforeach()

    find_program(_IOT_MANAGER_VCPKG_EXE vcpkg)
    if(_IOT_MANAGER_VCPKG_EXE)
        get_filename_component(_IOT_MANAGER_VCPKG_ROOT "${_IOT_MANAGER_VCPKG_EXE}" DIRECTORY)
        list(APPEND _IOT_MANAGER_VCPKG_ROOT_CANDIDATES "${_IOT_MANAGER_VCPKG_ROOT}")
    endif()

    list(APPEND _IOT_MANAGER_VCPKG_ROOT_CANDIDATES
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg"
        "D:/Dev/vcpkg"
        "C:/vcpkg"
    )

    foreach(_IOT_MANAGER_VCPKG_ROOT IN LISTS _IOT_MANAGER_VCPKG_ROOT_CANDIDATES)
        set(_IOT_MANAGER_VCPKG_TOOLCHAIN "${_IOT_MANAGER_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        if(EXISTS "${_IOT_MANAGER_VCPKG_TOOLCHAIN}")
            set(CMAKE_TOOLCHAIN_FILE "${_IOT_MANAGER_VCPKG_TOOLCHAIN}" CACHE FILEPATH "vcpkg toolchain file")
            message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
            break()
        endif()
    endforeach()

    unset(_IOT_MANAGER_VCPKG_ENV)
    unset(_IOT_MANAGER_VCPKG_EXE)
    unset(_IOT_MANAGER_VCPKG_ROOT)
    unset(_IOT_MANAGER_VCPKG_ROOT_CANDIDATES)
    unset(_IOT_MANAGER_VCPKG_TOOLCHAIN)
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
    message(STATUS "vcpkg toolchain not configured; set VCPKG_ROOT or CMAKE_TOOLCHAIN_FILE if dependencies are missing.")
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
    add_compile_options(/W4 /EHsc /bigobj /MP /utf-8)
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
        # 不启用 /GL + /LTCG。当前工程静态链接 Drogon/trantor/vcpkg 依赖，
        # MSVC 18 Release LTCG 下会在启动后的异步网络任务中触发 0xc0000409。
        # 使用 CMake/MSVC 默认 Release /O2，避免 /Ox 对现有异步代码中的 UB
        # 进行更激进优化；Release 也生成 PDB，方便现场 dump 直接定位到源码行。
        add_compile_options("$<$<CONFIG:Release>:/Zi>")
        add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL>")
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

# Drogon 1.9.x ships custom FindJsoncpp/FindBrotli modules that use plain
# find_library(). With Visual Studio multi-config generators and vcpkg manifest
# mode this can cache debug libraries into Release link lines, so pin the
# config-specific vcpkg libraries before Drogon resolves its dependencies.
if(WIN32 AND DEFINED VCPKG_TARGET_TRIPLET)
    if(DEFINED VCPKG_INSTALLED_DIR)
        set(_IOT_MANAGER_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")
    else()
        set(_IOT_MANAGER_VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
    endif()
    set(_IOT_MANAGER_VCPKG_TRIPLET_DIR
        "${_IOT_MANAGER_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
    )

    if(EXISTS "${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/include/json/json.h"
       AND EXISTS "${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/lib/jsoncpp.lib"
       AND EXISTS "${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/debug/lib/jsoncpp.lib")
        set(JSONCPP_INCLUDE_DIRS
            "${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/include"
            CACHE PATH "jsoncpp include dir" FORCE
        )
        set(JSONCPP_LIBRARIES
            "$<$<CONFIG:Debug>:${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/debug/lib/jsoncpp.lib>;$<$<NOT:$<CONFIG:Debug>>:${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/lib/jsoncpp.lib>"
            CACHE STRING "jsoncpp libraries" FORCE
        )
    endif()

    if(EXISTS "${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/include/brotli/decode.h")
        set(BROTLI_INCLUDE_DIR
            "${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/include"
            CACHE PATH "brotli include dir" FORCE
        )
        foreach(_IOT_MANAGER_BROTLI_LIB IN ITEMS brotlidec brotlienc brotlicommon)
            string(TOUPPER "${_IOT_MANAGER_BROTLI_LIB}" _IOT_MANAGER_BROTLI_VAR)
            set("${_IOT_MANAGER_BROTLI_VAR}_LIBRARY"
                "$<$<CONFIG:Debug>:${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/debug/lib/${_IOT_MANAGER_BROTLI_LIB}.lib>;$<$<NOT:$<CONFIG:Debug>>:${_IOT_MANAGER_VCPKG_TRIPLET_DIR}/lib/${_IOT_MANAGER_BROTLI_LIB}.lib>"
                CACHE STRING "${_IOT_MANAGER_BROTLI_LIB} libraries" FORCE
            )
        endforeach()
        unset(_IOT_MANAGER_BROTLI_LIB)
        unset(_IOT_MANAGER_BROTLI_VAR)
    endif()

    unset(_IOT_MANAGER_VCPKG_INSTALLED_DIR)
    unset(_IOT_MANAGER_VCPKG_TRIPLET_DIR)
endif()

# 查找 Drogon (通过 vcpkg)
find_package(Drogon CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)
find_package(pugixml CONFIG REQUIRED)

# 查找 mimalloc (高性能内存分配器)
find_package(mimalloc CONFIG REQUIRED)

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/server)
include_directories(${PROJECT_SOURCE_DIR}/server/modules/gb28181)

# 收集所有头文件
file(GLOB_RECURSE HEADERS "server/*.hpp" "server/*.h")

set(GB28181_SOURCES
    server/modules/gb28181/Gb28181Module.cpp
    server/modules/gb28181/api/ApiServer.cpp
    server/modules/gb28181/config/AppConfig.cpp
    server/modules/gb28181/device/DeviceRegistry.cpp
    server/modules/gb28181/media/StreamRegistry.cpp
    server/modules/gb28181/media/ZlmClient.cpp
    server/modules/gb28181/platform/WindowsFirewall.cpp
    server/modules/gb28181/sip/DigestAuth.cpp
    server/modules/gb28181/sip/SipMessage.cpp
    server/modules/gb28181/sip/SipServer.cpp
)

# 添加可执行文件
add_executable(${PROJECT_NAME} server/main.cpp ${GB28181_SOURCES})

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
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Drogon::Drogon
        mimalloc-static
        Boost::json
        OpenSSL::Crypto
        pugixml::pugixml
)
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 shell32)
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
