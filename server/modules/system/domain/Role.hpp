#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/system/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/SqlHelper.hpp"

/**
 * @brief 角色聚合根
 *
 * 使用示例：
 * @code
 * // 创建角色
 * co_await Role::create(data)
 *     .requireIf(!code.empty(), Role::codeUnique)
 *     .withMenus(menuIds)
 *     .save();
 *
 * // 更新角色
 * co_await Role::of(id)
 *     .require(Role::notSuperadmin)
 *     .update(data)
 *     .withMenus(data["menu_ids"])
 *     .save();
 *
 * // 删除角色
 * co_await Role::of(id)
 *     .require(Role::notSuperadmin)
 *     .require(Role::notInUse)
 *     .remove()
 *     .save();
 * @endcode
 */
class Role : public Aggregate<Role> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的角色
     */
    static Task<Role> of(int id) {
        Role role;
        co_await role.load(id);
        co_return role;
    }

    /**
     * @brief 创建新角色
     */
    static Role create(const Json::Value& data) {
        Role role;
        role.applyCreate(data);
        return role;
    }

    /**
     * @brief 分页查询角色列表
     * 优化：使用批量加载避免 N+1 查询问题
     */
    static Task<PagedResult<Role>> list(const Pagination& page, const std::string& status = "") {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted();
        if (!page.keyword.empty()) {
            qb.likeAny({"name", "code"}, page.keyword);
        }
        if (!status.empty()) qb.eq("status", status);

        // 计数
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM sys_role" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        // 查询数据
        std::string sql = "SELECT * FROM sys_role" + qb.whereClause()
                        + " ORDER BY id ASC" + page.limitClause();
        auto result = co_await db.execSqlCoro(sql, qb.params());

        if (result.empty()) {
            co_return PagedResult<Role>{{}, total, page.page, page.pageSize};
        }

        // 收集所有角色 ID
        std::vector<int> roleIds;
        roleIds.reserve(result.size());
        for (const auto& row : result) {
            roleIds.push_back(FieldHelper::getInt(row["id"]));
        }

        // 批量加载菜单关联（一次查询代替 N 次）
        auto menuMap = co_await batchLoadMenuIds(roleIds);

        // 构建结果
        std::vector<Role> roles;
        roles.reserve(result.size());
        for (const auto& row : result) {
            Role role;
            role.fromRow(row);
            // 直接使用批量加载的结果
            auto it = menuMap.find(role.id());
            if (it != menuMap.end()) {
                role.menuIds_ = it->second;
            }
            roles.push_back(std::move(role));
        }

        co_return PagedResult<Role>{std::move(roles), total, page.page, page.pageSize};
    }

    /**
     * @brief 批量加载角色菜单关联（避免 N+1 问题）
     */
    static Task<std::map<int, std::vector<int>>> batchLoadMenuIds(const std::vector<int>& roleIds) {
        std::map<int, std::vector<int>> result;
        if (roleIds.empty()) co_return result;

        // 构建 IN 子句
        std::string idList = SqlHelper::buildInClause(roleIds);

        DatabaseService db;
        std::string sql = "SELECT role_id, menu_id FROM sys_role_menu WHERE role_id IN (" + idList + ")";
        auto queryResult = co_await db.execSqlCoro(sql);

        for (const auto& row : queryResult) {
            int roleId = FieldHelper::getInt(row["role_id"]);
            int menuId = FieldHelper::getInt(row["menu_id"]);
            result[roleId].push_back(menuId);
        }

        co_return result;
    }

    /**
     * @brief 获取所有可用角色（下拉选择用）
     */
    static Task<Json::Value> all() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT id, name, code FROM sys_role
            WHERE status = 'enabled' AND deleted_at IS NULL AND code != 'superadmin'
            ORDER BY id ASC
        )");

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = FieldHelper::getString(row["name"]);
            item["code"] = FieldHelper::getString(row["code"]);
            items.append(item);
        }
        co_return items;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：不能是超级管理员角色
     */
    static Task<void> notSuperadmin(const Role& role) {
        if (role.code_ == Constants::ROLE_SUPERADMIN) {
            throw ForbiddenException("不能编辑或删除超级管理员角色");
        }
        co_return;
    }

    /**
     * @brief 约束：角色编码唯一
     */
    static Task<void> codeUnique(const Role& role) {
        if (role.code_.empty()) co_return;

        DatabaseService db;
        std::string sql = "SELECT 1 FROM sys_role WHERE code = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {role.code_};

        if (role.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(role.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("角色编码已存在");
        }
    }

    /**
     * @brief 约束：角色未被使用
     */
    static Task<void> notInUse(const Role& role) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM sys_user_role WHERE role_id = ?",
            {std::to_string(role.id())}
        );

        if (!result.empty() && FieldHelper::getInt(result[0]["count"]) > 0) {
            throw ValidationException("该角色已被用户使用，无法删除");
        }
        co_return;
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新角色信息
     */
    Role& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("code") && !data["code"].isNull()) {
            code_ = data["code"].asString();
            markDirty();
        }
        if (data.isMember("description")) {
            description_ = data["description"].asString();
            markDirty();
        }
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
        }
        return *this;
    }

    /**
     * @brief 设置角色菜单
     */
    Role& withMenus(const Json::Value& menuIds) {
        pendingMenuIds_.clear();
        if (menuIds.isArray()) {
            for (const auto& mid : menuIds) {
                pendingMenuIds_.push_back(mid.asInt());
            }
        }
        menusChanged_ = true;
        return *this;
    }

    /**
     * @brief 设置角色菜单（vector 版本）
     */
    Role& withMenus(const std::vector<int>& menuIds) {
        pendingMenuIds_ = menuIds;
        menusChanged_ = true;
        return *this;
    }

    /**
     * @brief 标记删除
     */
    Role& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& name() const { return name_; }
    const std::string& code() const { return code_; }
    const std::string& description() const { return description_; }
    const std::string& status() const { return status_; }
    const std::vector<int>& menuIds() const { return menuIds_; }
    const Json::Value& menus() const { return menus_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["code"] = code_;
        json["description"] = description_;
        json["status"] = status_;
        json["created_at"] = createdAt_;
        json["updated_at"] = updatedAt_;

        Json::Value ids(Json::arrayValue);
        for (int mid : menuIds_) {
            ids.append(mid);
        }
        json["menu_ids"] = ids;
        json["menus"] = menus_;

        return json;
    }

    // ==================== 持久化（由 Aggregate 基类调用）====================

    Task<void> doPersist(TransactionGuard& tx) {
        if (isDeleted()) {
            co_await persistDelete(tx);
        } else if (isNew()) {
            co_await persistCreate(tx);
        } else if (isDirty()) {
            co_await persistUpdate(tx);
        }

        if (menusChanged_) {
            co_await persistMenus(tx);
        }
    }

