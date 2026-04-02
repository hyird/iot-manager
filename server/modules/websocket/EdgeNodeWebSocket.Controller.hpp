#pragma once

#include "common/edgenode/AgentBridgeManager.hpp"
#include "common/edgenode/AgentProtocol.hpp"
#include <algorithm>
#include <cctype>

struct AgentWsSession {
    std::string agentSn;
    std::string agentModel;
    std::atomic<int> agentId{0};
    std::atomic<bool> activated{false};
};

class AgentWsController : public drogon::WebSocketController<AgentWsController> {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/agent/ws");
    WS_PATH_LIST_END

    static std::string toUpper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override {
        const auto agentSn = toUpper(req->getHeader("X-Agent-SN"));
        const auto agentModel = req->getHeader("X-Agent-Model");

        if (!AgentBridgeManager::instance().authorize(agentSn, agentModel)) {
            conn->send(buildError("auth_failed", "Agent认证失败，SN/model 缺失"));
            conn->shutdown(drogon::CloseCode::kViolation, "Auth failed");
            return;
        }

        auto session = std::make_shared<AgentWsSession>();
        session->agentSn = agentSn;
        session->agentModel = agentModel;
        conn->setContext(session);
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override {
        if (type == drogon::WebSocketMessageType::Ping) {
            conn->send(message, drogon::WebSocketMessageType::Pong);
            return;
        }
        if (!conn->hasContext()) return;

        // 二进制帧：根据首字节区分 shell 数据 / 控制消息
        if (type == drogon::WebSocketMessageType::Binary) {
            if (message.empty()) return;
            auto session = conn->getContext<AgentWsSession>();
            if (!session) return;

            uint8_t ch = static_cast<uint8_t>(message[0]);

            if (agent::isShellFrame(ch)) {
                // shell 数据帧（0x00/0x01），零拷贝转发给前端
                if (session->activated.load(std::memory_order_acquire)) {
                    AgentBridgeManager::instance().relayShellBinary(
                        session->agentId.load(std::memory_order_relaxed), message);
                }
                return;
            }

            // 控制消息帧（0x10/0x11），解压后走 JSON 处理
            auto payloadOpt = agent::parseBinaryControlFrame(message);
            if (!payloadOpt) {
                conn->send(buildError("bad_message", "二进制消息解析失败"));
                return;
            }
            handleJsonPayload(conn, session, *payloadOpt);
            return;
        }

        if (type != drogon::WebSocketMessageType::Text) {
            return;
        }

        // 兼容文本帧（向后兼容旧版 Agent）
        auto payloadOpt = agent::parseMessage(message);
        if (!payloadOpt) {
            conn->send(buildError("bad_message", "消息格式错误"));
            return;
        }

        auto session = conn->getContext<AgentWsSession>();
        if (!session) return;
        handleJsonPayload(conn, session, *payloadOpt);
    }

    void handleJsonPayload(const drogon::WebSocketConnectionPtr& conn,
                           const std::shared_ptr<AgentWsSession>& session,
                           const Json::Value& payload) {
        const auto typeName = payload.get("type", "").asString();
        const auto& data = payload["data"];

        if (typeName == agent::MESSAGE_HELLO) {
            const auto agentSn = toUpper(data.get("sn", session->agentSn).asString());
            const auto agentModel = data.get("model", session->agentModel).asString();
            const auto version = data.get("version", "").asString();
            const auto capabilities = data.get("capabilities", Json::Value(Json::objectValue));
            const auto runtime = data.get("runtime", Json::Value(Json::objectValue));
            drogon::async_run([conn, session, agentSn, agentModel, version, capabilities, runtime]() mutable -> Task<> {
                try {
                    auto activation = co_await AgentBridgeManager::instance().activateSession(
                        agentSn,
                        agentModel,
                        version,
                        capabilities,
                        runtime,
                        conn);
                    session->agentSn = activation.sn;
                    session->agentModel = activation.model;
                    session->agentId.store(activation.agentId, std::memory_order_relaxed);
                    session->activated.store(activation.approved, std::memory_order_release);

                    if (!activation.approved && conn->connected()) {
                        const auto code = activation.authStatus == AgentBridgeManager::AUTH_STATUS_REJECTED
                            ? "auth_rejected" : "auth_pending";
                        const auto message = activation.authStatus == AgentBridgeManager::AUTH_STATUS_REJECTED
                            ? "该节点已被平台拒绝接入"
                            : "该节点待平台同意接入";
                        conn->send(AgentWsController::buildError(code, message));
                        conn->shutdown(drogon::CloseCode::kViolation, "not_approved");
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR << "[AgentWS] hello failed: " << e.what();
                    session->agentId.store(0, std::memory_order_relaxed);
                    session->activated.store(false, std::memory_order_release);
                    if (conn->connected()) {
                        conn->send(AgentWsController::buildError("hello_failed", "Agent注册失败"));
                        conn->shutdown(drogon::CloseCode::kUnexpectedCondition, "hello_failed");
                    }
                }
            });
            return;
        }

        if (!session->activated.load(std::memory_order_acquire)) {
            conn->send(buildError("hello_required", "请先完成 Agent hello 握手"));
            return;
        }

        if (typeName == agent::MESSAGE_HEARTBEAT) {
            const auto version = data.get("version", "").asString();
            const auto capabilities = data.get("capabilities", Json::nullValue);
            const auto runtime = data.get("runtime", Json::Value(Json::objectValue));
            drogon::async_run([agentCode = session->agentSn, version, capabilities, runtime]() -> Task<> {
                co_await AgentBridgeManager::instance().refreshHeartbeat(
                    agentCode,
                    version,
                    capabilities,
                    runtime);
            });
            return;
        }

        if (typeName == agent::MESSAGE_CONFIG_APPLIED) {
            const auto configVersion = agent::parseConfigVersion(data);
            const auto runtime = data.get("runtime", Json::Value(Json::objectValue));
            drogon::async_run([agentCode = session->agentSn, configVersion, runtime]() -> Task<> {
                co_await AgentBridgeManager::instance().markConfigApplied(
                    agentCode,
                    configVersion,
                    runtime
                );
            });
            return;
        }

        if (typeName == agent::MESSAGE_CONFIG_APPLY_FAILED) {
            const auto configVersion = agent::parseConfigVersion(data);
            const auto error = data.get("error", "").asString();
            const auto runtime = data.get("runtime", Json::Value(Json::objectValue));
            drogon::async_run([agentCode = session->agentSn, configVersion, error, runtime]() -> Task<> {
                co_await AgentBridgeManager::instance().markConfigApplyFailed(
                    agentCode,
                    configVersion,
                    error,
                    runtime
                );
            });
            return;
        }

        if (typeName == agent::MESSAGE_DEVICE_DATA) {
            AgentBridgeManager::instance().handleDeviceData(
                session->agentId.load(std::memory_order_relaxed),
                data.get("deviceId", 0).asInt(),
                data.get("clientAddr", "").asString(),
                data.get("payload", "").asString());
            return;
        }

        if (typeName == agent::MESSAGE_DEVICE_PARSED) {
            AgentBridgeManager::instance().handleParsedDeviceData(
                session->agentId.load(std::memory_order_relaxed),
                data,
                conn);
            return;
        }

        if (typeName == agent::MESSAGE_ENDPOINT_CONNECTION) {
            AgentBridgeManager::instance().handleEndpointConnectionEvent(
                session->agentId.load(std::memory_order_relaxed),
                data.get("endpointId", "").asString(),
                data.get("clientAddr", "").asString(),
                data.get("connected", false).asBool());
            return;
        }

        if (typeName == agent::MESSAGE_ENDPOINT_STATUS) {
            const auto& items = data["items"];
            AgentBridgeManager::instance().handleEndpointStatusReport(
                session->agentId.load(std::memory_order_relaxed),
                items
            );
            return;
        }

        if (typeName == agent::MESSAGE_DEVICE_COMMAND_RESULT) {
            AgentBridgeManager::instance().handleDeviceCommandResult(session->agentSn, data);
            return;
        }

        // ==================== Shell 转发 (Agent → 前端) ====================
        if (typeName == agent::MESSAGE_SHELL_OPENED) {
            AgentBridgeManager::instance().handleShellOpened(
                session->agentId.load(std::memory_order_relaxed), data);
            return;
        }
        if (typeName == agent::MESSAGE_SHELL_DATA) {
            AgentBridgeManager::instance().handleShellData(
                session->agentId.load(std::memory_order_relaxed), data);
            return;
        }
        if (typeName == agent::MESSAGE_SHELL_CLOSED) {
            AgentBridgeManager::instance().handleShellClosed(
                session->agentId.load(std::memory_order_relaxed), data);
            return;
        }

        if (typeName == agent::MESSAGE_NETWORK_CONFIG_APPLIED) {
            drogon::async_run([agentCode = session->agentSn, data]() -> Task<> {
                co_await AgentBridgeManager::instance().handleNetworkConfigApplied(agentCode, data);
            });
            return;
        }

        if (typeName == agent::MESSAGE_NETWORK_CONFIG_FAILED) {
            AgentBridgeManager::instance().handleNetworkConfigFailed(session->agentSn, data);
            return;
        }

        if (typeName == agent::MESSAGE_PING) {
            Json::Value empty(Json::objectValue);
            auto frame = agent::buildBinaryMessage(agent::MESSAGE_PONG, empty);
            conn->send(frame, drogon::WebSocketMessageType::Binary);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        if (!conn->hasContext()) {
            return;
        }
        auto session = conn->getContext<AgentWsSession>();
        if (!session) {
            return;
        }
        drogon::async_run([agentCode = session->agentSn, conn]() -> Task<> {
            co_await AgentBridgeManager::instance().markOffline(agentCode, conn);
        });
    }

private:
    static std::string buildError(const std::string& code, const std::string& message) {
        Json::Value data(Json::objectValue);
        data["code"] = code;
        data["message"] = message;
        return agent::buildMessage(agent::MESSAGE_ERROR, data);
    }
};
