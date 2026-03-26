#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/protocol/ProtocolCommandCoordinator.hpp"
#include "common/protocol/ProtocolCommandStore.hpp"
#include "common/protocol/ProtocolResultWriter.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/network/WebSocketManager.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/domain/EventBus.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/DrogonLoopSelector.hpp"
#include "modules/device/domain/CommandRepository.hpp"
#include "modules/device/domain/Events.hpp"
#include "modules/protocol/domain/Events.hpp"

#include <map>
#include <optional>
#include <sstream>

/**
 * @brief 协议分发服务
 *
 * 职责：
 * - 将链路接收的数据路由到对应的协议适配器
 * - 管理适配器注册、生命周期事件分发
 * - 报文解析结果攒批写入（通过 ProtocolResultWriter）
 *
 * 适配器通过 registerAdapter() 外部注册，Dispatcher 不依赖具体协议实现。
 */
class ProtocolDispatcher {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    static ProtocolDispatcher& instance() {
        static ProtocolDispatcher inst;
        return inst;
    }

    /**
     * @brief 构建运行时上下文（供外部创建适配器时使用）
     * 必须在 initialize() 之后调用。
     */
    ProtocolRuntimeContext buildRuntimeContext() {
        return {
            [this](std::vector<ParsedFrameResult>&& results) {
                if (results.empty()) return;
                totalFramesProcessed_.fetch_add(
                    static_cast<int64_t>(results.size()), std::memory_order_relaxed);
                if (resultWriter_) {
                    resultWriter_->submit(std::move(results));
                }
            },
            *commandCoordinator_,
            [this](const std::string& commandKey, const std::string& responseCode,
                   bool success, int64_t responseRecordId) {
                notifyCommandCompletion(commandKey, responseCode, success, responseRecordId);
            },
            commandStore_
        };
    }

    /**
     * @brief 注册协议适配器（由启动流程调用）
     */
    void registerAdapter(std::unique_ptr<ProtocolAdapter> adapter) {
        if (!adapter) return;
        adapters_.emplace(std::string(adapter->protocol()), std::move(adapter));
    }

    /**
     * @brief 初始化协议分发器核心基础设施
     * 创建命令协调器、结果写入器，设置链路回调和事件订阅。
     * 不创建具体适配器 —— 适配器由外部通过 registerAdapter() 注册。
     */
    void initialize() {
        LOG_DEBUG << "[ProtocolDispatcher] Initializing...";

        commandCoordinator_ = std::make_unique<ProtocolCommandCoordinator>();

        // 攒批写入专用线程（需要单线程串行化访问 pendingBatch_，不可分散）
        batchLoop_ = DrogonLoopSelector::fixed(0);
        resultWriter_ = std::make_unique<ProtocolResultWriter>(
            [this](const std::string& commandKey, const std::string& responseCode,
                   bool success, int64_t responseRecordId) {
                notifyCommandCompletion(commandKey, responseCode, success, responseRecordId);
            }
        );
        resultWriter_->initialize(batchLoop_);
        resultWriter_->setConnectionChecker([this](int deviceId) {
            return isDeviceConnected(deviceId);
        });

        // 设置 TcpLinkManager 的数据回调
        TcpLinkManager::instance().setDataCallbackWithClient(
            [this](int linkId, const std::string& clientAddr, const std::string& data) {
                handleLinkData(linkId, clientAddr, data);
            }
        );

        // 设置连接状态回调
        TcpLinkManager::instance().setConnectionCallback(
            [this](int linkId, const std::string& clientAddr, bool connected) {
                handleLinkConnection(linkId, clientAddr, connected);
            }
        );

        // 订阅设备/协议配置变更事件
        registerEventSubscriptions();

        // 协议维护定时器（1 秒周期）— 运行在 TCP IO 池，与链路数据处理同域
        maintenanceLoop_ = TcpLinkManager::instance().getNextIoLoop();
        maintenanceLoop_->runEvery(1.0, [this]() {
            try {
                runProtocolMaintenance();
            } catch (const std::exception& e) {
                LOG_ERROR << "[ProtocolDispatcher] runProtocolMaintenance exception: " << e.what();
            } catch (...) {
                LOG_ERROR << "[ProtocolDispatcher] runProtocolMaintenance unknown exception";
            }
        });

        LOG_DEBUG << "[ProtocolDispatcher] Initialized";
    }

