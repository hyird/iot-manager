#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/network/WebSocketManager.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/domain/EventBus.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/AppException.hpp"
#include "modules/device/domain/CommandRepository.hpp"
#include "modules/device/domain/Events.hpp"
#include "modules/device/DeviceDataTransformer.hpp"
#include "modules/protocol/domain/Events.hpp"
#include "modules/alert/AlertEngine.hpp"
#include "sl651/SL651.hpp"
#include "modbus/Modbus.hpp"

/**
 * @brief 指令等待的共享状态（零轮询协程挂起/唤醒）
 *
 * 协程挂起后，由以下两种方式之一精确唤醒：
 * - 应答回调 resolve() → queueInLoop → handle.resume()（跨线程安全）
 * - 超时定时器 → handle.resume()（同线程，定时器在协程所在 EventLoop）
 *
 * resolved atomic 保证只唤醒一次，避免 double-resume。
 */
struct CommandWaitState {
    trantor::EventLoop* loop = nullptr;
    std::coroutine_handle<> handle;
    trantor::TimerId timerId{0};
    std::atomic<bool> resolved{false};
    std::atomic<bool> suspended{false};  // 协程是否真正挂起（防 double-resume）
    bool result = false;
};

/**
 * @brief 自定义 co_await Awaiter：零轮询指令等待
 *
 * 使用方式：bool success = co_await CommandAwaiter{state, timeoutMs};
 */
struct CommandAwaiter {
    std::shared_ptr<CommandWaitState> state;
    int timeoutMs;

    bool await_ready() const noexcept {
        // 极快响应：挂起前已有应答，跳过挂起
        return state->resolved.load(std::memory_order_acquire);
    }

    bool await_suspend(std::coroutine_handle<> h) {
        state->handle = h;

        // 注册超时定时器（在协程所在的 EventLoop 上触发）
        std::weak_ptr<CommandWaitState> weak = state;
        state->timerId = state->loop->runAfter(
            static_cast<double>(timeoutMs) / 1000.0,
            [weak]() {
                if (auto s = weak.lock()) {
                    if (!s->resolved.exchange(true, std::memory_order_acq_rel)) {
                        s->result = false;  // 超时
                        // 仅在协程真正挂起后才唤醒（防 double-resume）
                        if (s->suspended.load(std::memory_order_acquire)) {
                            s->handle.resume();
                        }
                    }
                }
            }
        );

        // 双重检查：挂起前 resolve() 可能已从 TcpIoPool 被调用
        if (state->resolved.load(std::memory_order_acquire)) {
            state->loop->invalidateTimer(state->timerId);
            return false;  // 取消挂起，直接进 await_resume
        }
        // 标记已挂起（queueInLoop 回调和定时器回调据此决定是否 resume）
        state->suspended.store(true, std::memory_order_release);
        return true;  // 正式挂起
    }

    bool await_resume() const noexcept { return state->result; }
};

/**
 * @brief 待应答的指令
 */
struct PendingCommand {
    std::string deviceCode;
    std::string funcCode;
    std::shared_ptr<CommandWaitState> waitState;
    std::chrono::steady_clock::time_point expireTime;
    int64_t downCommandId;  // 下行指令的数据库记录 ID
    int userId;             // 下发用户 ID
};

/**
 * @brief 协议分发服务
 * 负责将链路接收的数据路由到对应的协议解析器
 *
 * 写入优化：
 * - 报文解析结果通过 BatchWriter 攒批（100 条或 200ms 触发）
 * - 批量 INSERT 减少 DB 往返，ResourceVersion 每批只递增一次
 */
class ProtocolDispatcher {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    /** 攒批阈值：达到此数量立即 flush */
    static constexpr size_t BATCH_SIZE = 100;
    /** 攒批定时器间隔（秒）：超过此时间强制 flush */
    static constexpr double BATCH_FLUSH_INTERVAL_SEC = 0.2;

    static ProtocolDispatcher& instance() {
        static ProtocolDispatcher inst;
        return inst;
    }

    /**
     * @brief 初始化协议分发器（同步部分）
     * 设置 TcpLinkManager 的数据回调，创建协议处理器
     * 注意：Modbus 异步初始化需在数据库就绪后调用 initializeModbusAsync()
     */
    void initialize() {
        LOG_DEBUG << "[ProtocolDispatcher] Initializing...";

        // 创建 SL651 解析器
        sl651Parser_ = std::make_unique<sl651::SL651Parser>(
            // 获取设备配置的回调（协程版本）
            [this](int linkId, const std::string& remoteCode) -> Task<std::optional<sl651::DeviceConfig>> {
                co_return co_await getDeviceConfigAsync(linkId, remoteCode);
            },
            // 获取要素定义的回调
            [this](const sl651::DeviceConfig& config, const std::string& funcCode) -> std::vector<sl651::ElementDef> {
                return getElements(config, funcCode);
            }
        );

        // 设置指令应答回调
        sl651Parser_->setCommandResponseCallback(
            [this](const std::string& deviceCode, const std::string& funcCode, bool success, int64_t responseId) {
                notifyCommandResponse(deviceCode, funcCode, success, responseId);
            }
        );

        // 设置 TcpLinkManager 的数据回调（带客户端地址，用于建立设备连接映射）
        TcpLinkManager::instance().setDataCallbackWithClient(
            [this](int linkId, const std::string& clientAddr, const std::string& data) {
                onDataReceived(linkId, clientAddr, data);
            }
        );

        // 设置连接状态回调（TcpIoPool 线程直接处理，无需跨线程）
        TcpLinkManager::instance().setConnectionCallback(
            [this](int linkId, const std::string& clientAddr, bool connected) {
              try {
                // Modbus 轮询管理（定时器已在 TcpIoPool 线程）
                if (modbusHandler_) {
                    modbusHandler_->onConnectionChanged(linkId, clientAddr, connected);
                }

                // 以下操作线程安全，可直接在 TcpIoPool 调用
                if (!connected) {
                    DeviceConnectionCache::instance().removeByClient(linkId, clientAddr);
                }
                ResourceVersion::instance().incrementVersion("link");

                // WebSocket 推送连接状态变更
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
              }
            }
        );

        // 指定批量写入 IO 线程（所有报文解析结果汇聚到此线程攒批）
        batchLoop_ = drogon::app().getIOLoop(0);

        // 创建 Modbus 处理器（仅创建，不初始化——初始化需等数据库就绪）
        modbusHandler_ = std::make_unique<modbus::ModbusHandler>();
        modbusHandler_->setCommandResponseCallback(
            [this](const std::string& deviceCode, const std::string& funcCode, bool success, int64_t responseId) {
                notifyCommandResponse(deviceCode, funcCode, success, responseId);
            }
        );

        // 订阅设备/协议配置事件，触发 Modbus 重载
        registerEventSubscriptions();

        // 设备连接映射的清理完全由 TCP 断连回调（removeByClient）负责，
        // 不再按 lastSeen 定时清理，避免 DTU 在线但设备故障不响应时映射被误删。

        // 定期刷新物化视图（5 分钟周期，并发刷新不阻塞读取）
        batchLoop_->runEvery(300.0, []() {
            try {
                drogon::async_run([]() -> Task<> {
                    try {
                        DatabaseService dbService;
                        co_await dbService.execSqlCoro("SELECT refresh_device_data_views()");
                        LOG_DEBUG << "[ProtocolDispatcher] Materialized views refreshed";
                    } catch (const std::exception& e) {
                        LOG_WARN << "[ProtocolDispatcher] Failed to refresh materialized views: " << e.what();
                    }
                });
            } catch (const std::exception& e) {
                LOG_ERROR << "[ProtocolDispatcher] runEvery(refresh views) exception: " << e.what();
            } catch (...) {
                LOG_ERROR << "[ProtocolDispatcher] runEvery(refresh views) unknown exception";
            }
        });

        LOG_DEBUG << "[ProtocolDispatcher] Initialized";
    }

