#pragma once

/**
 * @brief WebSocket 连接会话（存储在连接 context 中）
 */
struct WsSession {
    int userId = 0;
    std::string username;
};

/**
 * @brief WebSocket 连接管理器（单例）
 *
 * 管理所有 WebSocket 连接，支持广播和定向推送。
 * 线程安全：使用 shared_mutex 保护连接注册表。
 *
 * 消息格式：
 * { "type": "device:realtime", "data": {...}, "ts": 1234567890 }
 */
class WebSocketManager {
public:
    using WebSocketConnectionPtr = drogon::WebSocketConnectionPtr;

    static WebSocketManager& instance() {
        static WebSocketManager mgr;
        return mgr;
    }

    static constexpr size_t MAX_CONNECTIONS_PER_USER = 10;
    static constexpr size_t MAX_TOTAL_CONNECTIONS = 10000;

    /** 注册连接（含连接数上限保护） */
    void addConnection(int userId, const WebSocketConnectionPtr& conn) {
        std::unique_lock lock(mutex_);
        if (allConns_.size() >= MAX_TOTAL_CONNECTIONS) {
            LOG_WARN << "[WS] Max total connections reached (" << MAX_TOTAL_CONNECTIONS
                     << "), rejecting User#" << userId;
            conn->forceClose();
            return;
        }
        auto& userSet = userConns_[userId];
        if (userSet.size() >= MAX_CONNECTIONS_PER_USER) {
            LOG_WARN << "[WS] Max connections per user reached (" << MAX_CONNECTIONS_PER_USER
                     << ") for User#" << userId;
            conn->forceClose();
            return;
        }
        userSet.insert(conn);
        allConns_.insert(conn);
        LOG_INFO << "[WS] User#" << userId << " connected, total: " << allConns_.size();
    }

    /** 移除连接 */
    void removeConnection(const WebSocketConnectionPtr& conn) {
        std::unique_lock lock(mutex_);
        allConns_.erase(conn);

        if (conn->hasContext()) {
            auto session = conn->getContext<WsSession>();
            if (session) {
                auto it = userConns_.find(session->userId);
                if (it != userConns_.end()) {
                    it->second.erase(conn);
                    if (it->second.empty()) {
                        userConns_.erase(it);
                    }
                }
                LOG_INFO << "[WS] User#" << session->userId
                         << " disconnected, total: " << allConns_.size();
            }
        }
    }

    /** 广播给所有连接（先快照再发送，避免持锁期间调用 send） */
    void broadcast(const std::string& type, const Json::Value& data) {
        auto msg = buildMessage(type, data);
        std::vector<WebSocketConnectionPtr> snapshot;
        {
            std::shared_lock lock(mutex_);
            if (allConns_.empty()) return;
            snapshot.assign(allConns_.begin(), allConns_.end());
        }
        for (const auto& conn : snapshot) {
            if (conn->connected()) {
                conn->send(msg);
            }
        }
    }

    /** 发送给指定用户（先快照再发送，避免持锁期间调用 send） */
    void sendToUser(int userId, const std::string& type, const Json::Value& data) {
        auto msg = buildMessage(type, data);
        std::vector<WebSocketConnectionPtr> snapshot;
        {
            std::shared_lock lock(mutex_);
            auto it = userConns_.find(userId);
            if (it == userConns_.end()) return;
            snapshot.assign(it->second.begin(), it->second.end());
        }
        for (const auto& conn : snapshot) {
            if (conn->connected()) {
                conn->send(msg);
            }
        }
    }

    /** 获取当前连接数 */
    size_t connectionCount() const {
        std::shared_lock lock(mutex_);
        return allConns_.size();
    }

    /** 获取在线用户数（去重） */
    size_t onlineUserCount() const {
        std::shared_lock lock(mutex_);
        return userConns_.size();
    }

    /** 构建 JSON 消息字符串（静态 builder 避免高频分配） */
    static std::string buildMessage(const std::string& type, const Json::Value& data) {
        static const Json::StreamWriterBuilder builder = []() {
            Json::StreamWriterBuilder b;
            b["indentation"] = "";
            return b;
        }();
        Json::Value msg(Json::objectValue);
        msg["type"] = type;
        msg["data"] = data;
        msg["ts"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        return Json::writeString(builder, msg);
    }

private:
    WebSocketManager() = default;

    std::unordered_set<WebSocketConnectionPtr> allConns_;
    std::unordered_map<int, std::unordered_set<WebSocketConnectionPtr>> userConns_;
    mutable std::shared_mutex mutex_;
};
