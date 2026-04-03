#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#endif

namespace agent_app {

class EdgeNodeNetworkConfigurator {
public:
    enum class Backend { NETWORKD, NETPLAN, IFUPDOWN, NONE };

    /**
     * @brief 检测当前活跃的网络管理后端
     *
     * 优先级: systemd-networkd > netplan > ifupdown/ifupdown2
     * 不支持 NetworkManager（无 GUI 环境）和裸 ip 命令（不持久化）
     */
    static Backend detectBackend() {
#ifdef __linux__
        if (runCommand("systemctl is-active systemd-networkd > /dev/null 2>&1").empty()) {
            return Backend::NETWORKD;
        }
        if (hasNetplan()) {
            return Backend::NETPLAN;
        }
        if (hasIfupdown()) {
            return Backend::IFUPDOWN;
        }
#endif
        return Backend::NONE;
    }

    static const char* backendName(Backend b) {
        switch (b) {
            case Backend::NETWORKD: return "systemd-networkd";
            case Backend::NETPLAN: return "netplan";
            case Backend::IFUPDOWN: return "ifupdown";
            case Backend::NONE: return "none";
        }
        return "unknown";
    }

    static bool isValidIpv4(const std::string& ip) {
#ifdef __linux__
        in_addr addr{};
        return !ip.empty() && ::inet_pton(AF_INET, ip.c_str(), &addr) == 1;
#else
        if (ip.empty()) return false;
        int parts = 0;
        int value = -1;
        for (const char ch : ip) {
            if (ch == '.') {
                if (value < 0 || value > 255) return false;
                ++parts;
                value = -1;
                continue;
            }
            if (ch < '0' || ch > '9') return false;
            value = value < 0 ? (ch - '0') : (value * 10 + (ch - '0'));
            if (value > 255) return false;
        }
        return parts == 3 && value >= 0 && value <= 255;
#endif
    }

    static int normalizePrefixLength(int prefixLength) {
        return prefixLength >= 1 && prefixLength <= 30 ? prefixLength : 24;
    }

    // ==================== 静态 IP ====================

    static std::string applyInterfaceAddress(const std::string& interfaceName,
                                             const std::string& ip,
                                             int prefixLength) {
        if (interfaceName.empty()) return "缺少网口名称";
        if (!isValidIpv4(ip)) return "网口IP格式错误: " + ip;
        prefixLength = normalizePrefixLength(prefixLength);

#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;

        const auto backend = detectBackend();
        switch (backend) {
            case Backend::NETWORKD:
                return applyStaticNetworkd(interfaceName, ip, prefixLength, "");
            case Backend::NETPLAN:
                return applyStaticNetplan(interfaceName, ip, prefixLength, "");
            case Backend::IFUPDOWN:
                return applyStaticIfupdown(interfaceName, ip, prefixLength, "");
            case Backend::NONE:
                return "未检测到网络管理工具 (systemd-networkd / netplan / ifupdown)";
        }
        return "不支持的后端";
#elif defined(_WIN32)
        const auto mask = prefixLengthToSubnetMask(prefixLength);
        return runCommand("netsh interface ip set address name=\"" + interfaceName
            + "\" static " + ip + " " + mask);
#else
        return "不支持的操作系统";
#endif
    }

    // ==================== DHCP ====================

    static std::string applyDhcp(const std::string& interfaceName) {
        if (interfaceName.empty()) return "缺少网口名称";

#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;

        const auto backend = detectBackend();
        switch (backend) {
            case Backend::NETWORKD:
                return applyDhcpNetworkd(interfaceName);
            case Backend::NETPLAN:
                return applyDhcpNetplan(interfaceName);
            case Backend::IFUPDOWN:
                return applyDhcpIfupdown(interfaceName);
            case Backend::NONE:
                return "未检测到网络管理工具 (systemd-networkd / netplan / ifupdown)";
        }
        return "不支持的后端";
#elif defined(_WIN32)
        auto error = runCommand("netsh interface ip set address name=\"" + interfaceName + "\" source=dhcp");
        if (!error.empty()) return error;
        runCommand("netsh interface ip set dns name=\"" + interfaceName + "\" source=dhcp");
        return {};
#else
        return "不支持的操作系统";
#endif
    }

    // ==================== 默认网关 ====================

    static std::string applyDefaultGateway(const std::string& interfaceName,
                                          const std::string& gateway) {
        if (gateway.empty()) return {};
        if (interfaceName.empty()) return "缺少网口名称";
        if (!isValidIpv4(gateway)) return "网关格式错误: " + gateway;

#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;

        // 网关在 applyStaticXxx 中一起写入配置文件
        // 此处仅作为补充：立即生效（临时），持久化由后端配置文件保证
        return runCommand("ip route replace default via " + gateway + " dev " + interfaceName);
#elif defined(_WIN32)
        return runCommand("netsh interface ip set address name=\"" + interfaceName
            + "\" gateway=" + gateway);
#else
        return "不支持的操作系统";
#endif
    }

