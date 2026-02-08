# L2 配置工具 - 仅 Windows 构建
if(NOT WIN32)
    message(STATUS "l2config: skipping (Windows-only)")
    return()
endif()

# ==================== Npcap SDK ====================

set(NPCAP_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/npcap-sdk" CACHE PATH "Npcap SDK directory")

if(NOT EXISTS "${NPCAP_SDK_DIR}/Include/pcap.h")
    message(WARNING
        "l2config: Npcap SDK not found at ${NPCAP_SDK_DIR}, skipping.\n"
        "  Download from: https://npcap.com/dist/npcap-sdk-1.16.zip\n"
        "  Extract to: ${NPCAP_SDK_DIR}/")
    return()
endif()

message(STATUS "l2config: using Npcap SDK at ${NPCAP_SDK_DIR}")

# ==================== ImGui (通过 vcpkg) ====================

find_package(imgui CONFIG REQUIRED)

# ==================== l2config 可执行文件 ====================

file(GLOB_RECURSE L2CONFIG_HEADERS "*.hpp")
file(GLOB_RECURSE L2CONFIG_COMMON "${CMAKE_SOURCE_DIR}/server/common/l2config/*.hpp")

add_executable(l2config WIN32
    main.cpp
    ${L2CONFIG_HEADERS}
    ${L2CONFIG_COMMON}
)

set_source_files_properties(${L2CONFIG_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(${L2CONFIG_COMMON} PROPERTIES HEADER_FILE_ONLY TRUE)

target_compile_features(l2config PRIVATE cxx_std_20)

target_include_directories(l2config PRIVATE
    "${CMAKE_SOURCE_DIR}/server"
    "${NPCAP_SDK_DIR}/Include"
)

# Npcap 库目录
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NPCAP_LIB_DIR "${NPCAP_SDK_DIR}/Lib/x64")
else()
    set(NPCAP_LIB_DIR "${NPCAP_SDK_DIR}/Lib")
endif()

target_link_directories(l2config PRIVATE "${NPCAP_LIB_DIR}")

# 链接: jsoncpp + imgui + Npcap + DirectX11 + Winsock
find_package(jsoncpp CONFIG QUIET)
if(jsoncpp_FOUND)
    set(JSON_LIB JsonCpp::JsonCpp)
else()
    set(JSON_LIB Drogon::Drogon)
endif()

target_link_libraries(l2config PRIVATE
    ${JSON_LIB}
    imgui::imgui
    wpcap Packet ws2_32 iphlpapi
    d3d11 dxgi d3dcompiler
)

if(MSVC)
    target_compile_options(l2config PRIVATE /W4 /EHsc /wd4068 /wd4996 /wd4244 /wd4267)
endif()

# WIN32 子系统需要 WinMain 入口点，但我们用 main()，需要设置链接选项
set_target_properties(l2config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>"
    LINK_FLAGS "/ENTRY:mainCRTStartup"
)
