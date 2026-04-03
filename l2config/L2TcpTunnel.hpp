#pragma once

/**
 * @brief L2 TCP 隧道 - 将本地 TCP 连接通过 L2 帧转发到远程设备
 *
 * PC 端: 监听 localhost:port，SSH 客户端连接后，
 * 通过 L2 帧与 ARM 设备的 sshd 建立双向隧道。
 */

#include "L2Transport.hpp"
#include "common/l2config/L2Protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace l2config_tool {

class L2TcpTunnel {
public:
    L2TcpTunnel(L2Transport& transport, const l2config::MacAddr& targetMac)
        : transport_(transport), targetMac_(targetMac) {}

    ~L2TcpTunnel() { stop(); }

    L2TcpTunnel(const L2TcpTunnel&) = delete;
    L2TcpTunnel& operator=(const L2TcpTunnel&) = delete;

    /**
     * @brief 外部转发帧给隧道处理
     *
     * 当 L2ConfigApp 管理 frameCallback 时，通过此方法将帧转发给隧道。
     */
    void handleFrame(const uint8_t* data, size_t len) {
        if (!running_) return;
        onL2Frame(data, len);
    }

    /**
     * @brief 启动隧道：监听本地 TCP 端口，等待 SSH 客户端连接
     * @param localPort 本地监听端口（默认 2222）
     * @param remotePort 远程目标端口（默认 22，即 sshd）
     * @param setCallback 是否自行设置 transport 帧回调（CLI 模式 true，GUI 模式 false）
     * @return 错误信息，空字符串表示成功
     */
    std::string start(uint16_t localPort = 2222, uint16_t remotePort = 22, bool setCallback = true) {
        if (running_) return "";

        localPort_ = localPort;
        remotePort_ = remotePort;

        // 创建 TCP 监听 socket
        listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            return "socket() failed: " + std::to_string(WSAGetLastError());
        }

        // 允许端口复用
        int optval = 1;
        ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&optval), sizeof(optval));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(localPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only

        if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            ::closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
            return "bind(127.0.0.1:" + std::to_string(localPort) + ") failed: "
                   + std::to_string(WSAGetLastError());
        }

        if (::listen(listenSocket_, 1) == SOCKET_ERROR) {
            ::closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
            return "listen() failed";
        }

        running_ = true;

        // 注册 L2 帧回调（CLI 模式下自行管理，GUI 模式由 L2ConfigApp 转发）
        if (setCallback) {
            transport_.setFrameCallback([this](const uint8_t* data, size_t len) {
                onL2Frame(data, len);
            });
        }

        // 启动 accept 线程
        acceptThread_ = std::thread([this]() { acceptLoop(); });

        return "";
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        // 关闭监听 socket（解除 accept 阻塞）
        if (listenSocket_ != INVALID_SOCKET) {
            ::closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }

        // 关闭客户端连接
        closeClient();

        // 通知等待中的条件变量
        {
            std::lock_guard<std::mutex> lock(connectMutex_);
            connectResult_ = ConnectResult::FAILED;
            connectCv_.notify_all();
        }

        if (acceptThread_.joinable()) acceptThread_.join();
        if (tcpReadThread_.joinable()) tcpReadThread_.join();
    }

    bool isRunning() const { return running_; }

