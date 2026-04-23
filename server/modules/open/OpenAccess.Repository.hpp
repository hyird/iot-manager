#pragma once

#include "OpenAccess.Shared.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/filters/ResourcePermission.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/ValidatorHelper.hpp"

class OpenAccessRepository {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    Task<Json::Value> listAccessKeys() {
        auto result = co_await dbService_.execSqlCoro(R"(
            SELECT
                ak.id,
                ak.name,
                ak.access_key_prefix,
                ak.status,
                ak.allow_realtime,
                ak.allow_history,
                ak.allow_command,
                ak.allow_alert,
                ak.expires_at,
                ak.last_used_at,
                ak.last_used_ip,
                ak.remark,
                ak.created_at,
                ak.updated_at,
                COUNT(DISTINCT w.id) AS webhook_count,
                COALESCE(
                    jsonb_agg(DISTINCT akd.device_id) FILTER (WHERE akd.device_id IS NOT NULL),
                    '[]'::jsonb
                ) AS device_ids
            FROM open_access_key ak
            LEFT JOIN open_access_key_device akd ON akd.access_key_id = ak.id
            LEFT JOIN open_webhook w ON w.access_key_id = ak.id AND w.deleted_at IS NULL
            WHERE ak.deleted_at IS NULL
            GROUP BY ak.id
            ORDER BY ak.id DESC
        )");

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = FieldHelper::getString(row["name"], "");
            item["accessKeyPrefix"] = FieldHelper::getString(row["access_key_prefix"], "");
            item["status"] = FieldHelper::getString(row["status"], "enabled");
            item["allowRealtime"] = FieldHelper::getBool(row["allow_realtime"], false);
            item["allowHistory"] = FieldHelper::getBool(row["allow_history"], false);
            item["allowCommand"] = FieldHelper::getBool(row["allow_command"], false);
            item["allowAlert"] = FieldHelper::getBool(row["allow_alert"], false);
            item["expiresAt"] = row["expires_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["expires_at"], ""));
            item["lastUsedAt"] = row["last_used_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["last_used_at"], ""));
            item["lastUsedIp"] = row["last_used_ip"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["last_used_ip"], ""));
            item["remark"] = row["remark"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["remark"], ""));
            item["createdAt"] = FieldHelper::getString(row["created_at"], "");
            item["updatedAt"] = FieldHelper::getString(row["updated_at"], "");
            item["webhookCount"] = static_cast<Json::Int64>(FieldHelper::getInt64(row["webhook_count"], 0));
            item["deviceIds"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["device_ids"], "[]"),
                Json::Value(Json::arrayValue)
            );
            items.append(std::move(item));
        }

        co_return items;
    }

    Task<Json::Value> createAccessKey(const Json::Value& data, int createdBy) {
        ValidatorHelper::requireNonEmptyString(data, "name", "AccessKey 名称").throwIfInvalid();
        validateStatus(data, "status");

        if (!data.isMember("deviceIds")) {
            throw ConflictException("必须配置可访问的设备 deviceIds");
        }

        auto deviceIds = OpenAccess::normalizeDeviceIds(data["deviceIds"]);
        co_await ensureDevicesExistAndAccessible(deviceIds, createdBy);

        bool allowRealtime = data.get("allowRealtime", true).asBool();
        bool allowHistory = data.get("allowHistory", true).asBool();
        bool allowCommand = data.get("allowCommand", false).asBool();
        bool allowAlert = data.get("allowAlert", false).asBool();
        if (!allowRealtime && !allowHistory && !allowCommand && !allowAlert) {
            throw ConflictException("实时、历史、控制、告警权限不能同时关闭");
        }

        std::string status = data.get("status", "enabled").asString();
        std::string expiresAt = data.get("expiresAt", "").asString();
        std::string remark = data.get("remark", "").asString();

        std::string accessKey = OpenAccess::generateAccessKey();
        std::string accessKeyHash = OpenAccess::sha256Hex(accessKey);
        std::string prefix = accessKey.substr(0, std::min<size_t>(14, accessKey.size()));

        auto result = co_await dbService_.execSqlCoro(R"(
            INSERT INTO open_access_key (
                name, access_key_prefix, access_key_hash, status,
                allow_realtime, allow_history, allow_command, allow_alert,
                expires_at, created_by, remark
            )
            VALUES (
                ?, ?, ?, ?, ?::boolean, ?::boolean, ?::boolean, ?::boolean,
                NULLIF(?, '')::timestamptz, ?::int, NULLIF(?, '')
            )
            RETURNING id
        )", {
            data["name"].asString(),
            prefix,
            accessKeyHash,
            status,
            OpenAccess::boolToSql(allowRealtime),
            OpenAccess::boolToSql(allowHistory),
            OpenAccess::boolToSql(allowCommand),
            OpenAccess::boolToSql(allowAlert),
            expiresAt,
            std::to_string(createdBy),
            remark
        });

        int accessKeyId = FieldHelper::getInt(result[0]["id"]);
        co_await replaceAccessKeyDevices(accessKeyId, deviceIds);

        Json::Value resp;
        resp["id"] = accessKeyId;
        resp["name"] = data["name"].asString();
        resp["status"] = status;
        resp["allowRealtime"] = allowRealtime;
        resp["allowHistory"] = allowHistory;
        resp["allowCommand"] = allowCommand;
        resp["allowAlert"] = allowAlert;
        resp["expiresAt"] = expiresAt.empty() ? Json::nullValue : Json::Value(expiresAt);
        resp["deviceIds"] = OpenAccess::toJsonArray(deviceIds);
        resp["accessKey"] = accessKey;
        resp["accessKeyPrefix"] = prefix;
        co_return resp;
    }

    Task<void> updateAccessKey(int id, const Json::Value& data, int updatedBy) {
        auto current = co_await getAccessKeyById(id);

        std::string name = data.isMember("name")
            ? data["name"].asString()
            : current["name"].asString();
        if (name.empty()) {
            throw ValidationException("AccessKey 名称不能为空");
        }

        validateStatus(data, "status");
        std::string status = data.isMember("status")
            ? data["status"].asString()
            : current["status"].asString();

        bool allowRealtime = data.isMember("allowRealtime")
            ? data["allowRealtime"].asBool()
            : current["allowRealtime"].asBool();
        bool allowHistory = data.isMember("allowHistory")
            ? data["allowHistory"].asBool()
            : current["allowHistory"].asBool();
        bool allowCommand = data.isMember("allowCommand")
            ? data["allowCommand"].asBool()
            : current["allowCommand"].asBool();
        bool allowAlert = data.isMember("allowAlert")
            ? data["allowAlert"].asBool()
            : current["allowAlert"].asBool();
        if (!allowRealtime && !allowHistory && !allowCommand && !allowAlert) {
            throw ConflictException("实时、历史、控制、告警权限不能同时关闭");
        }

        std::string expiresAt;
        if (data.isMember("expiresAt")) {
            expiresAt = data["expiresAt"].isNull() ? "" : data["expiresAt"].asString();
        } else if (!current["expiresAt"].isNull()) {
            expiresAt = current["expiresAt"].asString();
        }

        std::string remark;
        if (data.isMember("remark")) {
            remark = data["remark"].isNull() ? "" : data["remark"].asString();
        } else if (!current["remark"].isNull()) {
            remark = current["remark"].asString();
        }

        co_await dbService_.execSqlCoro(R"(
            UPDATE open_access_key
            SET name = ?,
                status = ?,
                allow_realtime = ?::boolean,
                allow_history = ?::boolean,
                allow_command = ?::boolean,
                allow_alert = ?::boolean,
                expires_at = NULLIF(?, '')::timestamptz,
                remark = NULLIF(?, ''),
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {
            name,
            status,
            OpenAccess::boolToSql(allowRealtime),
            OpenAccess::boolToSql(allowHistory),
            OpenAccess::boolToSql(allowCommand),
            OpenAccess::boolToSql(allowAlert),
            expiresAt,
            remark,
            std::to_string(id)
        });

        if (data.isMember("deviceIds")) {
            auto deviceIds = OpenAccess::normalizeDeviceIds(data["deviceIds"]);
            co_await ensureDevicesExistAndAccessible(deviceIds, updatedBy);
            co_await replaceAccessKeyDevices(id, deviceIds);
        }
    }

    Task<Json::Value> rotateAccessKey(int id) {
        auto current = co_await getAccessKeyById(id);

        std::string accessKey = OpenAccess::generateAccessKey();
        std::string accessKeyHash = OpenAccess::sha256Hex(accessKey);
        std::string prefix = accessKey.substr(0, std::min<size_t>(14, accessKey.size()));

        co_await dbService_.execSqlCoro(R"(
            UPDATE open_access_key
            SET access_key_hash = ?,
                access_key_prefix = ?,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {accessKeyHash, prefix, std::to_string(id)});

        Json::Value resp;
        resp["id"] = id;
        resp["name"] = current["name"];
        resp["accessKey"] = accessKey;
        resp["accessKeyPrefix"] = prefix;
        co_return resp;
    }

    Task<void> removeAccessKey(int id) {
        co_await getAccessKeyById(id);
        co_await dbService_.execSqlCoro(
            "DELETE FROM open_access_key_device WHERE access_key_id = ?::int",
            {std::to_string(id)}
        );
        co_await dbService_.execSqlCoro(R"(
            UPDATE open_webhook
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE access_key_id = ?::int
              AND deleted_at IS NULL
        )", {std::to_string(id)});
        co_await dbService_.execSqlCoro(R"(
            UPDATE open_access_key
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {std::to_string(id)});
    }

    Task<Json::Value> listWebhooks(int accessKeyId = 0) {
        std::string sql = R"(
            SELECT
                w.id,
                w.access_key_id,
                ak.name AS access_key_name,
                w.name,
                w.url,
                w.status,
                w.timeout_seconds,
                w.headers,
                w.event_types,
                w.secret,
                w.last_triggered_at,
                w.last_success_at,
                w.last_failure_at,
                w.last_http_status,
                w.last_error,
                w.created_at,
                w.updated_at,
                COALESCE(
                    jsonb_agg(DISTINCT akd.device_id) FILTER (WHERE akd.device_id IS NOT NULL),
                    '[]'::jsonb
                ) AS device_ids
            FROM open_webhook w
            INNER JOIN open_access_key ak ON ak.id = w.access_key_id AND ak.deleted_at IS NULL
            LEFT JOIN open_access_key_device akd ON akd.access_key_id = ak.id
            WHERE w.deleted_at IS NULL
        )";

        std::vector<std::string> params;
        if (accessKeyId > 0) {
            sql += " AND w.access_key_id = ?::int";
            params.push_back(std::to_string(accessKeyId));
        }

        sql += R"(
            GROUP BY w.id, ak.id, ak.name
            ORDER BY w.id DESC
        )";

        auto result = co_await dbService_.execSqlCoro(sql, params);

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["accessKeyId"] = FieldHelper::getInt(row["access_key_id"]);
            item["accessKeyName"] = FieldHelper::getString(row["access_key_name"], "");
            item["name"] = FieldHelper::getString(row["name"], "");
            item["url"] = FieldHelper::getString(row["url"], "");
            item["status"] = FieldHelper::getString(row["status"], "enabled");
            item["timeoutSeconds"] = FieldHelper::getInt(row["timeout_seconds"], 5);
            item["headers"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["headers"], "{}"),
                Json::Value(Json::objectValue)
            );
            item["eventTypes"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["event_types"], "[]"),
                Json::Value(Json::arrayValue)
            );
            item["deviceIds"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["device_ids"], "[]"),
                Json::Value(Json::arrayValue)
            );
            item["hasSecret"] = !FieldHelper::getString(row["secret"], "").empty();
            item["lastTriggeredAt"] = row["last_triggered_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["last_triggered_at"], ""));
            item["lastSuccessAt"] = row["last_success_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["last_success_at"], ""));
            item["lastFailureAt"] = row["last_failure_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["last_failure_at"], ""));
            item["lastHttpStatus"] = row["last_http_status"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["last_http_status"], 0));
            item["lastError"] = row["last_error"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["last_error"], ""));
            item["createdAt"] = FieldHelper::getString(row["created_at"], "");
            item["updatedAt"] = FieldHelper::getString(row["updated_at"], "");
            items.append(std::move(item));
        }

        co_return items;
    }

    Task<Json::Value> createWebhook(const Json::Value& data) {
        ValidatorHelper::requireNonEmptyString(data, "name", "Webhook 名称").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(data, "url", "Webhook 地址").throwIfInvalid();
        validateStatus(data, "status");

        int accessKeyId = resolveWebhookAccessKeyId(data);
        auto accessKey = co_await getAccessKeyById(accessKeyId);
        requireWebhookDeviceIds(accessKey, "创建");

        std::string url = data["url"].asString();
        OpenAccess::parseWebhookUrl(url);

        Json::Value headers = requireWebhookHeaders(data, Json::Value(Json::objectValue));

        Json::Value eventTypes = normalizeEventTypes(data);

        int timeoutSeconds = requireWebhookTimeoutSeconds(data, 5);

        std::string status = data.get("status", "enabled").asString();
        std::string secret = data.get("secret", "").asString();

        auto result = co_await dbService_.execSqlCoro(R"(
            INSERT INTO open_webhook (
                access_key_id, name, url, status, secret,
                headers, event_types, timeout_seconds
            )
            VALUES (
                ?::int, ?, ?, ?, NULLIF(?, ''),
                ?::jsonb, ?::jsonb, ?::int
            )
            RETURNING id
        )", {
            std::to_string(accessKeyId),
            data["name"].asString(),
            url,
            status,
            secret,
            JsonHelper::serialize(headers),
            JsonHelper::serialize(eventTypes),
            std::to_string(timeoutSeconds)
        });

        Json::Value resp;
        resp["id"] = FieldHelper::getInt(result[0]["id"]);
        resp["accessKeyId"] = accessKeyId;
        resp["name"] = data["name"].asString();
        resp["url"] = url;
        resp["status"] = status;
        resp["timeoutSeconds"] = timeoutSeconds;
        resp["headers"] = headers;
        resp["eventTypes"] = eventTypes;
        resp["hasSecret"] = !secret.empty();
        co_return resp;
    }

    Task<void> updateWebhook(int id, const Json::Value& data) {
        auto current = co_await getWebhookById(id);

        validateStatus(data, "status");

        int accessKeyId = resolveWebhookAccessKeyId(data, current["accessKeyId"].asInt());
        auto accessKey = co_await getAccessKeyById(accessKeyId);
        requireWebhookDeviceIds(accessKey, "绑定");

        std::string name = data.isMember("name")
            ? data["name"].asString()
            : current["name"].asString();
        if (name.empty()) {
            throw ValidationException("Webhook 名称不能为空");
        }

        std::string url = data.isMember("url")
            ? data["url"].asString()
            : current["url"].asString();
        OpenAccess::parseWebhookUrl(url);

        std::string status = data.isMember("status")
            ? data["status"].asString()
            : current["status"].asString();

        Json::Value headers = data.isMember("headers")
            ? data["headers"]
            : current["headers"];
        headers = requireWebhookHeaders(data, headers);

        Json::Value eventTypes = data.isMember("eventTypes")
            ? normalizeEventTypes(data)
            : current["eventTypes"];

        int timeoutSeconds = requireWebhookTimeoutSeconds(data, current["timeoutSeconds"].asInt());

        std::string secret;
        if (data.isMember("secret")) {
            secret = data["secret"].isNull() ? "" : data["secret"].asString();
        } else if (current.isMember("secret") && !current["secret"].isNull()) {
            secret = current["secret"].asString();
        }

        co_await dbService_.execSqlCoro(R"(
            UPDATE open_webhook
            SET access_key_id = ?::int,
                name = ?,
                url = ?,
                status = ?,
                secret = NULLIF(?, ''),
                headers = ?::jsonb,
                event_types = ?::jsonb,
                timeout_seconds = ?::int,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {
            std::to_string(accessKeyId),
            name,
            url,
            status,
            secret,
            JsonHelper::serialize(headers),
            JsonHelper::serialize(eventTypes),
            std::to_string(timeoutSeconds),
            std::to_string(id)
        });
    }

    Task<void> removeWebhook(int id) {
        co_await getWebhookById(id);
        co_await dbService_.execSqlCoro(R"(
            UPDATE open_webhook
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {std::to_string(id)});
    }

    Task<std::pair<Json::Value, int>> listAccessLogs(
        int page,
        int pageSize,
        int accessKeyId = 0,
        int webhookId = 0,
        int deviceId = 0,
        const std::string& direction = "",
        const std::string& action = "",
        const std::string& status = "",
        const std::string& eventType = ""
    ) {
        std::string where = " WHERE 1=1";
        std::vector<std::string> params;

        if (accessKeyId > 0) {
            where += " AND l.access_key_id = ?::int";
            params.push_back(std::to_string(accessKeyId));
        }
        if (webhookId > 0) {
            where += " AND l.webhook_id = ?::int";
            params.push_back(std::to_string(webhookId));
        }
        if (deviceId > 0) {
            where += " AND l.device_id = ?::int";
            params.push_back(std::to_string(deviceId));
        }
        if (!direction.empty()) {
            where += " AND l.direction = ?";
            params.push_back(direction);
        }
        if (!action.empty()) {
            where += " AND l.action = ?";
            params.push_back(action);
        }
        if (!status.empty()) {
            where += " AND l.status = ?";
            params.push_back(status);
        }
        if (!eventType.empty()) {
            where += " AND l.event_type = ?";
            params.push_back(eventType);
        }

        auto countResult = co_await dbService_.execSqlCoro(
            "SELECT COUNT(*) AS count FROM open_access_log l" + where,
            params
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"], 0);

        std::string sql = R"(
            SELECT
                l.id,
                l.access_key_id,
                ak.name AS access_key_name,
                l.webhook_id,
                w.name AS webhook_name,
                l.direction,
                l.action,
                l.event_type,
                l.status,
                l.http_method,
                l.target,
                l.request_ip,
                l.http_status,
                l.device_id,
                l.device_code,
                l.message,
                l.request_payload,
                l.response_payload,
                l.created_at
            FROM open_access_log l
            LEFT JOIN open_access_key ak ON ak.id = l.access_key_id
            LEFT JOIN open_webhook w ON w.id = l.webhook_id
        )" + where + " ORDER BY l.id DESC";

        if (pageSize > 0) {
            int offset = std::max(0, (page - 1) * pageSize);
            sql += " LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
        }

        auto result = co_await dbService_.execSqlCoro(sql, params);

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(FieldHelper::getInt64(row["id"], 0));
            item["accessKeyId"] = row["access_key_id"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["access_key_id"], 0));
            item["accessKeyName"] = row["access_key_name"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["access_key_name"], ""));
            item["webhookId"] = row["webhook_id"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["webhook_id"], 0));
            item["webhookName"] = row["webhook_name"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["webhook_name"], ""));
            item["direction"] = FieldHelper::getString(row["direction"], "");
            item["action"] = FieldHelper::getString(row["action"], "");
            item["eventType"] = row["event_type"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["event_type"], ""));
            item["status"] = FieldHelper::getString(row["status"], "");
            item["httpMethod"] = row["http_method"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["http_method"], ""));
            item["target"] = row["target"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["target"], ""));
            item["requestIp"] = row["request_ip"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["request_ip"], ""));
            item["httpStatus"] = row["http_status"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["http_status"], 0));
            item["deviceId"] = row["device_id"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["device_id"], 0));
            item["deviceCode"] = row["device_code"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["device_code"], ""));
            item["message"] = row["message"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["message"], ""));
            item["requestPayload"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["request_payload"], "{}"),
                Json::Value(Json::objectValue)
            );
            item["responsePayload"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["response_payload"], "{}"),
                Json::Value(Json::objectValue)
            );
            item["createdAt"] = FieldHelper::getString(row["created_at"], "");
            items.append(std::move(item));
        }

        co_return std::make_pair(items, total);
    }

    Task<std::pair<Json::Value, int>> listAlertRecords(
        const std::set<int>& allowedDeviceIds,
        int page,
        int pageSize,
        int requestedDeviceId = 0,
        int ruleId = 0,
        const std::string& status = "",
        const std::string& severity = ""
    ) {
        if (allowedDeviceIds.empty()) {
            co_return std::make_pair(Json::Value(Json::arrayValue), 0);
        }

        std::vector<std::string> params;
        params.reserve(allowedDeviceIds.size() + 4);

        std::string where = " WHERE r.device_id IN (" + OpenAccess::buildPlaceholders(allowedDeviceIds.size()) + ")";
        for (int deviceId : allowedDeviceIds) {
            params.push_back(std::to_string(deviceId));
        }

        if (requestedDeviceId > 0) {
            where += " AND r.device_id = ?::int";
            params.push_back(std::to_string(requestedDeviceId));
        }
        if (ruleId > 0) {
            where += " AND r.rule_id = ?::int";
            params.push_back(std::to_string(ruleId));
        }
        if (!status.empty()) {
            where += " AND r.status = ?";
            params.push_back(status);
        }
        if (!severity.empty()) {
            where += " AND r.severity = ?";
            params.push_back(severity);
        }

        auto countResult = co_await dbService_.execSqlCoro(
            "SELECT COUNT(*) AS count FROM alert_record r" + where,
            params
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"], 0);

        std::string sql = R"(
            SELECT
                r.id,
                r.rule_id,
                r.device_id,
                r.severity,
                r.status,
                r.message,
                r.detail,
                r.triggered_at,
                r.acknowledged_at,
                r.acknowledged_by,
                r.resolved_at,
                d.name AS device_name,
                ar.name AS rule_name
            FROM alert_record r
            LEFT JOIN device d ON d.id = r.device_id AND d.deleted_at IS NULL
            LEFT JOIN alert_rule ar ON ar.id = r.rule_id AND ar.deleted_at IS NULL
        )" + where + " ORDER BY r.triggered_at DESC";

        if (pageSize > 0) {
            int offset = std::max(0, (page - 1) * pageSize);
            sql += " LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
        }

        auto result = co_await dbService_.execSqlCoro(sql, params);

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(FieldHelper::getInt64(row["id"], 0));
            item["rule_id"] = FieldHelper::getInt(row["rule_id"], 0);
            item["rule_name"] = row["rule_name"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["rule_name"], ""));
            item["device_id"] = FieldHelper::getInt(row["device_id"], 0);
            item["device_name"] = row["device_name"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["device_name"], ""));
            item["severity"] = FieldHelper::getString(row["severity"], "");
            item["status"] = FieldHelper::getString(row["status"], "");
            item["message"] = FieldHelper::getString(row["message"], "");
            item["detail"] = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["detail"], "{}"),
                Json::Value(Json::objectValue)
            );
            item["triggered_at"] = FieldHelper::getString(row["triggered_at"], "");
            item["acknowledged_at"] = row["acknowledged_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["acknowledged_at"], ""));
            item["acknowledged_by"] = row["acknowledged_by"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["acknowledged_by"], 0));
            item["resolved_at"] = row["resolved_at"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getString(row["resolved_at"], ""));
            items.append(std::move(item));
        }

        co_return std::make_pair(items, total);
    }

    static Task<void> writeAccessLog(
        const std::string& direction,
        const std::string& action,
        int accessKeyId = 0,
        int webhookId = 0,
        const std::string& eventType = "",
        const std::string& status = "success",
        const std::string& httpMethod = "",
        const std::string& target = "",
        const std::string& requestIp = "",
        int httpStatus = 0,
        int deviceId = 0,
        const std::string& deviceCode = "",
        const std::string& message = "",
        const Json::Value& requestPayload = Json::Value(Json::objectValue),
        const Json::Value& responsePayload = Json::Value(Json::objectValue)
    ) {
        DatabaseService dbService;
        co_await dbService.execSqlCoro(R"(
            INSERT INTO open_access_log (
                access_key_id,
                webhook_id,
                direction,
                action,
                event_type,
                status,
                http_method,
                target,
                request_ip,
                http_status,
                device_id,
                device_code,
                message,
                request_payload,
                response_payload
            )
            VALUES (
                NULLIF(?, '')::int,
                NULLIF(?, '')::int,
                ?,
                ?,
                NULLIF(?, ''),
                ?,
                NULLIF(?, ''),
                NULLIF(?, ''),
                NULLIF(?, ''),
                NULLIF(?, '')::int,
                NULLIF(?, '')::int,
                NULLIF(?, ''),
                NULLIF(?, ''),
                ?::jsonb,
                ?::jsonb
            )
        )", {
            accessKeyId > 0 ? std::to_string(accessKeyId) : "",
            webhookId > 0 ? std::to_string(webhookId) : "",
            direction,
            action,
            eventType,
            status,
            httpMethod,
            target,
            requestIp,
            httpStatus > 0 ? std::to_string(httpStatus) : "",
            deviceId > 0 ? std::to_string(deviceId) : "",
            deviceCode,
            OpenAccess::sanitizeError(message, 1000),
            JsonHelper::serialize(requestPayload),
            JsonHelper::serialize(responsePayload)
        });
    }

    Task<int> tryResolveAccessKeyId(const std::string& accessKey) {
        if (accessKey.empty()) {
            co_return 0;
        }

        auto result = co_await dbService_.execSqlCoro(R"(
            SELECT id
            FROM open_access_key
            WHERE access_key_hash = ?
              AND deleted_at IS NULL
            LIMIT 1
        )", {OpenAccess::sha256Hex(accessKey)});

        co_return result.empty() ? 0 : FieldHelper::getInt(result[0]["id"], 0);
    }

    Task<OpenAccess::AccessKeySession> authenticate(const std::string& accessKey, const std::string& clientIp) {
        if (accessKey.empty()) {
            throw AppException(ErrorCodes::UNAUTHORIZED, "缺少 AccessKey", drogon::k401Unauthorized);
        }

        auto result = co_await dbService_.execSqlCoro(R"(
            SELECT
                id,
                name,
                status,
                allow_realtime,
                allow_history,
                allow_command,
                allow_alert,
                CASE
                    WHEN expires_at IS NOT NULL AND expires_at <= CURRENT_TIMESTAMP THEN TRUE
                    ELSE FALSE
                END AS expired
            FROM open_access_key
            WHERE access_key_hash = ?
              AND deleted_at IS NULL
            LIMIT 1
        )", {OpenAccess::sha256Hex(accessKey)});

        if (result.empty()) {
            throw AppException(ErrorCodes::UNAUTHORIZED, "AccessKey 无效", drogon::k401Unauthorized);
        }

        const auto& row = result[0];
        if (FieldHelper::getString(row["status"], "disabled") != "enabled") {
            throw ForbiddenException("AccessKey 已被禁用");
        }
        if (FieldHelper::getBool(row["expired"], false)) {
            throw AppException(ErrorCodes::UNAUTHORIZED, "AccessKey 已过期", drogon::k401Unauthorized);
        }

        OpenAccess::AccessKeySession session;
        session.id = FieldHelper::getInt(row["id"]);
        session.name = FieldHelper::getString(row["name"], "");
        session.allowRealtime = FieldHelper::getBool(row["allow_realtime"], false);
        session.allowHistory = FieldHelper::getBool(row["allow_history"], false);
        session.allowCommand = FieldHelper::getBool(row["allow_command"], false);
        session.allowAlert = FieldHelper::getBool(row["allow_alert"], false);

        auto deviceIds = co_await loadAccessKeyDeviceIds(session.id);
        session.deviceIds = {deviceIds.begin(), deviceIds.end()};
        if (session.deviceIds.empty()) {
            throw ForbiddenException("AccessKey 未配置可访问设备");
        }

        touchAccessKeyUsage(session.id, clientIp);
        co_return session;
    }

    Task<int> resolveDeviceId(const std::string& code, int deviceId = 0) {
        if (!code.empty()) {
            auto result = co_await dbService_.execSqlCoro(
                "SELECT id FROM device WHERE protocol_params->>'device_code' = ? AND deleted_at IS NULL LIMIT 1",
                {code}
            );
            int resolvedId = result.empty() ? 0 : FieldHelper::getInt(result[0]["id"]);
            if (deviceId > 0 && resolvedId > 0 && resolvedId != deviceId) {
                throw ConflictException("deviceId 与 code 不匹配");
            }
            if (resolvedId > 0) {
                co_return resolvedId;
            }
            if (deviceId <= 0) {
                co_return 0;
            }
        }

        if (deviceId > 0) {
            auto result = co_await dbService_.execSqlCoro(
                "SELECT id FROM device WHERE id = ?::int AND deleted_at IS NULL LIMIT 1",
                {std::to_string(deviceId)}
            );
            co_return result.empty() ? 0 : FieldHelper::getInt(result[0]["id"]);
        }

        co_return 0;
    }

    Task<std::vector<OpenAccess::WebhookTarget>> listActiveWebhookTargets(const std::set<int>& deviceIds) {
        if (deviceIds.empty()) {
            co_return std::vector<OpenAccess::WebhookTarget>{};
        }

        std::vector<std::string> params;
        params.reserve(deviceIds.size());
        for (int deviceId : deviceIds) {
            params.push_back(std::to_string(deviceId));
        }

        auto inClause = "(" + OpenAccess::buildPlaceholders(deviceIds.size()) + ")";
        std::string sql = R"(
            SELECT
                w.id,
                w.access_key_id,
                ak.name AS access_key_name,
                w.name,
                w.url,
                w.secret,
                w.timeout_seconds,
                w.headers,
                w.event_types,
                COALESCE(
                    jsonb_agg(DISTINCT akd.device_id) FILTER (WHERE akd.device_id IS NOT NULL),
                    '[]'::jsonb
                ) AS device_ids
            FROM open_webhook w
            INNER JOIN open_access_key ak ON ak.id = w.access_key_id
            INNER JOIN open_access_key_device akd ON akd.access_key_id = ak.id
            WHERE w.deleted_at IS NULL
              AND w.status = 'enabled'
              AND ak.deleted_at IS NULL
              AND ak.status = 'enabled'
              AND (ak.expires_at IS NULL OR ak.expires_at > CURRENT_TIMESTAMP)
              AND akd.device_id IN )" + inClause + R"(
            GROUP BY w.id, ak.id, ak.name
        )";

        auto result = co_await dbService_.execSqlCoro(sql, params);

        std::vector<OpenAccess::WebhookTarget> targets;
        targets.reserve(result.size());
        for (const auto& row : result) {
            OpenAccess::WebhookTarget target;
            target.id = FieldHelper::getInt(row["id"]);
            target.accessKeyId = FieldHelper::getInt(row["access_key_id"]);
            target.accessKeyName = FieldHelper::getString(row["access_key_name"], "");
            target.name = FieldHelper::getString(row["name"], "");
            target.url = FieldHelper::getString(row["url"], "");
            target.secret = FieldHelper::getString(row["secret"], "");
            target.timeoutSeconds = FieldHelper::getInt(row["timeout_seconds"], 5);
            target.headers = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["headers"], "{}"),
                Json::Value(Json::objectValue)
            );
            Json::Value eventTypes = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["event_types"], "[]"),
                Json::Value(Json::arrayValue)
            );
            for (const auto& eventType : eventTypes) {
                if (eventType.isString()) {
                    target.eventTypes.insert(eventType.asString());
                }
            }

            Json::Value ids = OpenAccess::parseJsonOrDefault(
                FieldHelper::getString(row["device_ids"], "[]"),
                Json::Value(Json::arrayValue)
            );
            for (const auto& value : ids) {
                if (value.isNumeric()) {
                    target.deviceIds.insert(value.asInt());
                }
            }

            targets.push_back(std::move(target));
        }

        co_return targets;
    }

    static Task<void> markWebhookSuccess(int id, int httpStatus) {
        DatabaseService dbService;
        co_await dbService.execSqlCoro(R"(
            UPDATE open_webhook
            SET last_triggered_at = CURRENT_TIMESTAMP,
                last_success_at = CURRENT_TIMESTAMP,
                last_http_status = ?::int,
                last_error = NULL,
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {std::to_string(httpStatus), std::to_string(id)});
    }

    static Task<void> markWebhookFailure(int id, const std::string& error, int httpStatus = 0) {
        DatabaseService dbService;
        co_await dbService.execSqlCoro(R"(
            UPDATE open_webhook
            SET last_triggered_at = CURRENT_TIMESTAMP,
                last_failure_at = CURRENT_TIMESTAMP,
                last_http_status = NULLIF(?, '')::int,
                last_error = NULLIF(?, ''),
                updated_at = CURRENT_TIMESTAMP
            WHERE id = ?::int
              AND deleted_at IS NULL
        )", {
            httpStatus > 0 ? std::to_string(httpStatus) : "",
            OpenAccess::sanitizeError(error),
            std::to_string(id)
        });
    }

