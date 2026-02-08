#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/device-group/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/TreeBuilder.hpp"

/**
 * @brief 设备分组聚合根
 *
 * 使用示例：
 * @code
 * // 创建分组
 * co_await DeviceGroup::create(data)
 *     .require(DeviceGroup::nameUnique)
 *     .save();
 *
 * // 更新分组
 * co_await DeviceGroup::of(id)
 *     .update(data)
 *     .save();
 *
 * // 删除分组
 * co_await DeviceGroup::of(id)
 *     .require(DeviceGroup::noChildren)
 *     .require(DeviceGroup::noDevices)
 *     .remove()
 *     .save();
 * @endcode
 */
class DeviceGroup : public Aggregate<DeviceGroup> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的分组
     */
    static Task<DeviceGroup> of(int id) {
        DeviceGroup group;
        co_await group.load(id);
        co_return group;
    }

    /**
     * @brief 创建新分组
     */
    static DeviceGroup create(const Json::Value& data) {
        DeviceGroup group;
        group.applyCreate(data);
        return group;
    }

    /**
     * @brief 获取分组列表（平铺）
     */
    static Task<Json::Value> list(const std::string& keyword = "", const std::string& status = "") {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted();
        if (!keyword.empty()) qb.likeAny({"name"}, keyword);
        if (!status.empty()) qb.eq("status", status);

        std::string sql = "SELECT * FROM device_group" + qb.whereClause()
                        + " ORDER BY sort_order ASC, id ASC";
        auto result = co_await db.execSqlCoro(sql, qb.params());

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            DeviceGroup group;
            group.fromRow(row);
            items.append(group.toJson());
        }
        co_return items;
    }

    /**
     * @brief 获取分组树形结构
     */
    static Task<Json::Value> tree(const std::string& status = "") {
        auto items = co_await list("", status);
        auto tree = TreeBuilder::build(items, "id", "parent_id", "children");
        TreeBuilder::sort(tree, "sort_order", true);
        co_return tree;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：分组名称唯一
     */
    static Task<void> nameUnique(const DeviceGroup& group) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM device_group WHERE name = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {group.name_};

        if (group.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(group.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("分组名称已存在");
        }
    }

    /**
     * @brief 约束：无子分组
     */
    static Task<void> noChildren(const DeviceGroup& group) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM device_group WHERE parent_id = ? AND deleted_at IS NULL",
            {std::to_string(group.id())}
        );

        if (!result.empty() && FieldHelper::getInt(result[0]["count"]) > 0) {
            throw ValidationException("该分组下存在子分组，无法删除");
        }
        co_return;
    }

    /**
     * @brief 约束：无关联设备
     */
    static Task<void> noDevices(const DeviceGroup& group) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM device WHERE group_id = ? AND deleted_at IS NULL",
            {std::to_string(group.id())}
        );

        if (!result.empty() && FieldHelper::getInt(result[0]["count"]) > 0) {
            throw ValidationException("该分组下存在设备，无法删除");
        }
        co_return;
    }

    /**
     * @brief 约束：父分组存在（parent_id > 0 时检查）
     */
    static Task<void> parentExists(const DeviceGroup& group) {
        if (group.parentId_ <= 0) co_return;

        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM device_group WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(group.parentId_)}
        );

        if (result.empty()) {
            throw ValidationException("父分组不存在");
        }
    }

    /**
     * @brief 约束：不能设置自己或子孙为父级（防止循环引用）
     */
    static Task<void> notSelfParent(const DeviceGroup& group) {
        if (group.parentId_ <= 0 || group.id() <= 0) co_return;
        if (group.parentId_ == group.id()) {
            throw ValidationException("不能将自己设为父分组");
        }

        // 递归检查：获取所有子孙 ID，确认新 parent_id 不在其中
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            WITH RECURSIVE descendants AS (
                SELECT id FROM device_group WHERE parent_id = ? AND deleted_at IS NULL
                UNION ALL
                SELECT dg.id FROM device_group dg
                INNER JOIN descendants d ON dg.parent_id = d.id
                WHERE dg.deleted_at IS NULL
            )
            SELECT id FROM descendants WHERE id = ?
        )", {std::to_string(group.id()), std::to_string(group.parentId_)});

        if (!result.empty()) {
            throw ValidationException("不能将子分组设为父分组，会造成循环引用");
        }
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新分组信息
     */
    DeviceGroup& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("parent_id")) {
            parentId_ = (data["parent_id"].isNull() || data["parent_id"].asInt() == 0)
                ? 0 : data["parent_id"].asInt();
            markDirty();
        }
        if (data.isMember("sort_order")) {
            sortOrder_ = data["sort_order"].asInt();
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

    /**
     * @brief 标记删除
     */
    DeviceGroup& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& name() const { return name_; }
    int parentId() const { return parentId_; }
    int sortOrder() const { return sortOrder_; }
    const std::string& status() const { return status_; }
    const std::string& remark() const { return remark_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["parent_id"] = parentId_ == 0 ? Json::Value::null : Json::Value(parentId_);
        json["sort_order"] = sortOrder_;
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

private:
    std::string name_;
    int parentId_ = 0;
    int sortOrder_ = 0;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    std::string remark_;
    std::string createdAt_;
    std::string updatedAt_;

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        parentId_ = (data.isMember("parent_id") && !data["parent_id"].isNull() && data["parent_id"].asInt() != 0)
            ? data["parent_id"].asInt() : 0;
        sortOrder_ = data.get("sort_order", 0).asInt();
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
        remark_ = data.get("remark", "").asString();
    }

    Task<void> load(int groupId) {
        auto result = co_await db().execSqlCoro(
            "SELECT * FROM device_group WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(groupId)}
        );

        if (result.empty()) {
            throw NotFoundException("设备分组不存在");
        }

        fromRow(result[0]);
        markLoaded();
    }

    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        parentId_ = row["parent_id"].isNull() ? 0 : row["parent_id"].as<int>();
        sortOrder_ = FieldHelper::getInt(row["sort_order"], 0);
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        remark_ = FieldHelper::getString(row["remark"], "");
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
    }

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO device_group (name, parent_id, sort_order, status, remark, created_at)
            VALUES (?, NULLIF(?, '0')::INT, ?, ?, ?, ?)
            RETURNING id
        )", {
            name_, std::to_string(parentId_), std::to_string(sortOrder_),
            status_, remark_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<DeviceGroupCreated>(id());
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE device_group
            SET name = ?, parent_id = NULLIF(?, '0')::INT,
                sort_order = ?, status = ?, remark = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, std::to_string(parentId_), std::to_string(sortOrder_),
            status_, remark_, TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<DeviceGroupUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        co_await tx.execSqlCoro(
            "UPDATE device_group SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<DeviceGroupDeleted>(id());
    }
};