    static std::string clearInterfaceAddress(const std::string& interfaceName) {
        if (interfaceName.empty()) return "缺少网口名称";
#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;
        return runCommand("ip addr flush dev " + interfaceName + " scope global");
#else
        return {};
#endif
    }

    static std::string clearDefaultGateway(const std::string& interfaceName) {
        if (interfaceName.empty()) return {};
#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;
        const auto error = runCommand("ip route del default dev " + interfaceName);
        if (isIgnorableRouteDeleteError(error)) return {};
        return error;
#else
        return {};
#endif
    }

    static std::string applyHostRoute(const std::string& interfaceName,
                                      const std::string& targetIp,
                                      const std::string& gateway,
                                      const std::string& sourceIp) {
        if (gateway.empty()) return {};
        if (interfaceName.empty()) return "缺少网口名称";
        if (!isValidIpv4(targetIp)) return "目标设备IP格式错误: " + targetIp;
        if (!isValidIpv4(gateway)) return "网关格式错误: " + gateway;
        if (!sourceIp.empty() && !isValidIpv4(sourceIp)) return "源地址格式错误: " + sourceIp;
#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;
        std::string command = "ip route replace " + targetIp + "/32 via " + gateway + " dev " + interfaceName;
        if (!sourceIp.empty()) command += " src " + sourceIp;
        return runCommand(command);
#else
        return {};
#endif
    }

    static std::string clearHostRoute(const std::string& interfaceName,
                                      const std::string& targetIp,
                                      const std::string& gateway) {
        if (gateway.empty()) return {};
        if (interfaceName.empty()) return "缺少网口名称";
        if (!isValidIpv4(targetIp)) return "目标设备IP格式错误: " + targetIp;
        if (!isValidIpv4(gateway)) return "网关格式错误: " + gateway;
#ifdef __linux__
        if (!isSafeInterfaceName(interfaceName)) return "网口名称非法: " + interfaceName;
        const auto error = runCommand("ip route del " + targetIp + "/32 via " + gateway + " dev " + interfaceName);
        if (isIgnorableRouteDeleteError(error)) return {};
        return error;
#else
        return {};
#endif
    }

    /**
     * @brief 执行命令并返回 stdout（public 供 AgentCapabilities 调用）
     */
    static std::string runCommandOutput(const std::string& command) {
        return runCommandOutputImpl(command);
    }

    // ==================== 桥接管理 ====================

    /**
     * @brief 创建网桥并设置成员端口
     *
     * @param bridgeName  桥接口名称 (如 br0)
     * @param ports       成员端口列表 (如 eth0, eth1)
     * @return 空字符串表示成功，否则返回错误信息
     */
    /**
     * @brief 通过系统网络管理工具创建网桥并设置成员端口
     *
     * 根据检测到的后端 (NetworkManager / systemd-networkd / netplan / ifupdown)
     * 写入对应的持久化配置，不使用 ip 命令临时操作。
     */
    static std::string createBridge(const std::string& bridgeName,
                                    const std::vector<std::string>& ports,
                                    const std::string& mode = "none",
                                    const std::string& ip = "",
                                    int prefixLength = 24,
                                    const std::string& gateway = "") {
#ifdef __linux__
        if (!isSafeInterfaceName(bridgeName)) return "桥接口名称非法: " + bridgeName;
        for (const auto& p : ports) {
            if (!isSafeInterfaceName(p)) return "端口名称非法: " + p;
        }

        const auto backend = detectBackend();
        switch (backend) {
            case Backend::NETWORKD:
                return createBridgeNetworkd(bridgeName, ports, mode, ip, prefixLength, gateway);
            case Backend::NETPLAN:
                return createBridgeNetplan(bridgeName, ports, mode, ip, prefixLength, gateway);
            case Backend::IFUPDOWN:
                return createBridgeIfupdown(bridgeName, ports, mode, ip, prefixLength, gateway);
            case Backend::NONE:
                return "未检测到网络管理工具 (systemd-networkd / netplan / ifupdown)";
        }
        return "不支持的后端";
#else
        (void)bridgeName; (void)ports; (void)mode; (void)ip; (void)prefixLength; (void)gateway;
        return "桥接仅支持 Linux 平台";
#endif
    }

    /**
     * @brief 通过系统网络管理工具删除网桥
     */
    static std::string deleteBridge(const std::string& bridgeName) {
#ifdef __linux__
        if (!isSafeInterfaceName(bridgeName)) return "桥接口名称非法: " + bridgeName;

        const auto backend = detectBackend();
        switch (backend) {
            case Backend::NETWORKD: return deleteBridgeNetworkd(bridgeName);
            case Backend::NETPLAN:  return deleteBridgeNetplan(bridgeName);
            case Backend::IFUPDOWN: return deleteBridgeIfupdown(bridgeName);
            case Backend::NONE:
                return "未检测到网络管理工具 (systemd-networkd / netplan / ifupdown)";
        }
        return "不支持的后端";
#else
        (void)bridgeName;
        return "桥接仅支持 Linux 平台";
#endif
    }

