#pragma once

#include "common/database/RedisService.hpp"
#include "common/utils/Constants.hpp"

/**
 * @brief 认证缓存管理器
 * 管理用户会话、角色权限、菜单缓存、Token 黑名单、登录限流等认证相关缓存
 */
class AuthCache {
private:
    RedisService redis_;
    int userSessionTtl_;
    int userMenusTtl_;
    int userRolesTtl_;

public:
    template<typename T = void> using Task = drogon::Task<T>;

    AuthCache()
        : userSessionTtl_(Constants::CACHE_TTL_USER_SESSION)
        , userMenusTtl_(Constants::CACHE_TTL_USER_MENUS)
        , userRolesTtl_(Constants::CACHE_TTL_USER_ROLES)
    {
        auto config = drogon::app().getCustomConfig();
        if (config.isMember("cache")) {
            userSessionTtl_ = config["cache"].get("user_session_ttl", Constants::CACHE_TTL_USER_SESSION).asInt();
            userMenusTtl_ = config["cache"].get("user_menus_ttl", Constants::CACHE_TTL_USER_MENUS).asInt();
            userRolesTtl_ = config["cache"].get("user_roles_ttl", Constants::CACHE_TTL_USER_ROLES).asInt();
        }
    }

    // ==================== 用户会话缓存 ====================

    /**
     * @brief 缓存用户会话信息
     */
    Task<bool> cacheUserSession(int userId, const Json::Value& userInfo) {
        std::string key = "session:user:" + std::to_string(userId);
        co_return co_await redis_.setJson(key, userInfo, userSessionTtl_);
    }

    /**
     * @brief 获取用户会话信息
     */
    Task<std::optional<Json::Value>> getUserSession(int userId) {
        std::string key = "session:user:" + std::to_string(userId);
        co_return co_await redis_.getJson(key);
    }

    /**
     * @brief 删除用户会话（用户登出、权限变更时调用）
     */
    Task<bool> deleteUserSession(int userId) {
        std::string key = "session:user:" + std::to_string(userId);
        co_return co_await redis_.del(key);
    }

    // ==================== 用户角色缓存 ====================

    /**
     * @brief 缓存用户角色
     */
    Task<bool> cacheUserRoles(int userId, const Json::Value& roles) {
        std::string key = "user:roles:" + std::to_string(userId);
        co_return co_await redis_.setJson(key, roles, userRolesTtl_);
    }

    /**
     * @brief 获取用户角色
     */
    Task<std::optional<Json::Value>> getUserRoles(int userId) {
        std::string key = "user:roles:" + std::to_string(userId);
        co_return co_await redis_.getJson(key);
    }

    /**
     * @brief 删除用户角色缓存
     */
    Task<bool> deleteUserRoles(int userId) {
        std::string key = "user:roles:" + std::to_string(userId);
        co_return co_await redis_.del(key);
    }

    // ==================== 用户菜单缓存 ====================

    /**
     * @brief 缓存用户菜单
     */
    Task<bool> cacheUserMenus(int userId, const Json::Value& menus) {
        std::string key = "user:menus:" + std::to_string(userId);
        co_return co_await redis_.setJson(key, menus, userMenusTtl_);
    }

    /**
     * @brief 获取用户菜单
     */
    Task<std::optional<Json::Value>> getUserMenus(int userId) {
        std::string key = "user:menus:" + std::to_string(userId);
        co_return co_await redis_.getJson(key);
    }

    /**
     * @brief 删除用户菜单缓存
     */
    Task<bool> deleteUserMenus(int userId) {
        std::string key = "user:menus:" + std::to_string(userId);
        co_return co_await redis_.del(key);
    }

    // ==================== 全局菜单缓存 ====================

    /**
     * @brief 缓存所有菜单（超级管理员使用）
     */
    Task<bool> cacheAllMenus(const Json::Value& menus) {
        co_return co_await redis_.setJson("menu:all", menus, userMenusTtl_);
    }

    /**
     * @brief 获取所有菜单
     */
    Task<std::optional<Json::Value>> getAllMenus() {
        co_return co_await redis_.getJson("menu:all");
    }

    /**
     * @brief 删除所有菜单缓存（菜单更新时调用）
     */
    Task<bool> deleteAllMenus() {
        co_return co_await redis_.del("menu:all");
    }

    // ==================== Token 黑名单 ====================

    /**
     * @brief 将 Token 加入黑名单（用于强制登出）
     */
    Task<bool> blacklistToken(const std::string& token, int ttl) {
        std::string key = "blacklist:token:" + token;
        co_return co_await redis_.set(key, "1", ttl);
    }

    /**
     * @brief 检查 Token 是否在黑名单中
     */
    Task<bool> isTokenBlacklisted(const std::string& token) {
        std::string key = "blacklist:token:" + token;
        co_return co_await redis_.exists(key);
    }

    // ==================== 登录失败限流 ====================

    /**
     * @brief 记录登录失败次数
     * @return 失败次数
     */
    Task<int64_t> recordLoginFailure(const std::string& username) {
        std::string key = "login:failed:" + username;
        co_return co_await redis_.incrWithExpire(key, Constants::LOGIN_FAILURE_WINDOW);
    }

    /**
     * @brief 清除登录失败记录
     */
    Task<bool> clearLoginFailure(const std::string& username) {
        std::string key = "login:failed:" + username;
        co_return co_await redis_.del(key);
    }

    /**
     * @brief 获取登录失败次数
     */
    Task<int64_t> getLoginFailureCount(const std::string& username) {
        std::string key = "login:failed:" + username;
        auto value = co_await redis_.get(key);
        if (!value) {
            co_return 0;
        }
        co_return std::stoll(*value);
    }

    // ==================== API 限流 ====================

    /**
     * @brief 检查 API 访问频率
     * @return 当前访问次数
     */
    Task<int64_t> checkRateLimit(int userId, const std::string& endpoint, [[maybe_unused]] int maxRequests, int windowSeconds) {
        std::string key = "ratelimit:" + std::to_string(userId) + ":" + endpoint;
        auto count = co_await redis_.incrWithExpire(key, windowSeconds);
        co_return count;
    }

    // ==================== 批量清除缓存 ====================

    /**
     * @brief 清除用户所有缓存（权限变更、角色变更时调用）
     */
    Task<void> clearUserCache(int userId) {
        // 并行清除三类缓存（3 次串行 Redis 往返 → 1 次并行往返）
        auto t1 = deleteUserSession(userId);
        auto t2 = deleteUserRoles(userId);
        auto t3 = deleteUserMenus(userId);
        co_await t1;
        co_await t2;
        co_await t3;
        LOG_INFO << "Cleared all cache for user: " << userId;
    }

    /**
     * @brief 清除所有用户的菜单缓存（菜单变更时调用）
     */
    Task<int> clearAllUserMenusCache() {
        co_await deleteAllMenus();
        auto count = co_await redis_.delPattern("user:menus:*");
        LOG_INFO << "Cleared menu cache for " << count << " users";
        co_return count;
    }

    /**
     * @brief 清除所有用户的角色缓存（角色权限变更时调用）
     */
    Task<int> clearAllUserRolesCache() {
        auto count = co_await redis_.delPattern("user:roles:*");
        LOG_INFO << "Cleared role cache for " << count << " users";
        co_return count;
    }

    /**
     * @brief 清除所有用户的会话缓存（菜单/角色变更时调用）
     */
    Task<int> clearAllUserSessionsCache() {
        auto count = co_await redis_.delPattern("session:user:*");
        LOG_INFO << "Cleared session cache for " << count << " users";
        co_return count;
    }
};
