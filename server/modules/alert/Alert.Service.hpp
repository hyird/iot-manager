#pragma once

#include <numeric>
#include "domain/AlertRule.hpp"
#include "domain/AlertRecord.hpp"
#include "AlertEngine.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 告警业务服务层
 *
 * 协调 AlertRule 聚合根操作，CRUD 后自动刷新 AlertEngine 内存规则。
 */
class AlertService {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    // ==================== 规则 CRUD ====================

    Task<void> createRule(const Json::Value& data) {
        co_await AlertRule::create(data)
            .require(AlertRule::nameUnique)
            .require(AlertRule::deviceExists)
            .save();

        co_await AlertEngine::instance().reloadRules();
    }

    Task<void> updateRule(int id, const Json::Value& data) {
        auto rule = co_await AlertRule::of(id);

        if (data.isMember("name")) {
            rule.require(AlertRule::nameUnique);
        }
        if (data.isMember("device_id")) {
            rule.require(AlertRule::deviceExists);
        }

        rule.update(data);
        co_await rule.save();

        co_await AlertEngine::instance().reloadRules();
    }

    Task<void> deleteRule(int id) {
        auto rule = co_await AlertRule::of(id);
        rule.remove();
        co_await rule.save();

        co_await AlertEngine::instance().reloadRules();
    }

    Task<Json::Value> getRuleDetail(int id) {
        auto rule = co_await AlertRule::of(id);
        co_return rule.toJson();
    }

    Task<PagedResult<AlertRule>> listRules(const Pagination& page, int deviceId = 0, const std::string& severity = "") {
        co_return co_await AlertRule::list(page, deviceId, severity);
    }

    // ==================== 告警记录 ====================

    Task<std::pair<Json::Value, int>> listRecords(const Pagination& page,
        int deviceId = 0, int ruleId = 0,
        const std::string& status = "", const std::string& severity = "") {
        co_return co_await AlertRecord::list(page.page, page.pageSize, deviceId, ruleId, status, severity);
    }

    Task<void> acknowledgeRecord(int64_t id, int userId) {
        co_await AlertRecord::acknowledge(id, userId);
    }

    Task<void> batchAcknowledge(const std::vector<int64_t>& ids, int userId) {
        co_await AlertRecord::batchAcknowledge(ids, userId);
    }

    // ==================== 统计 ====================

    Task<Json::Value> activeAlertStats() {
        co_return co_await AlertRecord::activeStats();
    }

    // ==================== 模板管理 ====================

