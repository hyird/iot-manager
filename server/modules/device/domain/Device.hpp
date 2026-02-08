#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/device/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/JsonHelper.hpp"

/**
 * @brief 设备聚合根
 *
 * IoT 系统核心实体，管理设备的基本信息和配置。
 *
 * 使用示例：
 * @code
 * // 创建设备
 * co_await Device::create(data)
 *     .require(Device::nameUnique)
 *     .require(Device::codeUnique)
 *     .require(Device::linkExists)
 *     .require(Device::protocolConfigExists)
 *     .save();
 *
 * // 更新设备
 * co_await Device::of(id)
 *     .update(data)
 *     .save();
 *
 * // 删除设备
 * co_await Device::of(id)
 *     .remove()
 *     .save();
 * @endcode
 */
class Device : public Aggregate<Device> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的设备
     */
    static Task<Device> of(int id) {
        Device device;
        co_await device.load(id);
        co_return device;
    }

    /**
     * @brief 创建新设备
     */
    static Device create(const Json::Value& data) {
        Device device;
        device.applyCreate(data);
        return device;
    }

    /**
     * @brief 分页查询设备列表
     */
    static Task<PagedResult<Device>> list(
        const Pagination& page,
        int linkId = 0,
        int protocolConfigId = 0,
        const std::string& status = ""
    ) {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted("d.deleted_at");
        if (!page.keyword.empty()) {
            qb.likeAny({"d.name", "d.protocol_params->>'device_code'"}, page.keyword);
        }
        if (linkId > 0) qb.eq("d.link_id", std::to_string(linkId));
        if (protocolConfigId > 0) qb.eq("d.protocol_config_id", std::to_string(protocolConfigId));
        if (!status.empty()) qb.eq("d.status", status);

        // 计数
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM device d" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        // 查询数据（包含关联信息）
        std::string sql = R"(
            SELECT d.*,
                   l.name as link_name, l.mode as link_mode, l.protocol as link_protocol,
                   p.name as protocol_name, p.protocol as protocol_type
            FROM device d
            LEFT JOIN link l ON d.link_id = l.id AND l.deleted_at IS NULL
            LEFT JOIN protocol_config p ON d.protocol_config_id = p.id AND p.deleted_at IS NULL
        )" + qb.whereClause() + " ORDER BY d.id DESC" + page.limitClause();

        auto result = co_await db.execSqlCoro(sql, qb.params());

        std::vector<Device> devices;
        devices.reserve(result.size());
        for (const auto& row : result) {
            Device device;
            device.fromRow(row, true);
            devices.push_back(std::move(device));
        }

        co_return PagedResult<Device>{std::move(devices), total, page.page, page.pageSize};
    }

    /**
     * @brief 获取所有设备选项（下拉选择用）
     */
    static Task<Json::Value> options() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT id, name, protocol_params->>'device_code' as device_code FROM device WHERE deleted_at IS NULL ORDER BY name ASC"
        );

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = FieldHelper::getString(row["name"]);
            item["device_code"] = FieldHelper::getString(row["device_code"], "");
            items.append(item);
        }
        co_return items;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：设备名称唯一
     */
    static Task<void> nameUnique(const Device& device) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM device WHERE name = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {device.name_};

        if (device.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(device.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("设备名称已存在");
        }
    }

    /**
     * @brief 约束：设备编码唯一
     */
    static Task<void> codeUnique(const Device& device) {
        auto code = device.deviceCode();
        if (code.empty()) co_return;

        DatabaseService db;
        std::string sql = "SELECT 1 FROM device WHERE protocol_params->>'device_code' = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {code};

        if (device.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(device.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("设备编码已存在");
        }
    }

    /**
     * @brief 约束：关联链路存在
     */
    static Task<void> linkExists(const Device& device) {
        if (device.linkId_ <= 0) co_return;

        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM link WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(device.linkId_)}
        );

        if (result.empty()) {
            throw ValidationException("关联的链路不存在");
        }
    }

    /**
     * @brief 约束：关联协议配置存在
     */
    static Task<void> protocolConfigExists(const Device& device) {
        if (device.protocolConfigId_ <= 0) co_return;

        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM protocol_config WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(device.protocolConfigId_)}
        );

        if (result.empty()) {
            throw ValidationException("关联的协议配置不存在");
        }
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新设备信息
     */
    Device& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        // link_id 和 protocol_config_id 创建后不可修改（避免历史数据不一致）
        if (data.isMember("group_id")) {
            groupId_ = (data["group_id"].isNull() || data["group_id"].asInt() == 0)
                ? 0 : data["group_id"].asInt();
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

        // 协议特有参数
        for (const auto& key : {"device_code", "online_timeout", "remote_control",
                                  "modbus_mode", "slave_id", "timezone",
                                  "heartbeat", "registration"}) {
            if (data.isMember(key)) {
                // 检测注册包/心跳包变更
                if ((std::string(key) == "registration" || std::string(key) == "heartbeat") &&
                    protocolParams_[key] != data[key]) {
                    registrationChanged_ = true;
                }
                protocolParams_[key] = data[key];
                markDirty();
            }
        }
        return *this;
    }

    /**
     * @brief 标记删除
     */
    Device& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& name() const { return name_; }
    int linkId() const { return linkId_; }
    int protocolConfigId() const { return protocolConfigId_; }
    int groupId() const { return groupId_; }
    const std::string& status() const { return status_; }
    const std::string& remark() const { return remark_; }
    const Json::Value& protocolParams() const { return protocolParams_; }

    // 协议参数便捷访问
    std::string deviceCode() const { return protocolParams_.get("device_code", "").asString(); }
    int onlineTimeout() const { return protocolParams_.get("online_timeout", 300).asInt(); }
    bool remoteControl() const { return protocolParams_.get("remote_control", true).asBool(); }
    std::string modbusMode() const { return protocolParams_.get("modbus_mode", "").asString(); }
    int slaveId() const { return protocolParams_.get("slave_id", 1).asInt(); }
    std::string timezone() const { return protocolParams_.get("timezone", "+08:00").asString(); }

    // 关联数据
    const std::string& linkName() const { return linkName_; }
    const std::string& linkMode() const { return linkMode_; }
    const std::string& linkProtocol() const { return linkProtocol_; }
    const std::string& protocolName() const { return protocolName_; }
    const std::string& protocolType() const { return protocolType_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["link_id"] = linkId_;
        json["protocol_config_id"] = protocolConfigId_;
        json["group_id"] = groupId_ > 0 ? Json::Value(groupId_) : Json::Value::null;
        json["status"] = status_;
        json["remark"] = remark_;
        json["created_at"] = createdAt_;
        json["updated_at"] = updatedAt_;

        // 协议参数展开为顶层字段（API 兼容）
        json["device_code"] = deviceCode();
        json["online_timeout"] = onlineTimeout();
        json["remote_control"] = remoteControl();
        json["timezone"] = timezone();
        auto mm = modbusMode();
        if (!mm.empty()) {
            json["modbus_mode"] = mm;
        }
        auto sid = slaveId();
        if (sid > 0) {
            json["slave_id"] = sid;
        }
        if (protocolParams_.isMember("heartbeat")) {
            json["heartbeat"] = protocolParams_["heartbeat"];
        }
        if (protocolParams_.isMember("registration")) {
            json["registration"] = protocolParams_["registration"];
        }

        // 关联信息
        json["link_name"] = linkName_;
        json["link_mode"] = linkMode_;
        json["link_protocol"] = linkProtocol_;
        json["protocol_name"] = protocolName_;
        json["protocol_type"] = protocolType_;

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
    // 设备基本字段
    std::string name_;
    int linkId_ = 0;
    int protocolConfigId_ = 0;
    int groupId_ = 0;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    Json::Value protocolParams_ = Json::objectValue;  // 协议特有参数 (JSONB)
    std::string remark_;
    std::string createdAt_;
    std::string updatedAt_;
    bool registrationChanged_ = false;  // 注册包/心跳包是否变更（用于断开连接重新注册）

    // 关联数据（只读）
    std::string linkName_;
    std::string linkMode_;
    std::string linkProtocol_;
    std::string protocolName_;
    std::string protocolType_;

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        linkId_ = data.get("link_id", 0).asInt();
        protocolConfigId_ = data.get("protocol_config_id", 0).asInt();
        groupId_ = (data.isMember("group_id") && !data["group_id"].isNull())
            ? data["group_id"].asInt() : 0;
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
        remark_ = data.get("remark", "").asString();

        // 收集协议特有参数
        protocolParams_ = Json::objectValue;
        for (const auto& key : {"device_code", "online_timeout", "remote_control",
                                  "modbus_mode", "slave_id", "timezone",
                                  "heartbeat", "registration"}) {
            if (data.isMember(key) && !data[key].isNull()) {
                protocolParams_[key] = data[key];
            }
        }
    }

    Task<void> load(int deviceId) {
        auto result = co_await db().execSqlCoro(R"(
            SELECT d.*,
                   l.name as link_name, l.mode as link_mode, l.protocol as link_protocol,
                   p.name as protocol_name, p.protocol as protocol_type
            FROM device d
            LEFT JOIN link l ON d.link_id = l.id AND l.deleted_at IS NULL
            LEFT JOIN protocol_config p ON d.protocol_config_id = p.id AND p.deleted_at IS NULL
            WHERE d.id = ? AND d.deleted_at IS NULL
        )", {std::to_string(deviceId)});

        if (result.empty()) {
            throw NotFoundException("设备不存在");
        }

        fromRow(result[0], true);
        markLoaded();
    }

    void fromRow(const Row& row, bool withRelations = false) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        linkId_ = FieldHelper::getInt(row["link_id"]);
        protocolConfigId_ = FieldHelper::getInt(row["protocol_config_id"]);
        groupId_ = row["group_id"].isNull() ? 0 : FieldHelper::getInt(row["group_id"]);
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        remark_ = FieldHelper::getString(row["remark"], "");
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");

        // 解析 protocol_params JSONB
        protocolParams_ = Json::objectValue;
        std::string ppStr = FieldHelper::getString(row["protocol_params"], "");
        if (!ppStr.empty()) {
            Json::CharReaderBuilder rb;
            std::istringstream iss(ppStr);
            std::string errs;
            Json::parseFromStream(rb, iss, &protocolParams_, &errs);
        }

        if (withRelations) {
            linkName_ = FieldHelper::getString(row["link_name"], "");
            linkMode_ = FieldHelper::getString(row["link_mode"], "");
            linkProtocol_ = FieldHelper::getString(row["link_protocol"], "");
            protocolName_ = FieldHelper::getString(row["protocol_name"], "");
            protocolType_ = FieldHelper::getString(row["protocol_type"], "");
        }
    }

    Task<void> persistCreate(TransactionGuard& tx) {
        std::string ppJson = JsonHelper::serialize(protocolParams_);

        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO device (name, link_id, protocol_config_id, group_id,
                               status, protocol_params, remark, created_at)
            VALUES (?, ?, ?, NULLIF(?, '0')::INT, ?, ?::jsonb, ?, ?) RETURNING id
        )", {
            name_, std::to_string(linkId_), std::to_string(protocolConfigId_),
            std::to_string(groupId_),
            status_, ppJson, remark_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<DeviceCreated>(id(), deviceCode());
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        std::string ppJson = JsonHelper::serialize(protocolParams_);

        co_await tx.execSqlCoro(R"(
            UPDATE device
            SET name = ?, link_id = ?, protocol_config_id = ?, group_id = NULLIF(?, '0')::INT,
                status = ?, protocol_params = ?::jsonb, remark = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, std::to_string(linkId_), std::to_string(protocolConfigId_),
            std::to_string(groupId_),
            status_, ppJson, remark_, TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<DeviceUpdated>(id(), linkId_, registrationChanged_);
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        co_await tx.execSqlCoro(
            "UPDATE device SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<DeviceDeleted>(id());
    }
};