private:
    enum class ConnectResult { PENDING, OK, FAILED };

    /**
     * @brief Accept 循环：接受 SSH 客户端连接并建立 L2 隧道
     */
    void acceptLoop() {
        while (running_) {
            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            SOCKET clientSock = ::accept(listenSocket_,
                                         reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
            if (clientSock == INVALID_SOCKET) {
                if (!running_) break;
                continue;
            }

            std::cout << "[L2Tunnel] SSH client connected" << std::endl;

            // 只允许一个客户端
            closeClient();

            clientSocket_ = clientSock;
            connId_ = l2config::nextSeqNum();

            // 发送 TCP_CONNECT 到设备
            uint16_t portBE = htons(remotePort_);
            auto frames = l2config::buildFrames(
                targetMac_, transport_.localMac(),
                l2config::MsgType::TCP_CONNECT, connId_,
                reinterpret_cast<const uint8_t*>(&portBE), 2);
            transport_.sendFrames(frames);

            // 等待 TCP_CONNECTED 或 TCP_CONNECT_FAIL
            {
                std::unique_lock<std::mutex> lock(connectMutex_);
                connectResult_ = ConnectResult::PENDING;
                bool ok = connectCv_.wait_for(lock,
                    std::chrono::seconds(static_cast<int>(l2config::TCP_CONNECT_TIMEOUT_SEC)),
                    [this]() { return connectResult_ != ConnectResult::PENDING; });

                if (!ok || connectResult_ != ConnectResult::OK) {
                    std::cerr << "[L2Tunnel] connect to device failed (timeout or rejected)" << std::endl;
                    closeClient();
                    continue;
                }
            }

            std::cout << "[L2Tunnel] tunnel established, connId=" << connId_ << std::endl;

            // 启动 TCP→L2 转发线程
            if (tcpReadThread_.joinable()) tcpReadThread_.join();
            tcpReadThread_ = std::thread([this]() { tcpReadLoop(); });

            // 等待连接结束
            if (tcpReadThread_.joinable()) {
                tcpReadThread_.join();
            }

            std::cout << "[L2Tunnel] tunnel closed, waiting for next connection..." << std::endl;
        }
    }

    /**
     * @brief 从 SSH 客户端读取数据，通过 L2 帧发送给设备
     */
    void tcpReadLoop() {
        char buf[4096];
        while (running_ && clientSocket_ != INVALID_SOCKET) {
            int n = ::recv(clientSocket_, buf, sizeof(buf), 0);
            if (n <= 0) break;

            auto frames = l2config::buildFrames(
                targetMac_, transport_.localMac(),
                l2config::MsgType::TCP_DATA, connId_,
                reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
            transport_.sendFrames(frames);
        }

        // SSH 客户端断开，通知设备端关闭隧道
        if (running_) {
            auto frames = l2config::buildFrames(
                targetMac_, transport_.localMac(),
                l2config::MsgType::TCP_CLOSE_MSG, connId_);
            transport_.sendFrames(frames);
        }

        closeClient();
    }

    /**
     * @brief 处理从设备收到的 L2 帧
     */
    void onL2Frame(const uint8_t* data, size_t len) {
        auto frameOpt = l2config::parseFrame(data, len);
        if (!frameOpt) return;

        const auto& frame = *frameOpt;

        // 忽略自己发出的帧
        if (frame.srcMac == transport_.localMac()) return;

        auto msgType = static_cast<l2config::MsgType>(frame.header.msgType);
        switch (msgType) {
            case l2config::MsgType::TCP_CONNECTED:
                if (frame.header.seqNum == connId_) {
                    std::lock_guard<std::mutex> lock(connectMutex_);
                    connectResult_ = ConnectResult::OK;
                    connectCv_.notify_all();
                }
                break;

            case l2config::MsgType::TCP_CONNECT_FAIL:
                if (frame.header.seqNum == connId_) {
                    std::string reason(frame.payload, frame.payload + frame.payloadLen);
                    std::cerr << "[L2Tunnel] device rejected: " << reason << std::endl;
                    std::lock_guard<std::mutex> lock(connectMutex_);
                    connectResult_ = ConnectResult::FAILED;
                    connectCv_.notify_all();
                }
                break;

            case l2config::MsgType::TCP_DATA:
                if (frame.header.seqNum == connId_) {
                    handleTcpData(frame);
                }
                break;

            case l2config::MsgType::TCP_CLOSE_MSG:
                if (frame.header.seqNum == connId_) {
                    std::cout << "[L2Tunnel] device closed connection" << std::endl;
                    closeClient();
                }
                break;

            default:
                break;
        }
    }

    /**
     * @brief 处理设备发来的 TCP 数据，转发给本地 SSH 客户端
     */
    void handleTcpData(const l2config::ParsedFrame& frame) {
        auto assembled = assembler_.feed(frame);
        if (!assembled) return;

        if (clientSocket_ == INVALID_SOCKET) return;

        const auto& data = *assembled;
        size_t sent = 0;
        while (sent < data.size()) {
            int n = ::send(clientSocket_,
                           reinterpret_cast<const char*>(data.data() + sent),
                           static_cast<int>(data.size() - sent), 0);
            if (n == SOCKET_ERROR) {
                closeClient();
                break;
            }
            sent += static_cast<size_t>(n);
        }
    }

    void closeClient() {
        if (clientSocket_ != INVALID_SOCKET) {
            ::shutdown(clientSocket_, SD_BOTH);
            ::closesocket(clientSocket_);
            clientSocket_ = INVALID_SOCKET;
        }
    }

    // ==================== 数据成员 ====================

    L2Transport& transport_;
    l2config::MacAddr targetMac_;

    uint16_t localPort_ = 2222;
    uint16_t remotePort_ = 22;
    uint16_t connId_ = 0;

    SOCKET listenSocket_ = INVALID_SOCKET;
    SOCKET clientSocket_ = INVALID_SOCKET;

    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::thread tcpReadThread_;

    l2config::FragmentAssembler assembler_;

    // TCP_CONNECT 等待结果
    std::mutex connectMutex_;
    std::condition_variable connectCv_;
    ConnectResult connectResult_ = ConnectResult::PENDING;
};

}  // namespace l2config_tool