private:
    DatabaseService dbService_;

    static void validateStatus(const Json::Value& data, const std::string& field) {
        if (!ValidatorHelper::isInListIfPresent(data, field, {"enabled", "disabled"})) {
            throw ValidationException(field + " 只能为 enabled 或 disabled");
        }
    }

    static int resolveWebhookAccessKeyId(const Json::Value& data, int defaultValue = 0) {
        int accessKeyId = data.isMember("accessKeyId")
            ? data["accessKeyId"].asInt()
            : defaultValue;
        if (accessKeyId <= 0) {
            throw ValidationException("请选择调用配置");
        }
        return accessKeyId;
    }

    static void requireWebhookDeviceIds(const Json::Value& accessKey, const std::string& action) {
        const auto& deviceIds = ValidatorHelper::requireArrayValue(
            accessKey["deviceIds"],
            "AccessKey 未配置可访问设备"
        );
        if (deviceIds.empty()) {
            throw ConflictException("AccessKey 未配置可访问设备，无法" + action + " webhook");
        }
    }

    static Json::Value requireWebhookHeaders(const Json::Value& data, const Json::Value& fallback) {
        if (!data.isMember("headers")) {
            return fallback;
        }
        return ValidatorHelper::requireObjectValue(data["headers"], "headers 必须是对象");
    }

    static int requireWebhookTimeoutSeconds(const Json::Value& data, int defaultValue) {
        return ValidatorHelper::optionalIntRangeField(
            data,
            "timeoutSeconds",
            defaultValue,
            1,
            30,
            "timeoutSeconds 必须在 1-30 秒之间"
        );
    }

    static Json::Value normalizeEventTypes(const Json::Value& data) {
        Json::Value eventTypes = data.isMember("eventTypes")
            ? data["eventTypes"]
            : Json::Value(Json::arrayValue);

        if (!eventTypes.isArray() || eventTypes.empty()) {
            Json::Value defaults(Json::arrayValue);
            defaults.append(OpenAccess::WEBHOOK_EVENT_DEVICE_DATA);
            return defaults;
        }

        std::set<std::string> uniqueTypes;
        Json::Value normalized(Json::arrayValue);
        for (const auto& item : eventTypes) {
            std::string eventType = item.asString();
            if (!OpenAccess::supportedWebhookEvents().contains(eventType)) {
                throw ValidationException(
                    "不支持的 webhook 事件: " + eventType
                );
            }
            if (uniqueTypes.insert(eventType).second) {
                normalized.append(eventType);
            }
        }
        return normalized;
    }

    Task<void> ensureDevicesExistAndAccessible(const std::vector<int>& deviceIds, int userId) {
        if (deviceIds.empty()) {
            throw ConflictException("至少配置一个可访问设备");
        }

        std::vector<std::string> params;
        params.reserve(deviceIds.size());
        for (int deviceId : deviceIds) {
            params.push_back(std::to_string(deviceId));
        }

        auto result = co_await dbService_.execSqlCoro(
            "SELECT id, name, created_by FROM device WHERE deleted_at IS NULL AND id IN (" +
            OpenAccess::buildPlaceholders(deviceIds.size()) + ")",
            params
        );

        if (result.size() != deviceIds.size()) {
            throw NotFoundException("deviceIds 中存在无效设备");
        }

        if (userId <= 0 || co_await PermissionChecker::isSuperAdmin(userId)) {
            co_return;
        }

        std::vector<int> sharedCheckIds;
        sharedCheckIds.reserve(deviceIds.size());
        std::unordered_map<int, std::string> deviceNames;
        deviceNames.reserve(result.size());

        for (const auto& row : result) {
            int deviceId = FieldHelper::getInt(row["id"]);
            int createdBy = row["created_by"].isNull() ? 0 : FieldHelper::getInt(row["created_by"]);
            deviceNames[deviceId] = FieldHelper::getString(row["name"], "ID:" + std::to_string(deviceId));
            if (createdBy != userId) {
                sharedCheckIds.push_back(deviceId);
            }
        }

        if (sharedCheckIds.empty()) {
            co_return;
        }

        auto sharePermissions = co_await ResourcePermission::loadDeviceSharePermissions(
            userId,
            sharedCheckIds
        );

        std::vector<std::string> forbiddenDevices;
        forbiddenDevices.reserve(sharedCheckIds.size());
        for (int deviceId : sharedCheckIds) {
            if (sharePermissions.contains(deviceId)) {
                continue;
            }
            forbiddenDevices.push_back(deviceNames.contains(deviceId)
                ? deviceNames[deviceId]
                : ("ID:" + std::to_string(deviceId)));
        }

        if (!forbiddenDevices.empty()) {
            throw ForbiddenException(
                "无权绑定以下设备: " + OpenAccess::sanitizeError(
                    [forbiddenDevices]() {
                        std::string joined;
                        for (size_t i = 0; i < forbiddenDevices.size(); ++i) {
                            if (i > 0) joined += ", ";
                            joined += forbiddenDevices[i];
                        }
                        return joined;
                    }(),
                    300
                )
            );
        }
    }

    Task<std::vector<int>> loadAccessKeyDeviceIds(int accessKeyId) {
        auto result = co_await dbService_.execSqlCoro(R"(
            SELECT device_id
            FROM open_access_key_device
            WHERE access_key_id = ?::int
            ORDER BY device_id ASC
        )", {std::to_string(accessKeyId)});

        std::vector<int> ids;
        ids.reserve(result.size());
        for (const auto& row : result) {
            ids.push_back(FieldHelper::getInt(row["device_id"]));
        }
        co_return ids;
    }

    Task<void> replaceAccessKeyDevices(int accessKeyId, const std::vector<int>& deviceIds) {
        co_await dbService_.execSqlCoro(
            "DELETE FROM open_access_key_device WHERE access_key_id = ?::int",
            {std::to_string(accessKeyId)}
        );

        for (int deviceId : deviceIds) {
            co_await dbService_.execSqlCoro(R"(
                INSERT INTO open_access_key_device (access_key_id, device_id)
                VALUES (?::int, ?::int)
                ON CONFLICT (access_key_id, device_id) DO NOTHING
            )", {std::to_string(accessKeyId), std::to_string(deviceId)});
        }
    }

    Task<Json::Value> getAccessKeyById(int id) {
        auto result = co_await dbService_.execSqlCoro(R"(
            SELECT
                id,
                name,
                status,
                allow_realtime,
                allow_history,
                allow_command,
                allow_alert,
                expires_at,
                remark
            FROM open_access_key
            WHERE id = ?::int
              AND deleted_at IS NULL
            LIMIT 1
        )", {std::to_string(id)});

        if (result.empty()) {
            throw NotFoundException("AccessKey 不存在");
        }

        auto deviceIds = co_await loadAccessKeyDeviceIds(id);

        Json::Value item;
        item["id"] = id;
        item["name"] = FieldHelper::getString(result[0]["name"], "");
        item["status"] = FieldHelper::getString(result[0]["status"], "enabled");
        item["allowRealtime"] = FieldHelper::getBool(result[0]["allow_realtime"], false);
        item["allowHistory"] = FieldHelper::getBool(result[0]["allow_history"], false);
        item["allowCommand"] = FieldHelper::getBool(result[0]["allow_command"], false);
        item["allowAlert"] = FieldHelper::getBool(result[0]["allow_alert"], false);
        item["expiresAt"] = result[0]["expires_at"].isNull()
            ? Json::nullValue
            : Json::Value(FieldHelper::getString(result[0]["expires_at"], ""));
        item["remark"] = result[0]["remark"].isNull()
            ? Json::nullValue
            : Json::Value(FieldHelper::getString(result[0]["remark"], ""));
        item["deviceIds"] = OpenAccess::toJsonArray(deviceIds);
        co_return item;
    }

    Task<Json::Value> getWebhookById(int id) {
        auto result = co_await dbService_.execSqlCoro(R"(
            SELECT
                w.id,
                w.access_key_id,
                w.name,
                w.url,
                w.status,
                w.timeout_seconds,
                w.headers,
                w.event_types,
                w.secret
            FROM open_webhook w
            WHERE w.id = ?::int
              AND w.deleted_at IS NULL
            LIMIT 1
        )", {std::to_string(id)});

        if (result.empty()) {
            throw NotFoundException("Webhook 不存在");
        }

        Json::Value item;
        item["id"] = id;
        item["accessKeyId"] = FieldHelper::getInt(result[0]["access_key_id"]);
        item["name"] = FieldHelper::getString(result[0]["name"], "");
        item["url"] = FieldHelper::getString(result[0]["url"], "");
        item["status"] = FieldHelper::getString(result[0]["status"], "enabled");
        item["timeoutSeconds"] = FieldHelper::getInt(result[0]["timeout_seconds"], 5);
        item["headers"] = OpenAccess::parseJsonOrDefault(
            FieldHelper::getString(result[0]["headers"], "{}"),
            Json::Value(Json::objectValue)
        );
        item["eventTypes"] = OpenAccess::parseJsonOrDefault(
            FieldHelper::getString(result[0]["event_types"], "[]"),
            Json::Value(Json::arrayValue)
        );
        item["secret"] = result[0]["secret"].isNull()
            ? Json::nullValue
            : Json::Value(FieldHelper::getString(result[0]["secret"], ""));
        item["hasSecret"] = !FieldHelper::getString(result[0]["secret"], "").empty();
        co_return item;
    }

    void touchAccessKeyUsage(int accessKeyId, const std::string& clientIp) {
        drogon::async_run([accessKeyId, clientIp]() -> Task<void> {
            try {
                DatabaseService dbService;
                co_await dbService.execSqlCoro(R"(
                    UPDATE open_access_key
                    SET last_used_at = CURRENT_TIMESTAMP,
                        last_used_ip = NULLIF(?, '')
                    WHERE id = ?::int
                      AND deleted_at IS NULL
                )", {clientIp, std::to_string(accessKeyId)});
            } catch (const std::exception& e) {
                LOG_WARN << "[OpenAccess] touchAccessKeyUsage failed: " << e.what();
            }
        });
    }
};