    /**
     * @brief 启动后台定时任务（物化视图刷新、DeviceCache 自愈等）
     * 与协议路由无关的维护任务，从 Dispatcher 核心初始化中分离。
     */
    void startBackgroundTasks() {
        // 后台任务使用独立 IO 线程，避免占用攒批写入线程
        backgroundLoop_ = DrogonLoopSelector::getNext();

        // 定期刷新物化视图（5 分钟周期）
        backgroundLoop_->runEvery(300.0, [this]() {
            bool expected = false;
            if (!viewRefreshing_.compare_exchange_strong(expected, true)) {
                LOG_DEBUG << "[BackgroundTasks] Skipping view refresh (previous still running)";
                return;
            }
            try {
                drogon::async_run([this]() -> Task<> {
                    try {
                        DatabaseService dbService;
                        co_await dbService.execSqlCoro("SELECT refresh_device_data_views()");
                        LOG_DEBUG << "[BackgroundTasks] Materialized views refreshed";
                    } catch (const std::exception& e) {
                        LOG_WARN << "[BackgroundTasks] Failed to refresh materialized views: " << e.what();
                    }
                    viewRefreshing_.store(false, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                viewRefreshing_.store(false, std::memory_order_release);
                LOG_ERROR << "[BackgroundTasks] runEvery(refresh views) exception: " << e.what();
            } catch (...) {
                viewRefreshing_.store(false, std::memory_order_release);
                LOG_ERROR << "[BackgroundTasks] runEvery(refresh views) unknown exception";
            }
        });
    }

    Task<> initializeProtocolAsync(const std::string& protocol) {
        auto* adapter = findAdapter(protocol);
        if (!adapter) {
            LOG_WARN << "[ProtocolDispatcher] No adapter registered for initialization: " << protocol;
            co_return;
        }
        co_await adapter->initializeAsync();
        LOG_INFO << "[ProtocolDispatcher] Protocol runtime initialized: " << protocol;
    }

    void handleLinkData(int linkId, const std::string& clientAddr, const std::string& data) {
        onDataReceived(linkId, clientAddr, data);
    }

    void handleLinkConnection(int linkId, const std::string& clientAddr, bool connected) {
        onConnectionChanged(linkId, clientAddr, connected);
    }

    /**
     * @brief Agent 设备数据：通过 deviceId 路由到协议适配器
     *
     * Agent 上报的数据已携带 deviceId，直接查找设备获取协议类型，
     * 然后路由到对应的适配器。Agent 设备 link_id = 0，
     * SL651 适配器通过 buildFromCache(0, deviceCode) 从帧内 device_code 匹配设备。
     */
    void handleDeviceData(int deviceId, const std::string& clientAddr, const std::string& data) {
        if (deviceId <= 0 || data.empty()) return;

        try {
            if (!DeviceCache::instance().isLoaded()) {
                LOG_WARN << "[Agent] DeviceCache not loaded, dropping device data for deviceId=" << deviceId;
                scheduleDeviceCacheReload();
                return;
            }

            auto deviceOpt = DeviceCache::instance().findByIdSync(deviceId);
            if (!deviceOpt) {
                LOG_WARN << "[Agent] Device not found in cache: deviceId=" << deviceId;
                return;
            }

            const auto& protocol = deviceOpt->protocolType;
            if (protocol.empty()) {
                LOG_WARN << "[Agent] Device has no protocol type: deviceId=" << deviceId;
                return;
            }

            auto* adapter = findAdapter(protocol);
            if (!adapter) {
                LOG_WARN << "[Agent] No adapter for protocol " << protocol
                         << " (deviceId=" << deviceId << ")";
                return;
            }

            std::vector<uint8_t> bytes(data.begin(), data.end());
            LOG_DEBUG << "[Agent] Device " << deviceId << " RX " << bytes.size()
                      << "B from " << clientAddr << " | " << bytesToHex(bytes);

            // Agent 设备 link_id = 0，适配器通过帧内标识（device_code/slave_id）匹配具体设备
            adapter->onDataReceived(0, clientAddr, std::move(bytes));
        } catch (const std::exception& e) {
            LOG_ERROR << "[Agent] handleDeviceData exception (deviceId=" << deviceId
                      << ", client=" << clientAddr << "): " << e.what();
        } catch (...) {
            LOG_ERROR << "[Agent] handleDeviceData unknown exception (deviceId=" << deviceId << ")";
        }
    }

    /**
     * @brief Agent 端点连接事件
     *
     * 处理 Agent 端点的 TCP 连接/断开事件。
     * 更新链路版本号并广播给前端 WebSocket 客户端。
     */
    void handleEndpointConnection(int agentId, const std::string& endpointId,
                                  const std::string& clientAddr, bool connected) {
        LOG_INFO << "[Agent] Endpoint " << endpointId << " "
                 << (connected ? "connected" : "disconnected")
                 << ": " << clientAddr << " (agentId=" << agentId << ")";

        // 更新版本号通知前端
        ResourceVersion::instance().incrementVersion("link");

        // 广播连接事件给 WebSocket 客户端
        if (WebSocketManager::instance().connectionCount() > 0) {
            Json::Value data;
            data["agentId"] = agentId;
            data["endpointId"] = endpointId;
            data["clientAddr"] = clientAddr;
            data["connected"] = connected;
            WebSocketManager::instance().broadcast("agent:connection", data);
        }
    }

    /** 获取待应答指令数 */
    size_t pendingCommandCount() const {
        return commandCoordinator_ ? commandCoordinator_->pendingCount() : 0;
    }

    /** 协议处理统计 */
    struct ProtocolStats {
        int64_t framesProcessed;
        int64_t batchFlushes;
        int64_t batchFallbacks;
        size_t pendingCommands;
    };

    /**
     * @brief 提交预解析结果（Agent device:parsed 通道）
     * 跳过协议解析，直接进入攒批写入流程
     */
    void submitParsedResults(std::vector<ParsedFrameResult>&& results) {
        if (results.empty()) return;
        totalFramesProcessed_.fetch_add(
            static_cast<int64_t>(results.size()), std::memory_order_relaxed);
        if (resultWriter_) {
            resultWriter_->submit(std::move(results));
        }
    }

    ProtocolStats getProtocolStats() const {
        return {
            totalFramesProcessed_.load(std::memory_order_relaxed),
            resultWriter_ ? resultWriter_->batchFlushCount() : 0,
            resultWriter_ ? resultWriter_->batchFallbackCount() : 0,
            pendingCommandCount()
        };
    }

    std::optional<ProtocolAdapterMetrics> getAdapterMetrics(const std::string& protocol) const {
        auto* adapter = findAdapter(protocol);
        if (!adapter) {
            return std::nullopt;
        }
        return adapter->getMetrics();
    }

    /**
     * @brief 下发指令到设备（等待应答）
     *
     * 支持两种路由：
     * - linkId > 0: 传统链路模式，通过 linkId 查找协议
     * - linkId = 0: Agent 模式，通过 deviceId 查找协议
     */
    Task<CommandResult> sendCommand(const CommandRequest& req) {
        std::string protocol;
        if (req.linkId > 0) {
            protocol = DeviceCache::instance().getProtocolByLinkIdSync(req.linkId);
        } else if (req.deviceId > 0) {
            auto deviceOpt = DeviceCache::instance().findByIdSync(req.deviceId);
            if (deviceOpt) {
                protocol = deviceOpt->protocolType;
            }
        }

        if (protocol.empty()) {
            LOG_ERROR << "[ProtocolDispatcher] 无法确定协议: linkId=" << req.linkId
                      << ", deviceId=" << req.deviceId;
            co_return CommandResult::error("无法确定设备协议");
        }

        auto* adapter = findAdapter(protocol);
        if (!adapter) {
            LOG_ERROR << "[ProtocolDispatcher] 协议未注册适配器: " << protocol;
            co_return CommandResult::error("协议未注册适配器");
        }

        co_return co_await adapter->sendCommand(req);
    }

    /**
     * @brief 检查设备是否在线（基于实际连接状态）
     */
    /**
     * @brief 检查设备是否在线
     * 线程安全：adapters_ 在 initialize() 阶段注册完毕后不再修改，
     * 运行时仅只读迭代，无需加锁。
     */
    bool isDeviceConnected(int deviceId) const {
        for (const auto& [protocol, adapter] : adapters_) {
            if (adapter && adapter->isDeviceConnected(deviceId)) {
                return true;
            }
        }
        return false;
    }

    void notifyCommandCompletion(
        const std::string& commandKey,
        const std::string& responseCode,
        bool success,
        int64_t responseRecordId) {
        if (commandCoordinator_) {
            commandCoordinator_->notifyCompletion(commandKey, responseCode, success, responseRecordId);
        }
    }

private:
    ProtocolDispatcher() = default;
    ~ProtocolDispatcher() = default;
    ProtocolDispatcher(const ProtocolDispatcher&) = delete;
    ProtocolDispatcher& operator=(const ProtocolDispatcher&) = delete;

    void registerEventSubscriptions() {
        auto& bus = EventBus::instance();

        bus.subscribe<DeviceCreated>([this](const DeviceCreated& e) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] DeviceCreated event, protocol="
                     << (e.protocol.empty() ? "<all>" : e.protocol);
            dispatchDeviceLifecycleEvent({
                DeviceLifecycleAction::Created,
                e.aggregateId,
                0,
                e.protocol,
                e.deviceCode,
                false
            });
            co_return;
        });

        bus.subscribe<DeviceUpdated>([this](const DeviceUpdated& e) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] DeviceUpdated event, protocol="
                     << (e.protocol.empty() ? "<all>" : e.protocol);
            dispatchDeviceLifecycleEvent({
                DeviceLifecycleAction::Updated,
                e.aggregateId,
                e.linkId,
                e.protocol,
                e.deviceCode,
                e.registrationChanged
            });
            co_return;
        });

