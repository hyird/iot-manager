#include <drogon/drogon.h>
#include <drogon/HttpAppFramework.h>
#include <iostream>
#include <filesystem>
#include <fstream>

// Utils
#include "common/utils/PlatformUtils.hpp"
#include "common/utils/LoggerManager.hpp"
#include "common/utils/ConfigManager.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"

// Filters
#include "common/filters/AuthFilter.hpp"
#include "common/filters/PermissionFilter.hpp"

// Database
#include "common/database/DatabaseInitializer.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/database/RedisService.hpp"

// Controllers - System Module
#include "modules/system/Auth.Controller.hpp"
#include "modules/system/User.Controller.hpp"
#include "modules/system/Role.Controller.hpp"
#include "modules/system/Menu.Controller.hpp"
#include "modules/system/Department.Controller.hpp"

// Controllers - Home Module
#include "modules/home/Home.Controller.hpp"

// Controllers - Link Module
#include "modules/link/Link.Controller.hpp"
#include "modules/link/Link.Service.hpp"

// Network
#include "common/network/TcpLinkManager.hpp"

// Controllers - Protocol Module
#include "modules/protocol/ProtocolConfig.Controller.hpp"

// Controllers - Device Module
#include "modules/device/Device.Controller.hpp"

// Protocol
#include "common/protocol/ProtocolDispatcher.hpp"

using namespace drogon;
namespace fs = std::filesystem;

/**
 * @brief 设置全局异常处理器
 */
void setupExceptionHandler() {
    app().setExceptionHandler([](const std::exception& e,
                                   const HttpRequestPtr& /*req*/,
                                   std::function<void (const HttpResponsePtr &)> &&callback) {
        Json::Value json;
        HttpStatusCode status = k500InternalServerError;

        if (const auto* appEx = dynamic_cast<const AppException*>(&e)) {
            json["code"] = appEx->getCode();
            json["message"] = appEx->getMessage();
            status = appEx->getStatus();
        } else {
            LOG_ERROR << "Unhandled exception: " << e.what();
            json["code"] = "INTERNAL_ERROR";
            json["message"] = "服务器内部错误";
        }

        json["status"] = static_cast<int>(status);
        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(status);
        callback(resp);
    });
}

/**
 * @brief 注册请求/响应拦截器
 */
void setupAdvices() {
    // 请求前拦截：记录请求开始时间和请求体
    app().registerPreHandlingAdvice([](const HttpRequestPtr &req) {
        req->attributes()->insert("startTime", std::chrono::steady_clock::now());

        // 记录请求体
        if (req->method() != Get && !req->body().empty()) {
            std::string body = std::string(req->body());
            if (body.length() > Constants::REQUEST_LOG_MAX_LENGTH) {
                body = body.substr(0, Constants::REQUEST_LOG_MAX_LENGTH) + "...(truncated)";
            }
            req->attributes()->insert("requestBody", body);
        }
    });

    // 请求后拦截：记录日志（ETag 由各 Controller 自行处理）
    app().registerPostHandlingAdvice([](const HttpRequestPtr &req, const HttpResponsePtr &resp) {

        // 记录请求日志
        std::string username = "-";
        try {
            username = req->attributes()->get<std::string>("username");
        } catch (...) {}

        std::string duration = "-";
        try {
            auto startTime = req->attributes()->get<std::chrono::steady_clock::time_point>("startTime");
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            duration = std::to_string(elapsed) + "ms";
        } catch (...) {}

        std::string body;
        try {
            body = req->attributes()->get<std::string>("requestBody");
        } catch (...) {}

        LOG_DEBUG << "[" << username << "] "
                  << req->methodString() << " " << req->path()
                  << (req->query().empty() ? "" : "?" + req->query())
                  << " -> " << static_cast<int>(resp->statusCode())
                  << " (" << duration << ")"
                  << (body.empty() ? "" : " " + body);
    });
}

/**
 * @brief 服务器启动回调
 */