    Task<void> createTemplate(const Json::Value& data) {
        DatabaseService db;
        std::string conditionsStr = data.get("conditions", Json::arrayValue).toStyledString();
        std::string applicableProtocols = data.get("applicable_protocols", Json::Value(Json::arrayValue)).toStyledString();
        int protocolConfigId = data.get("protocol_config_id", 0).asInt();

        co_await db.execSqlCoro(R"(
            INSERT INTO alert_rule_template
            (name, category, description, severity, conditions, logic, silence_duration,
             recovery_condition, recovery_wait_seconds, applicable_protocols, protocol_config_id, created_by)
            VALUES (?, ?, ?, ?, ?::jsonb, ?, ?, ?, ?, ?::jsonb, ?, ?)
        )", {
            data.get("name", "").asString(),
            data.get("category", "").asString(),
            data.get("description", "").asString(),
            data.get("severity", "warning").asString(),
            conditionsStr,
            data.get("logic", "and").asString(),
            std::to_string(data.get("silence_duration", 300).asInt()),
            data.get("recovery_condition", "reverse").asString(),
            std::to_string(data.get("recovery_wait_seconds", 60).asInt()),
            applicableProtocols,
            std::to_string(protocolConfigId),
            std::to_string(data.get("created_by", 0).asInt())
        });
    }

    Task<void> updateTemplate(int id, const Json::Value& data) {
        DatabaseService db;
        std::vector<std::string> setClauses;
        std::vector<std::string> params;

        if (data.isMember("name")) {
            setClauses.push_back("name = ?");
            params.push_back(data["name"].asString());
        }
        if (data.isMember("category")) {
            setClauses.push_back("category = ?");
            params.push_back(data["category"].asString());
        }
        if (data.isMember("description")) {
            setClauses.push_back("description = ?");
            params.push_back(data["description"].asString());
        }
        if (data.isMember("severity")) {
            setClauses.push_back("severity = ?");
            params.push_back(data["severity"].asString());
        }
        if (data.isMember("conditions")) {
            setClauses.push_back("conditions = ?::jsonb");
            params.push_back(data["conditions"].toStyledString());
        }
        if (data.isMember("logic")) {
            setClauses.push_back("logic = ?");
            params.push_back(data["logic"].asString());
        }
        if (data.isMember("silence_duration")) {
            setClauses.push_back("silence_duration = ?");
            params.push_back(std::to_string(data["silence_duration"].asInt()));
        }
        if (data.isMember("recovery_condition")) {
            setClauses.push_back("recovery_condition = ?");
            params.push_back(data["recovery_condition"].asString());
        }
        if (data.isMember("recovery_wait_seconds")) {
            setClauses.push_back("recovery_wait_seconds = ?");
            params.push_back(std::to_string(data["recovery_wait_seconds"].asInt()));
        }
        if (data.isMember("applicable_protocols")) {
            setClauses.push_back("applicable_protocols = ?::jsonb");
            params.push_back(data["applicable_protocols"].toStyledString());
        }
        if (data.isMember("protocol_config_id")) {
            setClauses.push_back("protocol_config_id = ?");
            params.push_back(std::to_string(data["protocol_config_id"].asInt()));
        }

        if (setClauses.empty()) co_return;

        setClauses.push_back("updated_at = CURRENT_TIMESTAMP");
        params.push_back(std::to_string(id));

        std::string sql = "UPDATE alert_rule_template SET " +
            std::accumulate(std::next(setClauses.begin()), setClauses.end(), setClauses[0],
                [](std::string a, const std::string& b) { return a + ", " + b; }) +
            " WHERE id = ? AND deleted_at IS NULL";

        co_await db.execSqlCoro(sql, params);
    }

    Task<void> deleteTemplate(int id) {
        DatabaseService db;
        co_await db.execSqlCoro(
            "UPDATE alert_rule_template SET deleted_at = CURRENT_TIMESTAMP WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(id)}
        );
    }

    Task<Json::Value> getTemplateDetail(int id) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT * FROM alert_rule_template WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(id)}
        );

        if (result.empty()) {
            throw NotFoundException("模板不存在");
        }

        const auto& row = result[0];
        Json::Value detail;
        detail["id"] = row["id"].as<int>();
        detail["name"] = row["name"].as<std::string>();
        detail["category"] = row["category"].isNull() ? "" : row["category"].as<std::string>();
        detail["description"] = row["description"].isNull() ? "" : row["description"].as<std::string>();
        detail["severity"] = row["severity"].as<std::string>();

        // JSONB 透传
        detail["conditions"] = JsonHelper::parse(row["conditions"].as<std::string>());
        detail["logic"] = row["logic"].as<std::string>();
        detail["silence_duration"] = row["silence_duration"].as<int>();
        detail["recovery_condition"] = row["recovery_condition"].isNull() ? "reverse" : row["recovery_condition"].as<std::string>();
        detail["recovery_wait_seconds"] = row["recovery_wait_seconds"].isNull() ? 60 : row["recovery_wait_seconds"].as<int>();
        detail["applicable_protocols"] = JsonHelper::parse(row["applicable_protocols"].as<std::string>());
        detail["protocol_config_id"] = row["protocol_config_id"].isNull() ? 0 : row["protocol_config_id"].as<int>();
        detail["created_by"] = row["created_by"].as<int>();
        detail["created_at"] = row["created_at"].as<std::string>();

        co_return detail;
    }

    Task<std::pair<Json::Value, int>> listTemplates(const Pagination& page, const std::string& category = "") {
        DatabaseService db;
        std::string where = " WHERE t.deleted_at IS NULL";
        std::vector<std::string> params;

        if (!category.empty()) {
            where += " AND t.category = ?";
            params.push_back(category);
        }

        // COUNT
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) AS count FROM alert_rule_template t" + where, params);
        int total = countResult.empty() ? 0 : countResult[0]["count"].as<int>();

        // 查询（使用 limitClause 支持可选分页）
        std::string sql = R"(
            SELECT t.*, pc.name AS config_name, pc.protocol AS protocol_type
            FROM alert_rule_template t
            LEFT JOIN protocol_config pc ON t.protocol_config_id = pc.id AND pc.deleted_at IS NULL
        )" + where + " ORDER BY t.created_at DESC" + page.limitClause();

        auto result = co_await db.execSqlCoro(sql, params);

        Json::Value list(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = row["id"].as<int>();
            item["name"] = row["name"].as<std::string>();
            item["category"] = row["category"].isNull() ? "" : row["category"].as<std::string>();
            item["description"] = row["description"].isNull() ? "" : row["description"].as<std::string>();
            item["severity"] = row["severity"].as<std::string>();
            item["logic"] = row["logic"].as<std::string>();
            item["silence_duration"] = row["silence_duration"].as<int>();
            item["protocol_config_id"] = row["protocol_config_id"].isNull() ? 0 : row["protocol_config_id"].as<int>();
            item["config_name"] = row["config_name"].isNull() ? "" : row["config_name"].as<std::string>();
            item["protocol_type"] = row["protocol_type"].isNull() ? "" : row["protocol_type"].as<std::string>();
            item["created_at"] = row["created_at"].as<std::string>();
            list.append(item);
        }

        co_return std::make_pair(list, total);
    }

    // ==================== 批量操作 ====================

    Task<void> batchDeleteRules(const std::vector<int>& ids) {
        if (ids.empty()) co_return;

        DatabaseService db;
        std::string placeholders;
        std::vector<std::string> params;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) placeholders += ", ";
            placeholders += "?";
            params.push_back(std::to_string(ids[i]));
        }

        co_await db.execSqlCoro(
            "UPDATE alert_rule SET deleted_at = CURRENT_TIMESTAMP WHERE id IN (" + placeholders + ") AND deleted_at IS NULL",
            params
        );

        co_await AlertEngine::instance().reloadRules();
    }

    Task<Json::Value> applyTemplate(int templateId, const std::vector<int>& deviceIds) {
        // 加载模板
        auto templateData = co_await getTemplateDetail(templateId);

        DatabaseService db;
        std::vector<int> createdIds;

        for (int deviceId : deviceIds) {
            // 获取设备名称
            auto deviceResult = co_await db.execSqlCoro(
                "SELECT name FROM device WHERE id = ? AND deleted_at IS NULL",
                {std::to_string(deviceId)}
            );

            if (deviceResult.empty()) continue;
            std::string deviceName = deviceResult[0]["name"].as<std::string>();

            // 构造规则名称
            std::string ruleName = templateData["name"].asString() + " - " + deviceName;

            // 检查是否已存在同名规则
            auto existingRule = co_await db.execSqlCoro(
                "SELECT id FROM alert_rule WHERE name = ? AND deleted_at IS NULL",
                {ruleName}
            );

            if (!existingRule.empty()) {
                LOG_WARN << "Rule already exists: " << ruleName;
                continue;
            }

            // 创建规则
            Json::Value ruleData;
            ruleData["name"] = ruleName;
            ruleData["device_id"] = deviceId;
            ruleData["severity"] = templateData["severity"];
            ruleData["conditions"] = templateData["conditions"];
            ruleData["logic"] = templateData["logic"];
            ruleData["silence_duration"] = templateData["silence_duration"];
            ruleData["recovery_condition"] = templateData.get("recovery_condition", "reverse");
            ruleData["recovery_wait_seconds"] = templateData.get("recovery_wait_seconds", 60);
            ruleData["status"] = "enabled";

            try {
                auto rule = AlertRule::create(ruleData);
                co_await rule.require(AlertRule::nameUnique)
                    .require(AlertRule::deviceExists)
                    .save();

                createdIds.push_back(rule.id());
            } catch (const std::exception& e) {
                LOG_WARN << "Failed to apply template for device " << deviceId << ": " << e.what();
            }
        }

        co_await AlertEngine::instance().reloadRules();

        Json::Value response;
        response["success"] = static_cast<int>(createdIds.size());
        response["total"] = static_cast<int>(deviceIds.size());
        response["createdIds"] = Json::Value(Json::arrayValue);
        for (int id : createdIds) {
            response["createdIds"].append(id);
        }

        co_return response;
    }

    // ==================== 聚合查询 ====================

    Task<Json::Value> getGroupedRecords(int days) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT ar.id AS rule_id, ar.name AS rule_name,
                   d.id AS device_id, d.name AS device_name,
                   r.severity,
                   COUNT(*) AS total_count,
                   COUNT(*) FILTER (WHERE r.status = 'active') AS active_count,
                   COUNT(*) FILTER (WHERE r.status = 'acknowledged') AS acked_count,
                   COUNT(*) FILTER (WHERE r.status = 'resolved') AS resolved_count,
                   MAX(r.triggered_at) AS latest_trigger_time
            FROM alert_record r
            LEFT JOIN alert_rule ar ON r.rule_id = ar.id AND ar.deleted_at IS NULL
            LEFT JOIN device d ON r.device_id = d.id AND d.deleted_at IS NULL
            WHERE r.triggered_at >= NOW() - INTERVAL '1 days' * ?
            GROUP BY ar.id, ar.name, d.id, d.name, r.severity
            ORDER BY total_count DESC
        )", {std::to_string(days)});

        Json::Value list(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["rule_id"] = row["rule_id"].isNull() ? 0 : row["rule_id"].as<int>();
            item["rule_name"] = row["rule_name"].isNull() ? "" : row["rule_name"].as<std::string>();
            item["device_id"] = row["device_id"].isNull() ? 0 : row["device_id"].as<int>();
            item["device_name"] = row["device_name"].isNull() ? "" : row["device_name"].as<std::string>();
            item["severity"] = row["severity"].as<std::string>();
            item["total_count"] = row["total_count"].as<int>();
            item["active_count"] = row["active_count"].as<int>();
            item["acked_count"] = row["acked_count"].as<int>();
            item["resolved_count"] = row["resolved_count"].as<int>();
            item["latest_trigger_time"] = row["latest_trigger_time"].as<std::string>();
            list.append(item);
        }

        co_return list;
    }

    Task<Json::Value> exportRecords(const std::string& startTime, const std::string& endTime,
                                      const std::string& severity, const std::string& status) {
        DatabaseService db;
        std::string where = " WHERE 1=1";
        std::vector<std::string> params;

        if (!startTime.empty()) {
            where += " AND r.triggered_at >= ?::timestamptz";
            params.push_back(startTime);
        }
        if (!endTime.empty()) {
            where += " AND r.triggered_at <= ?::timestamptz";
            params.push_back(endTime);
        }
        if (!severity.empty()) {
            where += " AND r.severity = ?";
            params.push_back(severity);
        }
        if (!status.empty()) {
            where += " AND r.status = ?";
            params.push_back(status);
        }

        // 显式列名：不取 detail JSONB 列，减少数据传输
        std::string sql = R"(
            SELECT r.id, r.severity, r.status, r.message,
                   r.triggered_at, r.acknowledged_at,
                   d.name AS device_name, ar.name AS rule_name
            FROM alert_record r
            LEFT JOIN device d ON r.device_id = d.id AND d.deleted_at IS NULL
            LEFT JOIN alert_rule ar ON r.rule_id = ar.id AND ar.deleted_at IS NULL
        )" + where + " ORDER BY r.triggered_at DESC";

        auto result = co_await db.execSqlCoro(sql, params);

        Json::Value list(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["device_name"] = row["device_name"].isNull() ? "" : row["device_name"].as<std::string>();
            item["rule_name"] = row["rule_name"].isNull() ? "" : row["rule_name"].as<std::string>();
            item["severity"] = row["severity"].as<std::string>();
            item["status"] = row["status"].as<std::string>();
            item["message"] = row["message"].as<std::string>();
            item["triggered_at"] = row["triggered_at"].as<std::string>();
            item["acknowledged_at"] = row["acknowledged_at"].isNull() ? Json::nullValue
                : Json::Value(row["acknowledged_at"].as<std::string>());
            list.append(item);
        }

        co_return list;
    }
};
