#pragma once

#include "common/network/WebSocketManager.hpp"
#include "common/utils/JwtUtils.hpp"
#include "common/cache/AuthCache.hpp"

/**
 * @brief WebSocket 控制器
 *
 * 路径：/ws
 * 认证：优先通过 Sec-WebSocket-Protocol 传递 JWT，兼容 query/header 回退
 * 心跳：客户端发送 {"type":"ping"} → 服务端回复 {"type":"pong"}
 */
class WsController : public drogon::WebSocketController<WsController> {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END

    WsController() {
        auto config = drogon::app().getCustomConfig();
        std::string secret = config["jwt"]["secret"].asString();
        int expiresIn = config["jwt"]["access_token_expires_in"].asInt();
        jwtUtils_ = std::make_shared<JwtUtils>(secret, expiresIn);
    }

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override {
        // 优先从 Sec-WebSocket-Protocol 获取 token（避免 URL query 泄露）
        std::string token = extractTokenFromWsProtocol(req->getHeader("Sec-WebSocket-Protocol"));
        // 兼容旧客户端：从 query 参数获取 token
        if (token.empty()) {
            token = req->getParameter("token");
        }
        if (token.empty()) {
            // 回退：尝试 Authorization header
            auto authHeader = req->getHeader("Authorization");
            if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ") {
                token = authHeader.substr(7);
            }
        }

        if (token.empty()) {
            conn->send(buildError("auth_failed", "认证令牌缺失"));
            conn->shutdown(drogon::CloseCode::kViolation, "No token");
            return;
        }

        // 异步验证 token（需要查询 Redis 黑名单）
        drogon::async_run([this, token, conn]() mutable -> Task<void> {
            try {
                bool isBlacklisted = co_await authCache_.isTokenBlacklisted(token);
                if (isBlacklisted) {
                    conn->send(buildError("auth_failed", "令牌已失效"));
                    conn->shutdown(drogon::CloseCode::kViolation, "Token blacklisted");
                    co_return;
                }

                Json::Value payload = jwtUtils_->verify(token);
                int userId = payload["userId"].asInt();
                std::string username = payload["username"].asString();

                // 存储会话信息到连接 context
                auto session = std::make_shared<WsSession>();
                session->userId = userId;
                session->username = username;
                conn->setContext(session);

                WebSocketManager::instance().addConnection(userId, conn);

                // 发送连接确认
                Json::Value data;
                data["userId"] = userId;
                data["username"] = username;
                conn->send(WebSocketManager::buildMessage("connected", data));

            } catch (const std::exception& e) {
                LOG_WARN << "[WS] Auth failed: " << e.what();
                conn->send(buildError("auth_failed", "令牌验证失败"));
                conn->shutdown(drogon::CloseCode::kViolation, "Auth failed");
            }
        });
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override {
        // WebSocket 协议级 Ping
        if (type == drogon::WebSocketMessageType::Ping) {
            conn->send(message, drogon::WebSocketMessageType::Pong);
            return;
        }

        if (type != drogon::WebSocketMessageType::Text) return;

        // 解析应用层消息
        Json::CharReaderBuilder builder;
        Json::Value msg;
        std::string errs;
        std::istringstream iss(message);
        if (!Json::parseFromStream(builder, iss, &msg, &errs)) return;

        std::string msgType = msg.get("type", "").asString();

        // 应用层心跳
        if (msgType == "ping") {
            Json::Value pong;
            conn->send(WebSocketManager::buildMessage("pong", pong));
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        WebSocketManager::instance().removeConnection(conn);
    }

private:
    std::shared_ptr<JwtUtils> jwtUtils_;
    AuthCache authCache_;

    static std::string trim(const std::string& value) {
        size_t start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    static std::string extractTokenFromWsProtocol(const std::string& protocolHeader) {
        if (protocolHeader.empty()) return "";

        std::vector<std::string> parts;
        std::stringstream ss(protocolHeader);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto token = trim(part);
            if (!token.empty()) {
                parts.push_back(token);
            }
        }

        // 约定：new WebSocket(url, ["auth", "<jwt>"])
        if (parts.size() >= 2 && parts[0] == "auth") {
            return parts[1];
        }

        // 兼容：只有一个协议值时，直接视为 token
        if (parts.size() == 1 && parts[0] != "auth") {
            return parts[0];
        }

        return "";
    }

    static std::string buildError(const std::string& code, const std::string& message) {
        Json::Value data;
        data["code"] = code;
        data["message"] = message;
        return WebSocketManager::buildMessage("error", data);
    }
};