void onServerStarted() {
    auto listeners = app().getListeners();
    std::cout << "Drogon Admin Server started" << std::endl;
    for (const auto& addr : listeners) {
        std::cout << "  -> http://" << addr.toIpPort() << std::endl;
        LOG_INFO << "Server listening on http://" << addr.toIpPort();
    }
    std::cout << "Logs: ./logs/server.log" << std::endl;

    // 初始化协议分发器
    ProtocolDispatcher::instance().initialize();

    // 初始化 TCP 链路管理器（使用独立的 IO 线程池）
    TcpLinkManager::instance().initialize(ConfigManager::getNumberOfThreads());

    // 异步初始化任务（顺序执行）
    async_run([]() -> Task<> {
        // 1. 初始化数据库（必须先完成）
        co_await DatabaseInitializer::initialize();

        // 2. 加载资源版本号（从 Redis，如果启用）
        co_await ResourceVersion::instance().loadFromRedis({
            "device", "user", "role", "menu", "department", "link", "protocol"
        });

        // 3. 启动所有已启用的链路
        LinkService linkService;
        co_await linkService.startAllEnabled();
    });
}

/**
 * @brief 服务器退出回调
 */
void onServerStopping() {
    LOG_INFO << "Server is stopping, cleaning up resources...";

    // 停止所有 TCP 链路
    TcpLinkManager::instance().stopAll();

    LOG_INFO << "All resources cleaned up";
}

/**
 * @brief 检查 Redis 是否可用
 */
bool checkRedisAvailable() {
    // 读取配置文件检查是否配置了 Redis
    std::vector<std::string> configPaths = {
        "./config/config.json",
        "../../config/config.json",
        "../config/config.json",
        "config.json"
    };

    for (const auto& path : configPaths) {
        if (fs::exists(path)) {
            try {
                std::ifstream ifs(path);
                if (ifs) {
                    Json::Value root;
                    Json::CharReaderBuilder builder;
                    std::string errs;
                    if (Json::parseFromStream(builder, ifs, &root, &errs)) {
                        // 检查是否配置了 Redis 客户端
                        bool hasRedis = root.isMember("redis_clients") &&
                                       root["redis_clients"].isArray() &&
                                       !root["redis_clients"].empty();

                        if (hasRedis) {
                            // 读取第一个 Redis 配置的 is_fast 字段
                            const auto& redisConfig = root["redis_clients"][0];
                            bool isFast = redisConfig.isMember("is_fast") &&
                                         redisConfig["is_fast"].asBool();
                            AppRedisConfig::useFast() = isFast;
                            LOG_INFO << "Redis is configured (fast mode: "
                                     << (isFast ? "true" : "false") << ")";
                            return true;
                        } else {
                            LOG_INFO << "Redis is not configured in config file";
                            return false;
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN << "Failed to check Redis config: " << e.what();
            }
        }
    }

    return false;
}

int main() {
    // 1. 平台特定初始化
    PlatformUtils::initialize();

    // 2. 初始化日志系统
    LoggerManager::initialize("./logs/server.log");

    // 3. 加载配置文件
    if (!ConfigManager::load()) {
        LOG_FATAL << "Failed to load config file";
        return 1;
    }

    // 4. 应用配置
    LoggerManager::setLogLevel(ConfigManager::getLogLevel());
    LoggerManager::setConsoleOutput(ConfigManager::isConsoleLogEnabled());

    // 5. 设置全局异常处理
    setupExceptionHandler();

    // 6. 注册请求/响应拦截器
    setupAdvices();

    // 7. 注册启动回调
    app().registerBeginningAdvice([]() {
        // 检查 Redis 是否可用（必须可用）
        bool redisAvailable = checkRedisAvailable();
        if (!redisAvailable) {
            LOG_FATAL << "Redis is not available. Server cannot start without Redis.";
            std::exit(1);
        }

        // 调用原始的启动回调
        onServerStarted();
    });

    // 8. SPA fallback: 非 API 路径且非静态文件返回 index.html
    app().setCustom404Page(HttpResponse::newFileResponse("./web/index.html"));

    // 9. 启动服务器
    app().run();

    // 10. 服务器退出后清理资源
    onServerStopping();

    return 0;
}
