// mimalloc: 全局替换 new/delete，必须在所有其他 include 之前
#include <mimalloc-new-delete.h>

// Utils
#include "common/utils/PlatformUtils.hpp"
#include "common/utils/LoggerManager.hpp"
#include "common/utils/ConfigManager.hpp"
#include "common/utils/ExceptionHandler.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"

// Filters
#include "common/filters/AuthFilter.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/filters/RequestAdvices.hpp"

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
#include "modules/link/domain/LinkEventHandlers.hpp"

// Network
#include "common/network/TcpLinkManager.hpp"

// Controllers - Protocol Module
#include "modules/protocol/ProtocolConfig.Controller.hpp"

// Controllers - Device Module
#include "modules/device/Device.Controller.hpp"
#include "modules/device-group/DeviceGroup.Controller.hpp"

// Controllers - Alert Module
#include "modules/alert/Alert.Controller.hpp"
#include "modules/alert/AlertEngine.hpp"

// WebSocket Module
#include "modules/websocket/WebSocket.Controller.hpp"
#include "modules/websocket/WsEventHandlers.hpp"

// Protocol
#include "common/protocol/ProtocolDispatcher.hpp"

using namespace drogon;

// ─── 启动错误输出 ──────────────────────────────────────────

/**
 * @brief 输出启动阶段错误到控制台和日志
 */
void printStartupError(const std::string& title, const std::string& detail,
                       const std::vector<std::string>& hints = {}) {
    std::string border(60, '=');
    std::cerr << "\n" << border << "\n";
    std::cerr << " [ERROR] " << title << "\n";
    std::cerr << std::string(60, '-') << "\n";
    std::cerr << "  " << detail << "\n";
    if (!hints.empty()) {
        std::cerr << "\n  请检查:\n";
        for (const auto& hint : hints) {
            std::cerr << "    - " << hint << "\n";
        }
    }
    std::cerr << border << "\n" << std::endl;

    LOG_FATAL << "[Startup] " << title << ": " << detail;
}

/**
 * @brief 根据失败阶段返回排查提示
 */
std::vector<std::string> getStageHints(const std::string& stage) {
    if (stage == "database:ping" || stage == "database:initialize") {
        return {
            "PostgreSQL 服务是否正在运行",
            "config 中 db_clients 的 host/port 是否正确",
            "数据库用户名和密码是否正确",
            "数据库名称是否存在",
            "防火墙/安全组是否放行了数据库端口",
        };
    }
    if (stage == "redis:ping") {
        return {
            "Redis 服务是否正在运行",
            "config 中 redis_clients 的 host/port 是否正确",
            "Redis 密码是否正确（requirepass）",
            "防火墙/安全组是否放行了 Redis 端口",
        };
    }
    if (stage == "cache:preload-device" || stage == "cache:invalidate") {
        return {
            "数据库连接是否稳定",
            "相关数据表是否已正确创建",
        };
    }
    if (stage == "resource-version:load-and-reset") {
        return {
            "Redis 连接是否稳定",
        };
    }
    if (stage == "alert-engine:initialize") {
        return {
            "告警规则表是否已正确创建",
            "数据库连接是否稳定",
        };
    }
    if (stage == "links:start-enabled") {
        return {
            "链路配置表是否已正确创建",
            "数据库连接是否稳定",
        };
    }
    return {};
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
    std::cout << "Logs: ./logs/iot-manager_*.log" << std::endl;

    // 初始化协议分发器
    ProtocolDispatcher::instance().initialize();

    // 初始化 TCP 链路管理器（使用独立的 IO 线程池）
    TcpLinkManager::instance().initialize(ConfigManager::getNumberOfThreads());

    // 注册事件处理器（在启动链路之前）
    LinkEventHandlers::registerAll();
    WsEventHandlers::registerAll();

    // 异步初始化任务（顺序执行）
    async_run([]() -> Task<> {
        std::string stage = "startup:init";
        try {
            stage = "database:ping";
            LOG_INFO << "[Startup] " << stage;
            DatabaseService dbHealthCheck;
            co_await dbHealthCheck.ping();

            stage = "redis:ping";
            LOG_INFO << "[Startup] " << stage;
            RedisService redisHealthCheck;
            co_await redisHealthCheck.ping();

            stage = "database:initialize";
            LOG_INFO << "[Startup] " << stage;
            // 1. 初始化数据库（必须先完成）
            co_await DatabaseInitializer::initialize();

            stage = "cache:invalidate";
            LOG_INFO << "[Startup] " << stage;
            // 2. 清理缓存（数据库初始化可能变更 schema，确保缓存与新结构一致）
            DeviceCache::instance().markStale();
            RealtimeDataCache::instance().invalidateAll();

            stage = "cache:preload-device";
            LOG_INFO << "[Startup] " << stage;
            // 3. 预加载 DeviceCache（确保 TcpIoPool 同步访问时有数据）
            co_await DeviceCache::instance().getDevices();

            stage = "resource-version:load-and-reset";
            LOG_INFO << "[Startup] " << stage;
            // 4. 加载资源版本号（从 Redis），然后重置以强制客户端重新获取数据
            co_await ResourceVersion::instance().loadFromRedis({
                "device", "user", "role", "menu", "department", "link", "protocol", "alert", "deviceGroup"
            });
            ResourceVersion::instance().resetAll();

            stage = "alert-engine:initialize";
            LOG_INFO << "[Startup] " << stage;
            // 5. 初始化告警引擎（加载启用的规则到内存）
            co_await AlertEngine::instance().initialize();
            AlertEngine::instance().startOfflineChecker(app().getLoop());

            stage = "links:start-enabled";
            LOG_INFO << "[Startup] " << stage;
            // 6. 启动所有已启用的链路
            LinkService linkService;
            co_await linkService.startAllEnabled();

            LOG_INFO << "[Startup] bootstrap completed";
        } catch (const std::exception& e) {
            printStartupError("启动阶段失败: " + stage, e.what(), getStageHints(stage));
            app().getLoop()->queueInLoop([]() {
                app().quit();
            });
        } catch (...) {
            printStartupError("启动阶段失败: " + stage, "未知异常", getStageHints(stage));
            app().getLoop()->queueInLoop([]() {
                app().quit();
            });
        }
    });
}

