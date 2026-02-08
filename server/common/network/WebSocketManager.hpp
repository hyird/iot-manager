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

    /** 注册连接 */
    void addConnection(int userId, const WebSocketConnectionPtr& conn) {
        std::unique_lock lock(mutex_);
        userConns_[userId].insert(conn);
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

    /** 广播给所有连接 */
    void broadcast(const std::string& type, const Json::Value& data) {
        auto msg = buildMessage(type, data);
        std::shared_lock lock(mutex_);
        if (allConns_.empty()) return;
        for (const auto& conn : allConns_) {
            if (conn->connected()) {
                conn->send(msg);
            }
        }
    }

    /** 发送给指定用户 */
    void sendToUser(int userId, const std::string& type, const Json::Value& data) {
        auto msg = buildMessage(type, data);
        std::shared_lock lock(mutex_);
        auto it = userConns_.find(userId);
        if (it != userConns_.end()) {
            for (const auto& conn : it->second) {
                if (conn->connected()) {
                    conn->send(msg);
                }
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

    /** 构建 JSON 消息字符串 */
    static std::string buildMessage(const std::string& type, const Json::Value& data) {
        Json::Value msg;
        msg["type"] = type;
        msg["data"] = data;
        msg["ts"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, msg);
    }

private:
    WebSocketManager() = default;

    std::set<WebSocketConnectionPtr> allConns_;
    std::map<int, std::set<WebSocketConnectionPtr>> userConns_;
    mutable std::shared_mutex mutex_;
};
