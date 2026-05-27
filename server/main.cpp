// mimalloc: 全局替换 new/delete，必须在所有其他 include 之前
#include <mimalloc-new-delete.h>

#include <atomic>
#include <csignal>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

// Utils
#include "common/utils/PlatformUtils.hpp"
#include "common/utils/LoggerManager.hpp"
#include "common/utils/ConfigManager.hpp"
#include "common/utils/ExceptionHandler.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/edgenode/AgentBridgeManager.hpp"
#include "common/network/WebSocketManager.hpp"

// Filters
#include "common/filters/AuthFilter.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/filters/RequestAdvices.hpp"

// Database
#include "common/database/DatabaseInitializer.hpp"
#include "common/database/DatabaseService.hpp"

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
#include "modules/edgenode/EdgeNodeController.hpp"

// Network
#include "common/network/TcpLinkManager.hpp"

// Controllers - Protocol Module
#include "modules/protocol/ProtocolConfig.Controller.hpp"

// Controllers - Device Module
#include "modules/device/Device.Controller.hpp"
#include "modules/device-group/DeviceGroup.Controller.hpp"
#include "modules/open/OpenAccess.Controller.hpp"
#include "modules/open/OpenWebhookEventHandlers.hpp"

// Controllers - Alert Module
#include "modules/alert/Alert.Controller.hpp"
#include "modules/alert/AlertEngine.hpp"

// WebSocket Module
#include "modules/websocket/EdgeNodeWebSocket.Controller.hpp"
#include "modules/websocket/WebSocket.Controller.hpp"
#include "modules/websocket/WsEventHandlers.hpp"

// GB28181 Module
#include "modules/gb28181/Gb28181Module.hpp"

// Protocol
#include "common/protocol/ProtocolDispatcher.hpp"
#include "common/protocol/sl651/SL651.DeviceConfigProvider.hpp"
#include "common/protocol/sl651/SL651.ProtocolAdapter.hpp"
#include "common/protocol/modbus/Modbus.ProtocolAdapter.hpp"
#include "common/protocol/s7/S7.ProtocolAdapter.hpp"

using namespace drogon;