        bus.subscribe<DeviceDeleted>([this](const DeviceDeleted& e) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] DeviceDeleted event, protocol="
                     << (e.protocol.empty() ? "<all>" : e.protocol);
            dispatchDeviceLifecycleEvent({
                DeviceLifecycleAction::Deleted,
                e.aggregateId,
                0,
                e.protocol,
                e.deviceCode,
                false
            });
            co_return;
        });

        bus.subscribe<ProtocolConfigCreated>([this](const ProtocolConfigCreated& e) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] ProtocolConfigCreated event, protocol="
                     << (e.protocol.empty() ? "<all>" : e.protocol);
            dispatchProtocolConfigLifecycleEvent({
                ProtocolConfigLifecycleAction::Created,
                e.aggregateId,
                e.protocol,
                e.name
            });
            co_return;
        });

        bus.subscribe<ProtocolConfigUpdated>([this](const ProtocolConfigUpdated& e) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] ProtocolConfigUpdated event, protocol="
                     << (e.protocol.empty() ? "<all>" : e.protocol);
            dispatchProtocolConfigLifecycleEvent({
                ProtocolConfigLifecycleAction::Updated,
                e.aggregateId,
                e.protocol,
                e.name
            });
            co_return;
        });

        bus.subscribe<ProtocolConfigDeleted>([this](const ProtocolConfigDeleted& e) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] ProtocolConfigDeleted event, protocol="
                     << (e.protocol.empty() ? "<all>" : e.protocol);
            dispatchProtocolConfigLifecycleEvent({
                ProtocolConfigLifecycleAction::Deleted,
                e.aggregateId,
                e.protocol,
                e.name
            });
            co_return;
        });

        LOG_INFO << "[ProtocolDispatcher] Event subscriptions registered";
    }

    void onDataReceived(int linkId, const std::string& clientAddr, const std::string& data) {
        try {
            std::vector<uint8_t> bytes(data.begin(), data.end());

            if (!DeviceCache::instance().isLoaded()) {
                LOG_WARN << "[ProtocolDispatcher] DeviceCache not loaded, dropping data from link " << linkId;
                scheduleDeviceCacheReload();
                return;
            }

            std::string protocol = DeviceCache::instance().getProtocolByLinkIdSync(linkId);
            if (protocol.empty()) return;

            auto* adapter = findAdapter(protocol);
            if (!adapter) {
                LOG_WARN << "[ProtocolDispatcher] No adapter registered for protocol: " << protocol;
                return;
            }

            adapter->onDataReceived(linkId, clientAddr, std::move(bytes));
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] onDataReceived exception (link=" << linkId
                      << ", client=" << clientAddr << "): " << e.what();
        } catch (...) {
            LOG_ERROR << "[ProtocolDispatcher] onDataReceived unknown exception (link=" << linkId
                      << ", client=" << clientAddr << ")";
        }
    }

    void onConnectionChanged(int linkId, const std::string& clientAddr, bool connected) {
        try {
            if (DeviceCache::instance().isLoaded()) {
                auto protocol = DeviceCache::instance().getProtocolByLinkIdSync(linkId);
                if (auto* adapter = findAdapter(protocol)) {
                    adapter->onConnectionChanged(linkId, clientAddr, connected);
                }
            } else {
                scheduleDeviceCacheReload();
                LOG_DEBUG << "[ProtocolDispatcher] DeviceCache not loaded, skip connection routing";
            }

            if (!connected) {
                DeviceConnectionCache::instance().removeByClient(linkId, clientAddr);
            }
            ResourceVersion::instance().incrementVersion("link");

            if (WebSocketManager::instance().connectionCount() > 0) {
                Json::Value data;
                data["linkId"] = linkId;
                data["clientAddr"] = clientAddr;
                data["connected"] = connected;
                WebSocketManager::instance().broadcast("link:connection", data);
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] connectionCallback exception (link=" << linkId
                      << ", client=" << clientAddr << "): " << e.what();
        } catch (...) {
            LOG_ERROR << "[ProtocolDispatcher] connectionCallback unknown exception (link=" << linkId
                      << ", client=" << clientAddr << ")";
        }
    }

    ProtocolAdapter* findAdapter(const std::string& protocol) const {
        auto it = adapters_.find(protocol);
        return it != adapters_.end() ? it->second.get() : nullptr;
    }

    void scheduleAdapterReload(const std::string& protocol) {
        auto* adapter = findAdapter(protocol);
        if (!adapter) return;

        // 防抖：500ms 内多次触发只执行最后一次 reload
        // 使用 shared_ptr<atomic> 避免 lambda 捕获 map 元素引用（map 扩容可能导致悬垂）
        std::shared_ptr<std::atomic<uint64_t>> pending;
        {
            std::lock_guard lock(reloadPendingMutex_);
            auto& ptr = adapterReloadPending_[protocol];
            if (!ptr) {
                ptr = std::make_shared<std::atomic<uint64_t>>(0);
            }
            pending = ptr;
        }
        auto generation = pending->fetch_add(1, std::memory_order_acq_rel) + 1;

        auto* ioLoop = DrogonLoopSelector::getNext();
        ioLoop->runAfter(0.5, [adapter, protocol, generation, pending]() {
            if (pending->load(std::memory_order_acquire) != generation) {
                return;  // 被更新的 generation 覆盖，跳过
            }
            drogon::async_run([adapter, protocol]() -> Task<> {
                try {
                    LOG_INFO << "[ProtocolDispatcher] Reloading adapter " << protocol;
                    co_await adapter->reloadAsync();
                } catch (const std::exception& e) {
                    LOG_ERROR << "[ProtocolDispatcher] Failed to reload adapter " << protocol
                              << ": " << e.what();
                }
            });
        });
    }

    void applyLifecycleImpact(const std::string& protocol, ProtocolLifecycleImpact impact) {
        if (impact == ProtocolLifecycleImpact::Reload) {
            scheduleAdapterReload(protocol);
        }
    }

    void dispatchDeviceLifecycleEvent(const DeviceLifecycleEvent& event) {
        if (!event.protocol.empty()) {
            if (auto* adapter = findAdapter(event.protocol)) {
                applyLifecycleImpact(event.protocol, adapter->onDeviceLifecycleEvent(event));
            }
            return;
        }

        for (const auto& [protocol, adapter] : adapters_) {
            if (adapter) {
                applyLifecycleImpact(protocol, adapter->onDeviceLifecycleEvent(event));
            }
        }
    }

    void dispatchProtocolConfigLifecycleEvent(const ProtocolConfigLifecycleEvent& event) {
        if (!event.protocol.empty()) {
            if (auto* adapter = findAdapter(event.protocol)) {
                applyLifecycleImpact(event.protocol, adapter->onProtocolConfigLifecycleEvent(event));
            }
            return;
        }

        for (const auto& [protocol, adapter] : adapters_) {
            if (adapter) {
                applyLifecycleImpact(protocol, adapter->onProtocolConfigLifecycleEvent(event));
            }
        }
    }

    void runProtocolMaintenance() {
        for (const auto& [protocol, adapter] : adapters_) {
            (void)protocol;
            if (adapter) {
                adapter->onMaintenanceTick();
            }
        }
    }

    static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
        std::ostringstream oss;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) oss << ' ';
            oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }

    /**
     * @brief 调度 DeviceCache 异步重加载
     * 当 onDataReceived 检测到 DeviceCache 未加载时调用。
     */
    void scheduleDeviceCacheReload() {
        auto cooldownTp = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(deviceCacheReloadCooldown_.load(std::memory_order_acquire)));
        if (cooldownTp.time_since_epoch().count() > 0) {
            if (std::chrono::steady_clock::now() < cooldownTp) return;
        }

        bool expected = false;
        if (!deviceCacheReloading_.compare_exchange_strong(expected, true)) {
            return;
        }

        LOG_WARN << "[ProtocolDispatcher] Scheduling DeviceCache reload...";
        auto* loop = DrogonLoopSelector::getNext();
        loop->queueInLoop([this]() {
            drogon::async_run([this]() -> Task<> {
                try {
                    co_await DeviceCache::instance().getDevices();
                    LOG_INFO << "[ProtocolDispatcher] DeviceCache reloaded successfully";
                    deviceCacheReloading_.store(false, std::memory_order_release);
                } catch (const std::exception& e) {
                    LOG_ERROR << "[ProtocolDispatcher] DeviceCache reload failed: " << e.what()
                              << ", will retry in 5s";
                    deviceCacheReloadCooldown_.store(
                        std::chrono::steady_clock::now().time_since_epoch().count()
                        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::seconds(5)).count(),
                        std::memory_order_release);
                    deviceCacheReloading_.store(false, std::memory_order_release);
                }
            });
        });
    }

    std::unique_ptr<ProtocolCommandCoordinator> commandCoordinator_;
    std::unique_ptr<ProtocolResultWriter> resultWriter_;
    ProtocolCommandStore commandStore_;
    std::map<std::string, std::unique_ptr<ProtocolAdapter>> adapters_;

    trantor::EventLoop* batchLoop_ = nullptr;          // 攒批写入专用（固定 IO 线程）
    trantor::EventLoop* maintenanceLoop_ = nullptr;    // 协议维护定时器
    trantor::EventLoop* backgroundLoop_ = nullptr;     // 后台任务（物化视图等）

    std::atomic<int64_t> totalFramesProcessed_{0};

    std::atomic<bool> deviceCacheReloading_{false};
    std::atomic<int64_t> deviceCacheReloadCooldown_{0};

    std::map<std::string, std::shared_ptr<std::atomic<uint64_t>>> adapterReloadPending_;  // 防抖 generation 计数器
    std::mutex reloadPendingMutex_;  // 保护 adapterReloadPending_ 的并发访问

    std::atomic<bool> viewRefreshing_{false};
};
