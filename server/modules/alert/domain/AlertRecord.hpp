#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/utils/AppException.hpp"

/**
 * @brief 告警记录辅助类
 *
 * 不继承聚合根（由系统自动生成，无需约束流程），使用静态方法直接操作数据库。
 */
class AlertRecord {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    /**
     * @brief 创建告警记录
     * @return 新记录 ID
     */
    static Task<int64_t> create(int ruleId, int deviceId, const std::string& severity,
                                 const std::string& message, const Json::Value& detail) {
        DatabaseService db;
        std::string detailStr = detail.toStyledString();

        auto result = co_await db.execSqlCoro(R"(
            INSERT INTO alert_record (rule_id, device_id, severity, status, message, detail, triggered_at)
            VALUES (?, ?, ?, 'active', ?, ?::jsonb, CURRENT_TIMESTAMP)
            RETURNING id
        )", {
            std::to_string(ruleId), std::to_string(deviceId),
            severity, message, detailStr
        });

        co_return result.empty() ? 0 : result[0]["id"].as<int64_t>();
    }

    /**
     * @brief 分页查询告警记录
     */
    static Task<std::pair<Json::Value, int>> list(int page, int pageSize,
        int deviceId = 0, int ruleId = 0,
        const std::string& status = "", const std::string& severity = "") {

        DatabaseService db;

        std::string where = " WHERE 1=1";
        std::vector<std::string> params;

        if (deviceId > 0) {
            where += " AND r.device_id = ?";
            params.push_back(std::to_string(deviceId));
        }
        if (ruleId > 0) {
            where += " AND r.rule_id = ?";
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

        // COUNT
        auto countParams = params;
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) AS count FROM alert_record r" + where, countParams);
        int total = countResult.empty() ? 0 : countResult[0]["count"].as<int>();

        // 分页查询（JOIN 设备名和规则名）
        std::string sql = R"(
            SELECT r.*, d.name AS device_name, ar.name AS rule_name
            FROM alert_record r
            LEFT JOIN device d ON r.device_id = d.id
            LEFT JOIN alert_rule ar ON r.rule_id = ar.id
        )" + where + " ORDER BY r.triggered_at DESC";

        if (pageSize > 0) {
            int offset = (page - 1) * pageSize;
            sql += " LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
        }

        auto result = co_await db.execSqlCoro(sql, params);

        Json::Value list(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["rule_id"] = row["rule_id"].as<int>();
            item["rule_name"] = row["rule_name"].isNull() ? "" : row["rule_name"].as<std::string>();
            item["device_id"] = row["device_id"].as<int>();
            item["device_name"] = row["device_name"].isNull() ? "" : row["device_name"].as<std::string>();
            item["severity"] = row["severity"].as<std::string>();
            item["status"] = row["status"].as<std::string>();
            item["message"] = row["message"].as<std::string>();

            // 解析 detail JSONB
            Json::CharReaderBuilder builder;
            Json::Value detailJson;
            std::string detailStr = row["detail"].as<std::string>();
            std::istringstream stream(detailStr);
            Json::parseFromStream(builder, stream, &detailJson, nullptr);
            item["detail"] = detailJson;

            item["triggered_at"] = row["triggered_at"].as<std::string>();
            item["acknowledged_at"] = row["acknowledged_at"].isNull() ? Json::nullValue
                : Json::Value(row["acknowledged_at"].as<std::string>());
            item["acknowledged_by"] = row["acknowledged_by"].isNull() ? Json::nullValue
                : Json::Value(row["acknowledged_by"].as<int>());
            item["resolved_at"] = row["resolved_at"].isNull() ? Json::nullValue
                : Json::Value(row["resolved_at"].as<std::string>());

            list.append(item);
        }

        co_return std::make_pair(list, total);
    }

    /**
     * @brief 确认告警
     */
    static Task<void> acknowledge(int64_t recordId, int userId) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            UPDATE alert_record
            SET status = 'acknowledged', acknowledged_at = CURRENT_TIMESTAMP, acknowledged_by = ?
            WHERE id = ? AND status = 'active'
        )", {std::to_string(userId), std::to_string(recordId)});
    }

    /**
     * @brief 批量确认告警
     */
    static Task<void> batchAcknowledge(const std::vector<int64_t>& ids, int userId) {
        if (ids.empty()) co_return;

        DatabaseService db;
        std::string placeholders;
        std::vector<std::string> params = {std::to_string(userId)};
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) placeholders += ", ";
            placeholders += "?";
            params.push_back(std::to_string(ids[i]));
        }

        co_await db.execSqlCoro(
            "UPDATE alert_record SET status = 'acknowledged', acknowledged_at = CURRENT_TIMESTAMP, "
            "acknowledged_by = ? WHERE id IN (" + placeholders + ") AND status = 'active'",
            params
        );
    }

    /**
     * @brief 恢复告警
     */
    static Task<void> resolve(int64_t recordId) {
        DatabaseService db;
        co_await db.execSqlCoro(R"(
            UPDATE alert_record
            SET status = 'resolved', resolved_at = CURRENT_TIMESTAMP
            WHERE id = ? AND status != 'resolved'
        )", {std::to_string(recordId)});
    }

    /**
     * @brief 活跃告警统计（按严重级别分组）
     */
    static Task<Json::Value> activeStats() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT
                COUNT(*) AS total,
                COUNT(*) FILTER (WHERE severity = 'critical') AS critical,
                COUNT(*) FILTER (WHERE severity = 'warning') AS warning,
                COUNT(*) FILTER (WHERE severity = 'info') AS info
            FROM alert_record
            WHERE status = 'active'
        )");

        Json::Value stats;
        if (!result.empty()) {
            stats["total"] = result[0]["total"].as<int>();
            stats["critical"] = result[0]["critical"].as<int>();
            stats["warning"] = result[0]["warning"].as<int>();
            stats["info"] = result[0]["info"].as<int>();
        } else {
            stats["total"] = 0;
            stats["critical"] = 0;
            stats["warning"] = 0;
            stats["info"] = 0;
        }
        co_return stats;
    }
};
