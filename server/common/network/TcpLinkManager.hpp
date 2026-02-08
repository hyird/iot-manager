#pragma once

#include "LinkState.hpp"

/**
 * @brief 链路连接信息（JSON 序列化用）
 */
struct LinkConnectionInfo {
    int linkId = 0;
    std::string name;
    std::string mode;           // "TCP Server" / "TCP Client"
    std::string ip;
    uint16_t port = 0;
    int clientCount = 0;
    std::vector<std::string> clients;
    std::string lastActivity;

    /**
     * @brief 转换为 JSON（连接状态从状态机获取）
     */
    Json::Value toJson(const LinkStateMachine& fsm) const {
        Json::Value json;
        json["link_id"] = linkId;
        json["name"] = name;
        json["mode"] = mode;
        json["ip"] = ip;
        json["port"] = port;
        json["conn_status"] = fsm.stateString();
        json["error_msg"] = fsm.errorMsg();
        json["client_count"] = clientCount;
        Json::Value clientsArr(Json::arrayValue);
        for (const auto& client : clients) {
            clientsArr.append(client);
        }
        json["clients"] = clientsArr;
        json["last_activity"] = lastActivity;
        return json;
    }
};

/**
 * @brief 单个链路的运行时信息（包含连接管理）
 */
struct LinkRuntime {
    using TcpServer = trantor::TcpServer;
    using TcpClient = trantor::TcpClient;
    using TcpConnectionPtr = trantor::TcpConnectionPtr;
    using EventLoop = trantor::EventLoop;

    std::shared_ptr<TcpServer> server;
    std::shared_ptr<TcpClient> client;
    TcpConnectionPtr clientConn;                    // Client 模式的连接
    std::set<TcpConnectionPtr> serverConns;         // Server 模式的所有客户端连接
    LinkConnectionInfo info;
    LinkStateMachine fsm;                           // 状态机管理连接生命周期
    EventLoop* loop = nullptr;                      // 该链路使用的 EventLoop
    mutable std::mutex connMutex;                   // 保护 serverConns 和 info 的并发访问
    std::atomic<time_t> lastActivityAtomic{0};      // 无锁活动时间戳（消息回调高频更新用）

    /** 无锁记录活动时间（仅在消息回调等高频路径使用） */
    void recordActivity() {
        lastActivityAtomic.store(std::time(nullptr), std::memory_order_relaxed);
    }

    /** 获取活动时间字符串（展示用，在持锁的低频路径调用） */
    std::string getLastActivityString() const {
        time_t t = lastActivityAtomic.load(std::memory_order_relaxed);
        if (t == 0) return info.lastActivity;
        return trantor::Date(static_cast<int64_t>(t) * 1000000).toDbString();
    }
};

/**
 * @brief TCP 链路管理器（单例）
 *
 * 通过 LinkStateMachine 统一管理连接状态转换，
 * 通过 ReconnectPolicy 实现指数退避重连。
 */
class TcpLinkManager {
public:
    using TcpServer = trantor::TcpServer;
    using TcpClient = trantor::TcpClient;
    using TcpConnectionPtr = trantor::TcpConnectionPtr;
    using MsgBuffer = trantor::MsgBuffer;
    using EventLoop = trantor::EventLoop;
    using EventLoopThreadPool = trantor::EventLoopThreadPool;
    using InetAddress = trantor::InetAddress;

    static TcpLinkManager& instance() {
        static TcpLinkManager inst;
        return inst;
    }

