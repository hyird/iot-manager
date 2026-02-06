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
#include "common/database/RedisChecker.hpp"

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

// Protocol
#include "common/protocol/ProtocolDispatcher.hpp"

using namespace drogon;

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

    // 注册 Link 事件处理器（在启动链路之前）
    LinkEventHandlers::registerAll();

    // 异步初始化任务（顺序执行）
    async_run([]() -> Task<> {
        // 1. 初始化数据库（必须先完成）
        co_await DatabaseInitializer::initialize();

        // 2. 清理缓存（数据库初始化可能变更 schema，确保缓存与新结构一致）
        DeviceCache::instance().markStale();

        // 3. 预加载 DeviceCache（确保 TcpIoPool 同步访问时有数据）
        co_await DeviceCache::instance().getDevices();

        RealtimeDataCache::instance().invalidateAll();

        // 3. 加载资源版本号（从 Redis），然后重置以强制客户端重新获取数据
        co_await ResourceVersion::instance().loadFromRedis({
            "device", "user", "role", "menu", "department", "link", "protocol"
        });
        ResourceVersion::instance().resetAll();

        // 4. 启动所有已启用的链路
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

int main() {
    // 1. 平台特定初始化
    PlatformUtils::initialize();

    // 2. 初始化日志系统（AsyncFileLogger 异步写盘 + 按日期轮转）
    LoggerManager::initialize("./logs");

    // 3. 加载配置文件
    if (!ConfigManager::load()) {
        LOG_FATAL << "Failed to load config file";
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
        // 检查 Redis 是否可用（必须可用）
        bool redisAvailable = RedisChecker::checkAvailable();
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
