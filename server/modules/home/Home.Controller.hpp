#pragma once

#include "Home.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 首页控制器
 *
 * 职责：参数校验 + 权限检查，业务逻辑委托给 HomeService。
 */
class HomeController : public drogon::HttpController<HomeController> {
private:
    HomeService service_;
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
    ADD_METHOD_TO(HomeController::monitor, "/api/home/monitor", Get, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取统计数据
     */
    Task<HttpResponsePtr> stats(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        auto data = co_await service_.getStats();
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

        auto data = co_await service_.getSystemInfo(uptime);
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

        co_await service_.clearAllCache();

        LOG_INFO << "All caches cleared by userId: " << req->attributes()->get<int>("userId");

        co_return Response::ok("缓存清理成功");
    }

    /**
     * @brief 获取系统监控数据
     */
    Task<HttpResponsePtr> monitor(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        auto data = co_await service_.getMonitorData();
        co_return Response::ok(data);
    }
};