    /**
     * @brief 初始化 TCP 线程池
     * @param numThreads 线程数量，0 表示使用 CPU 核心数
     */
    void initialize(size_t numThreads = 0) {
        if (initialized_) {
            LOG_WARN << "TcpLinkManager already initialized";
            return;
        }

        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0) {
                numThreads = 4;
            }
        }

        ioLoopPool_ = std::make_unique<EventLoopThreadPool>(numThreads, "TcpIoPool");
        ioLoopPool_->start();

        initialized_ = true;
        LOG_INFO << "TcpLinkManager initialized with " << numThreads << " IO threads";
    }

    bool isInitialized() const {
        return initialized_;
    }

    /**
     * @brief 启动 TCP Server
     */
    void startServer(int linkId, const std::string& name, const std::string& ip, uint16_t port) {
        stop(linkId);

        auto loop = getNextLoop();
        auto addr = InetAddress(ip, port);
#ifdef _WIN32
        auto server = std::make_shared<TcpServer>(loop, addr, "LinkServer_" + std::to_string(linkId), true, false);
#else
        auto server = std::make_shared<TcpServer>(loop, addr, "LinkServer_" + std::to_string(linkId));
#endif

        auto runtime = std::make_shared<LinkRuntime>();
        runtime->server = server;
        runtime->loop = loop;
        runtime->info.linkId = linkId;
        runtime->info.name = name;
        runtime->info.mode = Constants::LINK_MODE_TCP_SERVER;
        runtime->info.ip = ip;
        runtime->info.port = port;
        runtime->info.lastActivity = getCurrentTime();
        runtime->fsm.onStartServer();

        {
            std::unique_lock lock(mutex_);
            runtimes_[linkId] = runtime;
        }

        // 设置连接回调
        server->setConnectionCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn) {
            try {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                std::string clientAddr = conn->peerAddr().toIpPort();
                bool isConnected = conn->connected();
                {
                    std::lock_guard<std::mutex> lock(rt->connMutex);
                    if (isConnected) {
                        LOG_INFO << "[Link " << linkId << "] Client connected: " << clientAddr;
                        rt->serverConns.insert(conn);
                    } else {
                        LOG_INFO << "[Link " << linkId << "] Client disconnected: " << clientAddr;
                        rt->serverConns.erase(conn);
                    }
                    updateRuntimeClientsLocked(rt);
                    rt->info.lastActivity = getCurrentTime();
                }

                if (connectionCallback_) {
                    connectionCallback_(linkId, clientAddr, isConnected);
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "[Link " << linkId << "] Server connection callback error: " << e.what();
            }
        });

        // 设置消息回调
        server->setRecvMessageCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn, MsgBuffer* buf) {
            auto rt = runtimeWeak.lock();
            if (!rt) return;

            std::string data(buf->peek(), buf->readableBytes());
            buf->retrieveAll();

            std::string clientAddr = conn->peerAddr().toIpPort();
            LOG_TRACE << "[Link " << linkId << "] Recv " << data.size() << "B from " << clientAddr;

            totalBytesRx_.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
            totalPacketsRx_.fetch_add(1, std::memory_order_relaxed);

            // 无锁更新活动时间（高频消息路径避免锁竞争）
            rt->recordActivity();

            if (dataCallbackWithClient_) {
                dataCallbackWithClient_(linkId, clientAddr, data);
            } else if (dataCallback_) {
                dataCallback_(linkId, data);
            }
        });

        server->start();

        LOG_INFO << "[Link " << linkId << "] TCP Server started on " << ip << ":" << port;
    }

    /**
     * @brief 启动 TCP Client
     */
    void startClient(int linkId, const std::string& name, const std::string& ip, uint16_t port) {
        stop(linkId);

        auto loop = getNextLoop();

        auto runtime = std::make_shared<LinkRuntime>();
        runtime->loop = loop;
        runtime->info.linkId = linkId;
        runtime->info.name = name;
        runtime->info.mode = Constants::LINK_MODE_TCP_CLIENT;
        runtime->info.ip = ip;
        runtime->info.port = port;
        runtime->info.lastActivity = getCurrentTime();
        runtime->fsm.onStartClient();

        {
            std::unique_lock lock(mutex_);
            runtimes_[linkId] = runtime;
        }

        loop->runInLoop([this, loop, linkId, name, ip, port, runtime]() {
            auto addr = InetAddress(ip, port);
            auto client = std::make_shared<TcpClient>(loop, addr, "LinkClient_" + std::to_string(linkId));

            runtime->client = client;

            // 连接回调
            client->setConnectionCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn) {
                try {
                    auto rt = runtimeWeak.lock();
                    if (!rt) return;

                    std::string serverAddr = conn->peerAddr().toIpPort();
                    bool isConnected = conn->connected();
                    {
                        std::lock_guard<std::mutex> lock(rt->connMutex);
                        if (isConnected) {
                            LOG_INFO << "[Link " << linkId << "] Connected to server: " << serverAddr;
                            rt->clientConn = conn;
                            rt->fsm.onConnected();
                        } else {
                            LOG_INFO << "[Link " << linkId << "] Disconnected from server";
                            rt->clientConn.reset();
                            rt->fsm.onDisconnected();
                        }
                        rt->info.lastActivity = getCurrentTime();
                    }

                    if (connectionCallback_) {
                        connectionCallback_(linkId, serverAddr, isConnected);
                    }

                    if (!isConnected) {
                        scheduleReconnect(linkId, runtimeWeak);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR << "[Link " << linkId << "] Client connection callback error: " << e.what();
                }
            });

            // 消息回调
            client->setMessageCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn, MsgBuffer* buf) {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                std::string data(buf->peek(), buf->readableBytes());
                buf->retrieveAll();

                std::string serverAddr = conn->peerAddr().toIpPort();
                LOG_TRACE << "[Link " << linkId << "] Recv " << data.size() << "B from " << serverAddr;

                totalBytesRx_.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
                totalPacketsRx_.fetch_add(1, std::memory_order_relaxed);

                // 无锁更新活动时间（高频消息路径避免锁竞争）
                rt->recordActivity();

                if (dataCallbackWithClient_) {
                    dataCallbackWithClient_(linkId, serverAddr, data);
                } else if (dataCallback_) {
                    dataCallback_(linkId, data);
                }
            });

            // 连接错误回调
            client->setConnectionErrorCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)]() {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                {
                    std::lock_guard<std::mutex> lock(rt->connMutex);
                    rt->fsm.onConnectionError("Connection failed, retrying...");
                    LOG_WARN << "[Link " << linkId << "] Connection failed (attempt "
                             << rt->fsm.reconnectAttempts() << "), will retry...";
                }

                scheduleReconnect(linkId, runtimeWeak);
            });

            client->enableRetry();
            client->connect();

            LOG_INFO << "[Link " << linkId << "] TCP Client connecting to " << ip << ":" << port;
        });
    }

    /**
     * @brief 停止链路
     */
    void stop(int linkId) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::unique_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return;
            runtime = it->second;
            runtimes_.erase(it);
        }

        {
            std::lock_guard<std::mutex> lock(runtime->connMutex);
            runtime->fsm.onStop();
        }

        if (runtime->loop) {
            runtime->loop->runInLoop([runtime, linkId]() {
                if (runtime->server) {
                    runtime->server->stop();
                    LOG_INFO << "[Link " << linkId << "] TCP Server stopped";
                }
                if (runtime->client) {
                    runtime->client->disconnect();
                    LOG_INFO << "[Link " << linkId << "] TCP Client stopped";
                }
            });
        }
    }

    /**
     * @brief 停止所有链路
     */
    void stopAll() {
        std::map<int, std::shared_ptr<LinkRuntime>> toStop;
        {
            std::unique_lock lock(mutex_);
            toStop.swap(runtimes_);
        }

        for (auto& [id, runtime] : toStop) {
            {
                std::lock_guard<std::mutex> lock(runtime->connMutex);
                runtime->fsm.onStop();
            }

            if (runtime->loop) {
                runtime->loop->runInLoop([runtime, id]() {
                    if (runtime->server) {
                        runtime->server->stop();
                        LOG_INFO << "[Link " << id << "] TCP Server stopped";
                    }
                    if (runtime->client) {
                        runtime->client->disconnect();
                        LOG_INFO << "[Link " << id << "] TCP Client stopped";
                    }
                });
            }
        }

        // 无需阻塞等待：lambda 通过 shared_ptr 拷贝捕获 runtime，
        // IO 线程回调完成后自动释放，生命周期由引用计数保证
    }

    bool isRunning(int linkId) {
        std::shared_lock lock(mutex_);
        return runtimes_.count(linkId) > 0;
    }

    /**
     * @brief 获取单个链路状态
     */
    Json::Value getStatus(int linkId) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) {
                Json::Value empty;
                empty["link_id"] = linkId;
                empty["conn_status"] = "stopped";
                return empty;
            }
            runtime = it->second;
        }
        std::lock_guard<std::mutex> connLock(runtime->connMutex);
        return runtime->info.toJson(runtime->fsm);
    }

    /**
     * @brief 获取所有链路状态
     */
    Json::Value getAllStatus() {
        std::vector<std::shared_ptr<LinkRuntime>> runtimesCopy;
        {
            std::shared_lock lock(mutex_);
            for (const auto& [id, runtime] : runtimes_) {
                runtimesCopy.push_back(runtime);
            }
        }
        Json::Value result(Json::arrayValue);
        for (const auto& runtime : runtimesCopy) {
            std::lock_guard<std::mutex> connLock(runtime->connMutex);
            // 同步原子时间戳到 info（低频展示路径才做字符串格式化）
            auto atomicTime = runtime->getLastActivityString();
            if (!atomicTime.empty()) runtime->info.lastActivity = atomicTime;
            result.append(runtime->info.toJson(runtime->fsm));
        }
        return result;
    }

    /**
     * @brief 重载链路
     */
    void reload(int linkId, const std::string& name, const std::string& mode,
                const std::string& ip, uint16_t port, bool enabled) {
        if (!enabled) {
            stop(linkId);
            return;
        }

        if (mode == Constants::LINK_MODE_TCP_SERVER) {
            startServer(linkId, name, ip, port);
        } else if (mode == Constants::LINK_MODE_TCP_CLIENT) {
            startClient(linkId, name, ip, port);
        }
    }

    // ==================== 回调设置 ====================

    using DataCallback = std::function<void(int linkId, const std::string& data)>;
    void setDataCallback(DataCallback cb) {
        dataCallback_ = std::move(cb);
    }

    using DataCallbackWithClient = std::function<void(int linkId, const std::string& clientAddr, const std::string& data)>;
    void setDataCallbackWithClient(DataCallbackWithClient cb) {
        dataCallbackWithClient_ = std::move(cb);
    }

    using ConnectionCallback = std::function<void(int linkId, const std::string& clientAddr, bool connected)>;
    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }

    // ==================== 数据发送 ====================

    bool sendData(int linkId, const std::string& data) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return false;
            runtime = it->second;
        }

        std::lock_guard<std::mutex> connLock(runtime->connMutex);

        if (runtime->clientConn && runtime->clientConn->connected()) {
            runtime->clientConn->send(data);
            totalBytesTx_.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
            totalPacketsTx_.fetch_add(1, std::memory_order_relaxed);
            LOG_TRACE << "[Link " << linkId << "] Sent " << data.size() << "B";
            return true;
        }

        if (runtime->server && !runtime->serverConns.empty()) {
            int sentCount = 0;
            for (const auto& conn : runtime->serverConns) {
                if (conn->connected()) {
                    conn->send(data);
                    ++sentCount;
                }
            }
            totalBytesTx_.fetch_add(static_cast<int64_t>(data.size()) * sentCount, std::memory_order_relaxed);
            totalPacketsTx_.fetch_add(sentCount, std::memory_order_relaxed);
            LOG_DEBUG << "[Link " << linkId << "] Broadcast " << data.size() << " bytes to "
                     << runtime->serverConns.size() << " clients";
            return true;
        }

        return false;
    }

    bool sendToClient(int linkId, const std::string& clientAddr, const std::string& data) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return false;
            runtime = it->second;
        }

        std::lock_guard<std::mutex> connLock(runtime->connMutex);
        for (const auto& conn : runtime->serverConns) {
            if (conn->connected() && conn->peerAddr().toIpPort() == clientAddr) {
                conn->send(data);
                totalBytesTx_.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
                totalPacketsTx_.fetch_add(1, std::memory_order_relaxed);
                LOG_TRACE << "[Link " << linkId << "] Sent " << data.size() << "B to " << clientAddr;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 断开链路的所有 Server 连接（用于注册包变更后强制设备重新注册）
     */
    void disconnectServerClients(int linkId) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return;
            runtime = it->second;
        }

        std::lock_guard<std::mutex> connLock(runtime->connMutex);
        int count = 0;
        for (const auto& conn : runtime->serverConns) {
            if (conn->connected()) {
                conn->shutdown();
                ++count;
            }
        }
        if (count > 0) {
            LOG_INFO << "[Link " << linkId << "] Disconnected " << count
                     << " server clients for re-registration";
        }
    }

    /** 吞吐量统计 */
    struct TcpStats {
        int64_t bytesRx;
        int64_t bytesTx;
        int64_t packetsRx;
        int64_t packetsTx;
    };

    TcpStats getTcpStats() const {
        return {
            totalBytesRx_.load(std::memory_order_relaxed),
            totalBytesTx_.load(std::memory_order_relaxed),
            totalPacketsRx_.load(std::memory_order_relaxed),
            totalPacketsTx_.load(std::memory_order_relaxed)
        };
    }

    EventLoop* getLinkLoop(int linkId) const {
        std::shared_lock lock(mutex_);
        auto it = runtimes_.find(linkId);
        if (it != runtimes_.end()) {
            return it->second->loop;
        }
        return nullptr;
    }