    /**
     * @brief 异步初始化 Modbus 处理器（需在数据库初始化完成后调用）
     * 加载设备上下文，启动轮询定时器
     */
    Task<> initializeModbusAsync() {
        if (modbusHandler_) {
            co_await modbusHandler_->initialize();
            LOG_INFO << "[ProtocolDispatcher] Modbus handler initialized";
        }
    }

private:
    ProtocolDispatcher() = default;
    ~ProtocolDispatcher() = default;
    ProtocolDispatcher(const ProtocolDispatcher&) = delete;
    ProtocolDispatcher& operator=(const ProtocolDispatcher&) = delete;

    /**
     * @brief 注册设备/协议配置事件订阅
     * 设备或协议配置变更时，重载 Modbus 设备并清除协议缓存
     */
    void registerEventSubscriptions() {
        auto& bus = EventBus::instance();

        // 设备事件 → 重载 Modbus + 清除链路协议缓存
        bus.subscribe<DeviceCreated>([this](const DeviceCreated&) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] DeviceCreated event, reloading Modbus devices";
            reloadModbusDevices();
            co_return;
        });

        bus.subscribe<DeviceUpdated>([this](const DeviceUpdated&) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] DeviceUpdated event, reloading Modbus devices";
            reloadModbusDevices();
            co_return;
        });

        bus.subscribe<DeviceDeleted>([this](const DeviceDeleted&) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] DeviceDeleted event, reloading Modbus devices";
            reloadModbusDevices();
            co_return;
        });

        // 协议配置事件 → 重载 Modbus + 清除链路协议缓存
        bus.subscribe<ProtocolConfigCreated>([this](const ProtocolConfigCreated&) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] ProtocolConfigCreated event, reloading Modbus devices";
            reloadModbusDevices();
            co_return;
        });

        bus.subscribe<ProtocolConfigUpdated>([this](const ProtocolConfigUpdated&) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] ProtocolConfigUpdated event, reloading Modbus devices";
            reloadModbusDevices();
            co_return;
        });

        bus.subscribe<ProtocolConfigDeleted>([this](const ProtocolConfigDeleted&) -> Task<void> {
            LOG_INFO << "[ProtocolDispatcher] ProtocolConfigDeleted event, reloading Modbus devices";
            reloadModbusDevices();
            co_return;
        });

        LOG_INFO << "[ProtocolDispatcher] Event subscriptions registered";
    }

    /**
     * @brief 数据接收回调
     * @param linkId 链路ID
     * @param clientAddr 客户端地址（用于建立设备连接映射）
     * @param data 接收到的数据
     */
    void onDataReceived(int linkId, const std::string& clientAddr, const std::string& data) {
      try {
        // === 全部在 TcpIoPool 线程执行，零跨线程解析 ===
        std::vector<uint8_t> bytes(data.begin(), data.end());

        LOG_DEBUG << "[Link " << linkId << "] RX " << bytes.size() << "B from " << clientAddr
                  << " | " << modbus::ModbusUtils::toHexString(bytes);

        if (!DeviceCache::instance().isLoaded()) {
            LOG_WARN << "[ProtocolDispatcher] DeviceCache not loaded, dropping data from link " << linkId;
            scheduleDeviceCacheReload();
            return;
        }

        // === 心跳包 / 注册包预处理 ===
        auto devices = DeviceCache::instance().getDevicesByLinkIdSync(linkId);

        // 辅助：注册连接（同时注册 deviceCode key 和 Modbus deviceId key）
        auto doRegister = [&](const DeviceCache::CachedDevice& dev, bool logInfo, const char* matchType) {
            DeviceConnectionCache::instance().registerConnection(dev.deviceCode, linkId, clientAddr);
            // Modbus 设备无 deviceCode，额外注册 "modbus_<id>" key 供 ModbusHandler 路由使用
            if (dev.protocolType == Constants::PROTOCOL_MODBUS) {
                DeviceConnectionCache::instance().registerConnection(
                    "modbus_" + std::to_string(dev.id), linkId, clientAddr);
            }
            if (logInfo) {
                LOG_INFO << "[Link " << linkId << "] " << matchType << " matched device "
                         << (dev.deviceCode.empty() ? dev.name : dev.deviceCode) << " from " << clientAddr;
            } else {
                LOG_DEBUG << "[Link " << linkId << "] " << matchType << " matched device "
                          << (dev.deviceCode.empty() ? dev.name : dev.deviceCode) << " from " << clientAddr;
            }
        };

        // 1. 注册包匹配（ON 模式：智能判断完整匹配或前缀匹配）
        for (const auto& dev : devices) {
            if (dev.registrationMode != "OFF" && !dev.registrationBytes.empty()) {
                if (bytes == dev.registrationBytes) {
                    // 完整匹配：整包就是注册包
                    doRegister(dev, true, "Registration");
                    return;  // 注册包不传给协议解析器
                }
                if (bytes.size() > dev.registrationBytes.size() &&
                    std::equal(dev.registrationBytes.begin(), dev.registrationBytes.end(), bytes.begin())) {
                    // 前缀匹配：数据以注册包开头
                    doRegister(dev, true, "Registration prefix");
                    // 剥离前缀，继续协议解析
                    bytes.erase(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(dev.registrationBytes.size()));
                    break;
                }
            }
        }

        // 2. 心跳包匹配
        for (const auto& dev : devices) {
            if (dev.heartbeatMode != "OFF" && !dev.heartbeatBytes.empty()) {
                if (bytes == dev.heartbeatBytes) {
                    doRegister(dev, false, "Heartbeat");
                    return;  // 心跳包不传给协议解析器
                }
            }
        }

        // === 注册包拦截：配置了注册包的链路，未注册的连接不允许通过 ===
        bool requiresRegistration = std::any_of(devices.begin(), devices.end(),
            [](const auto& dev) { return dev.registrationMode != "OFF" && !dev.registrationBytes.empty(); });
        if (requiresRegistration &&
            !DeviceConnectionCache::instance().isClientRegistered(linkId, clientAddr)) {
            LOG_WARN << "[Link " << linkId << "] Unregistered client " << clientAddr
                     << ", dropping " << bytes.size() << "B";
            return;
        }

        // === 正常协议分发 ===
        std::string protocol = DeviceCache::instance().getProtocolByLinkIdSync(linkId);
        if (protocol.empty()) return;

        std::vector<ParsedFrameResult> results;

        if (protocol == Constants::PROTOCOL_SL651) {
            results = sl651Parser_->parseDataSync(linkId, clientAddr, bytes,
                [this](int lid, const std::string& code) {
                    return buildDeviceConfigFromCache(lid, code);
                });
        } else if (protocol == Constants::PROTOCOL_MODBUS) {
            results = modbusHandler_->parseDataSync(linkId, clientAddr, bytes);
        } else {
            LOG_WARN << "[ProtocolDispatcher] 未知协议: " << protocol;
        }

        // === 解析完成，投递到批量写入线程攒批 ===
        if (!results.empty()) {
            totalFramesProcessed_.fetch_add(static_cast<int64_t>(results.size()), std::memory_order_relaxed);
            batchLoop_->queueInLoop([this, results = std::move(results)]() mutable {
                enqueueBatchResults(std::move(results));
            });
        }
      } catch (const std::exception& e) {
        LOG_ERROR << "[ProtocolDispatcher] onDataReceived exception (link=" << linkId
                  << ", client=" << clientAddr << "): " << e.what();
      } catch (...) {
        LOG_ERROR << "[ProtocolDispatcher] onDataReceived unknown exception (link=" << linkId
                  << ", client=" << clientAddr << ")";
      }
    }

    /**
     * @brief 轮询获取下一个 Drogon IO 线程
     */
    trantor::EventLoop* getNextDrogonLoop() {
        size_t threadNum = drogon::app().getThreadNum();
        if (threadNum == 0) {
            return drogon::app().getLoop();  // 回退到主循环
        }
        size_t idx = ioLoopIndex_.fetch_add(1, std::memory_order_relaxed) % threadNum;
        return drogon::app().getIOLoop(idx);
    }

    // ==================== 批量写入（BatchWriter）====================

    /**
     * @brief 将解析结果加入攒批队列（仅在 batchLoop_ 线程调用）
     *
     * 触发条件：队列达到 BATCH_SIZE 立即 flush，否则 200ms 定时器 flush
     */
    void enqueueBatchResults(std::vector<ParsedFrameResult>&& results) {
        for (auto& r : results) {
            pendingBatch_.push_back(std::move(r));
        }

        // 启动定时器（首次入队时）
        if (!batchTimerActive_ && !pendingBatch_.empty()) {
            batchTimerActive_ = true;
            batchTimerId_ = batchLoop_->runAfter(BATCH_FLUSH_INTERVAL_SEC, [this]() {
                try {
                    batchTimerActive_ = false;
                    flushBatch();
                } catch (const std::exception& e) {
                    LOG_ERROR << "[ProtocolDispatcher] flushBatch timer exception: " << e.what();
                } catch (...) {
                    LOG_ERROR << "[ProtocolDispatcher] flushBatch timer unknown exception";
                }
            });
        }

        // 达到阈值立即 flush
        if (pendingBatch_.size() >= BATCH_SIZE) {
            if (batchTimerActive_) {
                batchLoop_->invalidateTimer(batchTimerId_);
                batchTimerActive_ = false;
            }
            flushBatch();
        }
    }

    /**
     * @brief flush 攒批队列（仅在 batchLoop_ 线程调用）
     */
    void flushBatch() {
        if (pendingBatch_.empty()) return;

        totalBatchFlushes_.fetch_add(1, std::memory_order_relaxed);

        std::vector<ParsedFrameResult> batch;
        batch.swap(pendingBatch_);

        drogon::async_run([this, batch = std::move(batch)]() -> Task<> {
            try {
                co_await saveBatchResults(batch);
            } catch (const std::exception& e) {
                LOG_ERROR << "[ProtocolDispatcher] flushBatch failed: " << e.what();
            }
        });
    }

    /**
     * @brief 批量保存解析结果到数据库
     * - 多值 INSERT 减少 DB 往返
     * - ResourceVersion 每批只递增一次
     * - 失败时回退到逐条写入
     */
    Task<void> saveBatchResults(const std::vector<ParsedFrameResult>& batch) {
        // MSVC 不支持 catch 中 co_await，用标志位重构
        bool batchFailed = false;
        try {
            // 1. 构建批量写入条目
            std::vector<CommandRepository::SaveItem> items;
            items.reserve(batch.size());
            for (const auto& r : batch) {
                items.push_back({r.deviceId, r.linkId, r.protocol, r.data, r.reportTime});
            }

            // 2. 批量 INSERT
            auto ids = co_await CommandRepository::saveBatch(items);

            // 3. 更新实时数据缓存（co_await 确保 Redis 写入完成）+ 处理指令应答
            for (size_t i = 0; i < batch.size(); ++i) {
                const auto& r = batch[i];
                co_await RealtimeDataCache::instance().updateAsync(r.deviceId, r.funcCode, r.data, r.reportTime);

                if (r.commandResponse && i < ids.size()) {
                    notifyCommandResponse(
                        r.commandResponse->deviceCode,
                        r.commandResponse->funcCode,
                        r.commandResponse->success,
                        ids[i]
                    );
                }
            }

            // 4. 告警规则检查（独立于 DB 写入，单条失败不影响其他）
            for (const auto& r : batch) {
                try {
                    co_await AlertEngine::instance().checkData(r.deviceId, r.data);
                } catch (const std::exception& e) {
                    LOG_WARN << "[ProtocolDispatcher] checkData failed for device="
                             << r.deviceId << ": " << e.what();
                }
            }

            // 5. 资源版本号：整批只递增一次
            ResourceVersion::instance().incrementVersion("device");

            LOG_TRACE << "[ProtocolDispatcher] Batch saved: " << batch.size() << " records";
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] saveBatchResults failed: " << e.what()
                      << ", falling back to individual saves";
            batchFailed = true;
            totalBatchFallbacks_.fetch_add(1, std::memory_order_relaxed);
        }
        // 回退到逐条写入（catch 外执行，避免 MSVC co_await in catch 限制）
        if (batchFailed) {
            size_t savedCount = 0;
            size_t failedCount = 0;
            for (const auto& r : batch) {
                try {
                    co_await CommandRepository::save(
                        r.deviceId, r.linkId, r.protocol, r.data, r.reportTime);
                    ++savedCount;
                } catch (const std::exception& e) {
                    ++failedCount;
                    // 仅记录首条失败的详细原因，避免 DB 不可用时刷屏
                    if (failedCount == 1) {
                        LOG_ERROR << "[ProtocolDispatcher] 逐条写入也失败: " << e.what();
                    }
                }
                // 无论 DB 是否成功，尽力更新实时缓存（Redis 可能仍可用）
                try {
                    RealtimeDataCache::instance().update(r.deviceId, r.funcCode, r.data, r.reportTime);
                } catch (...) {}
            }
            if (savedCount > 0) {
                ResourceVersion::instance().incrementVersion("device");
            }
            if (failedCount > 0) {
                LOG_ERROR << "[ProtocolDispatcher] 批次回退: " << batch.size() << " 条中 "
                          << failedCount << " 条写入数据库失败（DB 可能不可用）";
            }
        }

        // 5. WebSocket 推送已转换的实时数据（无论批量还是回退路径，都在 Redis 更新后推送）
        co_await broadcastRealtimeViaWs(batch);
    }

    /**
     * @brief WebSocket 推送已转换的实时数据
     *
     * 从 Redis 读回受影响设备的完整实时数据，通过 DeviceDataTransformer 转换为
     * 与 GET /api/device/realtime 相同的 {id, reportTime, elements, image} 格式推送。
     * 前端收到后可直接 setQueryData 更新缓存，无需 HTTP 请求。
     */
    Task<void> broadcastRealtimeViaWs(const std::vector<ParsedFrameResult>& batch) {
        if (WebSocketManager::instance().connectionCount() == 0) co_return;

        try {
            // 收集受影响的 deviceId（去重）
            std::set<int> affectedIds;
            for (const auto& r : batch) {
                affectedIds.insert(r.deviceId);
            }

            // 获取设备配置（内存缓存，快速）
            auto cachedDevices = co_await DeviceCache::instance().getDevices();
            std::map<int, const DeviceCache::CachedDevice*> deviceMap;
            for (const auto& d : cachedDevices) {
                deviceMap[d.id] = &d;
            }

            auto& realtimeCache = RealtimeDataCache::instance();
            Json::Value updates(Json::arrayValue);

            for (int deviceId : affectedIds) {
                auto it = deviceMap.find(deviceId);
                if (it == deviceMap.end()) continue;

                // 从 Redis 读取完整的实时数据（所有 funcCode，已被 updateAsync 写入）
                auto realtimeData = co_await realtimeCache.get(deviceId);
                auto latestTime = co_await realtimeCache.getLatestReportTime(deviceId);

                RealtimeDataCache::DeviceRealtimeData emptyData;
                const auto& data = realtimeData ? *realtimeData : emptyData;
                updates.append(DeviceDataTransformer::buildRealtimeItem(*it->second, data, latestTime));
            }

            Json::Value payload;
            payload["updates"] = std::move(updates);
            WebSocketManager::instance().broadcast("device:realtime", payload);
        } catch (const std::exception& e) {
            LOG_WARN << "[ProtocolDispatcher] broadcastRealtimeViaWs failed: " << e.what();
        }
    }

    /**
     * @brief 从 DeviceCache 同步构建 SL651 设备配置（TcpIoPool 线程调用）
     */
    std::optional<sl651::DeviceConfig> buildDeviceConfigFromCache(int linkId, const std::string& remoteCode) {
        auto cachedOpt = DeviceCache::instance().findByLinkAndCodeSync(linkId, remoteCode);
        if (!cachedOpt) return std::nullopt;

        const auto& cached = *cachedOpt;

        sl651::DeviceConfig config;
        config.deviceId = cached.id;
        config.deviceName = cached.name;
        config.deviceCode = cached.deviceCode;
        config.protocolConfigId = cached.protocolConfigId;
        config.linkId = cached.linkId;
        config.timezone = cached.timezone;

        // 从缓存的协议配置 JSON 解析要素定义
        if (!cached.protocolConfig.isNull()) {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            std::string configJson = Json::writeString(writer, cached.protocolConfig);
            parseElementsFromConfig(config, configJson);
        }

        return config;
    }

    /**
     * @brief 获取设备配置（协程版本）
     */
    Task<std::optional<sl651::DeviceConfig>> getDeviceConfigAsync(int linkId, const std::string& remoteCode) {
        try {
            auto dbClient = AppDbConfig::useFast()
                ? drogon::app().getFastDbClient("default")
                : drogon::app().getDbClient("default");

            auto result = co_await dbClient->execSqlCoro(R"(
                SELECT d.id, d.name, d.protocol_params, d.protocol_config_id, d.link_id,
                       pc.config
                FROM device d
                JOIN protocol_config pc ON d.protocol_config_id = pc.id
                WHERE d.link_id = $1 AND d.protocol_params->>'device_code' = $2
                  AND d.deleted_at IS NULL AND pc.deleted_at IS NULL
            )", std::to_string(linkId), remoteCode);

            if (result.empty()) {
                co_return std::nullopt;
            }

            // 解析 protocol_params JSONB
            Json::Value pp;
            std::string ppStr = result[0]["protocol_params"].isNull() ? "" : result[0]["protocol_params"].as<std::string>();
            if (!ppStr.empty()) {
                Json::CharReaderBuilder rb;
                std::istringstream iss(ppStr);
                std::string errs;
                Json::parseFromStream(rb, iss, &pp, &errs);
            }

            sl651::DeviceConfig config;
            config.deviceId = result[0]["id"].as<int>();
            config.deviceName = result[0]["name"].as<std::string>();
            config.deviceCode = pp.get("device_code", "").asString();
            config.protocolConfigId = result[0]["protocol_config_id"].as<int>();
            config.linkId = result[0]["link_id"].as<int>();
            config.timezone = pp.get("timezone", "+08:00").asString();

            // 解析协议配置中的要素定义
            std::string configJson = result[0]["config"].as<std::string>();
            parseElementsFromConfig(config, configJson);

            co_return config;

        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] 获取设备配置失败: " << e.what();
            co_return std::nullopt;
        }
    }

    /**
     * @brief 从协议配置 JSON 中解析要素定义
     */
    void parseElementsFromConfig(sl651::DeviceConfig& config, const std::string& configJson) {
        try {
            Json::Value root;
            Json::CharReaderBuilder builder;
            std::string errs;
            std::istringstream iss(configJson);

            if (!Json::parseFromStream(builder, iss, &root, &errs)) {
                LOG_ERROR << "[ProtocolDispatcher] Failed to parse config JSON: " << errs;
                return;
            }

            // 解析 funcs 数组
            if (!root.isMember("funcs") || !root["funcs"].isArray()) {
                return;
            }

            for (const auto& func : root["funcs"]) {
                std::string funcCode = func.get("funcCode", "").asString();
                if (funcCode.empty()) continue;

                // 保存功能码名称映射
                std::string funcName = func.get("name", "").asString();
                if (!funcName.empty()) {
                    config.funcNames[funcCode] = funcName;
                }

                // 保存功能码方向映射
                std::string dir = func.get("dir", "UP").asString();
                config.funcDirections[funcCode] = sl651::parseDirection(dir);

                std::vector<sl651::ElementDef> elements;

                // 解析 elements 数组
                if (func.isMember("elements") && func["elements"].isArray()) {
                    for (const auto& elem : func["elements"]) {
                        sl651::ElementDef def;
                        // id 为 UUID 字符串
                        def.id = elem.get("id", "").asString();
                        def.name = elem.get("name", "").asString();
                        def.funcCode = funcCode;
                        def.guideHex = elem.get("guideHex", "").asString();
                        def.encode = sl651::parseEncode(elem.get("encode", "BCD").asString());
                        def.length = elem.get("length", 0).asInt();
                        def.digits = elem.get("digits", 0).asInt();
                        def.unit = elem.get("unit", "").asString();
                        def.remark = elem.get("remark", "").asString();

                        // 解析字典配置（仅 DICT 编码类型）
                        if (elem.isMember("dictConfig") && elem["dictConfig"].isObject()) {
                            const auto& dictConfigJson = elem["dictConfig"];
                            sl651::DictConfig dictConfig;

                            std::string mapTypeStr = dictConfigJson.get("mapType", "VALUE").asString();
                            dictConfig.mapType = sl651::parseDictMapType(mapTypeStr);

                            if (dictConfigJson.isMember("items") && dictConfigJson["items"].isArray()) {
                                for (const auto& item : dictConfigJson["items"]) {
                                    sl651::DictMapItem mapItem;
                                    mapItem.key = item.get("key", "").asString();
                                    mapItem.label = item.get("label", "").asString();
                                    mapItem.value = item.get("value", "1").asString();  // 默认为 "1"

                                    // 解析依赖条件（仅 BIT 模式使用）
                                    if (item.isMember("dependsOn") && item["dependsOn"].isObject()) {
                                        const auto& dependsOnJson = item["dependsOn"];
                                        sl651::DictDependsOn dependsOn;

                                        std::string operatorStr = dependsOnJson.get("operator", "AND").asString();
                                        dependsOn.op = sl651::parseDictDependencyOperator(operatorStr);

                                        if (dependsOnJson.isMember("conditions") && dependsOnJson["conditions"].isArray()) {
                                            for (const auto& cond : dependsOnJson["conditions"]) {
                                                sl651::DictDependency dependency;
                                                dependency.bitIndex = cond.get("bitIndex", "").asString();
                                                dependency.bitValue = cond.get("bitValue", "1").asString();
                                                dependsOn.conditions.push_back(dependency);
                                            }
                                        }

                                        mapItem.dependsOn = dependsOn;
                                    }

                                    dictConfig.items.push_back(mapItem);
                                }
                            }

                            def.dictConfig = dictConfig;
                        }

                        elements.push_back(def);
                    }
                }

                config.elementsByFunc[funcCode] = elements;

                // 解析 responseElements 数组（下行功能码的应答要素）
                if (func.isMember("responseElements") && func["responseElements"].isArray()) {
                    std::vector<sl651::ElementDef> responseElements;
                    for (const auto& elem : func["responseElements"]) {
                        sl651::ElementDef def;
                        def.id = elem.get("id", "").asString();
                        def.name = elem.get("name", "").asString();
                        def.funcCode = funcCode;
                        def.guideHex = elem.get("guideHex", "").asString();
                        def.encode = sl651::parseEncode(elem.get("encode", "BCD").asString());
                        def.length = elem.get("length", 0).asInt();
                        def.digits = elem.get("digits", 0).asInt();
                        def.unit = elem.get("unit", "").asString();
                        def.remark = elem.get("remark", "").asString();

                        // 解析字典配置
                        if (elem.isMember("dictConfig") && elem["dictConfig"].isObject()) {
                            const auto& dictConfigJson = elem["dictConfig"];
                            sl651::DictConfig dictConfig;
                            dictConfig.mapType = sl651::parseDictMapType(dictConfigJson.get("mapType", "VALUE").asString());

                            if (dictConfigJson.isMember("items") && dictConfigJson["items"].isArray()) {
                                for (const auto& item : dictConfigJson["items"]) {
                                    sl651::DictMapItem mapItem;
                                    mapItem.key = item.get("key", "").asString();
                                    mapItem.label = item.get("label", "").asString();
                                    mapItem.value = item.get("value", "1").asString();

                                    if (item.isMember("dependsOn") && item["dependsOn"].isObject()) {
                                        const auto& dependsOnJson = item["dependsOn"];
                                        sl651::DictDependsOn dependsOn;
                                        dependsOn.op = sl651::parseDictDependencyOperator(dependsOnJson.get("operator", "AND").asString());

                                        if (dependsOnJson.isMember("conditions") && dependsOnJson["conditions"].isArray()) {
                                            for (const auto& cond : dependsOnJson["conditions"]) {
                                                sl651::DictDependency dependency;
                                                dependency.bitIndex = cond.get("bitIndex", "").asString();
                                                dependency.bitValue = cond.get("bitValue", "1").asString();
                                                dependsOn.conditions.push_back(dependency);
                                            }
                                        }
                                        mapItem.dependsOn = dependsOn;
                                    }
                                    dictConfig.items.push_back(mapItem);
                                }
                            }
                            def.dictConfig = dictConfig;
                        }
                        responseElements.push_back(def);
                    }
                    config.responseElementsByFunc[funcCode] = responseElements;
                }
            }

        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] Failed to parse elements from config: " << e.what();
        }
    }

    /**
     * @brief 获取指定功能码的要素定义
     */
    std::vector<sl651::ElementDef> getElements(const sl651::DeviceConfig& config, const std::string& funcCode) {
        auto it = config.elementsByFunc.find(funcCode);
        if (it != config.elementsByFunc.end()) {
            return it->second;
        }
        return {};
    }

