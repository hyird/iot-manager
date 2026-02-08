#pragma once

#include "Agent.Config.hpp"
#include "AgentCapabilities.hpp"
#include "common/l2config/L2Protocol.hpp"

#ifdef __linux__
#include <dirent.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <functional>
#include <thread>

namespace agent_app {

/**
 * @brief L2 二层配置桥接器
 *
 * 在 ARM 设备上通过 AF_PACKET 原始套接字监听自定义 EtherType (0x88B5) 帧,
 * 支持设备发现和 TCP 隧道（用于免 IP 的 SSH 访问）。
 *
 * 接口选择策略:
 *  - 桥接口 (br0) → 优先监听（覆盖所有桥成员端口）
 *  - 桥成员口 (被桥接的 eth0/eth1) → 跳过（已被桥覆盖）
 *  - 独立物理口 → 监听
 *  - 虚拟/隧道接口 → 跳过
 *
 * 使用单个 AF_PACKET socket 绑定 ifindex=0 监听所有选中接口,
 * recvfrom 的 sockaddr_ll 确定帧来源接口, 在同一接口回复。
 *
 * 线程模型:
 *  - recvThread_: 阻塞 recvfrom() 接收 L2 帧
 *  - 每个 TCP 隧道连接有一个 readThread_ 从 sshd 读取数据
 */
class L2ConfigBridge {
public:
    /// @brief 发现回调，参数为收到请求的接口 MAC，返回 JSON payload
    using DiscoverHandler = std::function<std::string(const l2config::MacAddr& localMac)>;

    L2ConfigBridge() = default;
    ~L2ConfigBridge() { stop(); }

    L2ConfigBridge(const L2ConfigBridge&) = delete;
    L2ConfigBridge& operator=(const L2ConfigBridge&) = delete;

    void setDiscoverHandler(DiscoverHandler handler) {
        discoverHandler_ = std::move(handler);
    }

    /**
     * @brief 启动 L2 桥接
     * @param interfaceName 指定网口名（空则自动枚举所有合适接口）
     * @return 错误信息，空字符串表示成功
     */
    std::string start([[maybe_unused]] const std::string& interfaceName = "") {
#ifndef __linux__
        return "L2ConfigBridge only supported on Linux";
#else
        if (running_) return "";

        // 创建 AF_PACKET 原始套接字
        rawFd_ = ::socket(AF_PACKET, SOCK_RAW, htons(l2config::ETHERTYPE_L2CONFIG));
        if (rawFd_ < 0) {
            return "failed to create raw socket: " + std::string(::strerror(errno))
                   + " (need root or CAP_NET_RAW)";
        }

        // 枚举接口
        if (!interfaceName.empty()) {
            auto info = getInterfaceInfo(interfaceName);
            if (!info) {
                ::close(rawFd_);
                rawFd_ = -1;
                return "interface not found: " + interfaceName;
            }
            interfaces_.push_back(*info);
        } else {
            interfaces_ = enumerateInterfaces();
            if (interfaces_.empty()) {
                ::close(rawFd_);
                rawFd_ = -1;
                return "no suitable ethernet interface found";
            }
        }

        // 绑定到所有接口 (ifindex=0)
        sockaddr_ll sll{};
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(l2config::ETHERTYPE_L2CONFIG);
        sll.sll_ifindex = 0;
        if (::bind(rawFd_, reinterpret_cast<sockaddr*>(&sll), sizeof(sll)) < 0) {
            ::close(rawFd_);
            rawFd_ = -1;
            interfaces_.clear();
            return "failed to bind raw socket";
        }

        running_ = true;
        recvThread_ = std::thread([this]() { recvLoop(); });
        broadcastThread_ = std::thread([this]() { broadcastLoop(); });

        std::cout << "[L2Config] started on " << interfaces_.size() << " interface(s):";
        for (const auto& iface : interfaces_) {
            std::cout << " " << iface.name << "(" << l2config::macToString(iface.mac) << ")";
        }
        std::cout << std::endl;
        return "";
#endif
    }

    void stop() {
        if (!running_) return;
        running_ = false;

#ifdef __linux__
        if (rawFd_ >= 0) {
            ::shutdown(rawFd_, SHUT_RDWR);
            ::close(rawFd_);
            rawFd_ = -1;
        }
#endif

        if (broadcastThread_.joinable()) {
            broadcastThread_.join();
        }
        if (recvThread_.joinable()) {
            recvThread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(tunnelMutex_);
            for (auto& [connId, conn] : tunnels_) {
                conn.running = false;
#ifdef __linux__
                if (conn.tcpFd >= 0) {
                    ::shutdown(conn.tcpFd, SHUT_RDWR);
                    ::close(conn.tcpFd);
                    conn.tcpFd = -1;
                }
#endif
                if (conn.readThread.joinable()) {
                    conn.readThread.join();
                }
            }
            tunnels_.clear();
        }

        interfaces_.clear();
        std::cout << "[L2Config] stopped" << std::endl;
    }

