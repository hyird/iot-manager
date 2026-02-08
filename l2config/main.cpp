/**
 * @brief L2 配置工具 - 二层 SSH 隧道
 *
 * 通过 L2 原始以太网帧发现 ARM 设备并建立 SSH 隧道，
 * 无需 IP 地址即可 SSH 登录设备。
 *
 * 无参数: 启动 GUI (ImGui + DirectX11)
 *
 * 命令行模式:
 *   l2config list-nics                          列举网卡
 *   l2config scan [--nic NAME]                  发现设备
 *   l2config ssh MAC [--nic NAME] [--port 2222] SSH 隧道
 */

#include "L2ConfigApp.hpp"
#include "L2TcpTunnel.hpp"
#include "L2Transport.hpp"
#include "common/l2config/L2Protocol.hpp"

#include <json/json.h>

#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static std::atomic<bool> g_running{true};

#ifdef _WIN32
static BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}
#else
static void signalHandler(int) {
    g_running = false;
}
#endif

static void printUsage() {
    std::cout << R"(
L2 Configuration Tool - Layer 2 SSH Tunnel

Usage:
  l2config                                    Launch GUI (default)
  l2config gui                                Launch GUI explicitly
  l2config list-nics                          List available network interfaces
  l2config scan [--nic NAME]                  Discover devices on the network
  l2config ssh MAC [--nic NAME] [--port 2222] Create SSH tunnel to device

Examples:
  l2config
  l2config scan --nic "Ethernet"
  l2config ssh AA:BB:CC:DD:EE:FF --port 2222

After establishing the tunnel, connect via:
  ssh root@localhost -p 2222
)" << std::endl;
}

// ==================== list-nics ====================

static int cmdListNics() {
    auto nics = l2config_tool::L2Transport::listInterfaces();
    if (nics.empty()) {
        std::cerr << "No network interfaces found. Is Npcap installed?" << std::endl;
        std::cerr << "Download: https://npcap.com/#download" << std::endl;
        return 1;
    }

    std::cout << "Available network interfaces:" << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    for (size_t i = 0; i < nics.size(); ++i) {
        const auto& nic = nics[i];
        std::cout << "  [" << (i + 1) << "] " << nic.displayName << std::endl;
        std::cout << "      MAC: " << l2config::macToString(nic.mac) << std::endl;
        std::cout << "      IP:  " << (nic.ip.empty() ? "(none)" : nic.ip) << std::endl;
        std::cout << "      Status: " << (nic.up ? "UP" : "DOWN") << std::endl;
        std::cout << std::endl;
    }

    return 0;
}

// ==================== scan ====================

struct ScanArgs {
    std::string nic;
};

static int cmdScan(const ScanArgs& args) {
    // 查找网卡
    l2config_tool::NicInfo nicInfo;
    if (args.nic.empty()) {
        auto nics = l2config_tool::L2Transport::listInterfaces();
        auto it = std::find_if(nics.begin(), nics.end(), [](const l2config_tool::NicInfo& n) {
            return n.up;
        });
        if (it == nics.end()) {
            std::cerr << "No UP network interface found. Use --nic to specify." << std::endl;
            return 1;
        }
        nicInfo = *it;
    } else {
        auto found = l2config_tool::L2Transport::findInterface(args.nic);
        if (!found) {
            std::cerr << "Interface \"" << args.nic << "\" not found. Use 'list-nics' to see available." << std::endl;
            return 1;
        }
        nicInfo = *found;
    }

    std::cout << "Scanning on: " << nicInfo.displayName
              << " (MAC=" << l2config::macToString(nicInfo.mac) << ")" << std::endl;

    l2config_tool::L2Transport transport;

    // 收集发现的设备
    struct DiscoveredDevice {
        l2config::MacAddr mac;
        std::string info;
    };
    std::vector<DiscoveredDevice> devices;
    std::mutex devicesMutex;

    transport.setFrameCallback([&](const uint8_t* data, size_t len) {
        auto frameOpt = l2config::parseFrame(data, len);
        if (!frameOpt) return;
        const auto& frame = *frameOpt;
        if (frame.srcMac == transport.localMac()) return;

        if (static_cast<l2config::MsgType>(frame.header.msgType) == l2config::MsgType::DISCOVER_RSP) {
            std::string payload(reinterpret_cast<const char*>(frame.payload), frame.payloadLen);
            std::lock_guard<std::mutex> lock(devicesMutex);
            // 去重
            for (const auto& d : devices) {
                if (d.mac == frame.srcMac) return;
            }
            devices.push_back({frame.srcMac, payload});
        }
    });

    auto error = transport.start(nicInfo.pcapName, nicInfo.mac);
    if (!error.empty()) {
        std::cerr << "Failed to start: " << error << std::endl;
        return 1;
    }

    // 发送 3 次发现广播，间隔 500ms
    for (int i = 0; i < 3 && g_running; ++i) {
        auto frames = l2config::buildFrames(
            l2config::BROADCAST_MAC, nicInfo.mac,
            l2config::MsgType::DISCOVER_REQ, 0);
        transport.sendFrames(frames);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 额外等待 500ms 收集响应
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    transport.stop();

    // 显示结果
    std::lock_guard<std::mutex> lock(devicesMutex);
    if (devices.empty()) {
        std::cout << "\nNo devices found." << std::endl;
        return 0;
    }

    std::cout << "\nDiscovered " << devices.size() << " device(s):" << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        std::cout << "  [" << (i + 1) << "] MAC: " << l2config::macToString(dev.mac) << std::endl;

        // 解析 JSON 信息
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::istringstream iss(dev.info);
        std::string errs;
        if (Json::parseFromStream(reader, iss, &root, &errs)) {
            if (root.isMember("code"))
                std::cout << "      Code: " << root["code"].asString() << std::endl;
            if (root.isMember("name"))
                std::cout << "      Name: " << root["name"].asString() << std::endl;
            if (root.isMember("version"))
                std::cout << "      Version: " << root["version"].asString() << std::endl;
            if (root.isMember("interfaces") && root["interfaces"].isArray()) {
                for (const auto& iface : root["interfaces"]) {
                    std::cout << "      Interface: " << iface.get("name", "?").asString()
                              << " " << iface.get("ip", "").asString();
                    if (iface.get("prefix_length", 0).asInt() > 0) {
                        std::cout << "/" << iface["prefix_length"].asInt();
                    }
                    std::cout << (iface.get("up", false).asBool() ? " [UP]" : " [DOWN]")
                              << std::endl;
                }
            }
        } else {
            std::cout << "      Info: " << dev.info << std::endl;
        }
        std::cout << std::endl;
    }

    return 0;
}