public:
    /** 获取待应答指令数 */
    size_t pendingCommandCount() const {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        return pendingCommands_.size();
    }

    /** 协议处理统计 */
    struct ProtocolStats {
        int64_t framesProcessed;
        int64_t batchFlushes;
        int64_t batchFallbacks;
        size_t pendingCommands;
    };

    ProtocolStats getProtocolStats() const {
        return {
            totalFramesProcessed_.load(std::memory_order_relaxed),
            totalBatchFlushes_.load(std::memory_order_relaxed),
            totalBatchFallbacks_.load(std::memory_order_relaxed),
            pendingCommandCount()
        };
    }

    /** 是否有 Modbus Handler */
    bool hasModbusHandler() const { return modbusHandler_ != nullptr; }

    /** 获取 Modbus 统计（需先检查 hasModbusHandler） */
    modbus::ModbusHandler::ModbusStats getModbusStats() const {
        return modbusHandler_->getModbusStats();
    }

    /** 是否有 SL651 Parser */
    bool hasSl651Parser() const { return sl651Parser_ != nullptr; }

    /** 获取 SL651 统计（需先检查 hasSl651Parser） */
    sl651::SL651Parser::Sl651Stats getSl651Stats() const {
        return sl651Parser_->getSl651Stats();
    }

    /**
     * @brief 重新加载 Modbus 设备配置（设备/协议变更时调用）
     */
    void reloadModbusDevices() {
        if (modbusHandler_) {
            auto* ioLoop = getNextDrogonLoop();
            ioLoop->queueInLoop([this]() {
                drogon::async_run([this]() -> Task<> {
                    try {
                        co_await modbusHandler_->reloadDevices();
                    } catch (const std::exception& e) {
                        LOG_ERROR << "[ProtocolDispatcher] Failed to reload Modbus devices: "
                                  << e.what();
                    }
                });
            });
        }
    }

    /**
     * @brief 下发指令到设备（等待应答）
     * @param linkId 链路ID
     * @param deviceCode 设备编码
     * @param funcCode 功能码
     * @param elements 要素数据
     * @param userId 下发用户 ID
     * @param timeoutMs 超时时间（毫秒），默认 10 秒
     * @return 设备应答成功返回 true，超时或应答失败返回 false
     */
    Task<bool> sendCommand(int linkId, const std::string& deviceCode, const std::string& funcCode,
                           const Json::Value& elements, int userId, int deviceId = 0,
                           int timeoutMs = 10000) {
        int64_t downCommandId = 0;
        std::string validationError;
        std::string generalError;
        try {
            // 获取协议类型（从缓存同步读取）
            std::string protocol = DeviceCache::instance().getProtocolByLinkIdSync(linkId);
            if (protocol.empty()) {
                LOG_ERROR << "[ProtocolDispatcher] 链路未关联协议: linkId=" << linkId;
                co_return false;
            }

            if (protocol == Constants::PROTOCOL_SL651) {
                // ===== SL651 协议 =====
                auto configOpt = buildDeviceConfigFromCache(linkId, deviceCode);
                if (!configOpt) {
                    LOG_ERROR << "[ProtocolDispatcher] SL651 设备未找到: code=" << deviceCode;
                    co_return false;
                }

                // 构建下行报文
                std::string data = sl651Parser_->buildCommand(deviceCode, funcCode, elements, *configOpt);
                if (data.empty()) {
                    downCommandId = co_await saveDownCommand(linkId, *configOpt, funcCode,
                        elements, "", userId, "SEND_FAILED", "构建报文失败");
                    co_return false;
                }

                // 定向发送
                auto connOpt = DeviceConnectionCache::instance().getConnection(deviceCode);
                if (!connOpt) {
                    downCommandId = co_await saveDownCommand(linkId, *configOpt, funcCode,
                        elements, data, userId, "SEND_FAILED", "未找到设备连接映射");
                    co_return false;
                }

                bool sent = TcpLinkManager::instance().sendToClient(connOpt->linkId, connOpt->clientAddr, data);
                if (!sent) {
                    downCommandId = co_await saveDownCommand(linkId, *configOpt, funcCode,
                        elements, data, userId, "SEND_FAILED", "TCP发送失败");
                    co_return false;
                }

                LOG_DEBUG << "[ProtocolDispatcher] SL651 定向发送到 " << connOpt->clientAddr;
                downCommandId = co_await saveDownCommand(linkId, *configOpt, funcCode, elements, data, userId, "PENDING");

            } else if (protocol == Constants::PROTOCOL_MODBUS) {
                // ===== Modbus 协议 =====
                // 优先用 deviceId 定位，fallback 到 deviceCode 匹配
                if (deviceId == 0) {
                    auto devices = DeviceCache::instance().getDevicesByLinkIdSync(linkId);
                    for (const auto& dev : devices) {
                        if (!deviceCode.empty() && dev.deviceCode == deviceCode) {
                            deviceId = dev.id;
                            break;
                        }
                    }
                }
                if (deviceId == 0) {
                    LOG_ERROR << "[ProtocolDispatcher] Modbus 设备未找到: id=" << deviceId << " code=" << deviceCode;
                    co_return false;
                }

                // 用 deviceCode 或 deviceId 字符串作为 pendingCommands_ 的 key
                std::string pendingKey = deviceCode.empty() ? ("modbus:" + std::to_string(deviceId)) : deviceCode;

                // 构建并发送写帧
                auto writeResult = modbusHandler_->writeRegister(deviceId, pendingKey, elements);
                if (!writeResult.sent) {
                    downCommandId = co_await saveModbusDownCommand(deviceId, linkId, funcCode,
                        elements, "", userId, "SEND_FAILED", "写帧发送失败");
                    co_return false;
                }

                downCommandId = co_await saveModbusDownCommand(deviceId, linkId, funcCode,
                    elements, writeResult.frameHex, userId, "PENDING");

            } else {
                LOG_ERROR << "[ProtocolDispatcher] 协议不支持下发指令: " << protocol;
                co_return false;
            }

            // 确定 pendingCommands_ 的 key（SL651 用 deviceCode，Modbus 用 pendingKey）
            std::string cmdKey = deviceCode;
            if (protocol == Constants::PROTOCOL_MODBUS) {
                cmdKey = deviceCode.empty() ? ("modbus:" + std::to_string(deviceId)) : deviceCode;
            }

            // 创建待应答记录（拒绝并发：同一设备同时只允许一条待应答指令）
            auto waitState = std::make_shared<CommandWaitState>();
            waitState->loop = trantor::EventLoop::getEventLoopOfCurrentThread();

            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto existIt = pendingCommands_.find(cmdKey);
                if (existIt != pendingCommands_.end()) {
                    // 检查是否已过期
                    if (std::chrono::steady_clock::now() < existIt->second.expireTime) {
                        LOG_WARN << "[ProtocolDispatcher] 设备 " << cmdKey << " 存在未完成的指令，拒绝并发下发";
                        co_await updateDownCommandStatus(downCommandId, "SEND_FAILED", "设备有未完成的指令");
                        co_return false;
                    }
                    // 已过期，清理旧指令
                    pendingCommands_.erase(existIt);
                }
                PendingCommand cmd;
                cmd.deviceCode = cmdKey;
                cmd.funcCode = funcCode;
                cmd.waitState = waitState;
                cmd.expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
                cmd.downCommandId = downCommandId;
                cmd.userId = userId;
                pendingCommands_.emplace(cmdKey, std::move(cmd));
            }

            LOG_INFO << "[ProtocolDispatcher] 指令已发送，等待应答: linkId=" << linkId
                     << ", device=" << cmdKey << ", func=" << funcCode;

            // 零轮询等待：协程挂起，由应答回调或超时定时器精确唤醒
            bool success = co_await CommandAwaiter{waitState, timeoutMs};

            // 清理
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingCommands_.erase(cmdKey);
            }

            if (success) {
                LOG_INFO << "[ProtocolDispatcher] 设备应答成功: device=" << cmdKey << ", func=" << funcCode;
                co_await updateDownCommandStatus(downCommandId, "SUCCESS");
            } else {
                LOG_WARN << "[ProtocolDispatcher] 设备应答超时或失败: device=" << cmdKey << ", func=" << funcCode;
                co_await updateDownCommandStatus(downCommandId, "TIMEOUT", "设备应答超时");
            }

            co_return success;
        } catch (const ValidationException& e) {
            // MSVC 不支持 catch 中 co_await，保存异常信息后在 catch 外处理
            LOG_WARN << "[ProtocolDispatcher] 指令校验失败: " << e.what();
            validationError = e.what();
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] 下发指令异常: " << e.what();
            generalError = e.what();
        }

        // catch 外执行异步操作
        if (!validationError.empty()) {
            if (downCommandId > 0) {
                co_await updateDownCommandStatus(downCommandId, "SEND_FAILED", validationError);
            }
            throw ValidationException(validationError);
        }
        if (!generalError.empty()) {
            if (downCommandId > 0) {
                co_await updateDownCommandStatus(downCommandId, "SEND_FAILED", generalError);
            }
            co_return false;
        }
        co_return false;  // 不应到达此处
    }

    /**
     * @brief 通知指令应答（由协议解析器调用）
     * @param deviceCode 设备编码
     * @param funcCode 应答帧功能码
     * @param success 应答是否成功
     * @param responseId 应答报文记录 ID
     */
    void notifyCommandResponse(const std::string& deviceCode, const std::string& funcCode, bool success, int64_t responseId) {
        int64_t downCommandId = 0;
        std::shared_ptr<CommandWaitState> waitState;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingCommands_.find(deviceCode);
            if (it != pendingCommands_.end()) {
                // 检查功能码是否匹配（应答功能码应与发送的相同，或者是 E1/E2）
                if (it->second.funcCode == funcCode ||
                    funcCode == sl651::FuncCodes::ACK_OK ||
                    funcCode == sl651::FuncCodes::ACK_ERR) {
                    downCommandId = it->second.downCommandId;
                    waitState = it->second.waitState;
                    LOG_DEBUG << "[ProtocolDispatcher] 指令应答已处理: device=" << deviceCode
                              << ", sentFunc=" << it->second.funcCode
                              << ", respFunc=" << funcCode << ", success=" << success;
                    // 不 erase：让 sendCommand 的清理逻辑统一处理
                }
            }
        }

        // 锁外唤醒协程（notifyCommandResponse 从 TcpIoPool 线程调用，
        // 必须 queueInLoop 到协程所在的 Drogon EventLoop 唤醒）
        if (waitState) {
            if (!waitState->resolved.exchange(true, std::memory_order_acq_rel)) {
                waitState->result = success;
                waitState->loop->queueInLoop([waitState]() {
                    waitState->loop->invalidateTimer(waitState->timerId);
                    // 仅在协程真正挂起后才唤醒（防 double-resume）
                    if (waitState->suspended.load(std::memory_order_acquire)) {
                        waitState->handle.resume();
                    }
                });
            }
        }

        // 异步更新下行指令记录，关联应答报文 ID（确保在 Drogon IO 线程执行）
        if (downCommandId > 0 && responseId > 0) {
            auto* ioLoop = getNextDrogonLoop();
            ioLoop->queueInLoop([this, downCommandId, responseId]() {
                drogon::async_run([this, downCommandId, responseId]() -> Task<> {
                    try {
                        co_await updateDownCommandResponse(downCommandId, responseId);
                    } catch (const std::exception& e) {
                        LOG_WARN << "[ProtocolDispatcher] updateDownCommandResponse failed: "
                                 << "downCommandId=" << downCommandId
                                 << ", responseId=" << responseId
                                 << ", error=" << e.what();
                    }
                });
            });
        }
    }