    /**
     * @brief 通过系统网络管理工具同步桥接口的成员端口列表
     */
    static std::string setBridgePorts(const std::string& bridgeName,
                                      const std::vector<std::string>& desiredPorts) {
#ifdef __linux__
        if (!isSafeInterfaceName(bridgeName)) return "桥接口名称非法: " + bridgeName;
        for (const auto& p : desiredPorts) {
            if (!isSafeInterfaceName(p)) return "端口名称非法: " + p;
        }

        const auto backend = detectBackend();
        switch (backend) {
            case Backend::NETWORKD: return setBridgePortsNetworkd(bridgeName, desiredPorts);
            case Backend::NETPLAN:  return setBridgePortsNetplan(bridgeName, desiredPorts);
            case Backend::IFUPDOWN: return setBridgePortsIfupdown(bridgeName, desiredPorts);
            case Backend::NONE:
                return "未检测到网络管理工具 (systemd-networkd / netplan / ifupdown)";
        }
        return "不支持的后端";
#else
        (void)bridgeName; (void)desiredPorts;
        return "桥接仅支持 Linux 平台";
#endif
    }

    /**
     * @brief 清理 iot 管理的历史网络配置，避免残留配置与新配置冲突
     *
     * - systemd-networkd: 删除 /etc/systemd/network/90-iot-*.network|*.netdev
     * - netplan: 删除 /etc/netplan/90-iot-*.yaml
     * - ifupdown: 删除 /etc/network/interfaces.d/iot-*
     */
    static std::string cleanupManagedConfigs(const std::vector<std::string>& targetInterfaces = {}) {
#ifdef __linux__
        const auto backend = detectBackend();
        switch (backend) {
            case Backend::NETWORKD:
                return cleanupManagedConfigsNetworkd(targetInterfaces);
            case Backend::NETPLAN:
                return cleanupManagedConfigsNetplan();
            case Backend::IFUPDOWN:
                return cleanupManagedConfigsIfupdown();
            case Backend::NONE:
                return {};
        }
        return {};
#else
        (void)targetInterfaces;
        return {};
#endif
    }

private:
    // ==================== systemd-networkd ====================

    /**
     * @brief 写入 systemd-networkd 配置文件并 reload
     *
     * 文件路径: /etc/systemd/network/90-iot-<iface>.network
     * 使用 90- 前缀确保优先级高于默认配置但低于手动配置
     */
    static std::string applyStaticNetworkd(const std::string& interfaceName,
                                           const std::string& ip, int prefixLength,
                                           const std::string& gateway) {
        const auto path = networkdConfigPath(interfaceName);
        const auto cidr = ip + "/" + std::to_string(prefixLength);

        std::string content =
            "[Match]\nName=" + interfaceName + "\n\n"
            "[Network]\nAddress=" + cidr + "\n";
        if (!gateway.empty()) {
            content += "Gateway=" + gateway + "\n";
        }

        auto error = writeFile(path, content);
        if (!error.empty()) return error;
        return reloadAndActivateNetworkd({interfaceName});
    }

    static std::string applyDhcpNetworkd(const std::string& interfaceName) {
        const auto path = networkdConfigPath(interfaceName);
        std::string content =
            "[Match]\nName=" + interfaceName + "\n\n"
            "[Network]\nDHCP=yes\n";

        auto error = writeFile(path, content);
        if (!error.empty()) return error;
        return reloadAndActivateNetworkd({interfaceName});
    }

    static std::string networkdConfigPath(const std::string& interfaceName) {
        return "/etc/systemd/network/90-iot-" + interfaceName + ".network";
    }

    static std::string cleanupManagedConfigsNetworkd(const std::vector<std::string>& targetInterfaces) {
        namespace fs = std::filesystem;

        // 先记录历史桥名，reload 后主动删接口（networkctl reload 不保证立即删）
        std::vector<std::string> bridges;
        std::set<std::string> targets;
        for (const auto& name : targetInterfaces) {
            if (isSafeInterfaceName(name)) targets.insert(name);
        }

        // 1) 删除所有 iot 托管文件
        const auto netdevList = runCommandOutputImpl("ls /etc/systemd/network/90-iot-*.netdev 2>/dev/null");
        for (const auto& path : splitLines(netdevList)) {
            std::ifstream ifs(path);
            if (!ifs) continue;
            std::string line;
            while (std::getline(ifs, line)) {
                if (line.rfind("Name=", 0) == 0) {
                    const auto bridgeName = line.substr(5);
                    if (!bridgeName.empty() && isSafeInterfaceName(bridgeName)) {
                        bridges.push_back(bridgeName);
                    }
                    break;
                }
            }
        }

        auto error = runCommand("rm -f /etc/systemd/network/90-iot-*.network /etc/systemd/network/90-iot-*.netdev");
        if (!error.empty()) return error;

        // 2) 删除与本次目标接口同名匹配的“其它”networkd文件（非 90-iot-*）
        if (!targets.empty()) {
            for (const auto& entry : fs::directory_iterator("/etc/systemd/network")) {
                if (!entry.is_regular_file()) continue;
                const auto p = entry.path();
                const auto filename = p.filename().string();
                const auto ext = p.extension().string();
                if (ext != ".network" && ext != ".netdev") continue;
                if (filename.rfind("90-iot-", 0) == 0) continue;

                std::ifstream ifs(p.string());
                if (!ifs) continue;
                std::string line;
                bool shouldDelete = false;
                while (std::getline(ifs, line)) {
                    if (line.rfind("Name=", 0) == 0) {
                        auto name = line.substr(5);
                        if (targets.count(name) > 0) {
                            shouldDelete = true;
                            if (ext == ".netdev") bridges.push_back(name);
                            break;
                        }
                    }
                    if (line.rfind("Bridge=", 0) == 0) {
                        auto bridgeName = line.substr(7);
                        if (targets.count(bridgeName) > 0) {
                            shouldDelete = true;
                            break;
                        }
                    }
                }
                if (shouldDelete) {
                    std::error_code ec;
                    fs::remove(p, ec);
                }
            }
        }

        error = reloadAndActivateNetworkd(targetInterfaces);
        if (!error.empty()) return error;

        for (const auto& bridge : bridges) {
            runCommand("ip link del " + bridge + " 2>/dev/null");
        }
        return {};
    }

