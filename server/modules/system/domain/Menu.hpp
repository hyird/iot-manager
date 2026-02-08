#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/system/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/TreeBuilder.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 菜单聚合根
 *
 * 使用示例：
 * @code
 * // 创建菜单
 * co_await Menu::create(data).save();
 *
 * // 更新菜单
 * co_await Menu::of(id)
 *     .require(Menu::notSelfParent)
 *     .update(data)
 *     .save();
 *
 * // 删除菜单
 * co_await Menu::of(id)
 *     .require(Menu::noChildren)
 *     .remove()
 *     .save();
 * @endcode
 */
class Menu : public Aggregate<Menu> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的菜单
     */
    static Task<Menu> of(int id) {
        Menu menu;
        co_await menu.load(id);
        co_return menu;
    }

    /**
     * @brief 创建新菜单
     */
    static Menu create(const Json::Value& data) {
        Menu menu;
        menu.applyCreate(data);
        return menu;
    }

    /**
     * @brief 获取菜单列表（平铺）
     */
    static Task<Json::Value> list(const std::string& keyword = "", const std::string& status = "") {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted();
        if (!keyword.empty()) qb.likeAny({"name", "path", "permission_code"}, keyword);
        if (!status.empty()) qb.eq("status", status);

        std::string sql = "SELECT * FROM sys_menu" + qb.whereClause()
                        + " ORDER BY sort_order ASC, id ASC";
        auto result = co_await db.execSqlCoro(sql, qb.params());

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Menu menu;
            menu.fromRow(row);
            items.append(menu.toJson());
        }
        co_return items;
    }

    /**
     * @brief 获取菜单树形结构
     */
    static Task<Json::Value> tree(const std::string& status = "") {
        auto items = co_await list("", status);
        auto tree = TreeBuilder::build(items, "id", "parent_id", "children");
        TreeBuilder::sort(tree, "sort_order", true);
        co_return tree;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：无子菜单
     */
    static Task<void> noChildren(const Menu& menu) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM sys_menu WHERE parent_id = ? AND deleted_at IS NULL",
            {std::to_string(menu.id())}
        );

        if (!result.empty() && FieldHelper::getInt(result[0]["count"]) > 0) {
            throw ValidationException("该菜单下存在子菜单，无法删除");
        }
        co_return;
    }

    /**
     * @brief 约束生成器：不能设为自己或其后代的子菜单（防止循环引用）
     */
    static Constraint<Menu> notSelfParent(int newParentId) {
        return [newParentId](const Menu& menu) -> Task<void> {
            if (newParentId == 0) co_return;
            if (newParentId == menu.id()) {
                throw ValidationException("不能将菜单设为自己的子菜单");
            }

            DatabaseService db;
            auto result = co_await db.execSqlCoro(R"(
                WITH RECURSIVE ancestors AS (
                    SELECT id, parent_id FROM sys_menu
                    WHERE id = ? AND deleted_at IS NULL
                    UNION ALL
                    SELECT m.id, m.parent_id FROM sys_menu m
                    INNER JOIN ancestors a ON a.parent_id = m.id
                    WHERE m.deleted_at IS NULL
                )
                SELECT 1 FROM ancestors WHERE id = ?
            )", {std::to_string(newParentId), std::to_string(menu.id())});

            if (!result.empty()) {
                throw ValidationException("不能将菜单设为其子菜单的下级，会导致循环引用");
            }
        };
    }

    /**
     * @brief 约束：permission_code 唯一（非空时）
     */
    static Task<void> permissionCodeUnique(const Menu& menu) {
        if (menu.permissionCode_.empty()) co_return;

        DatabaseService db;
        std::string sql = "SELECT 1 FROM sys_menu WHERE permission_code = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {menu.permissionCode_};

        if (menu.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(menu.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("权限标识已存在: " + menu.permissionCode_);
        }
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新菜单信息
     */
    Menu& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("path")) {
            path_ = data["path"].asString();
            markDirty();
        }
        if (data.isMember("icon")) {
            icon_ = data["icon"].asString();
            markDirty();
        }
        if (data.isMember("parent_id")) {
            parentId_ = data["parent_id"].isNull() ? 0 : data["parent_id"].asInt();
            markDirty();
        }
        if (data.isMember("sort_order")) {
            sortOrder_ = data["sort_order"].asInt();
            markDirty();
        }
        if (data.isMember("type")) {
            type_ = data["type"].asString();
            markDirty();
        }
        if (data.isMember("component")) {
            component_ = data["component"].asString();
            markDirty();
        }
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
        }
        if (data.isMember("permission_code")) {
            permissionCode_ = data["permission_code"].asString();
            markDirty();
        }
        if (data.isMember("is_default")) {
            isDefault_ = data["is_default"].asBool();
            markDirty();
        }
        return *this;
    }

    /**
     * @brief 标记删除
     */
    Menu& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& name() const { return name_; }
    const std::string& path() const { return path_; }
    const std::string& icon() const { return icon_; }
    int parentId() const { return parentId_; }
    int sortOrder() const { return sortOrder_; }
    const std::string& type() const { return type_; }
    const std::string& component() const { return component_; }
    const std::string& status() const { return status_; }
    const std::string& permissionCode() const { return permissionCode_; }
    bool isDefault() const { return isDefault_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["path"] = path_;
        json["icon"] = icon_;
        json["parent_id"] = parentId_;
        json["sort_order"] = sortOrder_;
        json["type"] = type_;
        json["component"] = component_;
        json["status"] = status_;
        json["permission_code"] = permissionCode_;
        json["is_default"] = isDefault_;
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
    std::string path_;
    std::string icon_;
    int parentId_ = 0;
    int sortOrder_ = 0;
    std::string type_ = Constants::MENU_TYPE_MENU;
    std::string component_;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    std::string permissionCode_;
    bool isDefault_ = false;
    std::string createdAt_;
    std::string updatedAt_;

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        path_ = data.get("path", "").asString();
        icon_ = data.get("icon", "").asString();
        parentId_ = data.get("parent_id", 0).isNull() ? 0 : data["parent_id"].asInt();
        sortOrder_ = data.get("sort_order", 0).asInt();
        type_ = data.get("type", Constants::MENU_TYPE_MENU).asString();
        component_ = data.get("component", "").asString();
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
        permissionCode_ = data.get("permission_code", "").asString();
        isDefault_ = data.get("is_default", false).asBool();
    }

    Task<void> load(int menuId) {
        auto result = co_await db().execSqlCoro(
            "SELECT * FROM sys_menu WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(menuId)}
        );

        if (result.empty()) {
            throw NotFoundException("菜单不存在");
        }

        fromRow(result[0]);
        markLoaded();
    }

    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        path_ = FieldHelper::getString(row["path"], "");
        icon_ = FieldHelper::getString(row["icon"], "");
        parentId_ = FieldHelper::getInt(row["parent_id"], 0);
        sortOrder_ = FieldHelper::getInt(row["sort_order"], 0);
        type_ = FieldHelper::getString(row["type"], Constants::MENU_TYPE_MENU);
        component_ = FieldHelper::getString(row["component"], "");
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        permissionCode_ = FieldHelper::getString(row["permission_code"], "");
        isDefault_ = FieldHelper::getBool(row["is_default"], false);
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
    }

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO sys_menu (name, path, icon, parent_id, sort_order, type,
                                  component, status, permission_code, is_default, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id
        )", {
            name_, path_, icon_, std::to_string(parentId_), std::to_string(sortOrder_),
            type_, component_, status_, permissionCode_,
            isDefault_ ? "1" : "0", TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<MenuCreated>(id());
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE sys_menu
            SET name = ?, path = ?, icon = ?, parent_id = ?, sort_order = ?,
                type = ?, component = ?, status = ?, permission_code = ?,
                is_default = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, path_, icon_, std::to_string(parentId_), std::to_string(sortOrder_),
            type_, component_, status_, permissionCode_,
            isDefault_ ? "1" : "0", TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<MenuUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        // 删除角色菜单关联
        co_await tx.execSqlCoro(
            "DELETE FROM sys_role_menu WHERE menu_id = ?",
            {std::to_string(id())}
        );

        // 软删除菜单
        co_await tx.execSqlCoro(
            "UPDATE sys_menu SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<MenuDeleted>(id());
    }
};
