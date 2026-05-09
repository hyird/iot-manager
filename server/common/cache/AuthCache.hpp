#pragma once

#include <drogon/drogon.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "common/utils/Constants.hpp"

class AuthCache {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    AuthCache()
        : userSessionTtl_(Constants::CACHE_TTL_USER_SESSION)
        , userMenusTtl_(Constants::CACHE_TTL_USER_MENUS)
        , userRolesTtl_(Constants::CACHE_TTL_USER_ROLES) {
        auto config = drogon::app().getCustomConfig();
        if (config.isMember("cache")) {
            userSessionTtl_ = config["cache"].get("user_session_ttl", Constants::CACHE_TTL_USER_SESSION).asInt();
            userMenusTtl_ = config["cache"].get("user_menus_ttl", Constants::CACHE_TTL_USER_MENUS).asInt();
            userRolesTtl_ = config["cache"].get("user_roles_ttl", Constants::CACHE_TTL_USER_ROLES).asInt();
        }
    }

    Task<bool> cacheUserSession(int userId, const Json::Value& userInfo) {
        co_return setJson("session:user:" + std::to_string(userId), userInfo, userSessionTtl_);
    }

    Task<std::optional<Json::Value>> getUserSession(int userId) {
        co_return getJson("session:user:" + std::to_string(userId));
    }

    Task<bool> deleteUserSession(int userId) {
        co_return eraseJson("session:user:" + std::to_string(userId));
    }

    Task<bool> cacheUserRoles(int userId, const Json::Value& roles) {
        co_return setJson("user:roles:" + std::to_string(userId), roles, userRolesTtl_);
    }

    Task<std::optional<Json::Value>> getUserRoles(int userId) {
        co_return getJson("user:roles:" + std::to_string(userId));
    }

    Task<bool> deleteUserRoles(int userId) {
        co_return eraseJson("user:roles:" + std::to_string(userId));
    }

    Task<bool> cacheUserMenus(int userId, const Json::Value& menus) {
        co_return setJson("user:menus:" + std::to_string(userId), menus, userMenusTtl_);
    }

    Task<std::optional<Json::Value>> getUserMenus(int userId) {
        co_return getJson("user:menus:" + std::to_string(userId));
    }

    Task<bool> deleteUserMenus(int userId) {
        co_return eraseJson("user:menus:" + std::to_string(userId));
    }

    Task<bool> cacheAllMenus(const Json::Value& menus) {
        co_return setJson("menu:all", menus, userMenusTtl_);
    }

    Task<std::optional<Json::Value>> getAllMenus() {
        co_return getJson("menu:all");
    }

    Task<bool> deleteAllMenus() {
        co_return eraseJson("menu:all");
    }

    Task<bool> blacklistToken(const std::string& token, int ttl) {
        if (token.empty() || ttl <= 0) {
            co_return false;
        }

        std::unique_lock lock(tokenBlacklistMutex_);
        tokenBlacklist_["blacklist:token:" + hashToken(token)] = {makeExpiry(ttl)};
        co_return true;
    }

    Task<bool> isTokenBlacklisted(const std::string& token) {
        if (token.empty()) {
            co_return false;
        }

        std::unique_lock lock(tokenBlacklistMutex_);
        auto key = "blacklist:token:" + hashToken(token);
        auto it = tokenBlacklist_.find(key);
        if (it == tokenBlacklist_.end()) {
            co_return false;
        }
        if (isExpired(it->second.expiresAt)) {
            tokenBlacklist_.erase(it);
            co_return false;
        }
        co_return true;
    }

    Task<int64_t> recordLoginFailure(const std::string& username) {
        co_return incrementCounter("login:failed:" + username, Constants::LOGIN_FAILURE_WINDOW);
    }

    Task<bool> clearLoginFailure(const std::string& username) {
        co_return eraseCounter("login:failed:" + username);
    }

    Task<int64_t> getLoginFailureCount(const std::string& username) {
        co_return getCounter("login:failed:" + username);
    }

    Task<bool> checkRateLimit(int userId, const std::string& endpoint, int maxRequests, int windowSeconds) {
        auto count = incrementCounter("ratelimit:" + std::to_string(userId) + ":" + endpoint, windowSeconds);
        co_return count <= maxRequests;
    }