/**
 * @brief 服务器退出回调
 */
void onServerStopping() {
    LOG_INFO << "Server is stopping, cleaning up resources...";

    // 1. 停止告警引擎离线检测定时器
    AlertEngine::instance().stopOfflineChecker();

    // 2. 停止所有 TCP 链路
    TcpLinkManager::instance().stopAll();

    // 3. 注销所有事件处理器（防止内存泄漏）
    EventBus::instance().unsubscribeAll();

    // 4. 清理缓存
    DeviceCache::instance().invalidate();

    LOG_INFO << "All resources cleaned up";
}

int main() {
    // 0. 验证 mimalloc 已激活
    int v = mi_version();
    std::cout << "mimalloc v" << (v / 100) << "." << (v % 100) << " active" << std::endl;

    // 1. 平台特定初始化
    PlatformUtils::initialize();

    // 2. 初始化日志系统（AsyncFileLogger 异步写盘 + 按日期轮转）
    LoggerManager::initialize("./logs");

    // 3. 加载并验证配置文件（失败时 ConfigManager 已输出详细错误）
    if (!ConfigManager::load()) {
        std::cerr << "Server startup aborted due to configuration errors." << std::endl;
        return 1;
    }

    // 4. 应用配置
    LoggerManager::setLogLevel(ConfigManager::getLogLevel());

    // 5. 设置全局异常处理
    AppExceptionHandler::setup();

    // 6. 注册请求/响应拦截器
    RequestAdvices::setup();

    // 7. 注册启动回调
    app().registerBeginningAdvice([]() {
        // 检查 DB 客户端是否可用（Drogon 根据配置创建连接池）
        try {
            DatabaseService dbService;
            if (!dbService.getClient()) {
                printStartupError("数据库客户端不可用",
                    "Drogon 未能创建 DB 客户端 'default'", {
                    "config 中 db_clients 配置是否正确",
                    "PostgreSQL 服务是否正在运行",
                    "数据库连接地址和端口是否可达",
                });
                std::exit(1);
            }
        } catch (const std::exception& e) {
            printStartupError("数据库客户端初始化失败", e.what(), {
                "config 中 db_clients 配置是否正确",
                "PostgreSQL 服务是否正在运行",
                "数据库连接地址和端口是否可达",
            });
            std::exit(1);
        }

        // 检查 Redis 客户端是否可用
        try {
            auto client = AppRedisConfig::useFast()
                ? drogon::app().getFastRedisClient("default")
                : drogon::app().getRedisClient("default");
            if (!client) {
                printStartupError("Redis 客户端不可用",
                    "Drogon 未能创建 Redis 客户端 'default'", {
                    "config 中 redis_clients 配置是否正确",
                    "Redis 服务是否正在运行",
                    "Redis 连接地址和端口是否可达",
                });
                std::exit(1);
            }
            LOG_INFO << "Redis client is configured (fast mode: "
                     << (AppRedisConfig::useFast() ? "true" : "false") << ")";
        } catch (const std::exception& e) {
            printStartupError("Redis 客户端初始化失败", e.what(), {
                "config 中 redis_clients 配置是否正确",
                "Redis 服务是否正在运行",
                "Redis 连接地址和端口是否可达",
            });
            std::exit(1);
        }

        // 所有基础设施就绪，执行启动流程
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
