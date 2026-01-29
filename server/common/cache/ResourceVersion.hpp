#pragma once

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <string>
#include <map>
#include <mutex>
#include "common/database/RedisService.hpp"

using namespace drogon;

/**
 * @brief 资源版本管理器
 *
 * 提供版本号的存取功能，具体的缓存 key 和清理由各 Controller/Service 自行管理。
 * 使用 UUID 作为版本标识，避免哈希冲突。
 *
 * 存储策略：内存 + Redis 双写，启动时从 Redis 加载
 */
class ResourceVersion {
public:
    /**
     * @brief 获取单例实例
     */
    static ResourceVersion& instance() {
        static ResourceVersion instance;
        return instance;
    }

    /**
     * @brief 获取资源版本号
     * @param key 资源 key（如 "device", "user" 等）
     */
    std::string getVersion(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = versions_.find(key);
        if (it == versions_.end()) {
            std::string uuid = drogon::utils::getUuid();
            versions_[key] = uuid;
            return uuid;
        }
        return it->second;
    }

    /**
     * @brief 更新版本号（数据变更时调用）
     * @param key 资源 key
     */
    void incrementVersion(const std::string& key) {
        std::string uuid;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            uuid = drogon::utils::getUuid();
            versions_[key] = uuid;
        }
        LOG_DEBUG << "[ResourceVersion] " << key << " version updated to " << uuid;

        // 异步同步到 Redis
        async_run([this, key, uuid]() -> Task<void> {
            co_await syncToRedis(key, uuid);
        });
    }

    /**
     * @brief 生成 ETag
     * @param key 资源 key
     */
    std::string makeETag(const std::string& key) {
        return "\"" + getVersion(key) + "\"";
    }

    /**
     * @brief 检查 ETag 是否匹配
     * @param key 资源 key
     * @param ifNoneMatch 请求头 If-None-Match 的值
     */
    bool checkMatch(const std::string& key, const std::string& ifNoneMatch) {
        if (ifNoneMatch.empty()) return false;
        return ifNoneMatch == makeETag(key);
    }

    /**
     * @brief 重置所有版本号（清理缓存时调用）
     */
    void resetAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, version] : versions_) {
            version = drogon::utils::getUuid();
            LOG_DEBUG << "[ResourceVersion] " << key << " version reset to " << version;
        }
        LOG_INFO << "[ResourceVersion] All " << versions_.size() << " versions reset";
    }

    /**
     * @brief 从 Redis 加载版本号到内存（启动时调用）
     * @param keys 需要加载的资源 key 列表
     */
    Task<void> loadFromRedis(const std::vector<std::string>& keys) {
        RedisService redis;
        int loaded = 0;

        for (const auto& key : keys) {
            std::string redisKey = "resource:version:" + key;
            auto value = co_await redis.get(redisKey);
            if (value && !value->empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                versions_[key] = *value;
                loaded++;
            }
        }

        LOG_INFO << "[ResourceVersion] Loaded " << loaded << " versions from Redis";
    }

private:
    ResourceVersion() = default;
    ResourceVersion(const ResourceVersion&) = delete;
    ResourceVersion& operator=(const ResourceVersion&) = delete;

    Task<void> syncToRedis(const std::string& key, const std::string& version) {
        RedisService redis;
        std::string redisKey = "resource:version:" + key;
        co_await redis.set(redisKey, version);
    }

    std::map<std::string, std::string> versions_;  // key -> UUID
    std::mutex mutex_;
};

/**
 * @brief ETag 工具函数（供 Controller 使用）
 */
namespace ETagUtils {
    /**
     * @brief 检查并处理 ETag（在 Controller 方法开头调用）
     * @return 如果返回非空响应，直接返回该响应（304）；否则继续执行业务逻辑
     */
    inline HttpResponsePtr checkETag(const HttpRequestPtr& req, const std::string& resourceKey) {
        if (req->method() != Get) return nullptr;

        std::string ifNoneMatch = req->getHeader("If-None-Match");
        if (!ifNoneMatch.empty() && ResourceVersion::instance().checkMatch(resourceKey, ifNoneMatch)) {
            LOG_DEBUG << "[ETag] 304 for " << req->path();
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k304NotModified);
            return resp;
        }
        return nullptr;
    }

    /**
     * @brief 为响应添加 ETag header
     */
    inline void addETag(const HttpResponsePtr& resp, const std::string& resourceKey) {
        resp->addHeader("ETag", ResourceVersion::instance().makeETag(resourceKey));
    }
}
