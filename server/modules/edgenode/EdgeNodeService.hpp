#pragma once

#include "common/edgenode/AgentBridgeManager.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/JsonHelper.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/ValidatorHelper.hpp"

class EdgeNodeService {
public:
    template<typename T = void> using Task = drogon::Task<T>;
    using ExpectedEndpointMap = std::unordered_map<int, Json::Value>;
    using RecentEventMap = std::unordered_map<int, Json::Value>;

    struct ConfigSyncRequestResult {
        bool found = false;
        bool dispatched = false;
        std::string name;
    };

    struct EventQueryResult {
        bool found = false;
        Json::Value items{Json::arrayValue};
    };

    /**
     * @brief 创建 Agent 节点（UI 预配置）
     */
    Task<Json::Value> create(const Json::Value& data) {
        auto code = data.get("code", "").asString();
        std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::toupper(c); });
        const auto name = data.get("name", "").asString();
        const auto secret = data.get("secret", "").asString();

        if (code.empty()) throw ValidationException("code 不能为空");
        if (name.empty()) throw ValidationException("name 不能为空");

        DatabaseService db;

        // 检查 code 唯一性
        auto existing = co_await db.execSqlCoro(R"(
            SELECT 1 FROM agent_node WHERE code = ? AND deleted_at IS NULL LIMIT 1
        )", {code});
        if (!existing.empty()) {
            throw ConflictException("Agent 编码已存在: " + code);
        }

        auto result = co_await db.execSqlCoro(R"(
            INSERT INTO agent_node (code, name, secret, is_online, config_status, created_at, updated_at)
            VALUES (?, ?, ?, FALSE, 'idle', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
            RETURNING id
        )", {code, name, secret});

        ResourceVersion::instance().incrementVersion("link");

        Json::Value item(Json::objectValue);
        item["id"] = FieldHelper::getInt(result[0]["id"]);
        item["code"] = code;
        item["name"] = name;
        co_return item;
    }

    /**
     * @brief 更新 Agent 节点（名称、密钥）
     */
    Task<void> update(int id, const Json::Value& data) {
        DatabaseService db;

        auto existing = co_await db.execSqlCoro(R"(
            SELECT 1 FROM agent_node WHERE id = ? AND deleted_at IS NULL LIMIT 1
        )", {std::to_string(id)});
        if (existing.empty()) {
            throw NotFoundException("采集 Agent 不存在");
        }

        std::vector<std::string> setClauses;
        std::vector<std::string> params;

        if (data.isMember("name") && !data["name"].asString().empty()) {
            setClauses.push_back("name = ?");
            params.push_back(data["name"].asString());
        }
        if (data.isMember("secret")) {
            setClauses.push_back("secret = ?");
            params.push_back(data["secret"].asString());
        }

        if (setClauses.empty()) {
            co_return;
        }

        setClauses.push_back("updated_at = CURRENT_TIMESTAMP");
        params.push_back(std::to_string(id));

        std::string sql = "UPDATE agent_node SET ";
        for (size_t i = 0; i < setClauses.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += setClauses[i];
        }
        sql += " WHERE id = ? AND deleted_at IS NULL";

        co_await db.execSqlCoro(sql, params);
        ResourceVersion::instance().incrementVersion("link");
    }

    Task<Json::Value> list() {
        DatabaseService db;
        const auto expectedEndpointsByAgent = co_await loadExpectedEndpointsByAgent();
        const auto recentEventsByAgent = co_await loadRecentEventsByAgent();
        auto result = co_await db.execSqlCoro(R"(
            SELECT id, code, name, version, is_online, last_seen, connected_at, capabilities, runtime,
                   expected_config_version, applied_config_version, config_status,
                   config_error, last_config_sync_at, last_config_applied_at, network_config
            FROM agent_node
            WHERE deleted_at IS NULL
            ORDER BY is_online DESC, name ASC, id ASC
        )");

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            const auto agentId = FieldHelper::getInt(row["id"]);
            const auto expectedIt = expectedEndpointsByAgent.find(agentId);
            const auto eventIt = recentEventsByAgent.find(agentId);
            items.append(buildItem(
                row,
                expectedIt != expectedEndpointsByAgent.end() ? expectedIt->second : Json::Value(Json::arrayValue),
                eventIt != recentEventsByAgent.end() ? eventIt->second : Json::Value(Json::arrayValue)
            ));
        }
        co_return items;
    }

    Task<Json::Value> options() {
        DatabaseService db;
        const auto expectedEndpointsByAgent = co_await loadExpectedEndpointsByAgent();
        auto result = co_await db.execSqlCoro(R"(
            SELECT id, code, name, version, is_online, last_seen, connected_at, capabilities, runtime,
                   expected_config_version, applied_config_version, config_status,
                   config_error, last_config_sync_at, last_config_applied_at, network_config
            FROM agent_node
            WHERE deleted_at IS NULL
            ORDER BY is_online DESC, name ASC, id ASC
        )");

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            const auto agentId = FieldHelper::getInt(row["id"]);
            const auto it = expectedEndpointsByAgent.find(agentId);
            auto item = buildItem(
                row,
                it != expectedEndpointsByAgent.end() ? it->second : Json::Value(Json::arrayValue)
            );
            item.removeMember("connected_at");
            item.removeMember("recent_events");
            item.removeMember("last_event_at");
            item.removeMember("expected_endpoints");
            item.removeMember("network_drift_count");
            items.append(item);
        }
        co_return items;
    }

    Task<ConfigSyncRequestResult> requestConfigSync(int id) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT id, name
            FROM agent_node
            WHERE id = ? AND deleted_at IS NULL
            LIMIT 1
        )", {std::to_string(id)});

        ConfigSyncRequestResult response;
        if (result.empty()) {
            co_return response;
        }

        response.found = true;
        response.name = FieldHelper::getString(result[0]["name"]);
        response.dispatched = AgentBridgeManager::instance().requestConfigSync(id);
        co_return response;
    }

    struct NetworkConfigResult {
        bool found = false;
        bool dispatched = false;
    };

    Task<NetworkConfigResult> updateNetworkConfig(int id, const Json::Value& data) {
        NetworkConfigResult response;

        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT id FROM agent_node WHERE id = ? AND deleted_at IS NULL
        )", {std::to_string(id)});

        if (result.empty()) {
            co_return response;
        }
        response.found = true;

        // 验证并构建网络配置
        const auto& interfaces = data["interfaces"];
        Json::Value networkConfig(Json::arrayValue);
        if (interfaces.isArray()) {
            for (const auto& iface : interfaces) {
                const auto name = iface.get("name", "").asString();
                if (name.empty()) continue;

                const auto mode = iface.get("mode", "dhcp").asString();
                const auto type = iface.get("type", "ethernet").asString();
                Json::Value item(Json::objectValue);
                item["name"] = name;
                item["mode"] = mode;
                item["type"] = type;

                // 桥接配置：透传 bridge_ports 和 action
                if (type == "bridge") {
                    if (iface.isMember("bridge_ports") && iface["bridge_ports"].isArray()) {
                        item["bridge_ports"] = iface["bridge_ports"];
                    }
                    if (iface.isMember("action")) {
                        item["action"] = iface["action"].asString();
                    }
                }

                if (mode == "static") {
                    const auto ip = iface.get("ip", "").asString();
                    if (ip.empty() && type != "bridge") continue;
                    if (!ip.empty()) {
                        item["ip"] = ip;
                        item["prefix_length"] = iface.get("prefix_length", 24).asInt();
                    }
                    const auto gateway = iface.get("gateway", "").asString();
                    if (!gateway.empty()) item["gateway"] = gateway;
                }

                networkConfig.append(item);
            }
        }

        // 保存到数据库
        co_await db.execSqlCoro(R"(
            UPDATE agent_node
            SET network_config = ?::jsonb,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ? AND deleted_at IS NULL
        )", {
            JsonHelper::serialize(networkConfig),
            std::to_string(id)
        });

        // 即时下发给在线 Agent
        if (!networkConfig.empty()) {
            response.dispatched = AgentBridgeManager::instance().sendNetworkConfig(id, networkConfig);
        }

        ResourceVersion::instance().incrementVersion("agent");
        co_return response;
    }

    struct DeleteResult {
        bool found = false;
        bool rejected = false;
        std::string rejectReason;
        std::string name;
    };

    Task<DeleteResult> remove(int id) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT id, code, name, is_online
            FROM agent_node
            WHERE id = ? AND deleted_at IS NULL
            LIMIT 1
        )", {std::to_string(id)});

        DeleteResult response;
        if (result.empty()) {
            co_return response;
        }

        response.found = true;
        response.name = FieldHelper::getString(result[0]["name"]);
        const auto isOnline = FieldHelper::getBool(result[0]["is_online"]);

        if (isOnline) {
            response.rejected = true;
            response.rejectReason = "Agent 当前在线，请先断开连接";
            co_return response;
        }

        // 检查是否有关联的设备（link_id = 0 且 protocol_params->>'agent_id' 匹配）
        auto deviceResult = co_await db.execSqlCoro(R"(
            SELECT COUNT(*) AS cnt
            FROM device
            WHERE link_id = 0
              AND (protocol_params->>'agent_id')::int = ?
              AND deleted_at IS NULL
        )", {std::to_string(id)});

        const int deviceCount = deviceResult.empty() ? 0 : FieldHelper::getInt(deviceResult[0]["cnt"]);
        if (deviceCount > 0) {
            response.rejected = true;
            response.rejectReason = "该 Agent 仍有 " + std::to_string(deviceCount)
                                  + " 个关联设备，请先删除设备";
            co_return response;
        }

        // 逻辑删除 agent_node
        co_await db.execSqlCoro(R"(
            UPDATE agent_node
            SET deleted_at = CURRENT_TIMESTAMP,
                is_online = FALSE,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ? AND deleted_at IS NULL
        )", {std::to_string(id)});

        ResourceVersion::instance().incrementVersion("link");

        co_return response;
    }

    // ========== Agent Endpoint CRUD ==========

    /**
     * @brief 获取某 Agent 的端点列表（含关联设备数）
     */
    Task<Json::Value> listEndpoints(int agentId) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT ep.id, ep.agent_id, ep.name, ep.transport, ep.mode, ep.protocol,
                   ep.ip, ep.port, ep.channel, ep.baud_rate, ep.status,
                   COUNT(d.id) AS device_count
            FROM agent_endpoint ep
            LEFT JOIN device d ON d.link_id = 0
              AND (d.protocol_params->>'agent_endpoint_id')::int = ep.id
              AND d.deleted_at IS NULL
            WHERE ep.agent_id = ? AND ep.deleted_at IS NULL
            GROUP BY ep.id
            ORDER BY ep.id ASC
        )", {std::to_string(agentId)});

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            items.append(buildEndpointItem(row));
        }
        co_return items;
    }

    /**
     * @brief 创建端点
     */
    Task<Json::Value> createEndpoint(int agentId, const Json::Value& data) {
        DatabaseService db;

        // 验证 Agent 存在
        auto agentResult = co_await db.execSqlCoro(R"(
            SELECT 1 FROM agent_node WHERE id = ? AND deleted_at IS NULL LIMIT 1
        )", {std::to_string(agentId)});
        if (agentResult.empty()) {
            throw NotFoundException("采集 Agent 不存在");
        }

        const auto name = data.get("name", "").asString();
        const auto transport = data.get("transport", "ethernet").asString();
        const auto mode = data.get("mode", "").asString();
        const auto protocol = data.get("protocol", "").asString();
        const auto ip = data.get("ip", "").asString();
        int port = 0;
        const auto channel = data.get("channel", "").asString();
        const auto baudRate = data.get("baud_rate", 0).asInt();

        if (name.empty()) throw ValidationException("端点名称不能为空");
        if (protocol.empty()) throw ValidationException("协议类型不能为空");
        if (transport != "ethernet" && transport != "serial") {
            throw ValidationException("传输方式必须为 ethernet 或 serial");
        }
        if (transport == "ethernet") {
            if (mode.empty()) throw ValidationException("以太网端点需要指定链路模式");
            port = ValidatorHelper::optionalIntRangeField(
                data,
                "port",
                0,
                1,
                65535,
                "端口范围 1-65535"
            );
        }
        if (transport == "serial" && channel.empty()) {
            throw ValidationException("串口端点需要指定通道");
        }

        auto result = co_await db.execSqlCoro(R"(
            INSERT INTO agent_endpoint (agent_id, name, transport, mode, protocol, ip, port, channel, baud_rate)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            RETURNING id
        )", {
            std::to_string(agentId), name, transport, mode, protocol,
            ip, std::to_string(port), channel, std::to_string(baudRate)
        });

        // 触发 Agent config sync
        AgentBridgeManager::instance().requestConfigSync(agentId);
        ResourceVersion::instance().incrementVersion("link");

        Json::Value item(Json::objectValue);
        item["id"] = FieldHelper::getInt(result[0]["id"]);
        item["agent_id"] = agentId;
        item["name"] = name;
        co_return item;
    }

    /**
     * @brief 更新端点
     */
    Task<void> updateEndpoint(int endpointId, const Json::Value& data) {
        DatabaseService db;

        auto existing = co_await db.execSqlCoro(R"(
            SELECT agent_id FROM agent_endpoint WHERE id = ? AND deleted_at IS NULL LIMIT 1
        )", {std::to_string(endpointId)});
        if (existing.empty()) {
            throw NotFoundException("端点不存在");
        }
        const int agentId = FieldHelper::getInt(existing[0]["agent_id"]);

        std::vector<std::string> setClauses;
        std::vector<std::string> params;

        auto addField = [&](const char* field, const char* column) {
            if (data.isMember(field) && !data[field].isNull()) {
                setClauses.push_back(std::string(column) + " = ?");
                if (data[field].isInt()) {
                    params.push_back(std::to_string(data[field].asInt()));
                } else {
                    params.push_back(data[field].asString());
                }
            }
        };

        addField("name", "name");
        addField("mode", "mode");
        addField("protocol", "protocol");
        addField("ip", "ip");
        addField("port", "port");
        addField("channel", "channel");
        addField("baud_rate", "baud_rate");
        addField("status", "status");

        if (setClauses.empty()) co_return;

        params.push_back(std::to_string(endpointId));

        std::string sql = "UPDATE agent_endpoint SET ";
        for (size_t i = 0; i < setClauses.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += setClauses[i];
        }
        sql += " WHERE id = ? AND deleted_at IS NULL";

        co_await db.execSqlCoro(sql, params);

        // 触发 Agent config sync
        AgentBridgeManager::instance().requestConfigSync(agentId);
        ResourceVersion::instance().incrementVersion("link");
    }

    struct EndpointDeleteResult {
        bool found = false;
        bool rejected = false;
        std::string rejectReason;
    };

    /**
     * @brief 删除端点（检查关联设备）
     */
    Task<EndpointDeleteResult> removeEndpoint(int endpointId) {
        DatabaseService db;

        auto existing = co_await db.execSqlCoro(R"(
            SELECT agent_id FROM agent_endpoint WHERE id = ? AND deleted_at IS NULL LIMIT 1
        )", {std::to_string(endpointId)});

        EndpointDeleteResult response;
        if (existing.empty()) {
            co_return response;
        }

        response.found = true;
        const int agentId = FieldHelper::getInt(existing[0]["agent_id"]);

        // 检查关联设备
        auto deviceResult = co_await db.execSqlCoro(R"(
            SELECT COUNT(*) AS cnt FROM device
            WHERE link_id = 0
              AND (protocol_params->>'agent_endpoint_id')::int = ?
              AND deleted_at IS NULL
        )", {std::to_string(endpointId)});

        const int deviceCount = deviceResult.empty() ? 0 : FieldHelper::getInt(deviceResult[0]["cnt"]);
        if (deviceCount > 0) {
            response.rejected = true;
            response.rejectReason = "该端点仍有 " + std::to_string(deviceCount) + " 个关联设备，请先删除设备";
            co_return response;
        }

        co_await db.execSqlCoro(R"(
            UPDATE agent_endpoint SET deleted_at = CURRENT_TIMESTAMP WHERE id = ? AND deleted_at IS NULL
        )", {std::to_string(endpointId)});

        AgentBridgeManager::instance().requestConfigSync(agentId);
        ResourceVersion::instance().incrementVersion("link");

        co_return response;
    }

    Task<EventQueryResult> recentEvents(int id, int hours, int limit) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT 1
            FROM agent_node
            WHERE id = ? AND deleted_at IS NULL
            LIMIT 1
        )", {std::to_string(id)});

        EventQueryResult response;
        if (result.empty()) {
            co_return response;
        }

        response.found = true;
        response.items = co_await loadRecentEventsForAgent(id, hours, limit);
        co_return response;
    }

