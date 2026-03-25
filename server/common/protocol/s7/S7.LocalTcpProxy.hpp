#pragma once

#include <atomic>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace s7 {

class S7LocalTcpProxy {
public:
    using ConnectionCallback = std::function<void(const std::string& clientAddr, bool connected)>;
    using DataCallback = std::function<void(const std::string& clientAddr, std::vector<uint8_t> bytes)>;

    S7LocalTcpProxy() = default;

    ~S7LocalTcpProxy() {
        stop();
    }

    S7LocalTcpProxy(const S7LocalTcpProxy&) = delete;
    S7LocalTcpProxy& operator=(const S7LocalTcpProxy&) = delete;

    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }

    void setDataCallback(DataCallback cb) {
        dataCallback_ = std::move(cb);
    }

    std::string start(std::string listenHost = "127.0.0.1", std::uint16_t listenPort = 102) {
        if (listenPort == 0) {
            listenPort = 102;
        }
        listenHost = normalizeHost(std::move(listenHost));
        if (listenHost.empty()) {
            return "S7 proxy listen host is invalid";
        }

        {
            std::lock_guard lock(stateMutex_);
            if (running_ && listenHost_ == listenHost && listenPort_ == listenPort) {
                return {};
            }
        }

        stop();

        if (!ensureSocketSubsystem()) {
            return "S7 proxy socket subsystem initialization failed";
        }

        SocketHandle listenSocket = createSocket();
        if (!isValidSocket(listenSocket)) {
            return "S7 proxy listen socket creation failed";
        }

        constexpr int reuse = 1;
        (void)::setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in listenAddr{};
        listenAddr.sin_family = AF_INET;
        listenAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        listenAddr.sin_port = htons(listenPort);

        if (::bind(listenSocket, reinterpret_cast<sockaddr*>(&listenAddr), sizeof(listenAddr)) != 0) {
            const std::string error = makeSocketError("bind(127.0.0.1:" + std::to_string(listenPort) + ") failed");
            closeSocket(listenSocket);
            return error;
        }

        if (::listen(listenSocket, 16) != 0) {
            const std::string error = makeSocketError("listen() failed");
            closeSocket(listenSocket);
            return error;
        }

        sockaddr_in boundAddr{};
        SocketLength boundLen = sizeof(boundAddr);
        if (::getsockname(listenSocket, reinterpret_cast<sockaddr*>(&boundAddr), &boundLen) != 0) {
            const std::string error = makeSocketError("getsockname() failed");
            closeSocket(listenSocket);
            return error;
        }

        {
            std::lock_guard lock(stateMutex_);
            listenHost_ = std::move(listenHost);
            listenPort_ = ntohs(boundAddr.sin_port);
            listenSocket_ = listenSocket;
            running_ = true;
        }

        try {
            acceptThread_ = std::thread([this]() { acceptLoop(); });
        } catch (...) {
            stop();
            throw;
        }

        return {};
    }

    void stop() {
        std::thread acceptThread;
        std::vector<std::thread> clientThreads;
        std::vector<std::shared_ptr<ClientState>> clientStates;

        {
            std::lock_guard lock(stateMutex_);
            running_ = false;

            if (isValidSocket(listenSocket_)) {
                closeSocket(listenSocket_);
                listenSocket_ = kInvalidSocket;
            }

            acceptThread = std::move(acceptThread_);
            clientThreads = std::move(clientThreads_);

            clientStates.reserve(clients_.size());
            for (auto& [clientAddr, state] : clients_) {
                (void)clientAddr;
                clientStates.push_back(state);
            }
        }

        for (const auto& state : clientStates) {
            if (!state) continue;
            std::lock_guard lock(state->mutex);
            if (isValidSocket(state->socket)) {
                shutdownSocket(state->socket);
                closeSocket(state->socket);
            }
            state->connected = false;
        }

        if (acceptThread.joinable()) {
            acceptThread.join();
        }
        for (auto& thread : clientThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        {
            std::lock_guard lock(stateMutex_);
            clients_.clear();
            listenPort_ = 0;
        }
    }

    bool isRunning() const {
        return running_.load(std::memory_order_acquire);
    }

    std::uint16_t listenPort() const {
        std::lock_guard lock(stateMutex_);
        return listenPort_;
    }

    std::string listenHost() const {
        std::lock_guard lock(stateMutex_);
        return listenHost_;
    }

    bool sendToClient(const std::string& clientAddr, const std::string& data) {
        return sendToClient(clientAddr, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

    bool sendToClient(const std::string& clientAddr, const std::vector<uint8_t>& bytes) {
        return sendToClient(clientAddr, bytes.data(), bytes.size());
    }

    void disconnectClient(const std::string& clientAddr) {
        std::shared_ptr<ClientState> state;
        {
            std::lock_guard lock(stateMutex_);
            auto it = clients_.find(clientAddr);
            if (it == clients_.end()) {
                return;
            }
            state = it->second;
        }

        if (!state) {
            return;
        }

        std::lock_guard lock(state->mutex);
        if (state->connected && isValidSocket(state->socket)) {
            state->connected = false;
            shutdownSocket(state->socket);
            closeSocket(state->socket);
        }
    }

    std::size_t clientCount() const {
        std::lock_guard lock(stateMutex_);
        std::size_t count = 0;
        for (const auto& [_, state] : clients_) {
            if (state && state->connected) {
                ++count;
            }
        }
        return count;
    }

    std::vector<std::string> clientAddrs() const {
        std::vector<std::string> result;
        std::lock_guard lock(stateMutex_);
        result.reserve(clients_.size());
        for (const auto& [clientAddr, state] : clients_) {
            if (state && state->connected) {
                result.push_back(clientAddr);
            }
        }
        return result;
    }

private:
#ifdef _WIN32
    using SocketHandle = SOCKET;
    using SocketLength = int;
    static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
    using SocketHandle = int;
    using SocketLength = socklen_t;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif

    struct ClientState {
        SocketHandle socket = kInvalidSocket;
        std::string clientAddr;
        bool connected = true;
        mutable std::mutex mutex;
    };

    static std::string normalizeHost(std::string host) {
        for (auto& ch : host) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (host.empty() || host == "0.0.0.0" || host == "*" || host == "localhost" ||
            host == "::" || host == "[::]" || host == "::1") {
            return "127.0.0.1";
        }
        return host;
    }

    static bool ensureSocketSubsystem() {
#ifdef _WIN32
        static std::once_flag once;
        static bool ok = false;
        std::call_once(once, []() {
            WSADATA data{};
            ok = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
        });
        return ok;
#else
        return true;
#endif
    }

    static SocketHandle createSocket() {
        return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    static bool isValidSocket(SocketHandle socket) {
        return socket != kInvalidSocket;
    }

    static void closeSocket(SocketHandle& socket) {
        if (!isValidSocket(socket)) {
            socket = kInvalidSocket;
            return;
        }

        shutdownSocket(socket);
#ifdef _WIN32
        (void)::closesocket(socket);
#else
        (void)::close(socket);
#endif
        socket = kInvalidSocket;
    }

    static void shutdownSocket(SocketHandle socket) {
        if (!isValidSocket(socket)) {
            return;
        }
#ifdef _WIN32
        (void)::shutdown(socket, SD_BOTH);
#else
        (void)::shutdown(socket, SHUT_RDWR);
#endif
    }

    static std::string makeSocketError(const std::string& prefix) {
#ifdef _WIN32
        return prefix + ": " + std::to_string(::WSAGetLastError());
#else
        return prefix + ": " + std::to_string(errno);
#endif
    }

    static std::string sockaddrToString(const sockaddr_in& addr) {
        char ip[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), ip, sizeof(ip)) == nullptr) {
            return "127.0.0.1";
        }
        return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    bool sendToClient(const std::string& clientAddr, const std::uint8_t* data, std::size_t size) {
        if (size == 0) {
            return true;
        }

        std::shared_ptr<ClientState> state;
        {
            std::lock_guard lock(stateMutex_);
            auto it = clients_.find(clientAddr);
            if (it == clients_.end()) {
                return false;
            }
            state = it->second;
        }

        if (!state) {
            return false;
        }

        SocketHandle socket = kInvalidSocket;
        {
            std::lock_guard lock(state->mutex);
            if (!state->connected || !isValidSocket(state->socket)) {
                return false;
            }
            socket = state->socket;
        }

        return sendAll(socket, data, size);
    }

    static bool sendAll(SocketHandle socket, const std::uint8_t* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const int rc = ::send(
                socket,
                reinterpret_cast<const char*>(data + sent),
                static_cast<int>(size - sent),
                0
            );
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    void acceptLoop() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in clientAddr{};
            SocketLength addrLen = sizeof(clientAddr);
            SocketHandle clientSocket = ::accept(
                listenSocket_,
                reinterpret_cast<sockaddr*>(&clientAddr),
                &addrLen
            );
            if (!isValidSocket(clientSocket)) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            auto state = std::make_shared<ClientState>();
            state->socket = clientSocket;
            state->clientAddr = sockaddrToString(clientAddr);

            {
                std::lock_guard lock(stateMutex_);
                clients_[state->clientAddr] = state;
                clientThreads_.emplace_back([this, state]() { clientLoop(state); });
            }

            if (connectionCallback_) {
                connectionCallback_(state->clientAddr, true);
            }
        }
    }

    void clientLoop(std::shared_ptr<ClientState> state) {
        std::array<std::uint8_t, 4096> buffer{};

        while (running_.load(std::memory_order_acquire)) {
            SocketHandle socket = kInvalidSocket;
            {
                std::lock_guard lock(state->mutex);
                if (!state->connected || !isValidSocket(state->socket)) {
                    break;
                }
                socket = state->socket;
            }

            const int received = ::recv(
                socket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()),
                0
            );
            if (received <= 0) {
                break;
            }

            if (dataCallback_) {
                dataCallback_(
                    state->clientAddr,
                    std::vector<uint8_t>(buffer.begin(), buffer.begin() + received)
                );
            }
        }

        bool shouldNotify = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->connected) {
                shouldNotify = true;
            }
            state->connected = false;
            if (isValidSocket(state->socket)) {
                shutdownSocket(state->socket);
                closeSocket(state->socket);
            }
        }

        if (shouldNotify && connectionCallback_) {
            connectionCallback_(state->clientAddr, false);
        }

        std::lock_guard lock(stateMutex_);
        auto it = clients_.find(state->clientAddr);
        if (it != clients_.end() && it->second == state) {
            clients_.erase(it);
        }
    }

    mutable std::mutex stateMutex_;
    std::atomic<bool> running_{false};
    std::string listenHost_ = "127.0.0.1";
    std::uint16_t listenPort_ = 0;
    SocketHandle listenSocket_ = kInvalidSocket;
    std::thread acceptThread_;
    std::vector<std::thread> clientThreads_;
    std::unordered_map<std::string, std::shared_ptr<ClientState>> clients_;
    ConnectionCallback connectionCallback_;
    DataCallback dataCallback_;
};

}  // namespace s7
