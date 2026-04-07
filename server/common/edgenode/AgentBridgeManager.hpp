#pragma once

#include "AgentProtocol.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/network/WebSocketManager.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/protocol/FrameResult.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/JsonHelper.hpp"
#include "common/utils/TimestampHelper.hpp"

#include <deque>
#include <cctype>

class EdgeNodeBridgeManager {
public:
    using WebSocketConnectionPtr = drogon::WebSocketConnectionPtr;
    template<typename T = void> using Task = drogon::Task<T>;

    // 设备中心的数据回调（deviceId 直接路由）
    using DeviceDataHandler = std::function<void(int deviceId, const std::string& clientAddr, const std::string& data)>;
    using ParsedDataHandler = std::function<void(std::vector<ParsedFrameResult>&&)>;
    using EndpointConnectionHandler = std::function<void(int agentId, const std::string& endpointId, const std::string& clientAddr, bool connected)>;

    static constexpr const char* CONFIG_STATUS_IDLE = "idle";
    static constexpr const char* CONFIG_STATUS_PENDING = "pending";
    static constexpr const char* CONFIG_STATUS_APPLIED = "applied";
    static constexpr const char* CONFIG_STATUS_FAILED = "failed";
    static constexpr const char* AUTH_STATUS_PENDING = "pending";
    static constexpr const char* AUTH_STATUS_APPROVED = "approved";
    static constexpr const char* AUTH_STATUS_REJECTED = "rejected";

    struct Session {
        int agentId = 0;
        std::string code;
        std::string sn;
        std::string model;
        std::string name;
        std::string version;
        agent::ConfigVersion expectedConfigVersion = 0;
        agent::ConfigVersion appliedConfigVersion = 0;
        std::string configStatus = CONFIG_STATUS_IDLE;
        std::string configError;
        Json::Value capabilities = Json::objectValue;
        WebSocketConnectionPtr conn;
        std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
    };

    struct ActivationResult {
        int agentId = 0;
        bool approved = false;
        std::string authStatus = AUTH_STATUS_PENDING;
        std::string code;
        std::string sn;
        std::string model;
        std::string name;
    };

    static EdgeNodeBridgeManager& instance() {
        static EdgeNodeBridgeManager inst;
        return inst;
    }

    /**
     * @brief 服务器启动时调用：重置所有 agent_node 为离线，清理残留事件数据
     */
    Task<void> resetOnStartup(int eventRetentionDays = 30) {
        DatabaseService db;

        auto resetResult = co_await db.execSqlCoro(R"(
            UPDATE agent_node
            SET is_online = FALSE,
                config_status = CASE
                    WHEN config_status = 'pending' THEN 'idle'
                    ELSE config_status
                END,
                updated_at = CURRENT_TIMESTAMP
            WHERE is_online = TRUE AND deleted_at IS NULL
            RETURNING id, code
        )");

        if (!resetResult.empty()) {
        LOG_INFO << "[EdgeNodeBridge] Startup: reset " << resetResult.size()
                     << " agent(s) to offline";
        }

        co_await db.execSqlCoro(R"(
            DELETE FROM agent_event
            WHERE created_at < CURRENT_TIMESTAMP - (? || ' days')::INTERVAL
        )", {std::to_string(eventRetentionDays)});

        LOG_INFO << "[EdgeNodeBridge] Startup: cleaned events older than "
                 << eventRetentionDays << " days";
    }

    bool authorize(const std::string& agentSn, const std::string& model) const {
        const auto normalizedSn = normalizeSn(agentSn);
        if (normalizedSn.empty()) return false;
        if (model.empty()) return false;
        return true;
    }

    Json::Value getRecentEvents(int agentId) const {
        Json::Value items(Json::arrayValue);
        if (agentId <= 0) return items;

        std::shared_lock lock(mutex_);
        auto it = recentEventsByAgent_.find(agentId);
        if (it == recentEventsByAgent_.end()) return items;

        for (const auto& item : it->second) {
            items.append(item);
        }
        return items;
    }

    void setIngressHandlers(DeviceDataHandler dataHandler, EndpointConnectionHandler connectionHandler) {
        std::unique_lock lock(mutex_);
        dataHandler_ = std::move(dataHandler);
        connectionHandler_ = std::move(connectionHandler);
    }

    void setParsedDataHandler(ParsedDataHandler handler) {
        std::unique_lock lock(mutex_);
        parsedDataHandler_ = std::move(handler);
    }

    /**
     * @brief 启动心跳超时检测定时器和事件清理定时器
     */
    void startHealthCheck(trantor::EventLoop* loop,
                          int heartbeatTimeoutSec = 90,
                          int eventRetentionDays = 30) {
        if (!loop) return;

        loop->runEvery(30.0, [this, heartbeatTimeoutSec]() {
            checkHeartbeatTimeouts(heartbeatTimeoutSec);
        });

        loop->runEvery(6 * 3600.0, [eventRetentionDays]() {
            drogon::async_run([eventRetentionDays]() -> Task<> {
                try {
                    DatabaseService db;
                    co_await db.execSqlCoro(R"(
                        DELETE FROM agent_event
                        WHERE created_at < CURRENT_TIMESTAMP - (? || ' days')::INTERVAL
                    )", {std::to_string(eventRetentionDays)});
                } catch (const std::exception& e) {
                    LOG_WARN << "[AgentBridge] Event cleanup failed: " << e.what();
                }
            });
        });

        LOG_INFO << "[AgentBridge] Health check started (timeout=" << heartbeatTimeoutSec
                 << "s, retention=" << eventRetentionDays << "d)";
    }

