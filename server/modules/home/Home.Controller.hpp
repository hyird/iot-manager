#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/AppException.hpp"
#include "common/cache/AuthCache.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 首页控制器
 */
class HomeController : public drogon::HttpController<HomeController> {
private:
    DatabaseService db_;
    AuthCache authCache_;
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HomeController::stats, "/api/home/stats", Get, "AuthFilter");
    ADD_METHOD_TO(HomeController::systemInfo, "/api/home/system", Get, "AuthFilter");
    ADD_METHOD_TO(HomeController::clearCache, "/api/home/cache/clear", Post, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取统计数据
     * 单条 SQL 合并所有 COUNT，减少 DB 往返
     */
    Task<HttpResponsePtr> stats(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        // 合并所有统计查询为单条 SQL（4 次往返 → 1 次）
        auto result = co_await db_.execSqlCoro(R"(
            SELECT
                (SELECT COUNT(*) FROM sys_user WHERE deleted_at IS NULL) AS user_count,
                (SELECT COUNT(*) FROM sys_role WHERE deleted_at IS NULL) AS role_count,
                (SELECT COUNT(*) FROM sys_menu WHERE deleted_at IS NULL AND type = 'page') AS menu_count,
                (SELECT COUNT(*) FROM sys_department WHERE deleted_at IS NULL) AS dept_count,
                (SELECT COUNT(*) FROM device WHERE deleted_at IS NULL) AS device_count,
                (SELECT COUNT(*) FROM link WHERE deleted_at IS NULL) AS link_count
        )");

        Json::Value data;
        data["userCount"] = result[0]["user_count"].as<int>();
        data["roleCount"] = result[0]["role_count"].as<int>();
        data["menuCount"] = result[0]["menu_count"].as<int>();
        data["departmentCount"] = result[0]["dept_count"].as<int>();
        data["deviceCount"] = result[0]["device_count"].as<int>();
        data["linkCount"] = result[0]["link_count"].as<int>();

        // 今日数据量：优先从连续聚合读取，回退到直接 COUNT
        // 注意：MSVC 不支持 catch 块中使用 co_await，用标志位重构
        bool useFallbackCount = false;
        try {
            auto todayResult = co_await db_.execSqlCoro(R"(
                SELECT COALESCE(SUM(record_count), 0) AS today_count
                FROM device_data_hourly
                WHERE bucket >= date_trunc('day', now())
            )");
            data["todayDataCount"] = static_cast<Json::Int64>(todayResult[0]["today_count"].as<int64_t>());
        } catch (...) {
            useFallbackCount = true;
        }
        if (useFallbackCount) {
            // 连续聚合不可用，回退到直接查询
            auto todayResult = co_await db_.execSqlCoro(R"(
                SELECT COUNT(*) AS today_count
                FROM device_data
                WHERE report_time >= date_trunc('day', now())
            )");
            data["todayDataCount"] = static_cast<Json::Int64>(todayResult[0]["today_count"].as<int64_t>());
        }

        co_return Response::ok(data);
    }

    /**
     * @brief 获取系统信息
     */
    Task<HttpResponsePtr> systemInfo(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();

        // 获取当前时间（UTC）
        auto currentTime = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(currentTime);
        std::ostringstream oss;
        std::tm tmBuf{};
#ifdef _WIN32
        gmtime_s(&tmBuf, &timeT);
#else
        gmtime_r(&timeT, &tmBuf);
#endif
        oss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%SZ");

        // 查询数据库时区配置
        auto tzResult = co_await db_.execSqlCoro("SHOW timezone");
        std::string dbTimezone = tzResult[0][0].as<std::string>();

        Json::Value data;
        data["version"] = "1.0.0";
        data["serverTime"] = oss.str();
        data["timezone"] = dbTimezone;
        data["uptime"] = static_cast<Json::Int64>(uptime);
#ifdef _WIN32
        data["platform"] = "Windows";
#elif __APPLE__
        data["platform"] = "macOS";
#else
        data["platform"] = "Linux";
#endif

        co_return Response::ok(data);
    }

    /**
     * @brief 清理所有缓存
     */
    Task<HttpResponsePtr> clearCache(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"system:cache:clear"}
        );

        // 清理认证相关缓存
        co_await authCache_.clearAllUserSessionsCache();
        co_await authCache_.clearAllUserRolesCache();
        co_await authCache_.clearAllUserMenusCache();

        // 清理设备缓存
        DeviceCache::instance().markStale();

        // 清理设备实时数据缓存
        RealtimeDataCache::instance().invalidateAll();

        // 重置所有资源版本
        ResourceVersion::instance().resetAll();

        LOG_INFO << "All caches cleared by userId: " << req->attributes()->get<int>("userId");

        co_return Response::ok("缓存清理成功");
    }
};