private:
    // 角色字段
    std::string name_;
    std::string code_;
    std::string description_;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    std::string createdAt_;
    std::string updatedAt_;

    // 关联数据
    std::vector<int> menuIds_;
    Json::Value menus_{Json::arrayValue};

    // 待保存的菜单
    std::vector<int> pendingMenuIds_;
    bool menusChanged_ = false;

    // ==================== 私有方法 ====================

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        code_ = data.get("code", "").asString();
        description_ = data.get("description", "").asString();
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
    }

    Task<void> load(int roleId) {
        auto result = co_await db().execSqlCoro(
            "SELECT * FROM sys_role WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(roleId)}
        );

        if (result.empty()) {
            throw NotFoundException("角色不存在");
        }

        fromRow(result[0]);
        co_await loadMenuIds();
        co_await loadMenus();
        markLoaded();
    }

    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        code_ = FieldHelper::getString(row["code"]);
        description_ = FieldHelper::getString(row["description"], "");
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
    }

    Task<void> loadMenuIds() {
        auto result = co_await db().execSqlCoro(
            "SELECT menu_id FROM sys_role_menu WHERE role_id = ?",
            {std::to_string(id())}
        );

        menuIds_.clear();
        for (const auto& row : result) {
            menuIds_.push_back(FieldHelper::getInt(row["menu_id"]));
        }
    }

    Task<void> loadMenus() {
        auto result = co_await db().execSqlCoro(R"(
            SELECT m.id, m.name, m.type, m.parent_id
            FROM sys_menu m
            INNER JOIN sys_role_menu rm ON m.id = rm.menu_id
            WHERE rm.role_id = ? AND m.deleted_at IS NULL
            ORDER BY m.sort_order ASC
        )", {std::to_string(id())});

        menus_ = Json::Value(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value menu;
            menu["id"] = FieldHelper::getInt(row["id"]);
            menu["name"] = FieldHelper::getString(row["name"]);
            menu["type"] = FieldHelper::getString(row["type"]);
            menu["parent_id"] = row["parent_id"].isNull()
                ? Json::Value::null
                : Json::Value(FieldHelper::getInt(row["parent_id"]));
            menus_.append(menu);
        }
    }

    // ==================== 持久化操作 ====================

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO sys_role (name, code, description, status, created_at)
            VALUES (?, ?, ?, ?, ?) RETURNING id
        )", {
            name_, code_, description_, status_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<RoleCreated>(id(), code_);
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE sys_role
            SET name = ?, code = ?, description = ?, status = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, code_, description_, status_,
            TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<RoleUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        // 删除菜单关联
        co_await tx.execSqlCoro(
            "DELETE FROM sys_role_menu WHERE role_id = ?",
            {std::to_string(id())}
        );

        // 软删除角色
        co_await tx.execSqlCoro(
            "UPDATE sys_role SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<RoleDeleted>(id());
    }

    Task<void> persistMenus(TransactionGuard& tx) {
        // 删除现有菜单
        co_await tx.execSqlCoro(
            "DELETE FROM sys_role_menu WHERE role_id = ?",
            {std::to_string(id())}
        );

        // 插入新菜单
        if (!pendingMenuIds_.empty()) {
            auto [valuesSql, params] = SqlHelper::buildBatchInsertValues(id(), pendingMenuIds_);
            co_await tx.execSqlCoro(
                "INSERT INTO sys_role_menu (role_id, menu_id) VALUES " + valuesSql, params);
        }

        raiseEvent<RoleMenusChanged>(id());
        menusChanged_ = false;
    }
};