    Task<ActivationResult> activateSession(const std::string& agentSn,
                                           const std::string& agentModel,
                                           const std::string& version,
                                           const Json::Value& capabilities,
                                           const Json::Value& runtime,
                                           const WebSocketConnectionPtr& conn) {
        ActivationResult result;
        result.sn = normalizeSn(agentSn);
        result.model = agentModel;
        result.code = result.sn;

        const auto upsertResult = co_await upsertAgentNode(
            result.sn,
            result.model,
            version,
            capabilities,
            runtime,
            true);
        result.agentId = upsertResult.agentId;
        result.name = upsertResult.name;
        result.authStatus = upsertResult.authStatus;
        result.approved = (result.authStatus == AUTH_STATUS_APPROVED);

        if (!result.approved) {
            Json::Value eventDetail(Json::objectValue);
            eventDetail["sn"] = result.sn;
            eventDetail["model"] = result.model;
            appendRecentEvent(
                result.agentId,
                "auth_pending",
                "warning",
                "节点待平台同意接入",
                eventDetail
            );
            co_return result;
        }

        std::shared_ptr<Session> oldSession;
        auto session = std::make_shared<Session>();
        session->agentId = result.agentId;
        session->code = result.code;
        session->sn = result.sn;
        session->model = result.model;
        session->name = result.name;
        session->version = version;
        session->capabilities = capabilities;
        session->conn = conn;
        session->lastSeen = std::chrono::steady_clock::now();

        {
            std::unique_lock lock(mutex_);
            auto it = sessionsByCode_.find(result.code);
            if (it != sessionsByCode_.end()) {
                oldSession = it->second;
            }
            if (oldSession) {
                session->expectedConfigVersion = oldSession->expectedConfigVersion;
                session->appliedConfigVersion = oldSession->appliedConfigVersion;
                session->configStatus = oldSession->configStatus;
                session->configError = oldSession->configError;
            }
            auto& currentVersion = configVersions_[result.agentId];
            currentVersion = std::max({
                currentVersion,
                session->expectedConfigVersion,
                session->appliedConfigVersion
            });
            sessionsByCode_[result.code] = session;
        }

        if (oldSession && oldSession->conn && oldSession->conn != conn) {
            oldSession->conn->shutdown(drogon::CloseCode::kNormalClosure, "replaced");
        }

        Json::Value eventDetail(Json::objectValue);
        eventDetail["version"] = version;
        eventDetail["sn"] = result.sn;
        eventDetail["model"] = result.model;
        appendRecentEvent(
            result.agentId,
            "session_online",
            "info",
            oldSession ? "Agent 已重新上线并替换旧会话" : "Agent 已上线",
            eventDetail
        );

        sendHelloAck(session);
        notifyNetworkConfigOnConnect(result.agentId);
        notifyConfigSync(result.agentId);
        co_return result;
    }

    Task<void> refreshHeartbeat(const std::string& agentCode,
                                const std::string& version,
                                const Json::Value& capabilities,
                                const Json::Value& runtime) {
        std::shared_ptr<Session> session;
        {
            std::unique_lock lock(mutex_);
            auto it = sessionsByCode_.find(agentCode);
            if (it == sessionsByCode_.end()) co_return;
            session = it->second;
            session->lastSeen = std::chrono::steady_clock::now();
            if (!version.empty()) session->version = version;
            if (capabilities.isObject() && !capabilities.empty()) {
                session->capabilities = capabilities;
            }
        }

        co_await upsertAgentNode(
            session->sn, session->model, session->version,
            session->capabilities, runtime, true);
    }