private:
    TcpLinkManager() = default;

    ~TcpLinkManager() {
        stopAll();
    }

    TcpLinkManager(const TcpLinkManager&) = delete;
    TcpLinkManager& operator=(const TcpLinkManager&) = delete;

    EventLoop* getNextLoop() {
        if (initialized_ && ioLoopPool_) {
            return ioLoopPool_->getNextLoop();
        }
        return drogon::app().getLoop();
    }

    /**
     * @brief 调度 TCP Client 断线重连（指数退避）
     *
     * 安全检查：runtime 已销毁、链路已被替换、已重连成功 → 均放弃重连
     */
    void scheduleReconnect(int linkId, const std::weak_ptr<LinkRuntime>& runtimeWeak) {
        auto rt = runtimeWeak.lock();
        if (!rt || !rt->loop) return;

        double delay;
        {
            std::lock_guard<std::mutex> lock(rt->connMutex);
            delay = rt->fsm.getReconnectDelay();
        }

        rt->loop->runAfter(delay, [this, linkId, runtimeWeak]() {
          try {
            auto rt2 = runtimeWeak.lock();
            if (!rt2) return;

            // 确认该 runtime 仍是当前链路的实例（未被 stop/reload 替换）
            {
                std::shared_lock lock(mutex_);
                auto it = runtimes_.find(linkId);
                if (it == runtimes_.end() || it->second != rt2) return;
            }

            std::string name, ip;
            uint16_t port;
            {
                std::lock_guard<std::mutex> lock(rt2->connMutex);
                if (rt2->fsm.state() == LinkState::Connected) return;
                rt2->fsm.onReconnecting();
                name = rt2->info.name;
                ip = rt2->info.ip;
                port = rt2->info.port;
            }

            LOG_INFO << "[Link " << linkId << "] Attempting reconnection (attempt "
                     << rt2->fsm.reconnectAttempts() << ") to " << ip << ":" << port;
            startClient(linkId, name, ip, port);
          } catch (const std::exception& e) {
            LOG_ERROR << "[Link " << linkId << "] Reconnect exception: " << e.what();
          } catch (...) {
            LOG_ERROR << "[Link " << linkId << "] Reconnect unknown exception";
          }
        });
    }

    // 注意：调用此函数时必须持有 rt->connMutex
    void updateRuntimeClientsLocked(const std::shared_ptr<LinkRuntime>& rt) {
        rt->info.clients.clear();
        for (const auto& conn : rt->serverConns) {
            rt->info.clients.push_back(conn->peerAddr().toIpPort());
        }
        rt->info.clientCount = static_cast<int>(rt->info.clients.size());
    }

    static std::string getCurrentTime() {
        return trantor::Date::now().toDbString();
    }

private:
    mutable std::shared_mutex mutex_;
    std::map<int, std::shared_ptr<LinkRuntime>> runtimes_;
    DataCallback dataCallback_;
    DataCallbackWithClient dataCallbackWithClient_;
    ConnectionCallback connectionCallback_;

    std::unique_ptr<EventLoopThreadPool> ioLoopPool_;
    std::atomic<bool> initialized_{false};

    // 吞吐量计数器（原子操作，无锁）
    std::atomic<int64_t> totalBytesRx_{0};
    std::atomic<int64_t> totalBytesTx_{0};
    std::atomic<int64_t> totalPacketsRx_{0};
    std::atomic<int64_t> totalPacketsTx_{0};
};
