#pragma once

#include <algorithm>
#include <cstring>
#include <set>

#ifdef __linux__
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#include "AgentNetworkConfigurator.hpp"

namespace agent_app {

class AgentCapabilitiesCollector {
public:
    static Json::Value collectCapabilities() {
        Json::Value capabilities(Json::objectValue);
        capabilities["network"] = Json::Value(Json::arrayValue);
        capabilities["interfaces"] = collectInterfaces();
        capabilities["supports"] = buildSupports();
        capabilities["network_backend"] = AgentNetworkConfigurator::backendName(
            AgentNetworkConfigurator::detectBackend());
        return capabilities;
    }

    static Json::Value collectRuntime(int managedLinkCount) {
        Json::Value runtime(Json::objectValue);
        runtime["hostname"] = detectHostname();
        runtime["managedLinkCount"] = managedLinkCount;
        runtime["arch"] = detectArch();
        runtime["platform"] = detectPlatform();
        return runtime;
    }

    /**
     * @brief 判断是否需要过滤的虚拟/隧道接口
     *
     * 过滤：lo、隧道(tun/tap/veth/docker/virbr)、CAN(can/vcan)、
     * wireguard(wg)、dummy、sit、gre 等
     * 注意：br- 桥接口不过滤，由 collectInterfaces() 中的桥接检测逻辑处理
     */
    static bool shouldFilterInterface(const std::string& name) {
        static const char* const prefixes[] = {
            "tun", "tap", "veth", "docker", "virbr",
            "can", "vcan", "wg", "dummy", "sit", "gre",
            "ip6tnl", "ip6gre", "erspan", "ifb",
            "ppp", "usb",
        };
        for (const auto* prefix : prefixes) {
            if (name.compare(0, std::strlen(prefix), prefix) == 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 判断接口是否为桥接口
     */
    static bool isBridgeInterface(const std::string& name) {
#ifdef __linux__
        const std::string path = "/sys/class/net/" + name + "/bridge";
        struct stat st{};
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#else
        (void)name;
        return false;
#endif
    }

private:
    /**
     * @brief 读取桥接口的成员端口列表
     *
     * 通过 /sys/class/net/<bridge>/brif/ 目录列出被桥接的端口
     */
    static std::vector<std::string> readBridgePorts([[maybe_unused]] const std::string& bridgeName) {
        std::vector<std::string> ports;
#ifdef __linux__
        const std::string path = "/sys/class/net/" + bridgeName + "/brif";
        DIR* dir = ::opendir(path.c_str());
        if (!dir) return ports;
        while (auto* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
                ports.push_back(name);
            }
        }
        ::closedir(dir);
        std::sort(ports.begin(), ports.end());
#endif
        return ports;
    }

    /**
     * @brief 检测接口的地址获取方式 (dhcp / static)
     *
     * systemd-networkd: 读取 .network 配置文件
     * netplan: 读取 yaml 配置
     * ifupdown: 读取 /etc/network/interfaces
     */
    static std::string detectInterfaceMethod([[maybe_unused]] const std::string& name) {
#ifdef __linux__
        const auto backend = AgentNetworkConfigurator::detectBackend();

        if (backend == AgentNetworkConfigurator::Backend::NETWORKD) {
            // 检查 systemd-networkd 的配置文件
            for (const auto& prefix : {"90-iot-", ""}) {
                const std::string path = "/etc/systemd/network/" + std::string(prefix) + name + ".network";
                std::ifstream ifs(path);
                if (!ifs) continue;
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                if (content.find("DHCP=yes") != std::string::npos ||
                    content.find("DHCP=ipv4") != std::string::npos) {
                    return "dhcp";
                }
                if (content.find("Address=") != std::string::npos) {
                    return "static";
                }
            }
            // networkctl 也可以查
            auto output = AgentNetworkConfigurator::runCommandOutput(
                "networkctl status " + name + " 2>/dev/null");
            if (output.find("DHCPv4") != std::string::npos) return "dhcp";
        }

        if (backend == AgentNetworkConfigurator::Backend::NETPLAN) {
            const std::string path = "/etc/netplan/90-iot-" + name + ".yaml";
            std::ifstream ifs(path);
            if (ifs) {
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                if (content.find("dhcp4: true") != std::string::npos) return "dhcp";
                if (content.find("addresses:") != std::string::npos) return "static";
            }
        }

        if (backend == AgentNetworkConfigurator::Backend::IFUPDOWN) {
            for (const auto& path : {
                std::string("/etc/network/interfaces.d/iot-") + name,
                std::string("/etc/network/interfaces")
            }) {
                std::ifstream ifs(path);
                if (!ifs) continue;
                std::string line;
                while (std::getline(ifs, line)) {
                    if (line.find("iface " + name + " ") != std::string::npos) {
                        if (line.find("dhcp") != std::string::npos) return "dhcp";
                        if (line.find("static") != std::string::npos) return "static";
                    }
                }
            }
        }

        // 兜底：检查是否有 dhclient/dhcpcd 进程在跑
        auto output = AgentNetworkConfigurator::runCommandOutput(
            "ps -eo args 2>/dev/null | grep -E 'dhclient|dhcpcd|udhcpc' | grep " + name);
        if (!output.empty() && output.find("grep") == std::string::npos) {
            return "dhcp";
        }
#endif
        return "unknown";
    }

    /**
     * @brief 检测接口的默认网关
     *
     * 解析 `ip route show dev <name>` 获取 default via X.X.X.X
     */
    static std::string detectDefaultGateway([[maybe_unused]] const std::string& name) {
#ifdef __linux__
        auto output = AgentNetworkConfigurator::runCommandOutput(
            "ip route show dev " + name + " 2>/dev/null");
        // 查找 "default via X.X.X.X" 行
        size_t pos = output.find("default via ");
        if (pos != std::string::npos) {
            pos += 12;  // skip "default via "
            size_t end = output.find_first_of(" \n\r", pos);
            if (end != std::string::npos) {
                return output.substr(pos, end - pos);
            }
            return output.substr(pos);
        }
#endif
        return "";
    }

    static Json::Value collectInterfaces() {
        Json::Value items(Json::arrayValue);

#ifdef __linux__
        ifaddrs* interfaces = nullptr;
        if (getifaddrs(&interfaces) != 0 || !interfaces) {
            return items;
        }

        std::map<std::string, Json::Value> interfaceMap;
        for (auto* ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || (ifa->ifa_flags & IFF_LOOPBACK)) {
                continue;
            }

            const std::string name = ifa->ifa_name;

            // 过滤隧道、CAN 等虚拟接口
            if (shouldFilterInterface(name)) {
                continue;
            }

            auto& item = interfaceMap[name];
            if (item.isNull()) {
                item = Json::Value(Json::objectValue);
                item["name"] = name;
                item["display_name"] = name;
                item["ip"] = "";
                item["prefix_length"] = 0;
            }
            item["up"] = (ifa->ifa_flags & IFF_UP) != 0;

            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
                continue;
            }

            char buffer[INET_ADDRSTRLEN] = {};
            auto* addr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            if (!::inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer))) {
                continue;
            }