private:
    static Json::Value buildEndpointItem(const drogon::orm::Row& row) {
        Json::Value item(Json::objectValue);
        item["id"] = FieldHelper::getInt(row["id"]);
        item["agent_id"] = FieldHelper::getInt(row["agent_id"]);
        item["name"] = FieldHelper::getString(row["name"]);
        item["transport"] = FieldHelper::getString(row["transport"], "ethernet");
        item["mode"] = FieldHelper::getString(row["mode"]);
        item["protocol"] = FieldHelper::getString(row["protocol"]);
        item["ip"] = FieldHelper::getString(row["ip"]);
        item["port"] = FieldHelper::getInt(row["port"]);
        item["channel"] = FieldHelper::getString(row["channel"]);
        item["baud_rate"] = FieldHelper::getInt(row["baud_rate"]);
        item["status"] = FieldHelper::getString(row["status"], "enabled");
        item["device_count"] = FieldHelper::getInt(row["device_count"]);
        return item;
    }

    static Json::Value parseJsonField(const drogon::orm::Field& field,
                                     Json::Value fallback = Json::Value(Json::objectValue)) {
        if (field.isNull()) {
            return fallback;
        }

        try {
            auto parsed = JsonHelper::parse(field.as<std::string>());
            if ((fallback.isObject() && parsed.isObject()) ||
                (fallback.isArray() && parsed.isArray())) {
                return parsed;
            }
            return fallback;
        } catch (...) {
            return fallback;
        }
    }

    /**
     * @brief 端点列表直接返回（网络配置归 Agent 管理，不再需要网卡漂移检测）
     */
    static Json::Value decorateExpectedEndpoints(const Json::Value& expectedEndpoints,
                                                  const Json::Value& /*reportedInterfaces*/) {
        if (!expectedEndpoints.isArray()) {
            return Json::Value(Json::arrayValue);
        }
        return expectedEndpoints;
    }

    static Json::Value buildItem(const drogon::orm::Row& row,
                                 const Json::Value& expectedEndpoints = Json::Value(Json::arrayValue),
                                 const Json::Value& recentEvents = Json::Value(Json::arrayValue)) {
        Json::Value item(Json::objectValue);
        item["id"] = FieldHelper::getInt(row["id"]);
        item["code"] = FieldHelper::getString(row["code"]);
        item["name"] = FieldHelper::getString(row["name"]);
        item["version"] = FieldHelper::getString(row["version"]);
        item["is_online"] = FieldHelper::getBool(row["is_online"]);
        item["last_seen"] = FieldHelper::getString(row["last_seen"]);
        item["connected_at"] = FieldHelper::getString(row["connected_at"]);
        item["expected_config_version"] = static_cast<Json::Int64>(
            FieldHelper::getInt64(row["expected_config_version"])
        );
        item["applied_config_version"] = static_cast<Json::Int64>(
            FieldHelper::getInt64(row["applied_config_version"])
        );
        item["config_status"] = FieldHelper::getString(row["config_status"], "idle");
        item["config_error"] = FieldHelper::getString(row["config_error"]);
        item["last_config_sync_at"] = FieldHelper::getString(row["last_config_sync_at"]);
        item["last_config_applied_at"] = FieldHelper::getString(row["last_config_applied_at"]);

        auto capabilities = parseJsonField(row["capabilities"]);
        auto runtime = parseJsonField(row["runtime"]);
        item["capabilities"] = capabilities;
        item["runtime"] = runtime;
        item["interfaces"] = capabilities.get("interfaces", Json::Value(Json::arrayValue));
        item["network_config"] = parseJsonField(row["network_config"], Json::Value(Json::arrayValue));
        item["expected_endpoints"] = expectedEndpoints;
        item["managed_device_count"] = runtime.get("managedDeviceCount", 0);
        item["recent_events"] = recentEvents;
        item["last_event_at"] = item["recent_events"].empty()
            ? Json::Value("")
            : item["recent_events"][0].get("ts", "").asString();
        return item;
    }

    /**
     * @brief 从 agent_endpoint 表加载每个 Agent 的端点信息（含关联设备列表）
     */
    static Task<ExpectedEndpointMap> loadExpectedEndpointsByAgent() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT ep.agent_id,
                   ep.id AS endpoint_id,
                   ep.name AS endpoint_name,
                   ep.transport,
                   ep.mode,
                   ep.protocol,
                   ep.ip,
                   ep.port,
                   ep.channel,
                   ep.baud_rate,
                   COUNT(d.id) AS device_count,
                   COALESCE(json_agg(
                       json_build_object(
                           'device_id', d.id,
                           'device_name', d.name,
                           'protocol', ep.protocol
                       )
                       ORDER BY d.name ASC, d.id ASC
                   ) FILTER (WHERE d.id IS NOT NULL), '[]'::json) AS devices
            FROM agent_endpoint ep
            LEFT JOIN device d ON d.link_id = 0
              AND (d.protocol_params->>'agent_endpoint_id')::int = ep.id
              AND d.deleted_at IS NULL
            WHERE ep.deleted_at IS NULL
            GROUP BY ep.id
            ORDER BY ep.agent_id ASC, ep.id ASC
        )");

        ExpectedEndpointMap endpointsByAgent;
        for (const auto& row : result) {
            const auto agentId = FieldHelper::getInt(row["agent_id"]);
            if (agentId <= 0) continue;

            auto& items = endpointsByAgent[agentId];
            if (!items.isArray()) {
                items = Json::Value(Json::arrayValue);
            }

            Json::Value item(Json::objectValue);
            item["id"] = FieldHelper::getInt(row["endpoint_id"]);
            item["name"] = FieldHelper::getString(row["endpoint_name"]);
            item["transport"] = FieldHelper::getString(row["transport"], "ethernet");
            item["mode"] = FieldHelper::getString(row["mode"]);
            item["protocol"] = FieldHelper::getString(row["protocol"]);
            item["ip"] = FieldHelper::getString(row["ip"]);
            item["port"] = FieldHelper::getInt(row["port"]);
            item["channel"] = FieldHelper::getString(row["channel"]);
            item["baud_rate"] = FieldHelper::getInt(row["baud_rate"]);
            item["device_count"] = FieldHelper::getInt(row["device_count"]);
            item["devices"] = parseJsonField(row["devices"], Json::Value(Json::arrayValue));
            items.append(item);
        }

        co_return endpointsByAgent;
    }

    static Task<RecentEventMap> loadRecentEventsByAgent(int hours = 24, int perAgentLimit = 6) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            WITH ranked_events AS (
                SELECT agent_id,
                       event_type,
                       level,
                       message,
                       detail,
                       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS event_ts,
                       ROW_NUMBER() OVER (PARTITION BY agent_id ORDER BY created_at DESC, id DESC) AS rn
                FROM agent_event
                WHERE created_at >= CURRENT_TIMESTAMP - (? || ' hours')::INTERVAL
            )
            SELECT agent_id,
                   json_agg(
                       json_build_object(
                           'type', event_type,
                           'level', level,
                           'message', message,
                           'ts', event_ts,
                           'detail', detail
                       )
                       ORDER BY rn ASC
                   ) AS recent_events
            FROM ranked_events
            WHERE rn <= ?
            GROUP BY agent_id
        )", {
            std::to_string(hours),
            std::to_string(perAgentLimit)
        });

        RecentEventMap recentEventsByAgent;
        for (const auto& row : result) {
            const auto agentId = FieldHelper::getInt(row["agent_id"]);
            if (agentId <= 0) {
                continue;
            }
            recentEventsByAgent[agentId] = parseJsonField(row["recent_events"], Json::Value(Json::arrayValue));
        }
        co_return recentEventsByAgent;
    }

    static Task<Json::Value> loadRecentEventsForAgent(int agentId, int hours, int limit) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT event_type,
                   level,
                   message,
                   detail,
                   to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS event_ts
            FROM agent_event
            WHERE agent_id = ?
              AND created_at >= CURRENT_TIMESTAMP - (? || ' hours')::INTERVAL
            ORDER BY created_at DESC, id DESC
            LIMIT ?
        )", {
            std::to_string(agentId),
            std::to_string(hours),
            std::to_string(limit)
        });

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item(Json::objectValue);
            item["type"] = FieldHelper::getString(row["event_type"]);
            item["level"] = FieldHelper::getString(row["level"], "info");
            item["message"] = FieldHelper::getString(row["message"]);
            item["ts"] = FieldHelper::getString(row["event_ts"]);
            item["detail"] = parseJsonField(row["detail"]);
            items.append(item);
        }
        co_return items;
    }
};

using AgentService = EdgeNodeService;