    // ==================== netplan ====================

    /**
     * @brief 检测 netplan 是否可用
     */
    static bool hasNetplan() {
        return runCommand("which netplan > /dev/null 2>&1").empty();
    }

    /**
     * @brief 写入 netplan 配置文件并 apply
     *
     * 文件路径: /etc/netplan/90-iot-<iface>.yaml
     */
    static std::string applyStaticNetplan(const std::string& interfaceName,
                                          const std::string& ip, int prefixLength,
                                          const std::string& gateway) {
        const auto path = "/etc/netplan/90-iot-" + interfaceName + ".yaml";
        const auto cidr = ip + "/" + std::to_string(prefixLength);

        std::string content =
            "network:\n"
            "  version: 2\n"
            "  ethernets:\n"
            "    " + interfaceName + ":\n"
            "      addresses:\n"
            "        - " + cidr + "\n";
        if (!gateway.empty()) {
            content +=
                "      routes:\n"
                "        - to: default\n"
                "          via: " + gateway + "\n";
        }

        auto error = writeFile(path, content);
        if (!error.empty()) return error;
        return runCommand("netplan apply");
    }

    static std::string applyDhcpNetplan(const std::string& interfaceName) {
        const auto path = "/etc/netplan/90-iot-" + interfaceName + ".yaml";
        std::string content =
            "network:\n"
            "  version: 2\n"
            "  ethernets:\n"
            "    " + interfaceName + ":\n"
            "      dhcp4: true\n";

        auto error = writeFile(path, content);
        if (!error.empty()) return error;
        return runCommand("netplan apply");
    }

    static std::string cleanupManagedConfigsNetplan() {
        auto error = runCommand("rm -f /etc/netplan/90-iot-*.yaml");
        if (!error.empty()) return error;
        return runCommand("netplan apply");
    }

    // ==================== ifupdown (/etc/network/interfaces) ====================

    /**
     * @brief 检测 ifupdown 是否可用
     *
     * 判断条件: ifup/ifdown 命令存在且 /etc/network/interfaces 文件存在
     */
    static bool hasIfupdown() {
        return runCommand("which ifup > /dev/null 2>&1").empty() &&
               runCommand("test -f /etc/network/interfaces").empty();
    }

    /**
     * @brief ifupdown 配置片段路径
     *
     * 写入 /etc/network/interfaces.d/iot-<iface>
     * 需要主 interfaces 文件包含 source interfaces.d 指令
     */
    static std::string ifupdownConfigPath(const std::string& interfaceName) {
        return "/etc/network/interfaces.d/iot-" + interfaceName;
    }

    /**
     * @brief 确保 /etc/network/interfaces 中有 source interfaces.d
     */
    static void ensureInterfacesDirSourced() {
        std::ifstream ifs("/etc/network/interfaces");
        if (!ifs) return;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();

        if (content.find("interfaces.d") != std::string::npos) return;

        // 追加 source 指令
        std::ofstream ofs("/etc/network/interfaces", std::ios::app);
        if (ofs) {
            ofs << "\nsource /etc/network/interfaces.d/*\n";
        }
    }