// ==================== ssh ====================

struct SshArgs {
    std::string mac;
    std::string nic;
    uint16_t localPort = 2222;
    uint16_t remotePort = 22;
};

static int cmdSsh(const SshArgs& args) {
    auto targetMac = l2config::stringToMac(args.mac);
    if (!targetMac) {
        std::cerr << "Invalid MAC address: " << args.mac << std::endl;
        return 1;
    }

    // 查找网卡
    l2config_tool::NicInfo nicInfo;
    if (args.nic.empty()) {
        auto nics = l2config_tool::L2Transport::listInterfaces();
        auto it = std::find_if(nics.begin(), nics.end(), [](const l2config_tool::NicInfo& n) {
            return n.up;
        });
        if (it == nics.end()) {
            std::cerr << "No UP network interface found. Use --nic to specify." << std::endl;
            return 1;
        }
        nicInfo = *it;
    } else {
        auto found = l2config_tool::L2Transport::findInterface(args.nic);
        if (!found) {
            std::cerr << "Interface \"" << args.nic << "\" not found." << std::endl;
            return 1;
        }
        nicInfo = *found;
    }

    std::cout << "Using interface: " << nicInfo.displayName
              << " (MAC=" << l2config::macToString(nicInfo.mac) << ")" << std::endl;
    std::cout << "Target device: " << l2config::macToString(*targetMac) << std::endl;

    l2config_tool::L2Transport transport;
    auto error = transport.start(nicInfo.pcapName, nicInfo.mac);
    if (!error.empty()) {
        std::cerr << "Failed to start transport: " << error << std::endl;
        return 1;
    }

    l2config_tool::L2TcpTunnel tunnel(transport, *targetMac);
    error = tunnel.start(args.localPort, args.remotePort);
    if (!error.empty()) {
        std::cerr << "Failed to start tunnel: " << error << std::endl;
        transport.stop();
        return 1;
    }

    std::cout << std::endl;
    std::cout << "SSH tunnel listening on 127.0.0.1:" << args.localPort << std::endl;
    std::cout << "Connect via:  ssh root@localhost -p " << args.localPort << std::endl;
    std::cout << "Press Ctrl+C to exit." << std::endl;

    // 等待 Ctrl+C
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down..." << std::endl;
    tunnel.stop();
    transport.stop();

    return 0;
}

// ==================== 参数解析 ====================

static std::string getArgValue(const std::vector<std::string>& args, const std::string& flag, size_t& i) {
    if (i + 1 < args.size()) {
        return args[++i];
    }
    std::cerr << "Missing value for " << flag << std::endl;
    return "";
}

int main(int argc, char* argv[]) {
    // 初始化 Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // 注册信号处理
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    // 设置控制台为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (args.empty()) {
        // 无参数时启动 GUI
        l2config_tool::L2ConfigApp app;
        return app.run();
    }

    const auto& cmd = args[0];

    if (cmd == "gui") {
        l2config_tool::L2ConfigApp app;
        return app.run();
    }

    if (cmd == "list-nics") {
        return cmdListNics();
    }

    if (cmd == "scan") {
        ScanArgs scanArgs;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--nic") {
                scanArgs.nic = getArgValue(args, "--nic", i);
            }
        }
        return cmdScan(scanArgs);
    }

    if (cmd == "ssh") {
        SshArgs sshArgs;
        if (args.size() < 2) {
            std::cerr << "Usage: l2config ssh MAC [--nic NAME] [--port 2222]" << std::endl;
            return 1;
        }
        sshArgs.mac = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--nic") {
                sshArgs.nic = getArgValue(args, "--nic", i);
            } else if (args[i] == "--port") {
                auto val = getArgValue(args, "--port", i);
                if (!val.empty()) sshArgs.localPort = static_cast<uint16_t>(std::stoi(val));
            } else if (args[i] == "--remote-port") {
                auto val = getArgValue(args, "--remote-port", i);
                if (!val.empty()) sshArgs.remotePort = static_cast<uint16_t>(std::stoi(val));
            }
        }
        return cmdSsh(sshArgs);
    }

    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printUsage();
        return 0;
    }

    std::cerr << "Unknown command: " << cmd << std::endl;
    printUsage();
    return 1;
}
