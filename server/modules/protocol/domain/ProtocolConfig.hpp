#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/protocol/domain/Events.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 协议配置聚合根
 *
 * 统一管理 SL651、Modbus 等协议的设备类型配置。
 * 配置数据存储在 JSONB 字段中。
 *
 * 使用示例：
 * @code
 * // 创建配置
 * co_await ProtocolConfig::create(data)
 *     .require(ProtocolConfig::protocolRequired)
 *     .require(ProtocolConfig::nameRequired)
 *     .require(ProtocolConfig::nameUnique)
 *     .save();
 *
 * // 更新配置
 * co_await ProtocolConfig::of(id)
 *     .update(data)
 *     .save();
 *
 * // 删除配置（校验无关联设备）
 * co_await ProtocolConfig::of(id)
 *     .require(ProtocolConfig::noDevices)
 *     .remove()
 *     .save();
 * @endcode
 */
class ProtocolConfig : public Aggregate<ProtocolConfig> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的配置
     */
    static Task<ProtocolConfig> of(int id) {
        ProtocolConfig config;
        co_await config.load(id);
        co_return config;
    }

    /**
     * @brief 创建新配置
     */
    static ProtocolConfig create(const Json::Value& data) {
        ProtocolConfig config;
        config.applyCreate(data);
        return config;
    }

    /**
     * @brief 分页查询配置列表
     */
    static Task<PagedResult<ProtocolConfig>> list(const Pagination& page, const std::string& protocol = "") {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted();
        if (!protocol.empty()) qb.eq("protocol", protocol);
        if (!page.keyword.empty()) qb.likeAny({"name", "remark"}, page.keyword);

        // 计数
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM protocol_config" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        // 查询数据
        std::string sql = "SELECT * FROM protocol_config" + qb.whereClause()
                        + " ORDER BY protocol ASC, id ASC" + page.limitClause();
        auto result = co_await db.execSqlCoro(sql, qb.params());

        std::vector<ProtocolConfig> configs;
        configs.reserve(result.size());
        for (const auto& row : result) {
            ProtocolConfig config;
            config.fromRow(row);
            configs.push_back(std::move(config));
        }

        co_return PagedResult<ProtocolConfig>{std::move(configs), total, page.page, page.pageSize};
    }

    /**
     * @brief 获取指定协议的配置选项（下拉选择用）
     */
    static Task<Json::Value> options(const std::string& protocol) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT id, name, enabled FROM protocol_config WHERE protocol = ? AND deleted_at IS NULL AND enabled = true ORDER BY name ASC",
            {protocol}
        );

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = FieldHelper::getString(row["name"]);
            items.append(item);
        }
        co_return items;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：协议类型必填
     */
    static Task<void> protocolRequired(const ProtocolConfig& config) {
        if (config.protocol_.empty()) {
            throw ValidationException("协议类型不能为空");
        }
        co_return;
    }

    /**
     * @brief 约束：名称必填
     */
    static Task<void> nameRequired(const ProtocolConfig& config) {
        if (config.name_.empty()) {
            throw ValidationException("名称不能为空");
        }
        co_return;
    }

    /**
     * @brief 约束：配置名称全局唯一（跨协议）
     */
    static Task<void> nameUnique(const ProtocolConfig& config) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM protocol_config WHERE name = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {config.name_};

        if (config.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(config.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("配置名称已存在");
        }
    }

    /**
     * @brief 约束：配置下无关联设备（删除前校验）
     */
    static Task<void> noDevices(const ProtocolConfig& config) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM device WHERE protocol_config_id = ? AND deleted_at IS NULL LIMIT 1",
            {std::to_string(config.id())}
        );
        if (!result.empty()) {
            throw ValidationException("该协议配置下存在关联设备，无法删除");
        }
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新配置信息
     */
    ProtocolConfig& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("enabled")) {
            enabled_ = data["enabled"].asBool();
            markDirty();
        }
        if (data.isMember("config")) {
            config_ = data["config"];
            markDirty();
        }
        if (data.isMember("remark")) {
            remark_ = data["remark"].asString();
            markDirty();
        }
        return *this;
    }

    /**
     * @brief 标记删除
     */
    ProtocolConfig& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& protocol() const { return protocol_; }
    const std::string& name() const { return name_; }
    bool enabled() const { return enabled_; }
    const Json::Value& config() const { return config_; }
    const std::string& remark() const { return remark_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["protocol"] = protocol_;
        json["name"] = name_;
        json["enabled"] = enabled_;
        json["config"] = config_;
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

private:
    std::string protocol_;
    std::string name_;
    bool enabled_ = true;
    Json::Value config_{Json::objectValue};
    std::string remark_;
    std::string createdAt_;
    std::string updatedAt_;

    void applyCreate(const Json::Value& data) {
        protocol_ = data.get("protocol", "").asString();
        name_ = data.get("name", "").asString();
        enabled_ = data.get("enabled", true).asBool();
        config_ = data.get("config", Json::objectValue);
        remark_ = data.get("remark", "").asString();
    }

    Task<void> load(int configId) {
        auto result = co_await db().execSqlCoro(
            "SELECT * FROM protocol_config WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(configId)}
        );

        if (result.empty()) {
            throw NotFoundException("配置不存在");
        }

        fromRow(result[0]);
        markLoaded();
    }

    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        protocol_ = FieldHelper::getString(row["protocol"]);
        name_ = FieldHelper::getString(row["name"]);
        enabled_ = row["enabled"].as<bool>();
        remark_ = FieldHelper::getString(row["remark"], "");
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");

        // 解析 JSONB config 字段
        std::string configStr = FieldHelper::getString(row["config"], "{}");
        Json::CharReaderBuilder reader;
        std::istringstream ss(configStr);
        std::string errs;
        if (!Json::parseFromStream(reader, ss, &config_, &errs)) {
            config_ = Json::objectValue;
        }
    }

    std::string serializeConfig() const {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        writer["emitUTF8"] = true;
        return Json::writeString(writer, config_);
    }

    // ==================== 持久化操作 ====================

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO protocol_config (protocol, name, enabled, config, remark, created_at)
            VALUES (?, ?, ?, ?::jsonb, ?, ?) RETURNING id
        )", {
            protocol_, name_, enabled_ ? "true" : "false",
            serializeConfig(), remark_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<ProtocolConfigCreated>(id(), protocol_, name_);
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE protocol_config
            SET name = ?, enabled = ?, config = ?::jsonb, remark = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, enabled_ ? "true" : "false", serializeConfig(),
            remark_, TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<ProtocolConfigUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        co_await tx.execSqlCoro(
            "UPDATE protocol_config SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<ProtocolConfigDeleted>(id());
    }
};
