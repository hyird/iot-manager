// mimalloc: 全局替换 new/delete，必须在所有其他 include 之前
#include <mimalloc-new-delete.h>

#include "EdgeNode.Config.hpp"
#include "EdgeNodeRuntime.hpp"

#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include <cstdlib>
#endif

#include <csignal>

static void abortHandler(int) {
    std::cerr << "[EdgeNode] ABORT signal - likely assertion failure in trantor::EventLoop" << std::endl;
    std::cerr << "[EdgeNode] Check if another EventLoop exists on the main thread" << std::endl;
    _exit(1);
}

int main(int argc, char* argv[]) {
    std::signal(SIGABRT, abortHandler);
#ifdef _WIN32
    // 设置控制台 UTF-8 编码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // 禁用 Windows 默认的 abort 弹窗，直接输出到 stderr
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[EdgeNode] WSAStartup failed" << std::endl;
        return 1;
    }
#endif

    // 支持 -c <config_path> 指定配置文件
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-c" && i + 1 < argc) {
            configPath = argv[++i];
        }
    }

    std::string error;
    auto configOpt = agent_app::EdgeNodeConfigLoader::load(configPath, &error);
    if (!configOpt) {
        std::cerr << "[EdgeNode] " << error << std::endl;
        return 1;
    }

    std::cout << "[EdgeNode] starting, code=" << configOpt->code
              << ", platform=" << configOpt->platformHost << "/agent/ws" << std::endl;

    try {
        std::cout << "[EdgeNode] creating runtime..." << std::endl;
        agent_app::EdgeNodeRuntime runtime(*configOpt);
        std::cout << "[EdgeNode] runtime created, starting..." << std::endl;
        runtime.run();
    } catch (const std::exception& e) {
        std::cerr << "[EdgeNode] fatal: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "[EdgeNode] fatal: unknown exception" << std::endl;
        return 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