    /**
     * @brief 从 /etc/network/interfaces 主文件中移除指定接口的配置段落
     *
     * 避免 interfaces.d/ 中的新配置与主文件中的旧配置冲突。
     * 识别并删除 auto/allow-hotplug/iface 开头且匹配接口名的段落
     * （包括后续的缩进行）。
     */
    static void removeIfaceFromMainInterfaces(const std::string& interfaceName) {
        const std::string mainPath = "/etc/network/interfaces";
        std::ifstream ifs(mainPath);
        if (!ifs) return;

        std::string result;
        std::string line;
        bool skipping = false;

        while (std::getline(ifs, line)) {
            // 判断当前行是否是目标接口的起始行
            bool isTargetStanza = false;
            if (line.find("auto " + interfaceName) == 0 ||
                line.find("allow-hotplug " + interfaceName) == 0 ||
                line.find("iface " + interfaceName + " ") == 0) {
                // 确保是精确匹配（不是 eth0 匹配 eth01）
                // auto eth0\n 或 auto eth0 后面没有更多字符
                const auto checkExact = [&](const std::string& prefix) -> bool {
                    if (line.find(prefix) != 0) return false;
                    auto afterName = prefix.size();
                    return afterName >= line.size() ||
                           line[afterName] == ' ' || line[afterName] == '\t' ||
                           line[afterName] == '\n' || line[afterName] == '\r';
                };
                isTargetStanza =
                    checkExact("auto " + interfaceName) ||
                    checkExact("allow-hotplug " + interfaceName) ||
                    (line.find("iface " + interfaceName + " ") == 0);
            }

            if (isTargetStanza) {
                skipping = true;
                continue;
            }

            // 在 skipping 模式下，跳过缩进的续行
            if (skipping) {
                if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
                    continue;  // 缩进行属于被跳过的段落
                }
                // 遇到非缩进行，段落结束
                skipping = false;
            }

            result += line + "\n";
        }
        ifs.close();

        // 只在确实有修改时才写回
        std::ifstream check(mainPath);
        std::string original((std::istreambuf_iterator<char>(check)),
                             std::istreambuf_iterator<char>());
        check.close();
        if (result != original) {
            writeFile(mainPath, result);
        }
    }

    static std::string applyStaticIfupdown(const std::string& interfaceName,
                                           const std::string& ip, int prefixLength,
                                           const std::string& gateway) {
        ensureInterfacesDirSourced();
        runCommand("mkdir -p /etc/network/interfaces.d");
        removeIfaceFromMainInterfaces(interfaceName);

        const auto path = ifupdownConfigPath(interfaceName);
        const auto mask = prefixLengthToSubnetMask(prefixLength);

        std::string content =
            "auto " + interfaceName + "\n"
            "iface " + interfaceName + " inet static\n"
            "    address " + ip + "\n"
            "    netmask " + mask + "\n";
        if (!gateway.empty()) {
            content += "    gateway " + gateway + "\n";
        }

        auto error = writeFile(path, content);
        if (!error.empty()) return error;

        // ifdown 可能失败（接口未 up），忽略错误
        runCommand("ifdown " + interfaceName + " 2>/dev/null");
        return runCommand("ifup " + interfaceName);
    }

    static std::string applyDhcpIfupdown(const std::string& interfaceName) {
        ensureInterfacesDirSourced();
        runCommand("mkdir -p /etc/network/interfaces.d");
        removeIfaceFromMainInterfaces(interfaceName);

        const auto path = ifupdownConfigPath(interfaceName);
        std::string content =
            "auto " + interfaceName + "\n"
            "iface " + interfaceName + " inet dhcp\n";

        auto error = writeFile(path, content);
        if (!error.empty()) return error;

        runCommand("ifdown " + interfaceName + " 2>/dev/null");
        return runCommand("ifup " + interfaceName);
    }

    static std::string cleanupManagedConfigsIfupdown() {
        auto error = runCommand("rm -f /etc/network/interfaces.d/iot-*");
        if (!error.empty()) return error;
        if (runCommand("which ifreload > /dev/null 2>&1").empty()) {
            return runCommand("ifreload -a");
        }
        return {};
    }

    // ==================== 桥接实现 ====================
    //
    // 每种后端实现三个操作: create / delete / setBridgePorts
    // 所有操作都通过系统网络管理工具写入持久化配置

#ifdef __linux__
    static std::vector<std::string> readCurrentBridgePorts(const std::string& bridgeName) {
        std::vector<std::string> ports;
        auto output = runCommandOutputImpl("ls /sys/class/net/" + bridgeName + "/brif/ 2>/dev/null");
        for (auto& line : splitLines(output)) {
            if (!line.empty()) ports.push_back(line);
        }
        return ports;
    }

    static std::string joinPorts(const std::vector<std::string>& ports, const std::string& sep) {
        std::string result;
        for (size_t i = 0; i < ports.size(); ++i) {
            if (i > 0) result += sep;
            result += ports[i];
        }
        return result;
    }

    // ---- systemd-networkd 桥接 ----
    //
    // 创建 .netdev 定义桥接设备，.network 为桥配置 IP，
    // 每个成员端口写一个 .network 指向 Bridge=<name>

    static std::string networkdNetdevPath(const std::string& name) {
        return "/etc/systemd/network/90-iot-" + name + ".netdev";
    }