    bool isRunning() const { return running_; }

    /// 返回第一个接口的 MAC（向后兼容）
    const l2config::MacAddr& localMac() const {
        static const l2config::MacAddr empty{};
        return interfaces_.empty() ? empty : interfaces_[0].mac;
    }

private:
    struct InterfaceInfo {
        int ifIndex = 0;
        std::string name;
        l2config::MacAddr mac{};
    };

#ifdef __linux__

    /**
     * @brief 通过 ioctl 获取接口信息
     */
    std::optional<InterfaceInfo> getInterfaceInfo(const std::string& name) {
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

        if (::ioctl(rawFd_, SIOCGIFINDEX, &ifr) < 0) return std::nullopt;
        int ifIndex = ifr.ifr_ifindex;

        if (::ioctl(rawFd_, SIOCGIFHWADDR, &ifr) < 0) return std::nullopt;
        l2config::MacAddr mac{};
        std::memcpy(mac.data(), ifr.ifr_hwaddr.sa_data, l2config::MAC_LEN);

        return InterfaceInfo{ifIndex, name, mac};
    }

    /**
     * @brief 检测接口是否为桥成员口（被桥接的端口）
     *
     * 桥成员口的 /sys/class/net/<if>/master 是指向桥接口的符号链接
     */
    static bool isBridgeSlave(const std::string& name) {
        const std::string path = "/sys/class/net/" + name + "/master";
        struct stat st{};
        return ::lstat(path.c_str(), &st) == 0;
    }

    /**
     * @brief 枚举所有应监听的接口
     *
     * 策略: 桥接口 + 独立物理口，排除桥成员口和虚拟接口
     */
    std::vector<InterfaceInfo> enumerateInterfaces() {
        std::vector<InterfaceInfo> result;

        DIR* dir = ::opendir("/sys/class/net");
        if (!dir) return result;

        while (auto* entry = ::readdir(dir)) {
            std::string name = entry->d_name;
            if (name == "." || name == ".." || name == "lo") continue;

            // 只要以太网类型 (ARPHRD_ETHER = 1)
            std::string typePath = "/sys/class/net/" + name + "/type";
            std::ifstream typeFile(typePath);
            int ifType = 0;
            if (!(typeFile >> ifType) || ifType != 1) continue;

            // 过滤虚拟/隧道接口 (tun, tap, veth, docker, ...)
            if (AgentCapabilitiesCollector::shouldFilterInterface(name)) continue;

            // 跳过桥成员口（它们已被桥接口覆盖）
            if (isBridgeSlave(name)) continue;

            // 桥接口允许通过（不再跳过），独立物理口也允许

            auto info = getInterfaceInfo(name);
            if (info) {
                result.push_back(*info);
                std::cout << "[L2Config] interface: " << name
                          << " MAC=" << l2config::macToString(info->mac)
                          << (AgentCapabilitiesCollector::isBridgeInterface(name) ? " [bridge]" : "")
                          << std::endl;
            }
        }
        ::closedir(dir);

        return result;
    }

    /**
     * @brief 根据 ifIndex 查找接口信息
     */
    const InterfaceInfo* findInterface(int ifIndex) const {
        for (const auto& iface : interfaces_) {
            if (iface.ifIndex == ifIndex) return &iface;
        }
        return nullptr;
    }

    /**
     * @brief 检查 MAC 是否为本机任一接口的 MAC
     */
    bool isLocalMac(const l2config::MacAddr& mac) const {
        for (const auto& iface : interfaces_) {
            if (iface.mac == mac) return true;
        }
        return false;
    }

    // ==================== 广播 & 收发循环 ====================

