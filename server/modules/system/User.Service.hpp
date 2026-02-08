#pragma once

#include "domain/User.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 用户服务（领域驱动重构版）
 *
 * 对比改进前：
 * - 改进前：~280 行，包含 SQL、缓存管理、事务处理
 * - 改进后：~50 行，纯业务协调
 *
 * 所有副作用（缓存失效、事件发布）由领域模型自动处理。
 */
class UserService {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 用户列表（分页）
     */
    Task<std::tuple<Json::Value, int>> list(
        const Pagination& page,
        const std::string& status = "",
        int departmentId = 0
    ) {
        auto result = co_await User::list(page, status, departmentId);

        Json::Value items(Json::arrayValue);
        for (const auto& user : result.items) {
            items.append(user.toJson());
        }

        co_return std::make_tuple(items, result.total);
    }

    /**
     * @brief 用户详情
     */
    Task<Json::Value> detail(int id) {
        auto user = co_await User::of(id);
        co_return user.toJson();
    }

    /**
     * @brief 创建用户
     */
    Task<void> create(const Json::Value& data) {
        co_await User::create(data)
            .require(User::usernameUnique)
            .withRoles(data["roleIds"])
            .save();
    }

    /**
     * @brief 更新用户
     */
    Task<void> update(int id, const Json::Value& data) {
        auto user = co_await User::of(id);

        user.require(User::notBuiltinAdmin)
            .update(data);

        if (data.isMember("roleIds")) {
            user.withRoles(data["roleIds"]);
        }

        co_await user.save();
    }

    /**
     * @brief 删除用户
     */
    Task<void> remove(int id, int currentUserId) {
        auto user = co_await User::of(id);
        co_await user.require(User::notBuiltinAdmin)
            .require(User::notSelf(currentUserId))
            .remove()
            .save();
    }
};