    static std::string createBridgeNetworkd(const std::string& bridgeName,
                                            const std::vector<std::string>& ports,
                                            const std::string& mode,
                                            const std::string& ip, int prefixLength,
                                            const std::string& gateway) {
        // 1) .netdev 定义桥设备
        std::string netdev = "[NetDev]\nName=" + bridgeName + "\nKind=bridge\n";
        auto error = writeFile(networkdNetdevPath(bridgeName), netdev);
        if (!error.empty()) return error;

        // 2) .network 为桥接口配置地址
        std::string network = "[Match]\nName=" + bridgeName + "\n\n[Network]\n";
        if (mode == "dhcp") {
            network += "DHCP=yes\n";
        } else if (mode == "static" && !ip.empty()) {
            network += "Address=" + ip + "/" + std::to_string(prefixLength) + "\n";
            if (!gateway.empty()) network += "Gateway=" + gateway + "\n";
        }
        error = writeFile(networkdConfigPath(bridgeName), network);
        if (!error.empty()) return error;

        // 3) 每个成员端口 → Bridge=<bridgeName>
        for (const auto& port : ports) {
            std::string portNet = "[Match]\nName=" + port + "\n\n[Network]\nBridge=" + bridgeName + "\n";
            error = writeFile(networkdConfigPath(port), portNet);
            if (!error.empty()) return "设置端口 " + port + " 失败: " + error;
        }

        return reloadAndActivateNetworkd(collectTargets(bridgeName, ports));
    }

    static std::string deleteBridgeNetworkd(const std::string& bridgeName) {
        // 删除成员端口的 .network
        auto ports = readCurrentBridgePorts(bridgeName);
        for (const auto& port : ports) {
            runCommand("rm -f " + networkdConfigPath(port));
        }
        // 删除桥的 .netdev 和 .network
        runCommand("rm -f " + networkdNetdevPath(bridgeName));
        runCommand("rm -f " + networkdConfigPath(bridgeName));
        auto error = reloadAndActivateNetworkd({bridgeName});
        if (!error.empty()) return error;
        // networkctl reload 不会立刻删掉接口，需手动
        runCommand("ip link del " + bridgeName + " 2>/dev/null");
        return {};
    }

    static std::string setBridgePortsNetworkd(const std::string& bridgeName,
                                              const std::vector<std::string>& desiredPorts) {
        auto currentPorts = readCurrentBridgePorts(bridgeName);
        std::set<std::string> currentSet(currentPorts.begin(), currentPorts.end());
        std::set<std::string> desiredSet(desiredPorts.begin(), desiredPorts.end());

        for (const auto& port : currentPorts) {
            if (desiredSet.count(port) == 0) {
                runCommand("rm -f " + networkdConfigPath(port));
            }
        }
        for (const auto& port : desiredPorts) {
            if (currentSet.count(port) == 0) {
                std::string portNet = "[Match]\nName=" + port + "\n\n[Network]\nBridge=" + bridgeName + "\n";
                auto error = writeFile(networkdConfigPath(port), portNet);
                if (!error.empty()) return "设置端口 " + port + " 失败: " + error;
            }
        }
        return reloadAndActivateNetworkd(collectTargets(bridgeName, desiredPorts));
    }

    // ---- netplan 桥接 ----
    //
    // 在 /etc/netplan/90-iot-<bridge>.yaml 里用 bridges: 段定义

    static std::string createBridgeNetplan(const std::string& bridgeName,
                                           const std::vector<std::string>& ports,
                                           const std::string& mode,
                                           const std::string& ip, int prefixLength,
                                           const std::string& gateway) {
        const auto path = "/etc/netplan/90-iot-" + bridgeName + ".yaml";

        std::string content =
            "network:\n"
            "  version: 2\n"
            "  bridges:\n"
            "    " + bridgeName + ":\n";

        if (!ports.empty()) {
            content += "      interfaces: [" + joinPorts(ports, ", ") + "]\n";
        }

        if (mode == "dhcp") {
            content += "      dhcp4: true\n";
        } else if (mode == "static" && !ip.empty()) {
            content += "      addresses:\n"
                       "        - " + ip + "/" + std::to_string(prefixLength) + "\n";
            if (!gateway.empty()) {
                content += "      routes:\n"
                           "        - to: default\n"
                           "          via: " + gateway + "\n";
            }
        }

        content +=
            "      parameters:\n"
            "        stp: false\n";

        auto error = writeFile(path, content);
        if (!error.empty()) return error;
        return runCommand("netplan apply");
    }

    static std::string deleteBridgeNetplan(const std::string& bridgeName) {
        const auto path = "/etc/netplan/90-iot-" + bridgeName + ".yaml";
        runCommand("rm -f " + path);
        auto error = runCommand("netplan apply");
        if (!error.empty()) return error;
        // netplan apply 有时不立刻删接口
        runCommand("ip link del " + bridgeName + " 2>/dev/null");
        return {};
    }

