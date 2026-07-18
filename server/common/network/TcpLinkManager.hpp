#pragma once

#include "LinkState.hpp"

#include <cstddef>
#include <coroutine>

/**
 * @brief 链路连接信息（JSON 序列化用）
 */
struct LinkConnectionInfo {
    int linkId = 0;
    std::string targetId;
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
        if (!targetId.empty()) json["target_id"] = targetId;
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
    std::atomic_bool reconnectScheduled{false};

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
     * @brief 获取 TCP IO 线程池中的下一个 EventLoop（round-robin）
     * 供协议层定时任务使用，使其与 TCP 链路在同一线程池中运行。
     * 未初始化时回退到 Drogon 主 EventLoop。
     */
    EventLoop* getNextIoLoop() {
        if (initialized_ && ioLoopPool_) {
            return ioLoopPool_->getNextLoop();
        }
        return drogon::app().getLoop();
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
        startClientTarget(linkId, "legacy-" + std::to_string(linkId), name, ip, port);
    }

    /** 启动 TCP Client 链路中的一个独立目标连接。 */
    void startClientTarget(int linkId, const std::string& targetId, const std::string& name,
                           const std::string& ip, uint16_t port) {
        stopClientTarget(linkId, targetId);

        auto loop = getNextLoop();

        auto runtime = std::make_shared<LinkRuntime>();
        runtime->loop = loop;
        runtime->info.linkId = linkId;
        runtime->info.targetId = targetId;
        runtime->info.name = name;
        runtime->info.mode = Constants::LINK_MODE_TCP_CLIENT;
        runtime->info.ip = ip;
        runtime->info.port = port;
        runtime->info.lastActivity = getCurrentTime();
        runtime->fsm.onStartClient();

        {
            std::unique_lock lock(mutex_);
            clientRuntimes_[clientRuntimeKey(linkId, targetId)] = runtime;
        }

        loop->runInLoop([this, loop, linkId, targetId, ip, port, runtime]() {
            auto addr = InetAddress(ip, port);
            auto client = std::make_shared<TcpClient>(
                loop, addr, "LinkClient_" + std::to_string(linkId) + "_" + targetId);

            runtime->client = client;

            // 连接回调
            client->setConnectionCallback([this, linkId, targetId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn) {
                try {
                    auto rt = runtimeWeak.lock();
                    if (!rt) return;

                    std::string serverAddr = conn->peerAddr().toIpPort();
                    bool isConnected = conn->connected();
                    {
                        std::lock_guard<std::mutex> lock(rt->connMutex);
                        if (isConnected) {
                            LOG_INFO << "[Link " << linkId << "/Target " << targetId
                                     << "] Connected to server: " << serverAddr;
                            rt->clientConn = conn;
                            rt->reconnectScheduled.store(false, std::memory_order_release);
                            rt->fsm.onConnected();
                        } else {
                            LOG_INFO << "[Link " << linkId << "/Target " << targetId
                                     << "] Disconnected from server";
                            rt->clientConn.reset();
                            rt->fsm.onDisconnected();
                        }
                        rt->info.lastActivity = getCurrentTime();
                    }

                    if (connectionCallback_) {
                        connectionCallback_(linkId, serverAddr, isConnected);
                    }

                    if (!isConnected) {
                        scheduleTargetReconnect(linkId, targetId, runtimeWeak);
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
            client->setConnectionErrorCallback([this, linkId, targetId, runtimeWeak = std::weak_ptr(runtime)]() {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                {
                    std::lock_guard<std::mutex> lock(rt->connMutex);
                    rt->fsm.onConnectionError("Connection failed, retrying...");
                    LOG_WARN << "[Link " << linkId << "/Target " << targetId
                             << "] Connection failed (attempt "
                             << rt->fsm.reconnectAttempts() << "), will retry...";
                }

                scheduleTargetReconnect(linkId, targetId, runtimeWeak);
            });

            client->connect();

            LOG_INFO << "[Link " << linkId << "/Target " << targetId
                     << "] TCP Client connecting to " << ip << ":" << port;
        });
    }

    /** 按 JSONB 目标列表重载一个 TCP Client 链路。 */
    void reloadClientTargets(int linkId, const std::string& linkName,
                             const Json::Value& targets, bool enabled) {
        stop(linkId);
        if (!enabled || !targets.isArray()) return;

        for (const auto& target : targets) {
            if (target.get("status", "enabled").asString() != "enabled") continue;
            const auto targetId = target.get("id", "").asString();
            const auto ip = target.get("ip", "").asString();
            const auto port = target.get("port", 0).asInt();
            if (targetId.empty() || ip.empty() || port <= 0 || port > 65535) continue;
            auto targetName = target.get("name", "").asString();
            if (targetName.empty()) targetName = ip + ":" + std::to_string(port);
            startClientTarget(linkId, targetId, linkName + "/" + targetName,
                              ip, static_cast<uint16_t>(port));
        }
    }

    /**
     * @brief 停止链路
     */
    void stop(int linkId) {
        std::vector<std::shared_ptr<LinkRuntime>> runtimes;
        {
            std::unique_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it != runtimes_.end()) {
                runtimes.push_back(it->second);
                runtimes_.erase(it);
            }
            for (auto clientIt = clientRuntimes_.begin(); clientIt != clientRuntimes_.end();) {
                if (clientIt->second->info.linkId == linkId) {
                    runtimes.push_back(clientIt->second);
                    clientIt = clientRuntimes_.erase(clientIt);
                } else {
                    ++clientIt;
                }
            }
        }

        for (const auto& runtime : runtimes) {
            stopRuntime(runtime);
        }
    }

    void stopClientTarget(int linkId, const std::string& targetId) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::unique_lock lock(mutex_);
            auto it = clientRuntimes_.find(clientRuntimeKey(linkId, targetId));
            if (it == clientRuntimes_.end()) return;
            runtime = it->second;
            clientRuntimes_.erase(it);
        }
        stopRuntime(runtime);
    }

    /**
     * @brief 停止所有链路
     */
    void stopAll() {
        stopAllAsync(nullptr);
    }

    drogon::Task<> stopAllCoro() {
        struct Awaiter {
            TcpLinkManager& manager;
            trantor::EventLoop* resumeLoop{nullptr};

            bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<> handle) {
                resumeLoop = trantor::EventLoop::getEventLoopOfCurrentThread();
                manager.stopAllAsync([this, handle]() mutable {
                    if (resumeLoop) {
                        resumeLoop->queueInLoop([handle]() mutable {
                            handle.resume();
                        });
                    } else {
                        handle.resume();
                    }
                });
            }

            void await_resume() const noexcept {}
        };

        co_await Awaiter{*this};
    }