    Task<int64_t> recordLoginFailureByIp(const std::string& ip) {
        co_return incrementCounter("login:failed:ip:" + ip, Constants::LOGIN_FAILURE_WINDOW);
    }

    Task<int64_t> getLoginFailureCountByIp(const std::string& ip) {
        co_return getCounter("login:failed:ip:" + ip);
    }

    Task<void> clearUserCache(int userId) {
        co_await deleteUserSession(userId);
        co_await deleteUserRoles(userId);
        co_await deleteUserMenus(userId);
        LOG_INFO << "Cleared all auth cache for user: " << userId;
    }

    Task<int> clearAllUserMenusCache() {
        co_await deleteAllMenus();
        auto count = eraseJsonByPrefix("user:menus:");
        LOG_INFO << "Cleared menu cache for " << count << " users";
        co_return count;
    }

    Task<int> clearAllUserRolesCache() {
        auto count = eraseJsonByPrefix("user:roles:");
        LOG_INFO << "Cleared role cache for " << count << " users";
        co_return count;
    }

    Task<int> clearAllUserSessionsCache() {
        auto count = eraseJsonByPrefix("session:user:");
        LOG_INFO << "Cleared session cache for " << count << " users";
        co_return count;
    }

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct JsonEntry {
        Json::Value value;
        TimePoint expiresAt;
    };

    struct ExpiringFlag {
        TimePoint expiresAt;
    };

    struct CounterEntry {
        int64_t count = 0;
        TimePoint expiresAt;
    };

    int userSessionTtl_;
    int userMenusTtl_;
    int userRolesTtl_;

    inline static std::unordered_map<std::string, JsonEntry> jsonCache_;
    inline static std::unordered_map<std::string, ExpiringFlag> tokenBlacklist_;
    inline static std::unordered_map<std::string, CounterEntry> counters_;
    inline static std::shared_mutex jsonCacheMutex_;
    inline static std::shared_mutex tokenBlacklistMutex_;
    inline static std::shared_mutex countersMutex_;

    static TimePoint makeExpiry(int ttlSeconds) {
        return Clock::now() + std::chrono::seconds(std::max(1, ttlSeconds));
    }

    static bool isExpired(TimePoint expiresAt) {
        return Clock::now() >= expiresAt;
    }

    static std::string hashToken(const std::string& token) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), hash);

        std::ostringstream oss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        return oss.str();
    }

    static bool setJson(const std::string& key, const Json::Value& value, int ttl) {
        std::unique_lock lock(jsonCacheMutex_);
        jsonCache_[key] = {value, makeExpiry(ttl)};
        return true;
    }

    static std::optional<Json::Value> getJson(const std::string& key) {
        std::unique_lock lock(jsonCacheMutex_);
        auto it = jsonCache_.find(key);
        if (it == jsonCache_.end()) {
            return std::nullopt;
        }
        if (isExpired(it->second.expiresAt)) {
            jsonCache_.erase(it);
            return std::nullopt;
        }
        return it->second.value;
    }

    static bool eraseJson(const std::string& key) {
        std::unique_lock lock(jsonCacheMutex_);
        return jsonCache_.erase(key) > 0;
    }

    static int eraseJsonByPrefix(const std::string& prefix) {
        std::unique_lock lock(jsonCacheMutex_);
        int count = 0;
        for (auto it = jsonCache_.begin(); it != jsonCache_.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                it = jsonCache_.erase(it);
                ++count;
            } else {
                ++it;
            }
        }
        return count;
    }

    static int64_t incrementCounter(const std::string& key, int windowSeconds) {
        std::unique_lock lock(countersMutex_);
        auto now = Clock::now();
        auto& entry = counters_[key];
        if (entry.count == 0 || now >= entry.expiresAt) {
            entry.count = 1;
            entry.expiresAt = now + std::chrono::seconds(std::max(1, windowSeconds));
        } else {
            ++entry.count;
        }
        return entry.count;
    }

    static int64_t getCounter(const std::string& key) {
        std::unique_lock lock(countersMutex_);
        auto it = counters_.find(key);
        if (it == counters_.end()) {
            return 0;
        }
        if (isExpired(it->second.expiresAt)) {
            counters_.erase(it);
            return 0;
        }
        return it->second.count;
    }

    static bool eraseCounter(const std::string& key) {
        std::unique_lock lock(countersMutex_);
        return counters_.erase(key) > 0;
    }
};