    Task<void> markConfigApplied(const std::string& agentCode,
                                 agent::ConfigVersion configVersion,
                                 const Json::Value& runtime) {
        if (configVersion <= 0) co_return;

        {
            std::unique_lock lock(mutex_);
            auto it = sessionsByCode_.find(agentCode);
            if (it == sessionsByCode_.end() || !it->second) co_return;

            auto& session = it->second;
            if (configVersion < session->expectedConfigVersion) co_return;

            session->expectedConfigVersion = std::max(session->expectedConfigVersion, configVersion);
            session->appliedConfigVersion = std::max(session->appliedConfigVersion, configVersion);
            session->configStatus = CONFIG_STATUS_APPLIED;
            session->configError.clear();
            configVersions_[session->agentId] = std::max(configVersions_[session->agentId], configVersion);
        }

        DatabaseService db;
        co_await db.execSqlCoro(R"(
            UPDATE agent_node
            SET runtime = ?::jsonb,
                expected_config_version = GREATEST(expected_config_version, ?),
                applied_config_version = GREATEST(applied_config_version, ?),
                config_status = ?,
                config_error = NULL,
                last_seen = CURRENT_TIMESTAMP,
                last_config_applied_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE code = ? AND deleted_at IS NULL
        )", {
            JsonHelper::serialize(runtime.isObject() ? runtime : Json::Value(Json::objectValue)),
            std::to_string(configVersion),
            std::to_string(configVersion),
            CONFIG_STATUS_APPLIED,
            agentCode
        });

        Json::Value eventDetail(Json::objectValue);
        eventDetail["configVersion"] = static_cast<Json::Int64>(configVersion);
        appendRecentEvent(resolveAgentIdByCode(agentCode), "config_applied", "success", "配置应用成功", eventDetail);

        ResourceVersion::instance().incrementVersion("agent");
    }

    Task<void> markConfigApplyFailed(const std::string& agentCode,
                                     agent::ConfigVersion configVersion,
                                     const std::string& error,
                                     const Json::Value& runtime) {
        if (configVersion <= 0) co_return;

        const auto finalError = error.empty() ? "Agent 配置应用失败" : error;
        {
            std::unique_lock lock(mutex_);
            auto it = sessionsByCode_.find(agentCode);
            if (it == sessionsByCode_.end() || !it->second) co_return;

            auto& session = it->second;
            if (configVersion < session->expectedConfigVersion) co_return;

            session->expectedConfigVersion = std::max(session->expectedConfigVersion, configVersion);
            session->configStatus = CONFIG_STATUS_FAILED;
            session->configError = finalError;
            configVersions_[session->agentId] = std::max(configVersions_[session->agentId], configVersion);
        }

        DatabaseService db;
        co_await db.execSqlCoro(R"(
            UPDATE agent_node
            SET runtime = ?::jsonb,
                expected_config_version = GREATEST(expected_config_version, ?),
                config_status = ?,
                config_error = ?,
                last_seen = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE code = ? AND deleted_at IS NULL
        )", {
            JsonHelper::serialize(runtime.isObject() ? runtime : Json::Value(Json::objectValue)),
            std::to_string(configVersion),
            CONFIG_STATUS_FAILED,
            finalError,
            agentCode
        });

        Json::Value eventDetail(Json::objectValue);
        eventDetail["configVersion"] = static_cast<Json::Int64>(configVersion);
        eventDetail["error"] = finalError;
        appendRecentEvent(resolveAgentIdByCode(agentCode), "config_failed", "error", "配置应用失败", eventDetail);

        ResourceVersion::instance().incrementVersion("agent");
    }

    Task<void> markOffline(const std::string& agentCode, const WebSocketConnectionPtr& conn = nullptr) {
        std::shared_ptr<Session> session;
        {
            std::unique_lock lock(mutex_);
            auto it = sessionsByCode_.find(agentCode);
            if (it == sessionsByCode_.end()) co_return;
            if (conn && it->second->conn != conn) co_return;
            session = it->second;
            sessionsByCode_.erase(it);

            // 清除该 Agent 的端点状态缓存
            for (auto sit = endpointStatuses_.begin(); sit != endpointStatuses_.end();) {
                if (sit->second.get("agent_id", 0).asInt() == session->agentId) {
                    sit = endpointStatuses_.erase(sit);
                } else {
                    ++sit;
                }
            }
        }

        DatabaseService db;
        co_await db.execSqlCoro(R"(
            UPDATE agent_node
            SET is_online = FALSE, updated_at = CURRENT_TIMESTAMP
            WHERE code = ? AND deleted_at IS NULL
        )", {agentCode});

        // 清理该 Agent 注册的设备连接状态（避免 Agent 离线后设备仍显示在线）
        DeviceConnectionCache::instance().removeByClient(0, "agent:" + std::to_string(session->agentId));

        // 清理 shell 会话，通知前端
        {
            std::unique_lock slock(mutex_);
            auto sit = shellClients_.find(session->agentId);
            if (sit != shellClients_.end()) {
                if (sit->second && sit->second->connected()) {
                    Json::Value closedData(Json::objectValue);
                    closedData["exitCode"] = -1;
                    closedData["reason"] = "agent_offline";
                    sit->second->send(WebSocketManager::buildMessage("shell:closed", closedData));
                }
                shellClients_.erase(sit);
            }
        }

        appendRecentEvent(session->agentId, "session_offline", "warning", "Agent 已离线");
        ResourceVersion::instance().incrementVersion("agent");
    }

    bool requestConfigSync(int agentId) {
        {
            std::shared_lock lock(mutex_);
            if (!hasOnlineSessionLocked(agentId)) return false;
        }
        notifyConfigSync(agentId);
        return true;
    }

    // ==================== 设备中心的数据处理 ====================

    /**
     * @brief 处理 Agent 上报的设备数据（device:data）
     */
    void handleDeviceData(int agentId,
                          int deviceId,
                          const std::string& clientAddr,
                          const std::string& payloadBase64) {
        if (agentId <= 0 || deviceId <= 0) {
            LOG_WARN << "[AgentBridge] Drop device:data with invalid ids, agentId="
                     << agentId << ", deviceId=" << deviceId;
            return;
        }

        try {
            auto payload = drogon::utils::base64Decode(payloadBase64);
            DeviceDataHandler handler;
            {
                std::shared_lock lock(mutex_);
                handler = dataHandler_;
            }
            if (handler) {
                handler(deviceId, clientAddr, payload);
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[AgentBridge] Failed to decode device:data, deviceId=" << deviceId
                     << ", client=" << clientAddr << ", error=" << e.what();
        }
    }

    /**
     * @brief 处理 Agent 上报的预解析数据（device:parsed）
     *
     * Agent 本地解析后上报 JSON 结果，Server 直接入库（跳过协议解析）。
     * 回复 ACK 让 Agent 标记 SQLite 缓存为已发送。
     */
    void handleParsedDeviceData(int agentId,
                                const Json::Value& data,
                                const WebSocketConnectionPtr& conn) {
        if (agentId <= 0) return;

        const auto& batch = data["batch"];
        if (!batch.isArray() || batch.empty()) return;

        std::vector<ParsedFrameResult> results;
        results.reserve(batch.size());

        for (const auto& item : batch) {
            ParsedFrameResult r;
            r.deviceId = item.get("deviceId", 0).asInt();
            r.linkId = 0;  // Agent 模式无 linkId
            r.protocol = item.get("protocol", "").asString();
            r.funcCode = item.get("funcCode", "").asString();
            r.data = item["data"];
            r.reportTime = item.get("reportTime", "").asString();

            if (r.deviceId <= 0 || r.protocol.empty()) continue;
            results.push_back(std::move(r));
        }

        if (!results.empty()) {
            // 为上报数据的设备注册连接状态，使 connected 判断正确
            for (const auto& r : results) {
                std::string deviceKey = "modbus_" + std::to_string(r.deviceId);
                DeviceConnectionCache::instance().registerConnection(
                    deviceKey, 0, "agent:" + std::to_string(agentId), 0);
            }

            ParsedDataHandler handler;
            {
                std::shared_lock lock(mutex_);
                handler = parsedDataHandler_;
            }
            if (handler) {
                handler(std::move(results));
            }
        }

        // 回复 ACK（如有 cacheIds），Agent 收到后标记 SQLite 缓存为已发送
        if (data.isMember("cacheIds") && data["cacheIds"].isArray()
            && conn && conn->connected()) {
            Json::Value ackData(Json::objectValue);
            ackData["cacheIds"] = data["cacheIds"];
            auto frame = agent::buildBinaryMessage(agent::MESSAGE_DEVICE_PARSED_ACK, ackData);
            conn->send(frame, drogon::WebSocketMessageType::Binary);
        }
    }

    /**
     * @brief 处理 Agent 上报的端点连接事件（endpoint:connection）
     */
    void handleEndpointConnectionEvent(int agentId,
                                       const std::string& endpointId,
                                       const std::string& clientAddr,
                                       bool connected) {
        EndpointConnectionHandler handler;
        {
            std::shared_lock lock(mutex_);
            handler = connectionHandler_;
        }
        if (handler) {
            handler(agentId, endpointId, clientAddr, connected);
        }
    }

    /**
     * @brief 处理 Agent 上报的端点状态（endpoint:status）
     */
    void handleEndpointStatusReport(int agentId, const Json::Value& items) {
        if (!items.isArray()) return;

        {
            std::unique_lock lock(mutex_);
            for (const auto& item : items) {
                const auto epId = item.get("id", item.get("endpointId", "")).asString();
                if (epId.empty()) continue;

                Json::Value normalized(Json::objectValue);
                normalized["endpoint_id"] = epId;
                normalized["agent_id"] = agentId;
                normalized["conn_status"] = item.get("conn_status", "stopped").asString();
                normalized["error_msg"] = item.get("error_msg", "").asString();
                normalized["client_count"] = item.get("client_count", 0).asInt();
                normalized["clients"] = item.get("clients", Json::Value(Json::arrayValue));
                normalized["last_activity"] =
                    item.get("last_activity", item.get("lastActivity", "")).asString();
                endpointStatuses_[epId] = normalized;
            }
        }

        ResourceVersion::instance().incrementVersion("agent");
    }

    /**
     * @brief 获取端点状态
     */
    Json::Value getEndpointStatus(const std::string& endpointId) const {
        std::shared_lock lock(mutex_);
        auto it = endpointStatuses_.find(endpointId);
        if (it != endpointStatuses_.end()) {
            auto json = it->second;
            int agentId = json.get("agent_id", 0).asInt();
            json["agent_online"] = hasOnlineSessionLocked(agentId);
            return json;
        }

        Json::Value json(Json::objectValue);
        json["endpoint_id"] = endpointId;
        json["conn_status"] = "stopped";
        json["client_count"] = 0;
        json["clients"] = Json::Value(Json::arrayValue);
        return json;
    }

    /**
     * @brief 向指定设备发送数据（通过 Agent WebSocket 转发）
     */
    bool sendToDevice(int agentId, int deviceId, const std::string& data) {
        Json::Value payload(Json::objectValue);
        payload["deviceId"] = deviceId;
        payload["payload"] = drogon::utils::base64Encode(data);
        return sendToAgent(agentId, agent::MESSAGE_DEVICE_SEND, payload);
    }

    /**
     * @brief 向指定设备的特定客户端发送数据
     */
    bool sendToDeviceClient(int agentId, int deviceId, const std::string& clientAddr, const std::string& data) {
        Json::Value payload(Json::objectValue);
        payload["deviceId"] = deviceId;
        payload["clientAddr"] = clientAddr;
        payload["payload"] = drogon::utils::base64Encode(data);
        return sendToAgent(agentId, agent::MESSAGE_DEVICE_SEND, payload);
    }

    /**
     * @brief 断开端点的所有客户端连接
     */
    bool disconnectEndpointClients(int agentId, const std::string& endpointId) {
        Json::Value payload(Json::objectValue);
        payload["endpointId"] = endpointId;
        return sendToAgent(agentId, agent::MESSAGE_ENDPOINT_DISCONNECT, payload);
    }

    // ==================== 设备命令转发 ====================

    using CommandResultCallback = std::function<void(const std::string& commandKey, const std::string& responseCode, bool success, int64_t responseRecordId)>;

    /**
     * @brief 向 Agent 转发设备写命令
     *
     * Agent 收到后构建 Modbus 帧发送，等待响应并回读确认。
     * 结果通过 device:command:result 回报。
     */
    bool sendDeviceCommand(int agentId,
                           const std::string& commandKey,
                           int deviceId,
                           const Json::Value& elements,
                           int readbackCount = 3) {
        Json::Value payload(Json::objectValue);
        payload["commandKey"] = commandKey;
        payload["deviceId"] = deviceId;
        payload["elements"] = elements;
        payload["readbackCount"] = readbackCount;
        return sendToAgent(agentId, agent::MESSAGE_DEVICE_COMMAND, payload);
    }

    /**
     * @brief 处理 Agent 回报的命令执行结果
     *
     * 通过 commandResultCallback_ 通知 ProtocolCommandCoordinator 完成等待。
     */
    void handleDeviceCommandResult(const std::string& /*agentCode*/,
                                   const Json::Value& data) {
        const auto commandKey = data.get("commandKey", "").asString();
        const bool success = data.get("success", false).asBool();

        if (commandKey.empty()) return;

        LOG_INFO << "[AgentBridge] device:command:result key=" << commandKey
                 << ", success=" << success;

        CommandResultCallback cb;
        {
            std::shared_lock lock(mutex_);
            cb = commandResultCallback_;
        }
        if (cb) {
            cb(commandKey, "MODBUS_WRITE", success, 0);
        }
    }

    void setCommandResultCallback(CommandResultCallback cb) {
        std::unique_lock lock(mutex_);
        commandResultCallback_ = std::move(cb);
    }

    // ==================== 网络配置 ====================

    /**
     * @brief 向 Agent 下发网络接口配置
     */
    bool sendNetworkConfig(int agentId, const Json::Value& networkConfig) {
        if (!networkConfig.isArray() || networkConfig.empty()) return false;

        Json::Value data(Json::objectValue);
        data["interfaces"] = networkConfig;
        bool sent = sendToAgent(agentId, agent::MESSAGE_NETWORK_CONFIG, data);

        if (sent) {
            appendRecentEvent(agentId, "network_config", "info",
                "已向 Agent 下发网络配置");
        }
        return sent;
    }

    /**
     * @brief 处理 Agent 回报的网络配置成功
     */
    Task<void> handleNetworkConfigApplied(const std::string& agentCode,
                                           const Json::Value& data) {
        int agentId = resolveAgentIdByCode(agentCode);
        const auto capabilities = data.get("capabilities", Json::Value(Json::objectValue));
        const auto runtime = data.get("runtime", Json::Value(Json::objectValue));

        // 更新 capabilities（刷新接口列表）
        if (capabilities.isObject() && !capabilities.empty()) {
            std::unique_lock lock(mutex_);
            auto it = sessionsByCode_.find(agentCode);
            if (it != sessionsByCode_.end() && it->second) {
                it->second->capabilities = capabilities;
            }
        }

        // 更新数据库中的 capabilities / runtime
        if (agentId > 0) {
            DatabaseService db;
            co_await db.execSqlCoro(R"(
                UPDATE agent_node
                SET capabilities = ?::jsonb,
                    runtime = ?::jsonb,
                    last_seen = CURRENT_TIMESTAMP,
                    updated_at = CURRENT_TIMESTAMP
                WHERE id = ? AND deleted_at IS NULL
            )", {
                JsonHelper::serialize(capabilities.isObject() ? capabilities : Json::Value(Json::objectValue)),
                JsonHelper::serialize(runtime.isObject() ? runtime : Json::Value(Json::objectValue)),
                std::to_string(agentId)
            });
        }

        Json::Value detail(Json::objectValue);
        detail["backend"] = data.get("backend", "").asString();
        detail["requestedInterfaceCount"] = data.get("requestedInterfaceCount", 0).asInt();
        detail["reportedInterfaceCount"] = data.get("reportedInterfaceCount", 0).asInt();
        appendRecentEvent(agentId, "network_config_applied", "success", "网络配置应用成功", detail);
        ResourceVersion::instance().incrementVersion("agent");
    }

    /**
     * @brief 处理 Agent 回报的网络配置失败
     */
    void handleNetworkConfigFailed(const std::string& agentCode, const Json::Value& data) {
        int agentId = resolveAgentIdByCode(agentCode);
        const auto ifName = data.get("interfaceName", "").asString();
        const auto error = data.get("error", "未知错误").asString();
        const auto capabilities = data.get("capabilities", Json::Value(Json::objectValue));
        const auto runtime = data.get("runtime", Json::Value(Json::objectValue));

        if (agentId > 0) {
            drogon::async_run([agentId, capabilities, runtime]() -> Task<> {
                DatabaseService db;
                co_await db.execSqlCoro(R"(
                    UPDATE agent_node
                    SET capabilities = ?::jsonb,
                        runtime = ?::jsonb,
                        last_seen = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE id = ? AND deleted_at IS NULL
                )", {
                    JsonHelper::serialize(capabilities.isObject() ? capabilities : Json::Value(Json::objectValue)),
                    JsonHelper::serialize(runtime.isObject() ? runtime : Json::Value(Json::objectValue)),
                    std::to_string(agentId)
                });
            });
        }

        Json::Value detail(Json::objectValue);
        detail["interfaceName"] = ifName;
        detail["error"] = error;
        detail["backend"] = data.get("backend", "").asString();
        detail["requestedInterfaceCount"] = data.get("requestedInterfaceCount", 0).asInt();
        detail["reportedInterfaceCount"] = data.get("reportedInterfaceCount", 0).asInt();
        appendRecentEvent(agentId, "network_config_failed", "error",
            "网络配置失败: " + error, detail);

        ResourceVersion::instance().incrementVersion("agent");
    }

    /**
     * @brief 检查 Agent 是否在线
     */
    bool isAgentOnline(int agentId) const {
        std::shared_lock lock(mutex_);
        return hasOnlineSessionLocked(agentId);
    }

    // ==================== Shell 转发 ====================

    /**
     * @brief 打开 Agent Shell 会话（前端 → Agent）
     *
     * 同一 Agent 同时只允许一个 shell 会话。
     * clientConn 记录发起请求的前端 WebSocket，用于将 Agent 输出回传。
     */
    bool openShell(int agentId, int cols, int rows, const WebSocketConnectionPtr& clientConn) {
        {
            std::unique_lock lock(mutex_);
            auto it = shellClients_.find(agentId);
            if (it != shellClients_.end() && it->second && it->second->connected()) {
                // 已有活跃 shell 会话，拒绝
                return false;
            }
            shellClients_[agentId] = clientConn;
        }
        Json::Value data(Json::objectValue);
        data["cols"] = cols;
        data["rows"] = rows;
        return sendToAgent(agentId, agent::MESSAGE_SHELL_OPEN, data);
    }

    bool writeShell(int agentId, const std::string& input) {
        Json::Value data(Json::objectValue);
        data["data"] = input;
        return sendToAgent(agentId, agent::MESSAGE_SHELL_DATA, data);
    }

    bool resizeShell(int agentId, int cols, int rows) {
        Json::Value data(Json::objectValue);
        data["cols"] = cols;
        data["rows"] = rows;
        return sendToAgent(agentId, agent::MESSAGE_SHELL_RESIZE, data);
    }

    bool closeShell(int agentId) {
        {
            std::unique_lock lock(mutex_);
            shellClients_.erase(agentId);
        }
        Json::Value data(Json::objectValue);
        return sendToAgent(agentId, agent::MESSAGE_SHELL_CLOSE, data);
    }

    /**
     * @brief Agent 回报 shell:opened（转发给前端）
     */
    void handleShellOpened(int agentId, const Json::Value& data) {
        sendToShellClient(agentId, "shell:opened", data);
        if (!data.get("success", true).asBool()) {
            std::unique_lock lock(mutex_);
            shellClients_.erase(agentId);
        }
    }

    /**
     * @brief Agent 回报 shell:data（JSON 文本，兼容旧版 Agent）
     */
    void handleShellData(int agentId, const Json::Value& data) {
        sendToShellClient(agentId, "shell:data", data);
    }

    /**
     * @brief 转发 Agent 的二进制 shell 数据帧（零拷贝）
     *
     * 直接将 Agent 发来的二进制帧原样转发给前端 WebSocket，
     * 不做 JSON 解析/序列化，最大化吞吐、最小化延迟。
     */
    void relayShellBinary(int agentId, const std::string& binaryData) {
        WebSocketConnectionPtr client;
        {
            std::shared_lock lock(mutex_);
            auto it = shellClients_.find(agentId);
            if (it == shellClients_.end()) return;
            client = it->second;
        }
        if (client && client->connected()) {
            client->send(binaryData, drogon::WebSocketMessageType::Binary);
        }
    }

    /**
     * @brief Agent 回报 shell:closed（转发给前端并清理会话）
     */
    void handleShellClosed(int agentId, const Json::Value& data) {
        sendToShellClient(agentId, "shell:closed", data);
        std::unique_lock lock(mutex_);
        shellClients_.erase(agentId);
    }

