#pragma once

#include <drogon/drogon.h>
#include <map>
#include <mutex>
#include <functional>
#include <future>
#include <chrono>
#include "common/database/DatabaseService.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "sl651/SL651.hpp"

using namespace drogon;

/**
 * @brief 待应答的指令
 */
struct PendingCommand {
    std::string deviceCode;
    std::string funcCode;
    std::promise<bool> promise;
    std::chrono::steady_clock::time_point expireTime;
};

/**
 * @brief 协议分发服务
 * 负责将链路接收的数据路由到对应的协议解析器
 */
class ProtocolDispatcher {
public:
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
            [this](const std::string& deviceCode, const std::string& funcCode, bool success) {
                notifyCommandResponse(deviceCode, funcCode, success);
            }
        );

        // 设置 TcpLinkManager 的数据回调（带客户端地址，用于建立设备连接映射）
        TcpLinkManager::instance().setDataCallbackWithClient(
            [this](int linkId, const std::string& clientAddr, const std::string& data) {
                onDataReceived(linkId, clientAddr, data);
            }
        );

        // 设置连接状态回调，用于清理断开连接的设备映射
        TcpLinkManager::instance().setConnectionCallback(
            [](int linkId, const std::string& clientAddr, bool connected) {
                if (!connected) {
                    DeviceConnectionCache::instance().removeByClient(linkId, clientAddr);
                }
            }
        );

        LOG_DEBUG << "[ProtocolDispatcher] Initialized";
    }