            item["ip"] = buffer;
            item["prefix_length"] = static_cast<Json::Int>(resolvePrefixLength(ifa->ifa_netmask));
        }
        freeifaddrs(interfaces);

        // 检测桥接关系，记录被桥接的端口
        std::set<std::string> bridgedPorts;
        for (auto& [name, item] : interfaceMap) {
            if (isBridgeInterface(name)) {
                auto ports = readBridgePorts(name);
                if (!ports.empty()) {
                    Json::Value portsArray(Json::arrayValue);
                    for (const auto& port : ports) {
                        portsArray.append(port);
                        bridgedPorts.insert(port);
                    }
                    item["bridge_ports"] = portsArray;
                }
            }
        }

        // 为每个接口检测地址模式和默认网关
        for (auto& [name, item] : interfaceMap) {
            item["method"] = detectInterfaceMethod(name);
            item["gateway"] = detectDefaultGateway(name);
        }

        // 输出：桥接口排前面，被桥接的端口不单独输出
        for (auto& [name, item] : interfaceMap) {
            if (bridgedPorts.count(name)) continue;  // 被桥接端口跳过
            items.append(item);
        }

#elif defined(_WIN32)
        ULONG bufferSize = 15000;
        std::vector<uint8_t> buffer(bufferSize);
        auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

        ULONG ret = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addresses, &bufferSize);

        if (ret == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(bufferSize);
            addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            ret = GetAdaptersAddresses(
                AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, addresses, &bufferSize);
        }

        if (ret != NO_ERROR) {
            return items;
        }

        for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
            // 跳过回环和隧道接口
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                adapter->IfType == IF_TYPE_TUNNEL) {
                continue;
            }

            // 只保留以太网和无线网卡
            if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
                adapter->IfType != IF_TYPE_IEEE80211) {
                continue;
            }

            Json::Value item(Json::objectValue);
            item["name"] = adapter->AdapterName;

            // 转换 FriendlyName (wchar_t → UTF-8)
            std::string displayName;
            if (adapter->FriendlyName) {
                int len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                              nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    displayName.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                        displayName.data(), len, nullptr, nullptr);
                }
            }
            item["display_name"] = displayName.empty() ? adapter->AdapterName : displayName;
            item["up"] = (adapter->OperStatus == IfOperStatusUp);
            item["ip"] = "";
            item["prefix_length"] = 0;

            // 获取第一个 IPv4 地址
            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;

                auto* sockAddr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                char ipBuf[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &sockAddr->sin_addr, ipBuf, sizeof(ipBuf))) {
                    item["ip"] = ipBuf;
                    item["prefix_length"] = static_cast<Json::Int>(unicast->OnLinkPrefixLength);
                }
                break;
            }

            items.append(item);
        }
#endif

        return items;
    }

#ifdef __linux__
    static int resolvePrefixLength(const sockaddr* netmask) {
        if (!netmask || netmask->sa_family != AF_INET) {
            return 0;
        }

        auto* addr = reinterpret_cast<const sockaddr_in*>(netmask);
        auto value = ntohl(addr->sin_addr.s_addr);
        int count = 0;
        while (value != 0) {
            count += (value & 1U) ? 1 : 0;
            value >>= 1U;
        }
        return count;
    }
#endif

    static Json::Value buildSupports() {
        Json::Value supports(Json::objectValue);
        supports["tcp"] = true;
        supports["serial"] = false;
        supports["rawRelay"] = true;
        return supports;
    }

    static std::string detectHostname() {
#ifdef __linux__
        char hostname[256] = {};
        if (::gethostname(hostname, sizeof(hostname) - 1) == 0) {
            return hostname;
        }
#endif
        if (const char* env = std::getenv("HOSTNAME")) {
            return env;
        }
        if (const char* env = std::getenv("COMPUTERNAME")) {
            return env;
        }
        return "iot-agent";
    }

    static std::string detectArch() {
#if defined(__aarch64__)
        return "arm64";
#elif defined(__arm__)
        return "arm";
#elif defined(__x86_64__) || defined(_M_X64)
        return "x86_64";
#else
        return "unknown";
#endif
    }

    static std::string detectPlatform() {
#ifdef __linux__
        utsname info{};
        if (::uname(&info) == 0) {
            return info.sysname;
        }
        return "linux";
#elif defined(_WIN32)
        return "windows";
#else
        return "unknown";
#endif
    }
};

}  // namespace agent_app
