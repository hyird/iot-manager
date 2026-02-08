#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/alert/domain/Events.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/JsonHelper.hpp"

/**
 * @brief 告警规则聚合根
 *
 * 使用示例：
 * @code
 * co_await AlertRule::create(data)
 *     .require(AlertRule::nameUnique)
 *     .require(AlertRule::deviceExists)
 *     .save();
 * @endcode
 */
class AlertRule : public Aggregate<AlertRule> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    static Task<AlertRule> of(int id) {
        AlertRule rule;
        co_await rule.load(id);
        co_return rule;
    }

    static AlertRule create(const Json::Value& data) {
        AlertRule rule;
        rule.applyCreate(data);
        return rule;
    }

    /**
     * @brief 分页查询规则列表
     */
    static Task<PagedResult<AlertRule>> list(
        const Pagination& page,
        int deviceId = 0,
        const std::string& severity = ""
    ) {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted("r.deleted_at");
        if (!page.keyword.empty()) {
            qb.likeAny({"r.name", "r.remark"}, page.keyword);
        }
        if (deviceId > 0) qb.eq("r.device_id", std::to_string(deviceId));
        if (!severity.empty()) qb.eq("r.severity", severity);

        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) AS count FROM alert_rule r" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        std::string sql = R"(
            SELECT r.*, d.name AS device_name
            FROM alert_rule r
            LEFT JOIN device d ON r.device_id = d.id AND d.deleted_at IS NULL
        )" + qb.whereClause() + " ORDER BY r.id DESC" + page.limitClause();

        auto result = co_await db.execSqlCoro(sql, qb.params());

        std::vector<AlertRule> rules;
        for (const auto& row : result) {
            AlertRule rule;
            rule.fromRow(row);
            rules.push_back(std::move(rule));
        }

        co_return PagedResult<AlertRule>{std::move(rules), total, page.page, page.pageSize};
    }

    /**
     * @brief 加载所有启用的规则（AlertEngine 启动用）
     */
    static Task<Json::Value> allEnabled() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT r.*, d.name AS device_name
            FROM alert_rule r
            LEFT JOIN device d ON r.device_id = d.id AND d.deleted_at IS NULL
            WHERE r.status = 'enabled' AND r.deleted_at IS NULL
            ORDER BY r.device_id, r.id
        )");

        Json::Value list(Json::arrayValue);
        for (const auto& row : result) {
            AlertRule rule;
            rule.fromRow(row);
            list.append(rule.toJson());
        }
        co_return list;
    }

    // ==================== 声明式约束 ====================

    static Task<void> nameUnique(const AlertRule& rule) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM alert_rule WHERE name = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {rule.name_};
        if (rule.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(rule.id()));
        }
        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("规则名称已存在");
        }
    }

    static Task<void> deviceExists(const AlertRule& rule) {
        if (rule.deviceId_ <= 0) {
            throw ValidationException("必须关联设备");
        }
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM device WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(rule.deviceId_)}
        );
        if (result.empty()) {
            throw ValidationException("关联的设备不存在");
        }
    }

    // ==================== 业务操作 ====================

    AlertRule& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("device_id")) {
            deviceId_ = data["device_id"].asInt();
            markDirty();
        }
        if (data.isMember("severity")) {
            severity_ = data["severity"].asString();
            markDirty();
        }
        if (data.isMember("conditions")) {
            conditions_ = data["conditions"];
            markDirty();
        }
        if (data.isMember("logic")) {
            logic_ = data["logic"].asString();
            markDirty();
        }
        if (data.isMember("silence_duration")) {
            silenceDuration_ = data["silence_duration"].asInt();
            markDirty();
        }
        if (data.isMember("recovery_condition")) {
            recoveryCondition_ = data["recovery_condition"].asString();
            markDirty();
        }
        if (data.isMember("recovery_wait_seconds")) {
            recoveryWaitSeconds_ = data["recovery_wait_seconds"].asInt();
            markDirty();
        }
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
        }
        if (data.isMember("remark")) {
            remark_ = data["remark"].asString();
            markDirty();
        }
        return *this;
    }

    AlertRule& remove() {
        markDeleted();
        return *this;
    }

    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["device_id"] = deviceId_;
        json["device_name"] = deviceName_;
        json["severity"] = severity_;
        json["conditions"] = conditions_;
        json["logic"] = logic_;
        json["silence_duration"] = silenceDuration_;
        json["recovery_condition"] = recoveryCondition_;
        json["recovery_wait_seconds"] = recoveryWaitSeconds_;
        json["status"] = status_;
        json["remark"] = remark_;
        json["created_at"] = createdAt_;
        json["updated_at"] = updatedAt_;
        return json;
    }

    // ==================== 持久化 ====================

    Task<void> doPersist(TransactionGuard& tx) {
        if (isDeleted()) {
            co_await persistDelete(tx);
        } else if (isNew()) {
            co_await persistCreate(tx);
        } else if (isDirty()) {
            co_await persistUpdate(tx);
        }
    }

    // Getters
    int deviceId() const { return deviceId_; }
    const std::string& name() const { return name_; }
    const std::string& severity() const { return severity_; }