private:
    ProtocolDispatcher() = default;
    ~ProtocolDispatcher() = default;
    ProtocolDispatcher(const ProtocolDispatcher&) = delete;
    ProtocolDispatcher& operator=(const ProtocolDispatcher&) = delete;

    /**
     * @brief 数据接收回调
     * @param linkId 链路ID
     * @param clientAddr 客户端地址（用于建立设备连接映射）
     * @param data 接收到的数据
     */
    void onDataReceived(int linkId, const std::string& clientAddr, const std::string& data) {

        // 将数据转换为字节数组
        std::vector<uint8_t> bytes(data.begin(), data.end());

        // 将处理转移到 Drogon 的 IO 工作线程，因为数据库操作依赖 IOThreadStorage
        // TcpLinkManager 使用独立线程池，需要确保在 Drogon IO 线程上执行数据库操作
        auto* ioLoop = getNextDrogonLoop();
        ioLoop->queueInLoop([this, linkId, clientAddr, bytes]() {
            // 使用协程处理，避免在 EventLoop 中同步调用数据库导致死锁
            async_run([this, linkId, clientAddr, bytes]() -> Task<> {
                // 获取链路关联的协议类型
                std::string protocol = co_await getLinkProtocolAsync(linkId);

                if (protocol.empty()) {
                    co_return;
                }

                // 根据协议类型分发（传递 clientAddr 用于建立设备连接映射）
                if (protocol == "SL651") {
                    co_await sl651Parser_->handleData(linkId, clientAddr, bytes);
                } else if (protocol == "MODBUS") {
                    LOG_WARN << "[ProtocolDispatcher] Modbus 协议暂未实现";
                } else {
                    LOG_WARN << "[ProtocolDispatcher] 未知协议: " << protocol;
                }
            });
        });
    }

    /**
     * @brief 轮询获取下一个 Drogon IO 线程
     */
    trantor::EventLoop* getNextDrogonLoop() {
        size_t threadNum = app().getThreadNum();
        if (threadNum == 0) {
            return app().getLoop();  // 回退到主循环
        }
        size_t idx = ioLoopIndex_.fetch_add(1, std::memory_order_relaxed) % threadNum;
        return app().getIOLoop(idx);
    }

    /**
     * @brief 获取链路关联的协议类型（异步版本）
     */
    Task<std::string> getLinkProtocolAsync(int linkId) {
        // 检查缓存
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto it = linkProtocolCache_.find(linkId);
            if (it != linkProtocolCache_.end()) {
                co_return it->second;
            }
        }

        // 从数据库查询
        try {
            auto dbClient = AppDbConfig::useFast()
                ? app().getFastDbClient("default")
                : app().getDbClient("default");

            auto result = co_await dbClient->execSqlCoro(R"(
                SELECT pc.protocol
                FROM device d
                JOIN protocol_config pc ON d.protocol_config_id = pc.id
                WHERE d.link_id = $1 AND d.deleted_at IS NULL AND pc.deleted_at IS NULL
                LIMIT 1
            )", std::to_string(linkId));

            if (!result.empty()) {
                std::string protocol = result[0]["protocol"].as<std::string>();

                // 缓存结果
                std::lock_guard<std::mutex> lock(cacheMutex_);
                linkProtocolCache_[linkId] = protocol;

                co_return protocol;
            } else {
                LOG_WARN << "[ProtocolDispatcher] linkId=" << linkId << " 未关联协议配置";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolDispatcher] 查询协议失败: " << e.what();
        }

        co_return "";
    }

    /**
     * @brief 获取设备配置（协程版本）
     */
    Task<std::optional<sl651::DeviceConfig>> getDeviceConfigAsync(int linkId, const std::string& remoteCode) {
        try {
            auto dbClient = AppDbConfig::useFast()
                ? app().getFastDbClient("default")
                : app().getDbClient("default");

            auto result = co_await dbClient->execSqlCoro(R"(
                SELECT d.id, d.name, d.device_code, d.protocol_config_id, d.link_id,
                       pc.config
                FROM device d
                JOIN protocol_config pc ON d.protocol_config_id = pc.id
                WHERE d.link_id = $1 AND d.device_code = $2
                  AND d.deleted_at IS NULL AND pc.deleted_at IS NULL
            )", std::to_string(linkId), remoteCode);

            if (result.empty()) {
                co_return std::nullopt;
            }

            sl651::DeviceConfig config;
            config.deviceId = result[0]["id"].as<int>();
            config.deviceName = result[0]["name"].as<std::string>();
            config.deviceCode = result[0]["device_code"].as<std::string>();
            config.protocolConfigId = result[0]["protocol_config_id"].as<int>();
            config.linkId = result[0]["link_id"].as<int>();

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
     * @brief 清除链路协议缓存
     */
    void clearLinkCache(int linkId) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        linkProtocolCache_.erase(linkId);
    }

    /**
     * @brief 清除所有缓存
     */
    void clearAllCache() {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        linkProtocolCache_.clear();
    }

    /**
     * @brief 下发指令到设备（等待应答）
     * @param linkId 链路ID
     * @param deviceCode 设备编码
     * @param funcCode 功能码
     * @param elements 要素数据
     * @param timeoutMs 超时时间（毫秒），默认 10 秒
     * @return 设备应答成功返回 true，超时或应答失败返回 false
     */
    Task<bool> sendCommand(int linkId, const std::string& deviceCode, const std::string& funcCode,
                           const Json::Value& elements, int timeoutMs = 10000) {
        try {
            // 获取设备配置
            auto configOpt = co_await getDeviceConfigAsync(linkId, deviceCode);
            if (!configOpt) {
                LOG_ERROR << "[ProtocolDispatcher] 设备未找到: linkId=" << linkId << ", code=" << deviceCode;
                co_return false;
            }

            // 获取协议类型
            std::string protocol = co_await getLinkProtocolAsync(linkId);
            if (protocol.empty()) {
                LOG_ERROR << "[ProtocolDispatcher] 链路未关联协议: linkId=" << linkId;
                co_return false;
            }

            // 根据协议类型构建报文
            std::string data;
            if (protocol == "SL651") {
                // 构建 SL651 下行报文
                data = sl651Parser_->buildCommand(deviceCode, funcCode, elements, *configOpt);
                if (data.empty()) {
                    LOG_ERROR << "[ProtocolDispatcher] 构建 SL651 报文失败";
                    co_return false;
                }
            } else {
                LOG_ERROR << "[ProtocolDispatcher] 协议不支持下发指令: " << protocol;
                co_return false;
            }

            // 创建待应答记录（以 deviceCode 为 key，每个设备同时只能有一个待应答指令）
            std::promise<bool> responsePromise;
            std::future<bool> responseFuture = responsePromise.get_future();

            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                // 移除旧的同设备记录（如果有）
                pendingCommands_.erase(deviceCode);
                // 添加新记录
                PendingCommand cmd;
                cmd.deviceCode = deviceCode;
                cmd.funcCode = funcCode;
                cmd.promise = std::move(responsePromise);
                cmd.expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
                pendingCommands_.emplace(deviceCode, std::move(cmd));
            }

            // 获取设备连接映射，尝试定向发送
            bool sent = false;
            auto connOpt = DeviceConnectionCache::instance().getConnection(deviceCode);
            if (connOpt) {
                // 定向发送到设备对应的客户端连接
                sent = TcpLinkManager::instance().sendToClient(connOpt->linkId, connOpt->clientAddr, data);
                if (sent) {
                    LOG_DEBUG << "[ProtocolDispatcher] 定向发送到 " << connOpt->clientAddr;
                }
            }

            // 如果定向发送失败或无映射，回退到广播模式
            if (!sent) {
                if (connOpt) {
                    LOG_WARN << "[ProtocolDispatcher] 定向发送失败，回退到广播模式: linkId=" << linkId;
                } else {
                    LOG_WARN << "[ProtocolDispatcher] 未找到设备连接映射，使用广播模式: device=" << deviceCode;
                }
                sent = TcpLinkManager::instance().sendData(linkId, data);
            }

            if (!sent) {
                LOG_ERROR << "[ProtocolDispatcher] 发送失败: linkId=" << linkId;
                // 移除待应答记录
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingCommands_.erase(deviceCode);
                co_return false;
            }

            LOG_INFO << "[ProtocolDispatcher] 指令已发送，等待应答: linkId=" << linkId
                     << ", device=" << deviceCode << ", func=" << funcCode;

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
                pendingCommands_.erase(deviceCode);
            }

            if (success) {
                LOG_INFO << "[ProtocolDispatcher] 设备应答成功: device=" << deviceCode << ", func=" << funcCode;
            } else {
                LOG_WARN << "[ProtocolDispatcher] 设备应答超时或失败: device=" << deviceCode << ", func=" << funcCode;
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
     */
    void notifyCommandResponse(const std::string& deviceCode, const std::string& funcCode, bool success) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto it = pendingCommands_.find(deviceCode);
        if (it != pendingCommands_.end()) {
            // 检查功能码是否匹配（应答功能码应与发送的相同，或者是 E1/E2）
            if (it->second.funcCode == funcCode ||
                funcCode == sl651::FuncCodes::ACK_OK ||
                funcCode == sl651::FuncCodes::ACK_ERR) {
                try {
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

private:
    std::unique_ptr<sl651::SL651Parser> sl651Parser_;
    std::map<int, std::string> linkProtocolCache_;
    std::mutex cacheMutex_;
    std::atomic<size_t> ioLoopIndex_{0};  // 轮询 IO 线程索引

    // 待应答指令
    std::map<std::string, PendingCommand> pendingCommands_;
    std::mutex pendingMutex_;
};
