#pragma once

#include "common/utils/Constants.hpp"

/**
 * @brief 链路连接信息
 */
struct LinkConnectionInfo {
    int linkId = 0;
    std::string name;
    std::string mode;           // "TCP Server" / "TCP Client"
    std::string ip;
    uint16_t port = 0;
    std::string connStatus;     // "stopped" / "listening" / "connected" / "connecting" / "error"
    std::string errorMsg;
    int clientCount = 0;
    std::vector<std::string> clients;
    std::string lastActivity;

    Json::Value toJson() const {
        Json::Value json;
        json["link_id"] = linkId;
        json["name"] = name;
        json["mode"] = mode;
        json["ip"] = ip;
        json["port"] = port;
        json["conn_status"] = connStatus;
        json["error_msg"] = errorMsg;
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
    EventLoop* loop = nullptr;                      // 该链路使用的 EventLoop
    mutable std::mutex connMutex;                   // 保护 serverConns 和 info 的并发访问
};

/**
 * @brief TCP 链路管理器（单例）
 * 优化版：每个链路独立 EventLoop，支持 Server 广播
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

        // 如果为 0，使用 CPU 核心数
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0) {
                numThreads = 4;  // 默认 4 线程
            }
        }

        // 创建独立的 EventLoopThreadPool
        ioLoopPool_ = std::make_unique<EventLoopThreadPool>(numThreads, "TcpIoPool");
        ioLoopPool_->start();

        initialized_ = true;
        LOG_INFO << "TcpLinkManager initialized with " << numThreads << " IO threads";
    }

    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const {
        return initialized_;
    }

    /**
     * @brief 启动 TCP Server
     */
    void startServer(int linkId, const std::string& name, const std::string& ip, uint16_t port) {
        // 先停止已存在的链路
        stop(linkId);

        auto loop = getNextLoop();
        auto addr = InetAddress(ip, port);
#ifdef _WIN32
        // Windows 不支持 SO_REUSEPORT，禁用以避免警告
        auto server = std::make_shared<TcpServer>(loop, addr, "LinkServer_" + std::to_string(linkId), true, false);
#else
        auto server = std::make_shared<TcpServer>(loop, addr, "LinkServer_" + std::to_string(linkId));
#endif

        // 初始化运行时信息
        auto runtime = std::make_shared<LinkRuntime>();
        runtime->server = server;
        runtime->loop = loop;
        runtime->info.linkId = linkId;
        runtime->info.name = name;
        runtime->info.mode = Constants::LINK_MODE_TCP_SERVER;
        runtime->info.ip = ip;
        runtime->info.port = port;
        runtime->info.connStatus = "listening";
        runtime->info.lastActivity = getCurrentTime();

        // 保存运行时（需要在设置回调前，因为回调可能立即触发）
        {
            std::unique_lock lock(mutex_);
            runtimes_[linkId] = runtime;
        }

        // 设置连接回调
        server->setConnectionCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn) {
            auto rt = runtimeWeak.lock();
            if (!rt) return;

            std::string clientAddr = conn->peerAddr().toIpPort();
            bool isConnected = conn->connected();
            {
                std::lock_guard<std::mutex> lock(rt->connMutex);
                if (isConnected) {
                    LOG_INFO << "[Link " << linkId << "] Client connected: " << clientAddr;
                    rt->serverConns.insert(conn);
                    updateRuntimeClientsLocked(rt);
                } else {
                    LOG_INFO << "[Link " << linkId << "] Client disconnected: " << clientAddr;
                    rt->serverConns.erase(conn);
                    updateRuntimeClientsLocked(rt);
                }
                rt->info.lastActivity = getCurrentTime();
            }

            // 通知连接状态变化（在锁外调用，避免死锁）
            if (connectionCallback_) {
                connectionCallback_(linkId, clientAddr, isConnected);
            }
        });

        // 设置消息回调
        server->setRecvMessageCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn, MsgBuffer* buf) {
            auto rt = runtimeWeak.lock();
            if (!rt) return;

            std::string data(buf->peek(), buf->readableBytes());
            buf->retrieveAll();

            std::string clientAddr = conn->peerAddr().toIpPort();
            LOG_INFO << "[Link " << linkId << "] Received from " << clientAddr
                     << " (" << data.size() << " bytes): " << toHexString(data);

            {
                std::lock_guard<std::mutex> lock(rt->connMutex);
                rt->info.lastActivity = getCurrentTime();
            }

            // 优先使用带客户端地址的回调
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
        // 先停止已存在的链路
        stop(linkId);

        auto loop = getNextLoop();

        // 初始化运行时信息（在当前线程创建）
        auto runtime = std::make_shared<LinkRuntime>();
        runtime->loop = loop;
        runtime->info.linkId = linkId;
        runtime->info.name = name;
        runtime->info.mode = Constants::LINK_MODE_TCP_CLIENT;
        runtime->info.ip = ip;
        runtime->info.port = port;
        runtime->info.connStatus = "connecting";
        runtime->info.lastActivity = getCurrentTime();

        // 保存运行时
        {
            std::unique_lock lock(mutex_);
            runtimes_[linkId] = runtime;
        }

        // 在 EventLoop 线程中创建 TcpClient 并设置回调
        loop->runInLoop([this, loop, linkId, name, ip, port, runtime]() {
            auto addr = InetAddress(ip, port);
            auto client = std::make_shared<TcpClient>(loop, addr, "LinkClient_" + std::to_string(linkId));

            runtime->client = client;

            // 设置连接回调
            client->setConnectionCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn) {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                std::string serverAddr = conn->peerAddr().toIpPort();
                bool isConnected = conn->connected();
                {
                    std::lock_guard<std::mutex> lock(rt->connMutex);
                    if (isConnected) {
                        LOG_INFO << "[Link " << linkId << "] Connected to server: " << serverAddr;
                        rt->clientConn = conn;
                        rt->info.connStatus = "connected";
                        rt->info.errorMsg = "";
                    } else {
                        LOG_INFO << "[Link " << linkId << "] Disconnected from server";
                        rt->clientConn.reset();
                        rt->info.connStatus = "connecting";  // 等待重连
                    }
                    rt->info.lastActivity = getCurrentTime();
                }

                // 通知连接状态变化（在锁外调用，避免死锁）
                if (connectionCallback_) {
                    connectionCallback_(linkId, serverAddr, isConnected);
                }
            });

            // 设置消息回调
            client->setMessageCallback([this, linkId, runtimeWeak = std::weak_ptr(runtime)](const TcpConnectionPtr& conn, MsgBuffer* buf) {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                std::string data(buf->peek(), buf->readableBytes());
                buf->retrieveAll();

                std::string serverAddr = conn->peerAddr().toIpPort();
                LOG_INFO << "[Link " << linkId << "] Received from server " << serverAddr
                         << " (" << data.size() << " bytes): " << toHexString(data);

                {
                    std::lock_guard<std::mutex> lock(rt->connMutex);
                    rt->info.lastActivity = getCurrentTime();
                }

                // 优先使用带客户端地址的回调（Client 模式传递服务器地址）
                if (dataCallbackWithClient_) {
                    dataCallbackWithClient_(linkId, serverAddr, data);
                } else if (dataCallback_) {
                    dataCallback_(linkId, data);
                }
            });

            // 设置连接错误回调
            client->setConnectionErrorCallback([linkId, runtimeWeak = std::weak_ptr(runtime)]() {
                auto rt = runtimeWeak.lock();
                if (!rt) return;

                LOG_WARN << "[Link " << linkId << "] Connection failed, will retry...";
                std::lock_guard<std::mutex> lock(rt->connMutex);
                rt->info.connStatus = "connecting";
                rt->info.errorMsg = "Connection failed, retrying...";
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

        // 在对应的 IO 线程中停止
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

        // 等待所有停止命令执行完成
        if (ioLoopPool_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    /**
     * @brief 检查链路是否正在运行
     */
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
        // 加锁后安全地获取 info
        std::lock_guard<std::mutex> connLock(runtime->connMutex);
        return runtime->info.toJson();
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
            result.append(runtime->info.toJson());
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

    /**
     * @brief 设置数据回调（不含客户端地址）
     */
    using DataCallback = std::function<void(int linkId, const std::string& data)>;
    void setDataCallback(DataCallback cb) {
        dataCallback_ = std::move(cb);
    }

    /**
     * @brief 设置数据回调（含客户端地址，用于多设备场景）
     */
    using DataCallbackWithClient = std::function<void(int linkId, const std::string& clientAddr, const std::string& data)>;
    void setDataCallbackWithClient(DataCallbackWithClient cb) {
        dataCallbackWithClient_ = std::move(cb);
    }

    /**
     * @brief 设置连接状态回调（用于清理断开连接的设备映射）
     */
    using ConnectionCallback = std::function<void(int linkId, const std::string& clientAddr, bool connected)>;
    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }

    /**
     * @brief 发送数据到指定链路
     */
    bool sendData(int linkId, const std::string& data) {
        std::shared_ptr<LinkRuntime> runtime;
        {
            std::shared_lock lock(mutex_);
            auto it = runtimes_.find(linkId);
            if (it == runtimes_.end()) return false;
            runtime = it->second;
        }

        std::lock_guard<std::mutex> connLock(runtime->connMutex);

        // TCP Client 模式
        if (runtime->clientConn && runtime->clientConn->connected()) {
            runtime->clientConn->send(data);
            LOG_DEBUG << "[Link " << linkId << "] Sent " << data.size() << " bytes to server";
            return true;
        }

        // TCP Server 模式：广播给所有客户端
        if (runtime->server && !runtime->serverConns.empty()) {
            for (const auto& conn : runtime->serverConns) {
                if (conn->connected()) {
                    conn->send(data);
                }
            }
            LOG_DEBUG << "[Link " << linkId << "] Broadcast " << data.size() << " bytes to "
                     << runtime->serverConns.size() << " clients";
            return true;
        }

        return false;
    }

    /**
     * @brief 发送数据到指定链路的指定客户端（Server 模式）
     */
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
                LOG_DEBUG << "[Link " << linkId << "] Sent " << data.size() << " bytes to " << clientAddr;
                return true;
            }
        }
        return false;
    }

private:
    TcpLinkManager() = default;

    ~TcpLinkManager() {
        stopAll();
        // 不等待线程池，让系统自然清理
        // 避免在程序退出时阻塞
    }

    TcpLinkManager(const TcpLinkManager&) = delete;
    TcpLinkManager& operator=(const TcpLinkManager&) = delete;

    EventLoop* getNextLoop() {
        // 如果已初始化，使用独立的 IO 线程池
        if (initialized_ && ioLoopPool_) {
            return ioLoopPool_->getNextLoop();
        }
        // 回退到主事件循环（未初始化时）
        return drogon::app().getLoop();
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

    static std::string toHexString(const std::string& data) {
        std::ostringstream oss;
        for (unsigned char c : data) {
            oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<int>(c) << " ";
        }
        return oss.str();
    }

private:
    mutable std::shared_mutex mutex_;
    std::map<int, std::shared_ptr<LinkRuntime>> runtimes_;
    DataCallback dataCallback_;
    DataCallbackWithClient dataCallbackWithClient_;
    ConnectionCallback connectionCallback_;

    // TCP 专用 IO 线程池
    std::unique_ptr<EventLoopThreadPool> ioLoopPool_;
    std::atomic<bool> initialized_{false};
};