    static std::string setBridgePortsNetplan(const std::string& bridgeName,
                                             const std::vector<std::string>& desiredPorts) {
        // netplan 不支持增量修改，读取当前桥的 IP 配置后重写整个文件
        const auto path = "/etc/netplan/90-iot-" + bridgeName + ".yaml";

        // 读当前桥接口的 IP 信息
        std::string currentMode = "none";
        std::string currentIp;
        int currentPrefix = 24;
        std::string currentGw;

        std::ifstream ifs(path);
        if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            if (content.find("dhcp4: true") != std::string::npos) {
                currentMode = "dhcp";
            } else if (content.find("addresses:") != std::string::npos) {
                currentMode = "static";
                // 简单提取 — 用 ip 命令获取更可靠
                auto ipOutput = runCommandOutputImpl("ip -4 -o addr show " + bridgeName + " 2>/dev/null");
                auto pos = ipOutput.find("inet ");
                if (pos != std::string::npos) {
                    pos += 5;
                    auto end = ipOutput.find_first_of(" ", pos);
                    auto cidr = ipOutput.substr(pos, end - pos);
                    auto slash = cidr.find('/');
                    if (slash != std::string::npos) {
                        currentIp = cidr.substr(0, slash);
                        currentPrefix = std::stoi(cidr.substr(slash + 1));
                    }
                }
                auto gwOutput = runCommandOutputImpl("ip route show dev " + bridgeName + " default 2>/dev/null");
                auto gwPos = gwOutput.find("default via ");
                if (gwPos != std::string::npos) {
                    gwPos += 12;
                    auto gwEnd = gwOutput.find_first_of(" \n", gwPos);
                    currentGw = gwOutput.substr(gwPos, gwEnd - gwPos);
                }
            }
        }

        return createBridgeNetplan(bridgeName, desiredPorts, currentMode,
                                   currentIp, currentPrefix, currentGw);
    }

    // ---- ifupdown / ifupdown2 桥接 ----
    //
    // /etc/network/interfaces.d/iot-<bridge>:
    //   auto br0
    //   iface br0 inet dhcp|static
    //       bridge-ports eth0 eth1
    //       bridge-stp off

    static std::string createBridgeIfupdown(const std::string& bridgeName,
                                            const std::vector<std::string>& ports,
                                            const std::string& mode,
                                            const std::string& ip, int prefixLength,
                                            const std::string& gateway) {
        ensureInterfacesDirSourced();
        runCommand("mkdir -p /etc/network/interfaces.d");
        removeIfaceFromMainInterfaces(bridgeName);
        for (const auto& port : ports) {
            removeIfaceFromMainInterfaces(port);
        }

        const auto path = ifupdownConfigPath(bridgeName);
        std::string content = "auto " + bridgeName + "\n";

        if (mode == "dhcp") {
            content += "iface " + bridgeName + " inet dhcp\n";
        } else if (mode == "static" && !ip.empty()) {
            content += "iface " + bridgeName + " inet static\n"
                       "    address " + ip + "\n"
                       "    netmask " + prefixLengthToSubnetMask(prefixLength) + "\n";
            if (!gateway.empty()) {
                content += "    gateway " + gateway + "\n";
            }
        } else {
            content += "iface " + bridgeName + " inet manual\n";
        }

        content += "    bridge-ports " + (ports.empty() ? "none" : joinPorts(ports, " ")) + "\n"
                   "    bridge-stp off\n";

        // 同时写每个成员端口为 manual + 无 IP（避免端口自己获取地址）
        for (const auto& port : ports) {
            const auto portPath = ifupdownConfigPath(port);
            std::string portContent = "auto " + port + "\n"
                                      "iface " + port + " inet manual\n";
            auto error = writeFile(portPath, portContent);
            if (!error.empty()) return "设置端口 " + port + " 配置失败: " + error;
        }

        auto error = writeFile(path, content);
        if (!error.empty()) return error;

        // ifreload 是 ifupdown2 的优势：无中断地应用变更
        if (runCommand("which ifreload > /dev/null 2>&1").empty()) {
            return runCommand("ifreload -a");
        }
        runCommand("ifdown " + bridgeName + " 2>/dev/null");
        return runCommand("ifup " + bridgeName);
    }

    static std::string deleteBridgeIfupdown(const std::string& bridgeName) {
        auto ports = readCurrentBridgePorts(bridgeName);
        runCommand("ifdown " + bridgeName + " 2>/dev/null");

        // 删除桥配置文件
        runCommand("rm -f " + ifupdownConfigPath(bridgeName));
        // 删除成员端口的 manual 配置
        for (const auto& port : ports) {
            runCommand("rm -f " + ifupdownConfigPath(port));
        }

        if (runCommand("which ifreload > /dev/null 2>&1").empty()) {
            return runCommand("ifreload -a");
        }
        // 手动清理
        runCommand("ip link del " + bridgeName + " 2>/dev/null");
        return {};
    }

    static std::string setBridgePortsIfupdown(const std::string& bridgeName,
                                              const std::vector<std::string>& desiredPorts) {
        // 读取当前桥配置文件，替换 bridge-ports 行
        const auto path = ifupdownConfigPath(bridgeName);
        std::ifstream ifs(path);
        if (!ifs) return "找不到桥接配置文件: " + path;

        std::string updated;
        std::string line;
        bool replaced = false;
        while (std::getline(ifs, line)) {
            if (line.find("bridge-ports") != std::string::npos) {
                updated += "    bridge-ports " + (desiredPorts.empty() ? "none" : joinPorts(desiredPorts, " ")) + "\n";
                replaced = true;
            } else {
                updated += line + "\n";
            }
        }
        ifs.close();
        if (!replaced) {
            updated += "    bridge-ports " + joinPorts(desiredPorts, " ") + "\n";
        }

        auto error = writeFile(path, updated);
        if (!error.empty()) return error;

        // 处理成员端口配置
        auto currentPorts = readCurrentBridgePorts(bridgeName);
        std::set<std::string> desiredSet(desiredPorts.begin(), desiredPorts.end());
        for (const auto& port : currentPorts) {
            if (desiredSet.count(port) == 0) {
                runCommand("rm -f " + ifupdownConfigPath(port));
            }
        }
        for (const auto& port : desiredPorts) {
            const auto portPath = ifupdownConfigPath(port);
            std::string portContent = "auto " + port + "\n"
                                      "iface " + port + " inet manual\n";
            writeFile(portPath, portContent);
        }

        if (runCommand("which ifreload > /dev/null 2>&1").empty()) {
            return runCommand("ifreload -a");
        }
        runCommand("ifdown " + bridgeName + " 2>/dev/null");
        return runCommand("ifup " + bridgeName);
    }