    void stopAllAsync(std::function<void()> onDone) {
        std::vector<std::shared_ptr<LinkRuntime>> toStop;
        {
            std::unique_lock lock(mutex_);
            for (auto& [id, runtime] : runtimes_) toStop.push_back(runtime);
            for (auto& [key, runtime] : clientRuntimes_) toStop.push_back(runtime);
            runtimes_.clear();
            clientRuntimes_.clear();
        }

        if (toStop.empty()) {
            if (onDone) {
                if (auto* currentLoop = trantor::EventLoop::getEventLoopOfCurrentThread()) {
                    currentLoop->queueInLoop(std::move(onDone));
                } else if (auto* appLoop = drogon::app().getLoop()) {
                    appLoop->queueInLoop(std::move(onDone));
                } else {
                    onDone();
                }
            }
            return;
        }

        auto remaining = std::make_shared<std::atomic_size_t>(toStop.size());
        auto finish = std::make_shared<std::function<void()>>(std::move(onDone));
        auto markDone = [remaining, finish]() {
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1 && *finish) {
                (*finish)();
            }
        };

        for (auto& runtime : toStop) {
            const auto id = runtime->info.linkId;
            const auto targetId = runtime->info.targetId;
            {
                std::lock_guard<std::mutex> lock(runtime->connMutex);
                runtime->fsm.onStop();
            }

            if (runtime->loop) {
                runtime->loop->runInLoop([runtime, id, targetId, markDone]() {
                    try {
                        if (runtime->server) {
                            runtime->server->stop();
                            LOG_INFO << "[Link " << id << "] TCP Server stopped";
                        }
                        if (runtime->client) {
                            runtime->client->disconnect();
                            LOG_INFO << "[Link " << id << (targetId.empty() ? "" : "/Target " + targetId)
                                     << "] TCP Client stopped";
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR << "[Link " << id << "] stopAll callback failed: " << e.what();
                    } catch (...) {
                        LOG_ERROR << "[Link " << id << "] stopAll callback failed with unknown exception";
                    }
                    markDone();
                });
            } else {
                markDone();
            }
        }
    }

