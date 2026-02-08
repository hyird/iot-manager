#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent_app {

/**
 * @brief Agent 端 Shell 会话管理器
 *
 * 通过 forkpty 创建 PTY，将 shell I/O 通过回调转发到 WebSocket 通道。
 * 同一时间只允许一个 shell 会话。
 */
class AgentShellManager {
public:
    using DataCallback = std::function<void(const std::string& data)>;
    using CloseCallback = std::function<void(int exitCode)>;

    AgentShellManager() = default;
    ~AgentShellManager() { close(); }

    AgentShellManager(const AgentShellManager&) = delete;
    AgentShellManager& operator=(const AgentShellManager&) = delete;

    bool isOpen() const { return running_.load(std::memory_order_relaxed); }

    /**
     * @brief 打开一个 shell 会话
     *
     * @param cols 终端列数
     * @param rows 终端行数
     * @param onData PTY 输出回调 (shell stdout/stderr → WebSocket)
     * @param onClose PTY 关闭回调
     * @return 空字符串表示成功，否则返回错误信息
     */
    std::string open(int cols, int rows, DataCallback onData, CloseCallback onClose) {
        if (running_.load(std::memory_order_relaxed)) {
            return "shell 会话已存在";
        }

#ifdef __linux__
        struct winsize ws {};
        ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
        ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);

        int masterFd = -1;
        pid_t pid = ::forkpty(&masterFd, nullptr, nullptr, &ws);

        if (pid < 0) {
            return std::string("forkpty 失败: ") + std::strerror(errno);
        }

        if (pid == 0) {
            // 子进程: 执行 shell
            // 设置环境变量
            ::setenv("TERM", "xterm-256color", 1);
            ::setenv("LANG", "en_US.UTF-8", 0);

            // 尝试常见 shell
            const char* shell = ::getenv("SHELL");
            if (!shell || ::access(shell, X_OK) != 0) shell = "/bin/bash";
            if (::access(shell, X_OK) != 0) shell = "/bin/sh";

            ::execlp(shell, shell, "-l", nullptr);
            // exec 失败
            ::_exit(127);
        }

        // 父进程
        masterFd_ = masterFd;
        childPid_ = pid;
        running_.store(true, std::memory_order_release);

        // 设置非阻塞读取
        int flags = ::fcntl(masterFd_, F_GETFL, 0);
        ::fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK);

        // 启动读取线程
        readThread_ = std::thread([this, onData = std::move(onData), onClose = std::move(onClose)]() {
            readLoop(std::move(onData), std::move(onClose));
        });

        std::cout << "[AgentShell] opened: pid=" << pid
                  << ", cols=" << ws.ws_col << ", rows=" << ws.ws_row << std::endl;
        return {};
#else
        (void)cols; (void)rows; (void)onData; (void)onClose;
        return "当前平台不支持 shell";
#endif
    }

    /**
     * @brief 向 PTY 写入数据 (前端键盘输入 → shell stdin)
     */
    void write(const std::string& data) {
#ifdef __linux__
        if (!running_.load(std::memory_order_relaxed) || masterFd_ < 0) return;
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = ::write(masterFd_, data.data() + written, data.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
#else
        (void)data;
#endif
    }

    /**
     * @brief 调整终端窗口大小
     */
    void resize(int cols, int rows) {
#ifdef __linux__
        if (!running_.load(std::memory_order_relaxed) || masterFd_ < 0) return;
        struct winsize ws {};
        ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
        ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
        ::ioctl(masterFd_, TIOCSWINSZ, &ws);
#else
        (void)cols; (void)rows;
#endif
    }

    /**
     * @brief 关闭 shell 会话
     */
    void close() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;

#ifdef __linux__
        if (masterFd_ >= 0) {
            ::close(masterFd_);
            masterFd_ = -1;
        }

        if (childPid_ > 0) {
            ::kill(childPid_, SIGHUP);
            int status = 0;
            // 等待子进程退出，最多 2 秒
            for (int i = 0; i < 20; ++i) {
                if (::waitpid(childPid_, &status, WNOHANG) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ::kill(childPid_, SIGKILL);
            ::waitpid(childPid_, &status, 0);
            childPid_ = -1;
        }
#endif

        if (readThread_.joinable()) {
            readThread_.join();
        }

        std::cout << "[AgentShell] closed" << std::endl;
    }

private:
#ifdef __linux__
    void readLoop(DataCallback onData, CloseCallback onClose) {
        constexpr size_t kBufSize = 16384;          // 增大读缓冲区
        constexpr int kFlushIntervalMs = 8;          // 合并窗口 8ms
        char buf[kBufSize];
        std::string pending;                         // 累积缓冲区

        struct pollfd pfd{};
        pfd.fd = masterFd_;
        pfd.events = POLLIN;

        while (running_.load(std::memory_order_relaxed)) {
            // 用 poll() 替代 sleep 轮询，立即响应数据到达
            int ret = ::poll(&pfd, 1, 50);           // 最长 50ms 超时
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (ret == 0) {
                // 超时 — 如果有累积数据则刷新
                if (!pending.empty() && onData) {
                    onData(std::move(pending));
                    pending.clear();
                }
                continue;
            }

            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                // 先读完可能残留的数据再退出
                while (true) {
                    ssize_t n = ::read(masterFd_, buf, kBufSize);
                    if (n <= 0) break;
                    pending.append(buf, static_cast<size_t>(n));
                }
                if (!pending.empty() && onData) {
                    onData(std::move(pending));
                    pending.clear();
                }
                break;
            }

            // 有数据可读 — 尽可能一次性读完
            bool gotData = false;
            while (true) {
                ssize_t n = ::read(masterFd_, buf, kBufSize);
                if (n > 0) {
                    pending.append(buf, static_cast<size_t>(n));
                    gotData = true;
                } else if (n == 0) {
                    // EOF
                    if (!pending.empty() && onData) {
                        onData(std::move(pending));
                        pending.clear();
                    }
                    goto loopEnd;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;  // 本轮读完了
                    }
                    if (errno == EIO) {
                        if (!pending.empty() && onData) {
                            onData(std::move(pending));
                            pending.clear();
                        }
                        goto loopEnd;
                    }
                    goto loopEnd;
                }
            }

            // 数据合并策略：如果累积超过 8KB 或已过合并窗口则立即发送
            if (gotData && !pending.empty()) {
                if (pending.size() >= 8192) {
                    if (onData) onData(std::move(pending));
                    pending.clear();
                } else {
                    // 短暂等待更多数据（合并小碎片）
                    int ret2 = ::poll(&pfd, 1, kFlushIntervalMs);
                    if (ret2 > 0 && (pfd.revents & POLLIN)) {
                        continue;  // 有更多数据，继续读
                    }
                    // 超时或无更多数据，发送
                    if (onData) onData(std::move(pending));
                    pending.clear();
                }
            }
        }

    loopEnd:
        // 收集子进程退出码
        int exitCode = -1;
        if (childPid_ > 0) {
            int status = 0;
            if (::waitpid(childPid_, &status, WNOHANG) > 0) {
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }

        running_.store(false, std::memory_order_release);

        if (onClose) {
            onClose(exitCode);
        }
    }
#endif

    std::atomic<bool> running_{false};
    std::thread readThread_;

#ifdef __linux__
    int masterFd_ = -1;
    pid_t childPid_ = -1;
#endif
};

}  // namespace agent_app
