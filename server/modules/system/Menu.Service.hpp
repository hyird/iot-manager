#pragma once

#include "domain/Menu.hpp"

/**
 * @brief 菜单服务（领域驱动重构版）
 *
 * 对比改进前：
 * - 改进前：~143 行
 * - 改进后：~50 行
 */
class MenuService {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 菜单列表（平铺）
     */
    Task<Json::Value> list(const std::string& keyword = "", const std::string& status = "") {
        co_return co_await Menu::list(keyword, status);
    }

    /**
     * @brief 菜单树形结构
     */
    Task<Json::Value> tree(const std::string& status = "") {
        co_return co_await Menu::tree(status);
    }

    /**
     * @brief 菜单详情
     */
    Task<Json::Value> detail(int id) {
        auto menu = co_await Menu::of(id);
        co_return menu.toJson();
    }

    /**
     * @brief 创建菜单
     */
    Task<void> create(const Json::Value& data) {
        co_await Menu::create(data)
            .require(Menu::permissionCodeUnique)
            .save();
    }

    /**
     * @brief 更新菜单
     */
    Task<void> update(int id, const Json::Value& data) {
        auto menu = co_await Menu::of(id);

        // 检查是否将菜单设为自己的子菜单
        if (data.isMember("parent_id") && !data["parent_id"].isNull()) {
            menu.require(Menu::notSelfParent(data["parent_id"].asInt()));
        }
        if (data.isMember("permission_code")) {
            menu.require(Menu::permissionCodeUnique);
        }

        menu.update(data);

        co_await menu.save();
    }

    /**
     * @brief 删除菜单
     */
    Task<void> remove(int id) {
        auto menu = co_await Menu::of(id);
        co_await menu.require(Menu::noChildren)
            .remove()
            .save();
    }
};