    bool isRunning(int linkId) {
        std::shared_lock lock(mutex_);
        if (runtimes_.count(linkId) > 0) return true;
        return std::any_of(clientRuntimes_.begin(), clientRuntimes_.end(),
                           [linkId](const auto& item) {
                               return item.second->info.linkId == linkId;
                           });
    }

    /**
     * @brief 获取单个链路状态
     */
    Json::Value getStatus(int linkId) {
        std::shared_ptr<LinkRuntime> runtime;
        std::vector<std::shared_ptr<LinkRuntime>> clientRuntimes;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it != runtimes_.end()) {
                runtime = it->second;
            } else {
                for (const auto& [key, candidate] : clientRuntimes_) {
                    if (candidate->info.linkId == linkId) clientRuntimes.push_back(candidate);
                }
            }
        }
        if (runtime) {
            std::lock_guard<std::mutex> connLock(runtime->connMutex);
            return runtime->info.toJson(runtime->fsm);
        }
        return aggregateClientStatus(linkId, clientRuntimes);
    }

    /**
     * @brief 获取所有链路状态
     */
    Json::Value getAllStatus() {
        std::vector<int> linkIds;
        {
            std::shared_lock lock(mutex_);
            for (const auto& [id, runtime] : runtimes_) linkIds.push_back(id);
            for (const auto& [key, runtime] : clientRuntimes_) {
                linkIds.push_back(runtime->info.linkId);
            }
        }
        std::sort(linkIds.begin(), linkIds.end());
        linkIds.erase(std::unique(linkIds.begin(), linkIds.end()), linkIds.end());
        Json::Value result(Json::arrayValue);
        for (const auto linkId : linkIds) result.append(getStatus(linkId));
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
            if (it != runtimes_.end()) {
                runtime = it->second;
            } else {
                for (const auto& [key, candidate] : clientRuntimes_) {
                    if (candidate->info.linkId != linkId) continue;
                    if (runtime) return false; // 多目标链路必须显式指定 target_id
                    runtime = candidate;
                }
                if (!runtime) return false;
            }
        }

        std::lock_guard<std::mutex> connLock(runtime->connMutex);

        if (runtime->clientConn && runtime->clientConn->connected()) {
            runtime->clientConn->send(data);
            totalBytesTx_.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
            totalPacketsTx_.fetch_add(1, std::memory_order_relaxed);
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
            return true;
        }

        return false;
    }

    bool sendToTarget(int linkId, const std::string& targetId, const std::string& data) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = clientRuntimes_.find(clientRuntimeKey(linkId, targetId));
            if (it == clientRuntimes_.end()) return false;
            runtime = it->second;
        }
        std::lock_guard<std::mutex> connLock(runtime->connMutex);
        if (!runtime->clientConn || !runtime->clientConn->connected()) return false;
        runtime->clientConn->send(data);
        totalBytesTx_.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
        totalPacketsTx_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief 向链路的所有 Server 连接广播数据，但排除指定客户端
     *
     * 用于 Modbus TCP Server 模式：设备无 DTU 映射时，广播查询但排除已映射到其他设备的客户端，
     * 避免查询被错误的 DTU 响应导致数据串台。
     */
    bool sendDataExcluding(int linkId, const std::string& data, const std::set<std::string>& excludeAddrs) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return false;
            runtime = it->second;
        }

        std::lock_guard<std::mutex> connLock(runtime->connMutex);

        if (runtime->server && !runtime->serverConns.empty()) {
            int sentCount = 0;
            for (const auto& conn : runtime->serverConns) {
                if (conn->connected() && excludeAddrs.find(conn->peerAddr().toIpPort()) == excludeAddrs.end()) {
                    conn->send(data);
                    ++sentCount;
                }
            }
            if (sentCount > 0) {
                totalBytesTx_.fetch_add(static_cast<int64_t>(data.size()) * sentCount, std::memory_order_relaxed);
                totalPacketsTx_.fetch_add(sentCount, std::memory_order_relaxed);
                return true;
            }
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
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 断开链路下指定 clientAddr 的 Server 连接（用于 DTU 重绑定时关闭旧连接）
     */
    void disconnectServerClient(int linkId, const std::string& clientAddr) {
        forceDisconnectServerClient(linkId, clientAddr);
    }

    /**
     * @brief 强制断开链路下指定 clientAddr 的 Server 连接，并立即通知上层
     *
     * 用于业务层判定连接已经不可用的场景，例如发送失败。
     * 即使底层 socket 还停留在 FIN_WAIT1，也会先触发一次连接断开回调，
     * 让注册表和会话状态立即清理。
     */
    void forceDisconnectServerClient(int linkId, const std::string& clientAddr) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return;
            runtime = it->second;
        }

        {
            std::lock_guard<std::mutex> connLock(runtime->connMutex);
            for (const auto& conn : runtime->serverConns) {
                if (conn->connected() && conn->peerAddr().toIpPort() == clientAddr) {
                    LOG_INFO << "[Link " << linkId << "] Force disconnect old DTU session: " << clientAddr;
                    conn->shutdown();
                    break;
                }
            }
        }

        if (connectionCallback_) {
            connectionCallback_(linkId, clientAddr, false);
        }
    }

    /**
     * @brief 强制断开 TCP Client 链路，并立即通知上层
     *
     * 用于业务层判定链路已失效的场景，例如发送失败。
     * client 模式只有一个远端地址，因此按 linkId 即可定位。
     */
    void forceDisconnectClient(int linkId) {
        std::vector<std::shared_ptr<LinkRuntime>> runtimes;
        {
            std::shared_lock lock(mutex_);
            for (const auto& [key, runtime] : clientRuntimes_) {
                if (runtime->info.linkId == linkId) runtimes.push_back(runtime);
            }
        }
        for (const auto& runtime : runtimes) notifyClientDisconnected(runtime);
    }

    void forceDisconnectTarget(int linkId, const std::string& targetId) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = clientRuntimes_.find(clientRuntimeKey(linkId, targetId));
            if (it == clientRuntimes_.end()) return;
            runtime = it->second;
        }
        notifyClientDisconnected(runtime);
    }

    /**
     * @brief 断开链路的所有 Server 连接（用于注册包变更后强制设备重新注册）
     */
    void disconnectServerClients(int linkId) {
        std::shared_ptr<LinkRuntime> runtime;
        std::vector<std::string> disconnectedClients;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return;
            runtime = it->second;
        }

        int count = 0;
        {
            std::lock_guard<std::mutex> connLock(runtime->connMutex);
            for (const auto& conn : runtime->serverConns) {
                if (conn->connected()) {
                    disconnectedClients.push_back(conn->peerAddr().toIpPort());
                    conn->shutdown();
                    ++count;
                }
            }
        }
        if (count > 0) {
            LOG_INFO << "[Link " << linkId << "] Disconnected " << count
                     << " server clients for re-registration";
        }

        if (connectionCallback_) {
            for (const auto& clientAddr : disconnectedClients) {
                connectionCallback_(linkId, clientAddr, false);
            }
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
        for (const auto& [key, runtime] : clientRuntimes_) {
            if (runtime->info.linkId == linkId) return runtime->loop;
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

    static std::string clientRuntimeKey(int linkId, const std::string& targetId) {
        return std::to_string(linkId) + ":" + targetId;
    }

    void stopRuntime(const std::shared_ptr<LinkRuntime>& runtime) {
        if (!runtime) return;
        {
            std::lock_guard<std::mutex> lock(runtime->connMutex);
            runtime->fsm.onStop();
        }
        if (!runtime->loop) return;
        const auto linkId = runtime->info.linkId;
        const auto targetId = runtime->info.targetId;
        runtime->loop->runInLoop([runtime, linkId, targetId]() {
            if (runtime->server) {
                runtime->server->stop();
                LOG_INFO << "[Link " << linkId << "] TCP Server stopped";
            }
            if (runtime->client) {
                runtime->client->disconnect();
                LOG_INFO << "[Link " << linkId << "/Target " << targetId
                         << "] TCP Client stopped";
            }
        });
    }

    Json::Value aggregateClientStatus(
        int linkId, const std::vector<std::shared_ptr<LinkRuntime>>& runtimes) const {
        Json::Value result(Json::objectValue);
        result["link_id"] = linkId;
        result["mode"] = Constants::LINK_MODE_TCP_CLIENT;
        result["targets"] = Json::Value(Json::arrayValue);
        result["clients"] = Json::Value(Json::arrayValue);

        int connected = 0;
        std::string lastActivity;
        for (const auto& runtime : runtimes) {
            std::lock_guard<std::mutex> lock(runtime->connMutex);
            auto target = runtime->info.toJson(runtime->fsm);
            const auto activity = runtime->getLastActivityString();
            if (!activity.empty()) {
                target["last_activity"] = activity;
                if (activity > lastActivity) lastActivity = activity;
            }
            result["targets"].append(target);
            if (runtime->fsm.state() == LinkState::Connected) {
                ++connected;
                if (runtime->clientConn) {
                    result["clients"].append(runtime->clientConn->peerAddr().toIpPort());
                }
            }
        }
        result["client_count"] = connected;
        result["last_activity"] = lastActivity;
        if (runtimes.empty()) result["conn_status"] = "stopped";
        else if (connected == static_cast<int>(runtimes.size())) result["conn_status"] = "connected";
        else if (connected > 0) result["conn_status"] = "partial";
        else result["conn_status"] = "connecting";
        return result;
    }

    void notifyClientDisconnected(const std::shared_ptr<LinkRuntime>& runtime) {
        std::string remoteAddr;
        {
            std::lock_guard<std::mutex> lock(runtime->connMutex);
            if (runtime->clientConn) {
                remoteAddr = runtime->clientConn->peerAddr().toIpPort();
                runtime->clientConn->shutdown();
            } else if (!runtime->info.ip.empty() && runtime->info.port > 0) {
                remoteAddr = runtime->info.ip + ":" + std::to_string(runtime->info.port);
            }
        }
        if (!remoteAddr.empty() && connectionCallback_) {
            connectionCallback_(runtime->info.linkId, remoteAddr, false);
        }
    }

    /**
     * @brief 调度 TCP Client 断线重连（指数退避）
     *
     * 安全检查：runtime 已销毁、链路已被替换、已重连成功 → 均放弃重连
     */
    void scheduleTargetReconnect(int linkId, const std::string& targetId,
                                 const std::weak_ptr<LinkRuntime>& runtimeWeak) {
        auto rt = runtimeWeak.lock();
        if (!rt || !rt->loop) return;
        bool expected = false;
        if (!rt->reconnectScheduled.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }

        double delay;
        {
            std::lock_guard<std::mutex> lock(rt->connMutex);
            delay = rt->fsm.getReconnectDelay();
        }

        rt->loop->runAfter(delay, [this, linkId, targetId, runtimeWeak]() {
          try {
            auto rt2 = runtimeWeak.lock();
            if (!rt2) return;
            rt2->reconnectScheduled.store(false, std::memory_order_release);

            // 确认该 runtime 仍是当前链路的实例（未被 stop/reload 替换）
            {
                std::shared_lock lock(mutex_);
                auto it = clientRuntimes_.find(clientRuntimeKey(linkId, targetId));
                if (it == clientRuntimes_.end() || it->second != rt2) return;
            }

            std::string ip;
            uint16_t port;
            {
                std::lock_guard<std::mutex> lock(rt2->connMutex);
                if (rt2->fsm.state() == LinkState::Connected) return;
                rt2->fsm.onReconnecting();
                ip = rt2->info.ip;
                port = rt2->info.port;
            }

            LOG_INFO << "[Link " << linkId << "/Target " << targetId
                     << "] Attempting reconnection (attempt "
                     << rt2->fsm.reconnectAttempts() << ") to " << ip << ":" << port;
            if (rt2->client) rt2->client->connect();
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
    std::map<std::string, std::shared_ptr<LinkRuntime>> clientRuntimes_;
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
