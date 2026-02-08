#pragma once

#ifdef _WIN32
// Windows 下不支持串口（Agent 仅在 Linux ARM 上运行）
class SerialLinkManager {
public:
    static SerialLinkManager& instance() {
        static SerialLinkManager inst;
        return inst;
    }

    using DataCallback = std::function<void(int linkId, const std::string& data)>;
    using ConnectionCallback = std::function<void(int linkId, bool connected)>;

    void setDataCallback(DataCallback) {}
    void setConnectionCallback(ConnectionCallback) {}
    void openPort(int, const std::string&, const std::string&, int) {}
    void closePort(int) {}
    void closeAll() {}
    bool sendData(int, const std::string&) { return false; }
    bool isOpen(int) const { return false; }
};

#else

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

/**
 * @brief 串口链路管理器（单例）
 *
 * 提供与 TcpLinkManager 兼容的 sendData / 回调接口，
 * 使 AgentModbusPoller 可以透明地通过串口发送/接收数据。
 *
 * 每个串口在独立线程中阻塞读取，数据通过回调传回。
 */
class SerialLinkManager {
public:
    using DataCallback = std::function<void(int linkId, const std::string& data)>;
    using ConnectionCallback = std::function<void(int linkId, bool connected)>;

    static SerialLinkManager& instance() {
        static SerialLinkManager inst;
        return inst;
    }

    void setDataCallback(DataCallback cb) {
        dataCallback_ = std::move(cb);
    }

    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }

    /**
     * @brief 打开串口
     * @param linkId  端点 key（与 TcpLinkManager 共享 key 空间）
     * @param name    显示名称
     * @param channel 串口设备路径，如 /dev/ttyS4
     * @param baudRate 波特率
     */
    void openPort(int linkId, const std::string& name,
                  const std::string& channel, int baudRate) {
        closePort(linkId);

        int fd = ::open(channel.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "[SerialLinkManager] failed to open " << channel
                      << ": " << std::strerror(errno) << std::endl;
            return;
        }

        // 清除 O_NONBLOCK，后续用阻塞读（带超时）
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        if (!configurePort(fd, baudRate)) {
            std::cerr << "[SerialLinkManager] failed to configure " << channel << std::endl;
            ::close(fd);
            return;
        }

        auto port = std::make_shared<PortRuntime>();
        port->fd = fd;
        port->linkId = linkId;
        port->name = name;
        port->channel = channel;
        port->baudRate = baudRate;
        port->running.store(true);

        {
            std::lock_guard lock(mutex_);
            ports_[linkId] = port;
        }

        // 启动读线程
        port->readThread = std::thread([this, port]() {
            readLoop(port);
        });

        std::cout << "[SerialLinkManager] opened " << channel
                  << " (baud=" << baudRate << ") as link " << linkId << std::endl;

        if (connectionCallback_) {
            connectionCallback_(linkId, true);
        }
    }

    void closePort(int linkId) {
        std::shared_ptr<PortRuntime> port;
        {
            std::lock_guard lock(mutex_);
            auto it = ports_.find(linkId);
            if (it == ports_.end()) return;
            port = it->second;
            ports_.erase(it);
        }

        port->running.store(false);
        if (port->fd >= 0) {
            ::close(port->fd);
            port->fd = -1;
        }
        if (port->readThread.joinable()) {
            port->readThread.join();
        }

        std::cout << "[SerialLinkManager] closed " << port->channel
                  << " (link " << linkId << ")" << std::endl;

        if (connectionCallback_) {
            connectionCallback_(linkId, false);
        }
    }

    void closeAll() {
        std::vector<int> ids;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, _] : ports_) {
                ids.push_back(id);
            }
        }
        for (int id : ids) {
            closePort(id);
        }
    }

    bool sendData(int linkId, const std::string& data) {
        std::shared_ptr<PortRuntime> port;
        {
            std::lock_guard lock(mutex_);
            auto it = ports_.find(linkId);
            if (it == ports_.end()) return false;
            port = it->second;
        }

        if (port->fd < 0 || !port->running.load()) return false;

        std::lock_guard writeLock(port->writeMutex);
        const auto* buf = reinterpret_cast<const uint8_t*>(data.data());
        size_t remaining = data.size();
        while (remaining > 0) {
            ssize_t n = ::write(port->fd, buf, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::cerr << "[SerialLinkManager] write error on link " << linkId
                          << ": " << std::strerror(errno) << std::endl;
                return false;
            }
            buf += n;
            remaining -= static_cast<size_t>(n);
        }

        // 等待数据发送完毕
        ::tcdrain(port->fd);
        return true;
    }

    bool isOpen(int linkId) const {
        std::lock_guard lock(mutex_);
        auto it = ports_.find(linkId);
        return it != ports_.end() && it->second->running.load();
    }

private:
    SerialLinkManager() = default;
    ~SerialLinkManager() { closeAll(); }
    SerialLinkManager(const SerialLinkManager&) = delete;
    SerialLinkManager& operator=(const SerialLinkManager&) = delete;

    struct PortRuntime {
        int fd = -1;
        int linkId = 0;
        std::string name;
        std::string channel;
        int baudRate = 0;
        std::atomic<bool> running{false};
        std::thread readThread;
        std::mutex writeMutex;
    };

    static speed_t toBaudConst(int baud) {
        switch (baud) {
            case 1200:   return B1200;
            case 2400:   return B2400;
            case 4800:   return B4800;
            case 9600:   return B9600;
            case 19200:  return B19200;
            case 38400:  return B38400;
            case 57600:  return B57600;
            case 115200: return B115200;
            case 230400: return B230400;
#ifdef B460800
            case 460800: return B460800;
#endif
#ifdef B921600
            case 921600: return B921600;
#endif
            default:     return B9600;
        }
    }

    static bool configurePort(int fd, int baudRate) {
        struct termios tty{};
        if (::tcgetattr(fd, &tty) != 0) return false;

        auto baud = toBaudConst(baudRate);
        ::cfsetispeed(&tty, baud);
        ::cfsetospeed(&tty, baud);

        // 8N1, 无流控
        tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
        tty.c_cflag |= CS8 | CLOCAL | CREAD;

        // 原始模式（无回显、无信号处理）
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

        // 禁用软件流控和特殊字符处理
        tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

        // 原始输出
        tty.c_oflag &= ~OPOST;

        // 读超时：VMIN=0, VTIME=2 → 最多等 200ms
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 2;

        ::tcflush(fd, TCIOFLUSH);
        return ::tcsetattr(fd, TCSANOW, &tty) == 0;
    }

    void readLoop(std::shared_ptr<PortRuntime> port) {
        constexpr size_t BUF_SIZE = 512;
        uint8_t buf[BUF_SIZE];

        while (port->running.load()) {
            ssize_t n = ::read(port->fd, buf, BUF_SIZE);
            if (n > 0) {
                std::string data(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
                if (dataCallback_) {
                    dataCallback_(port->linkId, data);
                }
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                std::cerr << "[SerialLinkManager] read error on " << port->channel
                          << ": " << std::strerror(errno) << std::endl;
                break;
            }
            // n == 0: VTIME 超时，继续循环
        }
    }

    mutable std::mutex mutex_;
    std::map<int, std::shared_ptr<PortRuntime>> ports_;
    DataCallback dataCallback_;
    ConnectionCallback connectionCallback_;
};

#endif
