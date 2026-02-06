#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/domain/EventBus.hpp"
#include "common/utils/Constants.hpp"
#include "modules/device/domain/CommandRepository.hpp"
#include "modules/device/domain/Events.hpp"
#include "modules/protocol/domain/Events.hpp"
#include "sl651/SL651.hpp"
#include "modbus/Modbus.hpp"

/**
 * @brief 待应答的指令
 */
struct PendingCommand {
    std::string deviceCode;
    std::string funcCode;
    std::promise<bool> promise;
    std::chrono::steady_clock::time_point expireTime;
    int64_t downCommandId;  // 下行指令的数据库记录 ID
    int userId;             // 下发用户 ID
};

/**
 * @brief 协议分发服务
 * 负责将链路接收的数据路由到对应的协议解析器
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
     * @brief 初始化协议分发器
     * 设置 TcpLinkManager 的数据回调
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
                // Modbus 轮询管理（定时器已在 TcpIoPool 线程）
                if (modbusHandler_) {
                    modbusHandler_->onConnectionChanged(linkId, clientAddr, connected);
                }

                // 以下操作线程安全，可直接在 TcpIoPool 调用
                if (!connected) {
                    DeviceConnectionCache::instance().removeByClient(linkId, clientAddr);
                }
                ResourceVersion::instance().incrementVersion("link");
            }
        );

        // 创建 Modbus 处理器并初始化
        modbusHandler_ = std::make_unique<modbus::ModbusHandler>();
        modbusHandler_->setCommandResponseCallback(
            [this](const std::string& deviceCode, const std::string& funcCode, bool success, int64_t responseId) {
                notifyCommandResponse(deviceCode, funcCode, success, responseId);
            }
        );
        auto* modbusLoop = getNextDrogonLoop();
        modbusLoop->queueInLoop([this]() {
            drogon::async_run([this]() -> Task<> {
                co_await modbusHandler_->initialize();
            });
        });

        // 订阅设备/协议配置事件，触发 Modbus 重载
        registerEventSubscriptions();

        LOG_DEBUG << "[ProtocolDispatcher] Initialized";
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
        // === 全部在 TcpIoPool 线程执行，零跨线程解析 ===
        std::vector<uint8_t> bytes(data.begin(), data.end());

        LOG_DEBUG << "[Link " << linkId << "] RX " << bytes.size() << "B from " << clientAddr
                  << " | " << modbus::ModbusUtils::toHexString(bytes);

        if (!DeviceCache::instance().isLoaded()) {
            LOG_WARN << "[ProtocolDispatcher] DeviceCache not loaded, dropping data";
            return;
        }

        // === 心跳包 / 注册包预处理 ===
        auto devices = DeviceCache::instance().getDevicesByLinkIdSync(linkId);

        // 1. 注册包匹配（ON 模式：智能判断完整匹配或前缀匹配）
        for (const auto* dev : devices) {
            if (dev->registrationMode != "OFF" && !dev->registrationBytes.empty()) {
                if (bytes == dev->registrationBytes) {
                    // 完整匹配：整包就是注册包
                    DeviceConnectionCache::instance().registerConnection(dev->deviceCode, linkId, clientAddr);
                    LOG_INFO << "[Link " << linkId << "] Registration matched device "
                             << dev->deviceCode << " from " << clientAddr;
                    return;  // 注册包不传给协议解析器
                }
                if (bytes.size() > dev->registrationBytes.size() &&
                    std::equal(dev->registrationBytes.begin(), dev->registrationBytes.end(), bytes.begin())) {
                    // 前缀匹配：数据以注册包开头
                    DeviceConnectionCache::instance().registerConnection(dev->deviceCode, linkId, clientAddr);
                    LOG_INFO << "[Link " << linkId << "] Registration prefix matched device "
                             << dev->deviceCode << " from " << clientAddr;
                    // 剥离前缀，继续协议解析
                    bytes.erase(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(dev->registrationBytes.size()));
                    break;
                }
            }
        }

        // 2. 心跳包匹配
        for (const auto* dev : devices) {
            if (dev->heartbeatMode != "OFF" && !dev->heartbeatBytes.empty()) {
                if (bytes == dev->heartbeatBytes) {
                    DeviceConnectionCache::instance().registerConnection(dev->deviceCode, linkId, clientAddr);
                    LOG_DEBUG << "[Link " << linkId << "] Heartbeat matched device "
                              << dev->deviceCode << " from " << clientAddr;
                    return;  // 心跳包不传给协议解析器
                }
            }
        }

        // === 注册包拦截：配置了注册包的链路，未注册的连接不允许通过 ===
        bool requiresRegistration = std::any_of(devices.begin(), devices.end(),
            [](const auto* dev) { return dev->registrationMode != "OFF" && !dev->registrationBytes.empty(); });
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

        // === 解析完成，投递到 Drogon IO 做 DB 写入 ===
        if (!results.empty()) {
            auto* ioLoop = getNextDrogonLoop();
            ioLoop->queueInLoop([this, results = std::move(results)]() {
                drogon::async_run([this, results = std::move(results)]() -> Task<> {
                    for (const auto& r : results) {
                        co_await saveParsedResult(r);
                    }
                });
            });
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

    /**
     * @brief 保存解析结果到数据库（Drogon IO 线程执行）
     */
    Task<void> saveParsedResult(const ParsedFrameResult& result) {
        try {
            // 委托给 CommandRepository 执行数据库操作
            int64_t recordId = co_await CommandRepository::save(
                result.deviceId, result.linkId, result.protocol, result.data, result.reportTime
            );

            // 更新实时数据缓存
            RealtimeDataCache::instance().update(result.deviceId, result.funcCode, result.data, result.reportTime);

            // 更新资源版本号
            ResourceVersion::instance().incrementVersion("device");

            // 处理指令应答通知（SL651 上行帧可能是指令应答）
            if (result.commandResponse) {
                notifyCommandResponse(
                    result.commandResponse->deviceCode,
                    result.commandResponse->funcCode,
                    result.commandResponse->success,
                    recordId
                );
            }

            LOG_TRACE << "[ProtocolDispatcher] Saved: device=" << result.deviceId
                      << " func=" << result.funcCode;
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] saveParsedResult failed: " << e.what();
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
                    for (const auto* dev : devices) {
                        if (!deviceCode.empty() && dev->deviceCode == deviceCode) {
                            deviceId = dev->id;
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

            // 创建待应答记录
            std::promise<bool> responsePromise;
            std::future<bool> responseFuture = responsePromise.get_future();

            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingCommands_.erase(cmdKey);
                PendingCommand cmd;
                cmd.deviceCode = cmdKey;
                cmd.funcCode = funcCode;
                cmd.promise = std::move(responsePromise);
                cmd.expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
                cmd.downCommandId = downCommandId;
                cmd.userId = userId;
                pendingCommands_.emplace(cmdKey, std::move(cmd));
            }

            LOG_INFO << "[ProtocolDispatcher] 指令已发送，等待应答: linkId=" << linkId
                     << ", device=" << cmdKey << ", func=" << funcCode;

            // 等待应答（使用协程轮询方式，每 100ms 检查一次）
            constexpr int pollIntervalMs = 100;
            int elapsedMs = 0;
            bool success = false;

            while (elapsedMs < timeoutMs) {
                // 检查 future 是否就绪
                auto status = responseFuture.wait_for(std::chrono::milliseconds(0));
                if (status == std::future_status::ready) {
                    success = responseFuture.get();
                    break;
                }

                // 协程休眠，让出执行权
                co_await drogon::sleepCoro(
                    trantor::EventLoop::getEventLoopOfCurrentThread(),
                    std::chrono::milliseconds(pollIntervalMs)
                );
                elapsedMs += pollIntervalMs;
            }

            // 清理（如果超时，promise 未被消费）
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
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] 下发指令异常: " << e.what();
            co_return false;
        }
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

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingCommands_.find(deviceCode);
            if (it != pendingCommands_.end()) {
                // 检查功能码是否匹配（应答功能码应与发送的相同，或者是 E1/E2）
                if (it->second.funcCode == funcCode ||
                    funcCode == sl651::FuncCodes::ACK_OK ||
                    funcCode == sl651::FuncCodes::ACK_ERR) {
                    try {
                        downCommandId = it->second.downCommandId;
                        it->second.promise.set_value(success);
                        LOG_DEBUG << "[ProtocolDispatcher] 指令应答已处理: device=" << deviceCode
                                  << ", sentFunc=" << it->second.funcCode
                                  << ", respFunc=" << funcCode << ", success=" << success;
                    } catch (const std::future_error&) {
                        // promise 已被设置，忽略
                    }
                    pendingCommands_.erase(it);
                }
            }
        }

        // 异步更新下行指令记录，关联应答报文 ID（确保在 Drogon IO 线程执行）
        if (downCommandId > 0 && responseId > 0) {
            auto* ioLoop = getNextDrogonLoop();
            ioLoop->queueInLoop([this, downCommandId, responseId]() {
                drogon::async_run([this, downCommandId, responseId]() -> Task<> {
                    co_await updateDownCommandResponse(downCommandId, responseId);
                });
            });
        }
    }

private:
    std::unique_ptr<sl651::SL651Parser> sl651Parser_;
    std::unique_ptr<modbus::ModbusHandler> modbusHandler_;
    std::atomic<size_t> ioLoopIndex_{0};  // 轮询 IO 线程索引

    // 待应答指令
    std::map<std::string, PendingCommand> pendingCommands_;
    std::mutex pendingMutex_;

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
