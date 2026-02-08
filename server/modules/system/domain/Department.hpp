#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/system/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/TreeBuilder.hpp"

/**
 * @brief 部门聚合根
 *
 * 使用示例：
 * @code
 * // 创建部门
 * co_await Department::create(data)
 *     .requireIf(!code.empty(), Department::codeUnique)
 *     .save();
 *
 * // 更新部门
 * co_await Department::of(id)
 *     .update(data)
 *     .save();
 *
 * // 删除部门
 * co_await Department::of(id)
 *     .require(Department::noChildren)
 *     .require(Department::noUsers)
 *     .remove()
 *     .save();
 * @endcode
 */
class Department : public Aggregate<Department> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的部门
     */
    static Task<Department> of(int id) {
        Department dept;
        co_await dept.load(id);
        co_return dept;
    }

    /**
     * @brief 创建新部门
     */
    static Department create(const Json::Value& data) {
        Department dept;
        dept.applyCreate(data);
        return dept;
    }

    /**
     * @brief 获取部门列表（平铺）
     */
    static Task<Json::Value> list(const std::string& keyword = "", const std::string& status = "") {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted();
        if (!keyword.empty()) qb.likeAny({"name", "code"}, keyword);
        if (!status.empty()) qb.eq("status", status);

        std::string sql = "SELECT * FROM sys_department" + qb.whereClause()
                        + " ORDER BY sort_order ASC, id ASC";
        auto result = co_await db.execSqlCoro(sql, qb.params());

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Department dept;
            dept.fromRow(row);
            items.append(dept.toJson());
        }
        co_return items;
    }

    /**
     * @brief 获取部门树形结构
     */
    static Task<Json::Value> tree(const std::string& status = "") {
        auto items = co_await list("", status);
        auto tree = TreeBuilder::build(items, "id", "parent_id", "children");
        TreeBuilder::sort(tree, "sort_order", true);
        co_return tree;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：部门编码唯一
     */
    static Task<void> codeUnique(const Department& dept) {
        if (dept.code_.empty()) co_return;

        DatabaseService db;
        std::string sql = "SELECT 1 FROM sys_department WHERE code = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {dept.code_};

        if (dept.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(dept.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("部门编码已存在");
        }
    }

    /**
     * @brief 约束生成器：不能将部门设为其后代的下级（防止循环引用）
     */
    static Constraint<Department> notCircularParent(int newParentId) {
        return [newParentId](const Department& dept) -> Task<void> {
            if (newParentId == 0) co_return;
            if (newParentId == dept.id()) {
                throw ValidationException("不能将部门设为自己的子部门");
            }

            DatabaseService db;
            auto result = co_await db.execSqlCoro(R"(
                WITH RECURSIVE ancestors AS (
                    SELECT id, parent_id FROM sys_department
                    WHERE id = ? AND deleted_at IS NULL
                    UNION ALL
                    SELECT d.id, d.parent_id FROM sys_department d
                    INNER JOIN ancestors a ON a.parent_id = d.id
                    WHERE d.deleted_at IS NULL
                )
                SELECT 1 FROM ancestors WHERE id = ?
            )", {std::to_string(newParentId), std::to_string(dept.id())});

            if (!result.empty()) {
                throw ValidationException("不能将部门设为其子部门的下级，会导致循环引用");
            }
        };
    }

    /**
     * @brief 约束：无子部门
     */
    static Task<void> noChildren(const Department& dept) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM sys_department WHERE parent_id = ? AND deleted_at IS NULL",
            {std::to_string(dept.id())}
        );

        if (!result.empty() && FieldHelper::getInt(result[0]["count"]) > 0) {
            throw ValidationException("该部门下存在子部门，无法删除");
        }
        co_return;
    }

    /**
     * @brief 约束：无关联用户
     */
    static Task<void> noUsers(const Department& dept) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM sys_user WHERE department_id = ? AND deleted_at IS NULL",
            {std::to_string(dept.id())}
        );

        if (!result.empty() && FieldHelper::getInt(result[0]["count"]) > 0) {
            throw ValidationException("该部门下存在用户，无法删除");
        }
        co_return;
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新部门信息
     */
    Department& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("code")) {
            code_ = data["code"].asString();
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
        if (data.isMember("leader_id")) {
            leaderId_ = data["leader_id"].isNull() ? 0 : data["leader_id"].asInt();
            markDirty();
        }
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
        }
        return *this;
    }

    /**
     * @brief 标记删除
     */
    Department& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& name() const { return name_; }
    const std::string& code() const { return code_; }
    int parentId() const { return parentId_; }
    int sortOrder() const { return sortOrder_; }
    int leaderId() const { return leaderId_; }
    const std::string& status() const { return status_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["code"] = code_;
        json["parent_id"] = parentId_ == 0 ? Json::Value::null : Json::Value(parentId_);
        json["sort_order"] = sortOrder_;
        json["leader_id"] = leaderId_ == 0 ? Json::Value::null : Json::Value(leaderId_);
        json["status"] = status_;
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
    std::string code_;
    int parentId_ = 0;
    int sortOrder_ = 0;
    int leaderId_ = 0;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    std::string createdAt_;
    std::string updatedAt_;

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        code_ = data.get("code", "").asString();
        parentId_ = (data.isMember("parent_id") && !data["parent_id"].isNull() && data["parent_id"].asInt() != 0)
            ? data["parent_id"].asInt() : 0;
        sortOrder_ = data.get("sort_order", 0).asInt();
        leaderId_ = (data.isMember("leader_id") && !data["leader_id"].isNull())
            ? data["leader_id"].asInt() : 0;
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
    }

    Task<void> load(int deptId) {
        auto result = co_await db().execSqlCoro(
            "SELECT * FROM sys_department WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(deptId)}
        );

        if (result.empty()) {
            throw NotFoundException("部门不存在");
        }

        fromRow(result[0]);
        markLoaded();
    }

    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        code_ = FieldHelper::getString(row["code"], "");
        parentId_ = row["parent_id"].isNull() ? 0 : row["parent_id"].as<int>();
        sortOrder_ = FieldHelper::getInt(row["sort_order"], 0);
        leaderId_ = row["leader_id"].isNull() ? 0 : row["leader_id"].as<int>();
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
    }

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO sys_department (name, code, parent_id, sort_order, leader_id, status, created_at)
            VALUES (?, NULLIF(?, ''), NULLIF(?, '0')::INT, ?, NULLIF(?, '0')::INT, ?, ?)
            RETURNING id
        )", {
            name_, code_, std::to_string(parentId_), std::to_string(sortOrder_),
            std::to_string(leaderId_), status_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<DepartmentCreated>(id());
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE sys_department
            SET name = ?, code = NULLIF(?, ''), parent_id = NULLIF(?, '0')::INT,
                sort_order = ?, leader_id = NULLIF(?, '0')::INT, status = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, code_, std::to_string(parentId_), std::to_string(sortOrder_),
            std::to_string(leaderId_), status_, TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<DepartmentUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        co_await tx.execSqlCoro(
            "UPDATE sys_department SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<DepartmentDeleted>(id());
    }
};
