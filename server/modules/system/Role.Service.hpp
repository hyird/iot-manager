#pragma once

#include "domain/Role.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 角色服务（领域驱动重构版）
 *
 * 对比改进前：
 * - 改进前：~236 行，包含 SQL、缓存管理、事务处理
 * - 改进后：~60 行，纯业务协调
 */
class RoleService {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 角色列表（分页）
     */
    Task<std::tuple<Json::Value, int>> list(const Pagination& page, const std::string& status = "") {
        auto result = co_await Role::list(page, status);

        Json::Value items(Json::arrayValue);
        for (const auto& role : result.items) {
            items.append(role.toJson());
        }

        co_return std::make_tuple(items, result.total);
    }

    /**
     * @brief 所有可用角色（下拉选择）
     */
    Task<Json::Value> all() {
        co_return co_await Role::all();
    }

    /**
     * @brief 角色详情
     */
    Task<Json::Value> detail(int id) {
        auto role = co_await Role::of(id);
        co_return role.toJson();
    }

    /**
     * @brief 创建角色
     */
    Task<void> create(const Json::Value& data) {
        std::string code = data.get("code", "").asString();

        co_await Role::create(data)
            .requireIf(!code.empty(), Role::codeUnique)
            .withMenus(data["menu_ids"])
            .save();
    }

    /**
     * @brief 更新角色
     */
    Task<void> update(int id, const Json::Value& data) {
        auto role = co_await Role::of(id);

        // 如果修改了 code，需要检查唯一性
        bool codeChanged = data.isMember("code") && !data["code"].isNull()
                        && !data["code"].asString().empty();

        role.require(Role::notSuperadmin)
            .requireIf(codeChanged, Role::codeUnique)
            .update(data);

        if (data.isMember("menu_ids")) {
            role.withMenus(data["menu_ids"]);
        }

        co_await role.save();
    }

    /**
     * @brief 删除角色
     */
    Task<void> remove(int id) {
        auto role = co_await Role::of(id);
        co_await role.require(Role::notSuperadmin)
            .require(Role::notInUse)
            .remove()
            .save();
    }
};