private:
    std::unique_ptr<sl651::SL651Parser> sl651Parser_;
    std::unique_ptr<modbus::ModbusHandler> modbusHandler_;
    std::atomic<size_t> ioLoopIndex_{0};  // 轮询 IO 线程索引

    // 批量写入（仅在 batchLoop_ 线程访问，无需锁）
    trantor::EventLoop* batchLoop_ = nullptr;
    std::vector<ParsedFrameResult> pendingBatch_;
    trantor::TimerId batchTimerId_{0};
    bool batchTimerActive_ = false;

    // 待应答指令
    std::map<std::string, PendingCommand> pendingCommands_;
    mutable std::mutex pendingMutex_;

    // 处理统计计数器（原子操作，无锁）
    std::atomic<int64_t> totalFramesProcessed_{0};
    std::atomic<int64_t> totalBatchFlushes_{0};
    std::atomic<int64_t> totalBatchFallbacks_{0};

    // DeviceCache 自动重加载（防止启动时 DB 不可用导致缓存永久为空）
    std::atomic<bool> deviceCacheReloading_{false};

    /**
     * @brief 调度 DeviceCache 异步重加载
     *
     * 当 onDataReceived 检测到 DeviceCache 未加载时调用。
     * 使用 atomic 标志防止并发触发，失败后 10 秒冷却再允许重试。
     */
    void scheduleDeviceCacheReload() {
        bool expected = false;
        if (!deviceCacheReloading_.compare_exchange_strong(expected, true)) {
            return;  // 已有重加载任务在执行
        }

        LOG_WARN << "[ProtocolDispatcher] Scheduling DeviceCache reload...";
        auto* loop = getNextDrogonLoop();
        loop->queueInLoop([this]() {
            drogon::async_run([this]() -> Task<> {
                try {
                    co_await DeviceCache::instance().getDevices();
                    LOG_INFO << "[ProtocolDispatcher] DeviceCache reloaded successfully";
                    deviceCacheReloading_.store(false);
                } catch (const std::exception& e) {
                    LOG_ERROR << "[ProtocolDispatcher] DeviceCache reload failed: " << e.what()
                              << ", will retry in 10s";
                    // 失败后 10 秒冷却期，防止高频重试
                    batchLoop_->runAfter(10.0, [this]() {
                        deviceCacheReloading_.store(false);
                    });
                }
            });
        });
    }

    /**
     * @brief 保存下行指令到数据库
     * @param status 指令状态: PENDING/SUCCESS/TIMEOUT/SEND_FAILED
     * @param failReason 失败原因（可选）
     * @return 返回插入记录的 ID
     */
    Task<int64_t> saveDownCommand(int linkId, const sl651::DeviceConfig& config,
                                   const std::string& funcCode,
                                   const Json::Value& elements, const std::string& rawData, int userId,
                                   const std::string& status = "PENDING", const std::string& failReason = "") {
        // 构建 JSONB 数据（协议相关的数据构建仍在此处）
        Json::Value data;
        data["funcCode"] = funcCode;

        auto funcNameIt = config.funcNames.find(funcCode);
        if (funcNameIt != config.funcNames.end()) {
            data["funcName"] = funcNameIt->second;
        }
        data["direction"] = "DOWN";
        data["userId"] = userId;
        data["status"] = status;
        if (!failReason.empty()) {
            data["failReason"] = failReason;
        }

        // 原始报文（HEX 格式）
        Json::Value rawArr(Json::arrayValue);
        std::ostringstream hexOss;
        for (unsigned char c : rawData) {
            hexOss << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        rawArr.append(hexOss.str());
        data["raw"] = rawArr;

        // 下发的要素数据
        Json::Value dataObj(Json::objectValue);
        if (elements.isArray()) {
            auto funcElemIt = config.elementsByFunc.find(funcCode);
            if (funcElemIt != config.elementsByFunc.end()) {
                std::map<std::string, const sl651::ElementDef*> elementDefMap;
                for (const auto& elemDef : funcElemIt->second) {
                    elementDefMap[elemDef.id] = &elemDef;
                }

                for (const auto& elem : elements) {
                    std::string elementId = elem.get("elementId", "").asString();
                    std::string value = elem.get("value", "").asString();

                    auto defIt = elementDefMap.find(elementId);
                    if (defIt != elementDefMap.end()) {
                        const auto* elemDef = defIt->second;
                        std::string key = funcCode + "_" + elemDef->guideHex;

                        Json::Value e;
                        e["name"] = elemDef->name;
                        e["value"] = value;
                        e["elementId"] = elementId;
                        if (!elemDef->unit.empty()) {
                            e["unit"] = elemDef->unit;
                        }
                        dataObj[key] = e;
                    } else {
                        Json::Value e;
                        e["name"] = "";
                        e["value"] = value;
                        e["elementId"] = elementId;
                        dataObj[elementId] = e;
                    }
                }
            }
        }
        data["data"] = dataObj;

        // 获取当前 UTC 时间
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{now - dp};

        std::ostringstream timeOss;
        timeOss << std::setfill('0')
                << std::setw(4) << static_cast<int>(ymd.year()) << "-"
                << std::setw(2) << static_cast<unsigned>(ymd.month()) << "-"
                << std::setw(2) << static_cast<unsigned>(ymd.day()) << "T"
                << std::setw(2) << hms.hours().count() << ":"
                << std::setw(2) << hms.minutes().count() << ":"
                << std::setw(2) << hms.seconds().count() << "Z";

        // 委托给 CommandRepository 执行数据库操作
        co_return co_await CommandRepository::save(
            config.deviceId, linkId, Constants::PROTOCOL_SL651, data, timeOss.str()
        );
    }

    /**
     * @brief 保存 Modbus 下行指令到数据库
     */
    Task<int64_t> saveModbusDownCommand(int deviceId, int linkId, const std::string& funcCode,
                                         const Json::Value& elements, const std::string& rawHex, int userId,
                                         const std::string& status = "PENDING", const std::string& failReason = "") {
        Json::Value data;
        data["funcCode"] = funcCode;
        data["funcName"] = "写寄存器";
        data["direction"] = "DOWN";
        data["userId"] = userId;
        data["status"] = status;
        if (!failReason.empty()) {
            data["failReason"] = failReason;
        }

        // 原始报文
        Json::Value rawArr(Json::arrayValue);
        rawArr.append(rawHex);
        data["raw"] = rawArr;

        // 下发的要素数据
        Json::Value dataObj(Json::objectValue);
        if (elements.isArray()) {
            for (const auto& elem : elements) {
                std::string elementId = elem.get("elementId", "").asString();
                std::string value = elem.get("value", "").asString();
                Json::Value e;
                e["name"] = elementId;
                e["value"] = value;
                dataObj[elementId] = e;
            }
        }
        data["data"] = dataObj;

        // UTC 时间
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{now - dp};

        std::ostringstream timeOss;
        timeOss << std::setfill('0')
                << std::setw(4) << static_cast<int>(ymd.year()) << "-"
                << std::setw(2) << static_cast<unsigned>(ymd.month()) << "-"
                << std::setw(2) << static_cast<unsigned>(ymd.day()) << "T"
                << std::setw(2) << hms.hours().count() << ":"
                << std::setw(2) << hms.minutes().count() << ":"
                << std::setw(2) << hms.seconds().count() << "Z";

        co_return co_await CommandRepository::save(
            deviceId, linkId, Constants::PROTOCOL_MODBUS, data, timeOss.str()
        );
    }

    /**
     * @brief 更新下行指令记录，关联应答报文 ID
     */
    Task<void> updateDownCommandResponse(int64_t downCommandId, int64_t responseId) {
        co_await CommandRepository::linkResponse(downCommandId, responseId);
    }

    /**
     * @brief 更新下行指令状态
     */
    Task<void> updateDownCommandStatus(int64_t downCommandId, const std::string& status,
                                        const std::string& failReason = "") {
        co_await CommandRepository::updateStatus(downCommandId, status, failReason);
    }
};