#endif

    // ==================== 工具方法 ====================

    static bool isSafeInterfaceName(const std::string& interfaceName) {
        if (interfaceName.empty()) return false;
        for (const char ch : interfaceName) {
            const bool valid =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == ':' || ch == '@';
            if (!valid) return false;
        }
        return true;
    }

    static bool isIgnorableRouteDeleteError(const std::string& error) {
        return error.find("No such process") != std::string::npos ||
               error.find("Cannot find device") != std::string::npos;
    }

    static std::string shellQuote(const std::string& s) {
        // 简单单引号包裹，内部单引号转义
        std::string result = "'";
        for (char ch : s) {
            if (ch == '\'') result += "'\\''";
            else result += ch;
        }
        result += "'";
        return result;
    }

    static std::vector<std::string> splitLines(const std::string& s) {
        std::vector<std::string> lines;
        std::string line;
        for (char ch : s) {
            if (ch == '\n') {
                if (!line.empty()) lines.push_back(line);
                line.clear();
            } else if (ch != '\r') {
                line += ch;
            }
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    }

    static std::string writeFile(const std::string& path, const std::string& content) {
        std::ofstream ofs(path, std::ios::trunc);
        if (!ofs) return "无法写入文件: " + path;
        ofs << content;
        ofs.close();
        return ofs.good() ? std::string{} : "写入文件失败: " + path;
    }

    static std::string prefixLengthToSubnetMask(int prefixLength) {
        uint32_t mask = prefixLength > 0 ? ~((1u << (32 - prefixLength)) - 1) : 0;
        return std::to_string((mask >> 24) & 0xFF) + "."
             + std::to_string((mask >> 16) & 0xFF) + "."
             + std::to_string((mask >> 8) & 0xFF) + "."
             + std::to_string(mask & 0xFF);
    }

    /**
     * @brief 执行命令，成功返回空字符串，失败返回错误信息
     */
    static std::string runCommand(const std::string& command) {
        std::array<char, 256> buffer{};
        std::string output;
#ifdef _WIN32
        FILE* pipe = ::_popen(command.c_str(), "r");
#else
        const auto finalCommand = command + " 2>&1";
        FILE* pipe = ::popen(finalCommand.c_str(), "r");
#endif
        if (!pipe) return "无法执行系统网络命令";

        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }

#ifdef _WIN32
        const int exitCode = ::_pclose(pipe);
#else
        const int exitCode = ::pclose(pipe);
#endif
        if (exitCode == 0) return {};

        output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
        while (!output.empty() && (output.back() == '\n' || output.back() == ' ' || output.back() == '\t')) {
            output.pop_back();
        }
#ifdef __linux__
        if (output.find("Operation not permitted") != std::string::npos) {
            return "缺少网络配置权限(CAP_NET_ADMIN/root): " + output;
        }
#endif
        if (output.empty()) return "系统网络命令执行失败: " + command;
        return output;
    }

    static std::vector<std::string> collectTargets(const std::string& bridgeName,
                                                   const std::vector<std::string>& ports) {
        std::vector<std::string> targets;
        if (isSafeInterfaceName(bridgeName)) {
            targets.push_back(bridgeName);
        }
        for (const auto& port : ports) {
            if (isSafeInterfaceName(port)) {
                targets.push_back(port);
            }
        }
        return targets;
    }

    static std::string reloadAndActivateNetworkd(const std::vector<std::string>& interfaces) {
        auto error = runCommand("networkctl reload");
        if (!error.empty()) return error;

        for (const auto& iface : interfaces) {
            if (!isSafeInterfaceName(iface)) continue;
            runCommand("networkctl reconfigure " + iface + " 2>/dev/null");
            runCommand("networkctl up " + iface + " 2>/dev/null");
        }
        return {};
    }

    static std::string runCommandOutputImpl(const std::string& command) {
        std::array<char, 256> buffer{};
        std::string output;
#ifdef _WIN32
        FILE* pipe = ::_popen(command.c_str(), "r");
#else
        FILE* pipe = ::popen(command.c_str(), "r");
#endif
        if (!pipe) return {};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
#ifdef _WIN32
        ::_pclose(pipe);
#else
        ::pclose(pipe);
#endif
        return output;
    }
};

using AgentNetworkConfigurator = EdgeNodeNetworkConfigurator;

}  // namespace agent_app