namespace {

std::atomic_bool gShutdownRequested{false};
std::atomic_bool gShutdownCleanupDone{false};

void broadcastShutdownSnapshot(const std::string& reason) {
    if (WebSocketManager::instance().connectionCount() == 0) {
        return;
    }

    try {
        const auto devices = DeviceCache::instance().getDevicesSnapshotSync();
        Json::Value deviceIds(Json::arrayValue);
        Json::Value updates(Json::arrayValue);
        for (const auto& device : devices) {
            if (device.id <= 0) {
                continue;
            }
            deviceIds.append(device.id);

            Json::Value update(Json::objectValue);
            update["id"] = device.id;
            update["reportTime"] = Json::nullValue;
            update["connected"] = false;
            update["connectionState"] = "offline";
            updates.append(std::move(update));
        }

        if (!deviceIds.empty()) {
            Json::Value offline(Json::objectValue);
            offline["deviceIds"] = std::move(deviceIds);
            offline["reason"] = reason;
            WebSocketManager::instance().broadcast("device:offline", offline);

            Json::Value realtime(Json::objectValue);
            realtime["updates"] = std::move(updates);
            realtime["reason"] = reason;
            WebSocketManager::instance().broadcast("device:realtime", realtime);
        }

        auto linkStatuses = TcpLinkManager::instance().getAllStatus();
        if (linkStatuses.isArray()) {
            for (const auto& link : linkStatuses) {
                const int linkId = link.get("link_id", 0).asInt();
                const auto& clients = link["clients"];
                if (linkId <= 0 || !clients.isArray()) {
                    continue;
                }
                for (const auto& client : clients) {
                    const std::string clientAddr = client.asString();
                    if (clientAddr.empty()) {
                        continue;
                    }
                    Json::Value data(Json::objectValue);
                    data["linkId"] = linkId;
                    data["clientAddr"] = clientAddr;
                    data["connected"] = false;
                    data["reason"] = reason;
                    WebSocketManager::instance().broadcast("link:connection", data);
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN << "[Shutdown] Broadcast snapshot failed: " << e.what();
    } catch (...) {
        LOG_WARN << "[Shutdown] Broadcast snapshot failed with unknown exception";
    }
}

void performShutdownCleanup(const std::string& reason) {
    bool expected = false;
    if (!gShutdownCleanupDone.compare_exchange_strong(expected, true)) {
        return;
    }

    LOG_INFO << "Server is stopping, cleaning up resources... reason=" << reason;

    broadcastShutdownSnapshot(reason);

    // 0. Stop GB28181 SIP/media runtime first so no new camera traffic enters shutdown.
    Gb28181Module::instance().stop();

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

void requestShutdown(const char* reason) {
    bool expected = false;
    if (!gShutdownRequested.compare_exchange_strong(expected, true)) {
        LOG_WARN << "[Shutdown] Forced exit after repeated signal";
        std::_Exit(128);
    }

    auto shutdown = [reason = std::string(reason ? reason : "signal")]() {
        performShutdownCleanup(reason);
        app().quit();
    };

    try {
        if (auto* loop = app().getLoop()) {
            loop->queueInLoop(std::move(shutdown));
            return;
        }
    } catch (...) {
    }

    shutdown();
}

void signalHandler(int signal) {
    requestShutdown(signal == SIGINT ? "SIGINT" : "SIGTERM");
}

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD signal) {
    switch (signal) {
        case CTRL_C_EVENT:
            requestShutdown("CTRL_C_EVENT");
            return TRUE;
        case CTRL_BREAK_EVENT:
            requestShutdown("CTRL_BREAK_EVENT");
            return TRUE;
        case CTRL_CLOSE_EVENT:
            requestShutdown("CTRL_CLOSE_EVENT");
            return TRUE;
        case CTRL_LOGOFF_EVENT:
            requestShutdown("CTRL_LOGOFF_EVENT");
            return TRUE;
        case CTRL_SHUTDOWN_EVENT:
            requestShutdown("CTRL_SHUTDOWN_EVENT");
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

void installShutdownSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif
}

}  // namespace

// ─── 启动错误输出 ──────────────────────────────────────────

/**
 * @brief 输出启动阶段错误到控制台和日志
 */
/**
 * @brief 致命错误退出（Windows 下暂停，避免控制台闪退）
 */
[[noreturn]] void fatalExit(int code = 1) {
#ifdef _WIN32
    std::cerr << "Press Enter to exit..." << std::endl;
    std::cin.get();
#endif
    std::exit(code);
}

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
    if (stage == "database:ping" || stage == "database:initialize" || stage == "database:migrate") {
        return {
            "PostgreSQL 服务是否正在运行",
            "config 中 db_clients 的 host/port 是否正确",
            "数据库用户名和密码是否正确",
            "数据库名称是否存在",
            "防火墙/安全组是否放行了数据库端口",
            "检查 schema_migrations 表中是否有失败记录",
        };
    }
    if (stage == "cache:preload-device" || stage == "cache:invalidate") {
        return {
            "数据库连接是否稳定",
            "相关数据表是否已正确创建",
        };
    }
    if (stage == "protocol:modbus-initialize") {
        return {
            "数据库连接是否稳定",
            "device 和 protocol_config 表是否已正确创建",
        };
    }
    if (stage == "protocol:s7-initialize") {
        return {
            "数据库连接是否稳定",
            "device 和 protocol_config 表是否已正确创建",
            "S7 自研客户端协议栈是否初始化成功",
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

    // Initialize the shared TCP IO pool before modules that bind network sockets.
    TcpLinkManager::instance().initialize(ConfigManager::getNumberOfThreads());

    try {
        Gb28181Module::instance().start();
    } catch (const std::exception& e) {
        printStartupError("GB28181 module startup failed", e.what(), {
            "Check custom_config.gb28181.sip host/port",
            "Check whether SIP port 5060 is already in use",
            "Check ZLM media configuration",
        });
        app().getLoop()->queueInLoop([]() {
            app().quit();
        });
        return;
    }

    // 初始化协议分发器基础设施
    auto& dispatcher = ProtocolDispatcher::instance();
    dispatcher.initialize();

    // 注册协议适配器
    {
        static sl651::SL651DeviceConfigProvider sl651ConfigProvider;
        auto runtimeCtx = dispatcher.buildRuntimeContext();
        dispatcher.registerAdapter(
            std::make_unique<sl651::SL651ProtocolAdapter>(runtimeCtx, sl651ConfigProvider));
        dispatcher.registerAdapter(
            std::make_unique<modbus::ModbusProtocolAdapter>(runtimeCtx));
        dispatcher.registerAdapter(
            std::make_unique<s7::S7ProtocolAdapter>(runtimeCtx));
    }

    // 启动后台维护任务（物化视图刷新等）
    dispatcher.startBackgroundTasks();

    AgentBridgeManager::instance().setIngressHandlers(
        [](int deviceId, const std::string& clientAddr, const std::string& data) {
            ProtocolDispatcher::instance().handleDeviceData(deviceId, clientAddr, data);
        },
        [](int agentId, const std::string& endpointId, const std::string& clientAddr, bool connected) {
            ProtocolDispatcher::instance().handleEndpointConnection(agentId, endpointId, clientAddr, connected);
        }
    );
    AgentBridgeManager::instance().setParsedDataHandler(
        [](std::vector<ParsedFrameResult>&& results) {
            ProtocolDispatcher::instance().submitParsedResults(std::move(results));
        }
    );
    AgentBridgeManager::instance().setCommandResultCallback(
        [](const std::string& commandKey, const std::string& responseCode, bool success, int64_t responseRecordId) {
            ProtocolDispatcher::instance().notifyCommandCompletion(commandKey, responseCode, success, responseRecordId);
        }
    );

    // 启动 Agent 心跳超时检测和事件清理
    AgentBridgeManager::instance().startHealthCheck(app().getLoop());

    // 注册事件处理器（在启动链路之前）
    LinkEventHandlers::registerAll();
    WsEventHandlers::registerAll();
    OpenWebhookEventHandlers::registerAll();

    // 异步初始化任务（顺序执行）
    async_run([]() -> Task<> {
        std::string stage = "startup:init";
        try {
            stage = "database:ping";
            LOG_INFO << "[Startup] " << stage;
            DatabaseService dbHealthCheck;
            co_await dbHealthCheck.ping();

            stage = "database:initialize";
            LOG_INFO << "[Startup] " << stage;
            // 1. 初始化数据库（必须先完成）
            co_await DatabaseInitializer::initialize();

            stage = "cache:invalidate";
            LOG_INFO << "[Startup] " << stage;
            // 2. 清理缓存（数据库初始化可能变更 schema，确保缓存与新结构一致）
            DeviceCache::instance().markStale();

            stage = "cache:preload-device";
            LOG_INFO << "[Startup] " << stage;
            // 3. 预加载 DeviceCache（确保 TcpIoPool 同步访问时有数据）
            co_await DeviceCache::instance().getDevices();

            stage = "protocol:modbus-initialize";
            LOG_INFO << "[Startup] " << stage;
            // 4. 初始化协议运行时（当前需预热 Modbus，依赖数据库表和 DeviceCache）
            co_await ProtocolDispatcher::instance().initializeProtocolAsync(Constants::PROTOCOL_MODBUS);

            stage = "protocol:s7-initialize";
            LOG_INFO << "[Startup] " << stage;
            co_await ProtocolDispatcher::instance().initializeProtocolAsync(Constants::PROTOCOL_S7);

            stage = "resource-version:reset";
            LOG_INFO << "[Startup] " << stage;
            ResourceVersion::instance().resetAll({
                "device", "user", "role", "menu", "department", "link", "protocol", "alert", "deviceGroup", "agent"
            });

            stage = "alert-engine:initialize";
            LOG_INFO << "[Startup] " << stage;
            // 5. 初始化告警引擎（加载启用的规则到内存）
            co_await AlertEngine::instance().initialize();
            AlertEngine::instance().startOfflineChecker(app().getLoop());

            stage = "agent:reset-online-status";
            LOG_INFO << "[Startup] " << stage;
            // 6. 重置 Agent 在线状态（服务器重启后清理残留）
            co_await AgentBridgeManager::instance().resetOnStartup();

            stage = "links:start-enabled";
            LOG_INFO << "[Startup] " << stage;
            // 7. 启动所有已启用的链路
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
    performShutdownCleanup("app_stop");
}

int main() {
    std::cout << "[Server] starting..." << std::endl;
    installShutdownSignalHandlers();

    // 0. 验证 mimalloc 已激活
    int v = mi_version();
    std::cout << "mimalloc v" << (v / 100) << "." << (v % 100) << " active" << std::endl;

    // 1. 平台特定初始化
    PlatformUtils::initialize();

    // 2. 初始化日志系统（AsyncFileLogger 异步写盘，仅按日期轮转，禁用大小分段）
    LoggerManager::initialize("./logs");

    // 3. 加载并验证配置文件（失败时 ConfigManager 已输出详细错误）
    if (!ConfigManager::load()) {
        std::cerr << "Server startup aborted due to configuration errors." << std::endl;
        fatalExit();
    }

    // 4. 应用配置
    LoggerManager::setLogLevel(ConfigManager::getLogLevel());

    // 5. 设置全局异常处理
    AppExceptionHandler::setup();

    // 6. 注册请求/响应拦截器
    RequestAdvices::setup();

    try {
        Gb28181Module::instance().initialize();
    } catch (const std::exception& e) {
        printStartupError("GB28181 module configuration failed", e.what(), {
            "Check custom_config.gb28181 in config/config.json",
            "Set custom_config.gb28181.enabled=false to disable the module",
        });
        fatalExit();
    }

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
                fatalExit();
            }
        } catch (const std::exception& e) {
            printStartupError("数据库客户端初始化失败", e.what(), {
                "config 中 db_clients 配置是否正确",
                "PostgreSQL 服务是否正在运行",
                "数据库连接地址和端口是否可达",
            });
            fatalExit();
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