private:
    std::string name_;
    int deviceId_ = 0;
    std::string severity_ = "warning";
    Json::Value conditions_{Json::arrayValue};
    std::string logic_ = "and";
    int silenceDuration_ = 300;
    std::string recoveryCondition_ = "reverse";
    int recoveryWaitSeconds_ = 60;
    std::string status_ = "enabled";
    std::string remark_;
    std::string createdAt_;
    std::string updatedAt_;
    // 关联只读
    std::string deviceName_;

    void applyCreate(const Json::Value& data) {
        name_ = data["name"].asString();
        deviceId_ = data["device_id"].asInt();
        severity_ = data.get("severity", "warning").asString();
        if (data.isMember("conditions")) conditions_ = data["conditions"];
        logic_ = data.get("logic", "and").asString();
        silenceDuration_ = data.get("silence_duration", 300).asInt();
        recoveryCondition_ = data.get("recovery_condition", "reverse").asString();
        recoveryWaitSeconds_ = data.get("recovery_wait_seconds", 60).asInt();
        status_ = data.get("status", "enabled").asString();
        remark_ = data.get("remark", "").asString();
    }

    Task<void> load(int id) {
        auto result = co_await db().execSqlCoro(R"(
            SELECT r.*, d.name AS device_name
            FROM alert_rule r
            LEFT JOIN device d ON r.device_id = d.id AND d.deleted_at IS NULL
            WHERE r.id = ? AND r.deleted_at IS NULL
        )", {std::to_string(id)});

        if (result.empty()) {
            throw NotFoundException("告警规则不存在");
        }
        fromRow(result[0]);
    }

    void fromRow(const Row& row) {
        setId(row["id"].as<int>());
        name_ = row["name"].as<std::string>();
        deviceId_ = row["device_id"].as<int>();
        severity_ = row["severity"].as<std::string>();

        // JSONB → Json::Value
        conditions_ = JsonHelper::parse(row["conditions"].as<std::string>());

        logic_ = row["logic"].as<std::string>();
        silenceDuration_ = row["silence_duration"].as<int>();
        recoveryCondition_ = row["recovery_condition"].isNull() ? "reverse" : row["recovery_condition"].as<std::string>();
        recoveryWaitSeconds_ = row["recovery_wait_seconds"].isNull() ? 60 : row["recovery_wait_seconds"].as<int>();
        status_ = row["status"].as<std::string>();
        remark_ = row["remark"].isNull() ? "" : row["remark"].as<std::string>();
        createdAt_ = row["created_at"].as<std::string>();
        updatedAt_ = row["updated_at"].isNull() ? "" : row["updated_at"].as<std::string>();

        if (!row["device_name"].isNull()) {
            deviceName_ = row["device_name"].as<std::string>();
        }

        markLoaded();
    }

    Task<void> persistCreate(TransactionGuard& tx) {
        std::string condJson = JsonHelper::serialize(conditions_);

        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO alert_rule (name, device_id, severity, conditions, logic, silence_duration,
                                    recovery_condition, recovery_wait_seconds, status, remark, created_at)
            VALUES (?, ?, ?, ?::jsonb, ?, ?, ?, ?, ?, ?, ?)
            RETURNING id
        )", {
            name_, std::to_string(deviceId_), severity_,
            condJson, logic_, std::to_string(silenceDuration_),
            recoveryCondition_, std::to_string(recoveryWaitSeconds_),
            status_, remark_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<AlertRuleCreated>(id());
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        std::string condJson = JsonHelper::serialize(conditions_);

        co_await tx.execSqlCoro(R"(
            UPDATE alert_rule
            SET name = ?, device_id = ?, severity = ?, conditions = ?::jsonb,
                logic = ?, silence_duration = ?, recovery_condition = ?, recovery_wait_seconds = ?,
                status = ?, remark = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, std::to_string(deviceId_), severity_,
            condJson, logic_, std::to_string(silenceDuration_),
            recoveryCondition_, std::to_string(recoveryWaitSeconds_),
            status_, remark_, TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<AlertRuleUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        co_await tx.execSqlCoro(
            "UPDATE alert_rule SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );
        raiseEvent<AlertRuleDeleted>(id());
    }
};