    /**
     * @brief 周期性广播 DISCOVER_RSP（信标模式）
     *
     * ARM 端主动广播设备信息，PC 端被动监听即可发现设备。
     * 同时保留 DISCOVER_REQ 的响应能力，支持 PC 主动扫描。
     */
    void broadcastLoop() {
        static constexpr int BROADCAST_INTERVAL_SEC = 3;

        while (running_) {
            for (const auto& iface : interfaces_) {
                std::string payload;
                if (discoverHandler_) {
                    payload = discoverHandler_(iface.mac);
                } else {
                    payload = R"({"mac":")" + l2config::macToString(iface.mac) + R"("})";
                }

                auto frames = l2config::buildFrames(
                    l2config::BROADCAST_MAC, iface.mac,
                    l2config::MsgType::DISCOVER_RSP, 0,
                    payload);
                sendFrames(frames, iface.ifIndex);
            }

            // 分段 sleep，以便快速响应 stop()
            for (int i = 0; i < BROADCAST_INTERVAL_SEC * 10 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void recvLoop() {
        uint8_t buf[2048];
        while (running_) {
            sockaddr_ll from{};
            socklen_t fromLen = sizeof(from);
            ssize_t n = ::recvfrom(rawFd_, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) {
                if (!running_) break;
                if (errno == EINTR) continue;
                std::cerr << "[L2Config] recvfrom error: " << ::strerror(errno) << std::endl;
                break;
            }

            // 只处理我们监听的接口上的帧
            const auto* iface = findInterface(from.sll_ifindex);
            if (!iface) continue;

            auto frameOpt = l2config::parseFrame(buf, static_cast<size_t>(n));
            if (!frameOpt) continue;

            const auto& frame = *frameOpt;

            // 忽略自己发出的帧
            if (isLocalMac(frame.srcMac)) continue;

            auto msgType = static_cast<l2config::MsgType>(frame.header.msgType);
            switch (msgType) {
                case l2config::MsgType::DISCOVER_REQ:
                    handleDiscoverReq(frame, *iface);
                    break;
                case l2config::MsgType::TCP_CONNECT:
                    handleTcpConnect(frame, *iface);
                    break;
                case l2config::MsgType::TCP_DATA:
                    handleTcpData(frame);
                    break;
                case l2config::MsgType::TCP_CLOSE:
                    handleTcpClose(frame);
                    break;
                default:
                    break;
            }
        }
    }

    // ==================== 设备发现 ====================

    void handleDiscoverReq(const l2config::ParsedFrame& frame, const InterfaceInfo& iface) {
        std::cout << "[L2Config] discovery request from " << l2config::macToString(frame.srcMac)
                  << " on " << iface.name << std::endl;

        std::string payload;
        if (discoverHandler_) {
            payload = discoverHandler_(iface.mac);
        } else {
            payload = R"({"mac":")" + l2config::macToString(iface.mac) + R"("})";
        }

        auto frames = l2config::buildFrames(
            frame.srcMac, iface.mac,
            l2config::MsgType::DISCOVER_RSP, 0,
            payload);
        sendFrames(frames, iface.ifIndex);
    }

    // ==================== TCP 隧道 ====================

    void handleTcpConnect(const l2config::ParsedFrame& frame, const InterfaceInfo& iface) {
        if (frame.payloadLen < 2) {
            sendTcpConnectFail(frame.srcMac, iface, frame.header.seqNum, "invalid payload");
            return;
        }

        uint16_t targetPort;
        std::memcpy(&targetPort, frame.payload, 2);
        targetPort = ntohs(targetPort);

        const uint16_t connId = frame.header.seqNum;
        std::cout << "[L2Config] TCP_CONNECT from " << l2config::macToString(frame.srcMac)
                  << " on " << iface.name << ", connId=" << connId
                  << ", port=" << targetPort << std::endl;

        int tcpFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (tcpFd < 0) {
            sendTcpConnectFail(frame.srcMac, iface, connId, "socket() failed");
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(targetPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(tcpFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(tcpFd);
            sendTcpConnectFail(frame.srcMac, iface, connId,
                               "connect to 127.0.0.1:" + std::to_string(targetPort) + " failed: "
                               + std::strerror(errno));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(tunnelMutex_);
            auto& conn = tunnels_[connId];
            conn.tcpFd = tcpFd;
            conn.connId = connId;
            conn.peerMac = frame.srcMac;
            conn.localMac = iface.mac;
            conn.ifIndex = iface.ifIndex;
            conn.running = true;
            conn.readThread = std::thread([this, connId]() {
                tunnelReadLoop(connId);
            });
        }

        auto frames = l2config::buildFrames(
            frame.srcMac, iface.mac,
            l2config::MsgType::TCP_CONNECTED, connId);
        sendFrames(frames, iface.ifIndex);

        std::cout << "[L2Config] tunnel established, connId=" << connId
                  << ", port=" << targetPort << std::endl;
    }

    void handleTcpData(const l2config::ParsedFrame& frame) {
        auto assembled = assembler_.feed(frame);
        if (!assembled) return;

        const uint16_t connId = frame.header.seqNum;

        std::lock_guard<std::mutex> lock(tunnelMutex_);
        auto it = tunnels_.find(connId);
        if (it == tunnels_.end() || !it->second.running) return;

        const auto& data = *assembled;
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(it->second.tcpFd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                closeTunnel(connId);
                break;
            }
            sent += static_cast<size_t>(n);
        }
    }

    void handleTcpClose(const l2config::ParsedFrame& frame) {
        const uint16_t connId = frame.header.seqNum;
        std::cout << "[L2Config] TCP_CLOSE received, connId=" << connId << std::endl;

        std::lock_guard<std::mutex> lock(tunnelMutex_);
        closeTunnelLocked(connId, false);
    }

    void tunnelReadLoop(uint16_t connId) {
        uint8_t buf[4096];
        l2config::MacAddr peerMac{};
        l2config::MacAddr localMac{};
        int ifIndex = 0;
        int tcpFd = -1;

        {
            std::lock_guard<std::mutex> lock(tunnelMutex_);
            auto it = tunnels_.find(connId);
            if (it == tunnels_.end()) return;
            peerMac = it->second.peerMac;
            localMac = it->second.localMac;
            ifIndex = it->second.ifIndex;
            tcpFd = it->second.tcpFd;
        }

        while (running_) {
            ssize_t n = ::recv(tcpFd, buf, sizeof(buf), 0);
            if (n <= 0) break;

            auto frames = l2config::buildFrames(
                peerMac, localMac,
                l2config::MsgType::TCP_DATA, connId,
                buf, static_cast<size_t>(n));

            if (rawFd_ >= 0) {
                sendFrames(frames, ifIndex);
            } else {
                break;
            }
        }

        std::lock_guard<std::mutex> lock(tunnelMutex_);
        closeTunnelLocked(connId, true);
    }

    void closeTunnel(uint16_t connId) {
        closeTunnelLocked(connId, true);
    }

    void closeTunnelLocked(uint16_t connId, bool sendClose) {
        auto it = tunnels_.find(connId);
        if (it == tunnels_.end()) return;

        auto& conn = it->second;
        if (!conn.running) return;

        conn.running = false;

        if (sendClose && rawFd_ >= 0) {
            auto frames = l2config::buildFrames(
                conn.peerMac, conn.localMac,
                l2config::MsgType::TCP_CLOSE, connId);
            sendFrames(frames, conn.ifIndex);
        }

        if (conn.tcpFd >= 0) {
            ::shutdown(conn.tcpFd, SHUT_RDWR);
            ::close(conn.tcpFd);
            conn.tcpFd = -1;
        }

        if (conn.readThread.joinable() && conn.readThread.get_id() != std::this_thread::get_id()) {
            conn.readThread.detach();
        }

        std::cout << "[L2Config] tunnel closed, connId=" << connId << std::endl;
    }

    void sendTcpConnectFail(const l2config::MacAddr& dstMac, const InterfaceInfo& iface,
                            uint16_t connId, const std::string& reason) {
        std::cout << "[L2Config] TCP_CONNECT_FAIL connId=" << connId << ": " << reason << std::endl;
        auto frames = l2config::buildFrames(
            dstMac, iface.mac,
            l2config::MsgType::TCP_CONNECT_FAIL, connId,
            reason);
        sendFrames(frames, iface.ifIndex);
    }

    // ==================== 帧发送 ====================

    void sendFrames(const std::vector<std::vector<uint8_t>>& frames, int ifIndex) {
        if (rawFd_ < 0) return;

        sockaddr_ll dest{};
        dest.sll_family = AF_PACKET;
        dest.sll_ifindex = ifIndex;
        dest.sll_halen = l2config::MAC_LEN;

        for (const auto& frame : frames) {
            std::memcpy(dest.sll_addr, frame.data(), l2config::MAC_LEN);
            ::sendto(rawFd_, frame.data(), frame.size(), 0,
                     reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        }
    }

#endif  // __linux__

    // ==================== 数据成员 ====================

    struct TunnelConnection {
        int tcpFd = -1;
        uint16_t connId = 0;
        l2config::MacAddr peerMac{};
        l2config::MacAddr localMac{};   // 建立隧道时的本地接口 MAC
        int ifIndex = 0;                // 建立隧道时的接口索引
        std::atomic<bool> running{false};
        std::thread readThread;
    };

    [[maybe_unused]] std::vector<InterfaceInfo> interfaces_;
    [[maybe_unused]] int rawFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread recvThread_;
    std::thread broadcastThread_;

    DiscoverHandler discoverHandler_;
    l2config::FragmentAssembler assembler_;

    std::map<uint16_t, TunnelConnection> tunnels_;
    std::mutex tunnelMutex_;
};

}  // namespace agent_app