private:
    EdgeNodeBridgeManager() = default;

    struct UpsertAgentResult {
        int agentId = 0;
        std::string name;
        std::string authStatus = AUTH_STATUS_PENDING;
    };

    static std::string normalizeSn(std::string sn) {
        std::transform(sn.begin(), sn.end(), sn.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return sn;
    }

    static std::string buildDefaultName(const std::string& sn, const std::string& model) {
        auto suffix = sn;
        if (suffix.size() > 6) {
            suffix = suffix.substr(suffix.size() - 6);
        }
        return model.empty() ? ("Edge-" + suffix) : (model + "-" + suffix);
    }

    Task<UpsertAgentResult> upsertAgentNode(const std::string& agentSn,
                                            const std::string& agentModel,
                                            const std::string& version,
                                            const Json::Value& capabilities,
                                            const Json::Value& runtime,
                                            bool online) {
        const auto normalizedSn = normalizeSn(agentSn);
        const auto generatedName = buildDefaultName(normalizedSn, agentModel);

        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            INSERT INTO agent_node (
                code, sn, model, name, version, capabilities, runtime, is_online,
                auth_status, approved_at, last_seen, connected_at, created_at, updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?::jsonb, ?::jsonb, ?::boolean,
                    ?, NULL, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
            ON CONFLICT (code) WHERE deleted_at IS NULL DO UPDATE
            SET sn = EXCLUDED.sn,
                model = CASE WHEN EXCLUDED.model <> '' THEN EXCLUDED.model ELSE agent_node.model END,
                version = CASE WHEN EXCLUDED.version <> '' THEN EXCLUDED.version ELSE agent_node.version END,
                capabilities = EXCLUDED.capabilities,
                runtime = EXCLUDED.runtime,
                is_online = CASE WHEN agent_node.auth_status = 'approved' THEN EXCLUDED.is_online ELSE FALSE END,
                last_seen = CASE WHEN agent_node.auth_status = 'approved' THEN CURRENT_TIMESTAMP ELSE agent_node.last_seen END,
                connected_at = CASE
                    WHEN agent_node.auth_status <> 'approved' THEN agent_node.connected_at
                    WHEN agent_node.is_online THEN agent_node.connected_at
                    ELSE CURRENT_TIMESTAMP
                END,
                updated_at = CURRENT_TIMESTAMP
            RETURNING id, name, auth_status
        )", {
            normalizedSn,
            normalizedSn,
            agentModel,
            generatedName,
            version,
            JsonHelper::serialize(capabilities.isObject() ? capabilities : Json::Value(Json::objectValue)),
            JsonHelper::serialize(runtime.isObject() ? runtime : Json::Value(Json::objectValue)),
            online ? "true" : "false",
            AUTH_STATUS_PENDING
        });

        ResourceVersion::instance().incrementVersion("agent");
        UpsertAgentResult upsert;
        if (!result.empty()) {
            upsert.agentId = FieldHelper::getInt(result[0]["id"]);
            upsert.name = FieldHelper::getString(result[0]["name"], generatedName);
            upsert.authStatus = FieldHelper::getString(result[0]["auth_status"], AUTH_STATUS_PENDING);
        }
        co_return upsert;
    }

    void sendHelloAck(const std::shared_ptr<Session>& session) {
        if (!session || !session->conn || !session->conn->connected()) return;

        Json::Value data(Json::objectValue);
        data["agentId"] = session->agentId;
        data["agentCode"] = session->code;
        data["sn"] = session->sn;
        data["model"] = session->model;
        data["name"] = session->name;
        data["version"] = session->version;
        auto frame = agent::buildBinaryMessage(agent::MESSAGE_HELLO_ACK, data);
        session->conn->send(frame, drogon::WebSocketMessageType::Binary);
    }

    /**
     * @brief Agent 上线时检查是否有持久化的网络配置，自动下发
     */
    void notifyNetworkConfigOnConnect(int agentId) {
        drogon::async_run([this, agentId]() -> Task<> {
            try {
                DatabaseService db;
                auto result = co_await db.execSqlCoro(R"(
                    SELECT network_config FROM agent_node
                    WHERE id = ? AND deleted_at IS NULL
                )", {std::to_string(agentId)});

                if (result.empty()) co_return;

                auto configStr = FieldHelper::getString(result[0]["network_config"], "[]");
                Json::Value networkConfig;
                Json::CharReaderBuilder rb;
                std::istringstream iss(configStr);
                std::string errs;
                Json::parseFromStream(rb, iss, &networkConfig, &errs);

                if (networkConfig.isArray() && !networkConfig.empty()) {
                    sendNetworkConfig(agentId, networkConfig);
                }
            } catch (const std::exception& e) {
                LOG_WARN << "[AgentBridge] Failed to load network config for agent "
                         << agentId << ": " << e.what();
            }
        });
    }

    /**
     * @brief 从 agent_endpoint + device 表查询该 Agent 的端点和设备，下发配置
     */
    void notifyConfigSync(int agentId) {
        const auto configVersion = reserveNextConfigVersion(agentId);

        drogon::async_run([this, agentId, configVersion]() -> Task<> {
            DatabaseService db;

            // 1) 查询该 Agent 的所有端点
            auto epRows = co_await db.execSqlCoro(R"(
                SELECT id, name, transport, mode, protocol, ip, port, channel, baud_rate
                FROM agent_endpoint
                WHERE agent_id = ? AND status = 'enabled' AND deleted_at IS NULL
                ORDER BY id ASC
            )", {std::to_string(agentId)});

            // 2) 查询该 Agent 的所有启用设备（JOIN protocol_config 获取协议配置）
            auto devices = co_await db.execSqlCoro(R"(
                SELECT d.id, d.name, d.protocol_params, d.protocol_config_id,
                       pc.config AS protocol_config
                FROM device d
                LEFT JOIN protocol_config pc ON d.protocol_config_id = pc.id AND pc.deleted_at IS NULL
                WHERE d.link_id = 0
                  AND (d.protocol_params->>'agent_id')::INT = ?
                  AND d.status = 'enabled'
                  AND d.deleted_at IS NULL
                ORDER BY d.id ASC
            )", {std::to_string(agentId)});

            // 3) 按 agent_endpoint_id 分组设备
            std::map<int, std::vector<agent::EndpointDevice>> devicesByEndpoint;
            for (const auto& row : devices) {
                auto ppStr = FieldHelper::getString(row["protocol_params"], "");
                Json::Value pp;
                if (!ppStr.empty()) {
                    Json::CharReaderBuilder rb;
                    std::istringstream iss(ppStr);
                    std::string errs;
                    Json::parseFromStream(rb, iss, &pp, &errs);
                }

                int epId = pp.get("agent_endpoint_id", 0).asInt();
                if (epId <= 0) continue;

                agent::EndpointDevice dev;
                dev.id = FieldHelper::getInt(row["id"]);
                dev.name = FieldHelper::getString(row["name"]);
                dev.deviceCode = pp.get("device_code", "").asString();
                dev.slaveId = pp.get("slave_id", 0).asInt();
                dev.modbusMode = pp.get("modbus_mode", "").asString();
                dev.heartbeat = pp.get("heartbeat", Json::objectValue);
                dev.registration = pp.get("registration", Json::objectValue);
                dev.timezone = pp.get("timezone", "+08:00").asString();
                dev.protocolConfigId = FieldHelper::getInt(row["protocol_config_id"]);

                // 解析 protocol_config.config JSONB，完整下发给 Agent
                auto pcStr = FieldHelper::getString(row["protocol_config"], "");
                if (!pcStr.empty()) {
                    Json::CharReaderBuilder pcRb;
                    std::istringstream pcIss(pcStr);
                    std::string pcErrs;
                    Json::parseFromStream(pcRb, pcIss, &dev.protocolConfig, &pcErrs);
                }

                devicesByEndpoint[epId].push_back(std::move(dev));
            }

            // 4) 构建 DeviceEndpoint 列表
            Json::Value endpoints(Json::arrayValue);
            for (const auto& epRow : epRows) {
                int epId = FieldHelper::getInt(epRow["id"]);

                agent::DeviceEndpoint ep;
                ep.id = std::to_string(epId);
                ep.name = FieldHelper::getString(epRow["name"]);
                ep.transport = FieldHelper::getString(epRow["transport"], "ethernet");
                ep.mode = FieldHelper::getString(epRow["mode"]);
                ep.protocol = FieldHelper::getString(epRow["protocol"]);
                ep.ip = FieldHelper::getString(epRow["ip"]);
                ep.port = FieldHelper::getInt(epRow["port"]);
                ep.channel = FieldHelper::getString(epRow["channel"]);
                ep.baudRate = FieldHelper::getInt(epRow["baud_rate"]);

                auto it = devicesByEndpoint.find(epId);
                if (it != devicesByEndpoint.end()) {
                    ep.devices = std::move(it->second);
                }

                endpoints.append(ep.toJson());
            }

            // 更新数据库状态
            co_await db.execSqlCoro(R"(
                UPDATE agent_node
                SET expected_config_version = ?,
                    config_status = ?,
                    config_error = NULL,
                    last_config_sync_at = CURRENT_TIMESTAMP,
                    updated_at = CURRENT_TIMESTAMP
                WHERE id = ? AND deleted_at IS NULL
            )", {
                std::to_string(configVersion),
                CONFIG_STATUS_PENDING,
                std::to_string(agentId)
            });
            ResourceVersion::instance().incrementVersion("agent");

            // 发送给 Agent
            std::shared_ptr<Session> session;
            {
                std::shared_lock lock(mutex_);
                session = findOnlineSessionLocked(agentId);
            }

            if (!session || !session->conn || !session->conn->connected()) {
                Json::Value queuedDetail(Json::objectValue);
                queuedDetail["configVersion"] = static_cast<Json::Int64>(configVersion);
                queuedDetail["endpointCount"] = static_cast<Json::Int>(endpoints.size());
                appendRecentEvent(agentId, "config_queued", "warning",
                    "配置变更已排队，等待 Agent 上线", queuedDetail);
                co_return;
            }

            Json::Value data(Json::objectValue);
            data["configVersion"] = static_cast<Json::Int64>(configVersion);
            data["endpoints"] = endpoints;
            auto syncFrame = agent::buildBinaryMessage(agent::MESSAGE_CONFIG_SYNC, data);
            session->conn->send(syncFrame, drogon::WebSocketMessageType::Binary);

            Json::Value dispatchedDetail(Json::objectValue);
            dispatchedDetail["configVersion"] = static_cast<Json::Int64>(configVersion);
            dispatchedDetail["endpointCount"] = static_cast<Json::Int>(endpoints.size());
            int deviceCount = 0;
            for (const auto& ep : endpoints) {
                deviceCount += ep.get("devices", Json::arrayValue).size();
            }
            dispatchedDetail["deviceCount"] = deviceCount;
            appendRecentEvent(agentId, "config_sync", "info", "已向 Agent 下发配置", dispatchedDetail);
        });
    }

    void sendToShellClient(int agentId, const std::string& type, const Json::Value& data) {
        WebSocketConnectionPtr client;
        {
            std::shared_lock lock(mutex_);
            auto it = shellClients_.find(agentId);
            if (it == shellClients_.end()) return;
            client = it->second;
        }
        if (client && client->connected()) {
            client->send(WebSocketManager::buildMessage(type, data));
        }
    }

    bool sendToAgent(int agentId, const std::string& type, const Json::Value& payload) {
        std::shared_ptr<Session> session;
        {
            std::shared_lock lock(mutex_);
            session = findOnlineSessionLocked(agentId);
        }

        if (!session || !session->conn || !session->conn->connected()) return false;
        auto frame = agent::buildBinaryMessage(type, payload);
        session->conn->send(frame, drogon::WebSocketMessageType::Binary);
        return true;
    }

    bool hasOnlineSessionLocked(int agentId) const {
        for (const auto& [code, session] : sessionsByCode_) {
            (void)code;
            if (session && session->agentId == agentId && session->conn && session->conn->connected()) {
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<Session> findOnlineSessionLocked(int agentId) const {
        for (const auto& [code, session] : sessionsByCode_) {
            (void)code;
            if (session && session->agentId == agentId && session->conn && session->conn->connected()) {
                return session;
            }
        }
        return nullptr;
    }

    agent::ConfigVersion reserveNextConfigVersion(int agentId) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        std::unique_lock lock(mutex_);
        auto& currentVersion = configVersions_[agentId];
        currentVersion = std::max<agent::ConfigVersion>(
            static_cast<agent::ConfigVersion>(now),
            currentVersion + 1
        );

        for (const auto& [code, session] : sessionsByCode_) {
            (void)code;
            if (session && session->agentId == agentId) {
                session->expectedConfigVersion = currentVersion;
                session->configStatus = CONFIG_STATUS_PENDING;
                session->configError.clear();
            }
        }

        return currentVersion;
    }

    void checkHeartbeatTimeouts(int timeoutSec) {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(timeoutSec);
        std::vector<std::string> timedOutCodes;

        {
            std::shared_lock lock(mutex_);
            for (const auto& [code, session] : sessionsByCode_) {
                if (session && (now - session->lastSeen) > timeout) {
                    timedOutCodes.push_back(code);
                }
            }
        }

        for (const auto& code : timedOutCodes) {
            LOG_WARN << "[AgentBridge] Heartbeat timeout, closing agent=" << code;
            drogon::async_run([this, code]() -> Task<> {
                WebSocketConnectionPtr conn;
                {
                    std::shared_lock lock(mutex_);
                    auto it = sessionsByCode_.find(code);
                    if (it != sessionsByCode_.end() && it->second) {
                        conn = it->second->conn;
                    }
                }
                if (conn && conn->connected()) {
                    conn->shutdown(drogon::CloseCode::kNormalClosure, "heartbeat_timeout");
                } else {
                    co_await markOffline(code);
                }
            });
        }
    }

    int resolveAgentIdByCode(const std::string& agentCode) const {
        if (agentCode.empty()) return 0;
        std::shared_lock lock(mutex_);
        auto it = sessionsByCode_.find(agentCode);
        if (it != sessionsByCode_.end() && it->second) return it->second->agentId;
        return 0;
    }

    void appendRecentEvent(int agentId,
                           const std::string& type,
                           const std::string& level,
                           const std::string& message,
                           const Json::Value& detail = Json::Value(Json::objectValue)) {
        if (agentId <= 0) return;

        Json::Value item(Json::objectValue);
        item["type"] = type;
        item["level"] = level;
        item["message"] = message;
        item["ts"] = TimestampHelper::now();
        if (detail.isObject() && !detail.empty()) {
            item["detail"] = detail;
        }

        std::unique_lock lock(mutex_);
        auto& queue = recentEventsByAgent_[agentId];
        queue.push_front(item);
        while (queue.size() > MAX_RECENT_EVENTS_PER_AGENT) {
            queue.pop_back();
        }

        const auto serializedDetail = JsonHelper::serialize(
            detail.isObject() ? detail : Json::Value(Json::objectValue)
        );
        drogon::async_run([agentId, type, level, message, serializedDetail]() -> Task<> {
            try {
                DatabaseService db;
                co_await db.execSqlCoro(R"(
                    INSERT INTO agent_event (agent_id, event_type, level, message, detail, created_at)
                    VALUES (?, ?, ?, ?, ?::jsonb, CURRENT_TIMESTAMP)
                )", {
                    std::to_string(agentId), type, level, message, serializedDetail
                });
            } catch (const std::exception& e) {
                LOG_WARN << "[EdgeNodeBridge] Persist agent event failed, agentId=" << agentId
                         << ", event=" << type << ", error=" << e.what();
            }
            co_return;
        });
    }

private:
    static constexpr size_t MAX_RECENT_EVENTS_PER_AGENT = 20;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessionsByCode_;
    std::unordered_map<std::string, Json::Value> endpointStatuses_;       // endpointId → status
    std::unordered_map<int, agent::ConfigVersion> configVersions_;
    std::unordered_map<int, std::deque<Json::Value>> recentEventsByAgent_;
    DeviceDataHandler dataHandler_;
    ParsedDataHandler parsedDataHandler_;
    EndpointConnectionHandler connectionHandler_;
    CommandResultCallback commandResultCallback_;
    std::unordered_map<int, WebSocketConnectionPtr> shellClients_;   // agentId → 前端 shell ws 连接
};

using AgentBridgeManager = EdgeNodeBridgeManager;
