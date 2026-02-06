#pragma once

#include "Modbus.Types.hpp"
#include "Modbus.Utils.hpp"
#include "common/protocol/ParsedResult.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/utils/Constants.hpp"

namespace modbus {

/** Modbus 虚拟功能码（数据存储用） */
inline constexpr const char* FUNC_READ = "MODBUS_READ";

/** Modbus 合并间距 */
inline constexpr int MERGE_GAP = 10;

/** Modbus 默认轮询间隔（秒） */
inline constexpr int DEFAULT_READ_INTERVAL = 1;

/** Modbus 请求超时（毫秒） */
inline constexpr int RESPONSE_TIMEOUT_MS = 5000;

/**
 * @brief Modbus 协议处理器
 *
 * 职责：
 * 1. 从 DeviceCache 加载 Modbus 设备配置
 * 2. 按 readInterval 定时发送寄存器读取请求
 * 3. 接收并解析 Modbus 响应
 * 4. 将解析的数据存入数据库和缓存
 *
 * 轮询由连接事件驱动：
 * - 连接建立 → 启动该链路上设备的轮询
 * - 连接断开（TCP Client） → 停止该链路上设备的轮询
 */
class ModbusHandler {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    // ==================== 生命周期 ====================

    /**
     * @brief 初始化：加载 Modbus 设备配置
     * 轮询不在此处启动，由 onConnectionChanged 事件驱动
     */
    Task<void> initialize() {
        LOG_INFO << "[Modbus] Initializing handler...";
        co_await loadDeviceContexts();

        if (deviceContexts_.empty()) {
            LOG_INFO << "[Modbus] No Modbus devices found, handler idle";
            co_return;
        }

        LOG_INFO << "[Modbus] Loaded " << deviceContexts_.size()
                 << " device(s), waiting for link connections to start polling";

        // 尝试为已经运行的链路启动轮询（如果链路比 Handler 先启动）
        startPollingForRunningLinks();
    }

    /**
     * @brief 停止所有轮询定时器
     */
    void shutdown() {
        stopAllPolling();
        LOG_INFO << "[Modbus] Handler shutdown";
    }

    /**
     * @brief 重新加载设备配置（设备变更时调用）
     */
    Task<void> reloadDevices() {
        LOG_INFO << "[Modbus] Reloading devices...";
        stopAllPolling();
        // 强制刷新 DeviceCache，避免竞态返回旧数据
        co_await DeviceCache::instance().refreshCache();
        co_await loadDeviceContexts();
        startPollingForRunningLinks();
        LOG_INFO << "[Modbus] Reloaded, " << deviceContexts_.size() << " device(s)";
    }

    // ==================== 连接事件（由 ProtocolDispatcher 调用） ====================

    /**
     * @brief 链路连接状态变化回调
     * @param linkId 链路 ID
     * @param clientAddr 客户端地址
     * @param connected 连接/断开
     */
    void onConnectionChanged(int linkId, const std::string& clientAddr, bool connected) {
        if (connected) {
            LOG_INFO << "[Modbus] Link " << linkId << " connected, starting poll";
            startPollingForLink(linkId);
        } else {
            bool isTcpClient = false;
            {
                std::lock_guard<std::mutex> lock(contextMutex_);
                for (const auto& [id, ctx] : deviceContexts_) {
                    if (ctx.linkId == linkId && ctx.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
                        isTcpClient = true;
                        break;
                    }
                }
            }

            if (isTcpClient) {
                LOG_INFO << "[Modbus] Link " << linkId << " disconnected (Client)";
                stopPollingForLink(linkId);
                markDevicesOfflineForLink(linkId);
            } else {
                LOG_INFO << "[Modbus] Link " << linkId << " client " << clientAddr << " disconnected";
                markDeviceOfflineByClient(linkId, clientAddr);
            }
        }
    }

    // ==================== 数据接收 ====================

    /**
     * @brief 处理接收到的数据（被 ProtocolDispatcher 调用）
     */
    Task<void> handleData(int linkId, const std::string& clientAddr, const std::vector<uint8_t>& data) {
        FrameMode mode = getLinkFrameMode(linkId);

        std::vector<ModbusResponse> parsedFrames;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            auto& linkBuffer = buffers_[linkId];
            linkBuffer.insert(linkBuffer.end(), data.begin(), data.end());

            while (!linkBuffer.empty()) {
                ModbusResponse response;
                size_t consumed = ModbusUtils::parseResponse(mode, linkBuffer, response);
                if (consumed == 0) break;
                linkBuffer.erase(linkBuffer.begin(), linkBuffer.begin() + consumed);
                parsedFrames.push_back(std::move(response));
            }
        }

        for (auto& response : parsedFrames) {
            if (!clientAddr.empty()) {
                std::string connKey = "modbus_" + std::to_string(response.slaveId);
                DeviceConnectionCache::instance().registerConnection(connKey, linkId, clientAddr);
            }

            if (response.isException) {
                LOG_WARN << "[Modbus] Exception: slave=" << static_cast<int>(response.slaveId)
                         << " FC=" << static_cast<int>(response.functionCode)
                         << " code=" << static_cast<int>(response.exceptionCode);
            } else {
                co_await processResponse(linkId, clientAddr, response);
            }
        }
    }

    // ==================== 同步解析接口（TcpIoPool 线程使用） ====================

    /**
     * @brief 同步处理接收到的数据（TcpIoPool 线程调用）
     * @return 解析结果列表，由调用方投递到 Drogon IO 线程保存
     */
    std::vector<ParsedFrameResult> parseDataSync(int linkId, const std::string& clientAddr,
                                                  const std::vector<uint8_t>& data) {
        std::vector<ParsedFrameResult> results;

        FrameMode mode = getLinkFrameMode(linkId);

        std::vector<ModbusResponse> parsedFrames;
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            auto& linkBuffer = buffers_[linkId];
            linkBuffer.insert(linkBuffer.end(), data.begin(), data.end());

            while (!linkBuffer.empty()) {
                ModbusResponse response;
                size_t consumed = ModbusUtils::parseResponse(mode, linkBuffer, response);
                if (consumed == 0) break;
                linkBuffer.erase(linkBuffer.begin(), linkBuffer.begin() + consumed);
                parsedFrames.push_back(std::move(response));
            }
        }

        for (auto& response : parsedFrames) {
            if (!clientAddr.empty()) {
                std::string connKey = "modbus_" + std::to_string(response.slaveId);
                DeviceConnectionCache::instance().registerConnection(connKey, linkId, clientAddr);
            }

            if (response.isException) {
                LOG_WARN << "[Modbus] Exception: slave=" << static_cast<int>(response.slaveId)
                         << " FC=" << static_cast<int>(response.functionCode)
                         << " code=" << static_cast<int>(response.exceptionCode);
            } else {
                auto result = processResponseSync(linkId, clientAddr, response);
                if (result) {
                    results.push_back(std::move(*result));
                }
            }
        }

        return results;
    }

private:
    // ==================== 同步响应处理 ====================

    /**
     * @brief 同步处理 Modbus 响应，返回 ParsedFrameResult（不涉及 DB 操作）
     */
    std::optional<ParsedFrameResult> processResponseSync(int linkId, const std::string& /*clientAddr*/,
                                                          const ModbusResponse& response) {
        std::string pendingKey = std::to_string(linkId) + ":"
            + std::to_string(response.slaveId) + ":"
            + std::to_string(response.functionCode);

        PendingRequest pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingRequests_.find(pendingKey);
            if (it == pendingRequests_.end()) {
                LOG_WARN << "[Modbus] No pending request for key=" << pendingKey
                         << " (unexpected response or already timed out)";
                return std::nullopt;
            }
            pending = std::move(it->second);
            pendingRequests_.erase(it);
        }

        DeviceContext ctx;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            auto it = deviceContexts_.find(pending.deviceId);
            if (it == deviceContexts_.end()) return std::nullopt;
            ctx = it->second;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pending.sentTime).count();

        auto registerValues = extractRegisterValues(ctx, pending.group, response.data);

        if (registerValues.empty()) {
            LOG_WARN << "[Modbus] No values: " << ctx.deviceName << " latency=" << elapsed << "ms";
            return std::nullopt;
        }

        LOG_DEBUG << "[Modbus] RX " << ctx.deviceName
                  << " slave=" << static_cast<int>(ctx.slaveId)
                  << " " << registerTypeToString(pending.group.registerType)
                  << " addr=" << pending.group.startAddress
                  << " qty=" << pending.group.totalQuantity
                  << " byteOrder=" << byteOrderToString(ctx.byteOrder)
                  << " | " << registerValues.size() << " values"
                  << " latency=" << elapsed << "ms"
                  << " data=[" << ModbusUtils::toHexString(response.data) << "]"
                  << " → " << formatRegisterValues(registerValues);

        // 构建 ParsedFrameResult（从 saveData 提取 JSON 构建逻辑）
        ParsedFrameResult result;
        result.deviceId = ctx.deviceId;
        result.linkId = ctx.linkId;
        result.protocol = Constants::PROTOCOL_MODBUS;
        result.funcCode = FUNC_READ;

        Json::Value jsonData;
        jsonData["funcCode"] = FUNC_READ;
        jsonData["direction"] = "UP";

        Json::Value dataObj(Json::objectValue);
        for (const auto& [key, elem] : registerValues) {
            dataObj[key] = elem;
        }
        jsonData["data"] = dataObj;

        result.data = jsonData;

        // 生成 UTC 时间
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
        result.reportTime = timeOss.str();

        return result;
    }

    // ==================== 原有异步方法（仍保留供 handleData 使用） ====================

private:
    // ==================== 设备上下文管理 ====================

    /**
     * @brief 从 DeviceCache 加载所有 Modbus 设备
     */
    Task<void> loadDeviceContexts() {
        auto cachedDevices = co_await DeviceCache::instance().getDevices();

        std::lock_guard<std::mutex> lock(contextMutex_);
        deviceContexts_.clear();

        int total = 0, loaded = 0, skipped = 0;
        for (const auto& device : cachedDevices) {
            if (device.protocolType != Constants::PROTOCOL_MODBUS) continue;
            total++;

            if (device.status != Constants::USER_STATUS_ENABLED) {
                LOG_DEBUG << "[Modbus] Skip disabled: " << device.name;
                skipped++;
                continue;
            }

            auto ctx = buildDeviceContext(device);
            if (ctx.readGroups.empty()) {
                LOG_WARN << "[Modbus] Device " << device.name << " (id=" << device.id
                         << ") has no registers configured, skipping";
                skipped++;
                continue;
            }

            LOG_DEBUG << "[Modbus] Loaded: " << ctx.deviceName
                      << " slave=" << static_cast<int>(ctx.slaveId)
                      << " regs=" << ctx.registers.size()
                      << " interval=" << ctx.readInterval << "s"
                      << " byteOrder=" << byteOrderToString(ctx.byteOrder);

            deviceContexts_[device.id] = std::move(ctx);
            loaded++;
        }

        LOG_INFO << "[Modbus] Device loading complete: total=" << total
                 << " loaded=" << loaded << " skipped=" << skipped;
    }

    /**
     * @brief 构建单个设备的轮询上下文
     */
    DeviceContext buildDeviceContext(const DeviceCache::CachedDevice& device) {
        DeviceContext ctx;
        ctx.deviceId = device.id;
        ctx.deviceName = device.name;
        ctx.linkId = device.linkId;
        ctx.linkMode = device.linkMode;
        ctx.slaveId = device.slaveId;

        // 帧模式：TCP Server 固定 RTU（串口透传），TCP Client 可选 TCP/RTU
        if (device.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
            ctx.frameMode = parseFrameMode(device.modbusMode);
        } else {
            ctx.frameMode = FrameMode::RTU;
        }

        // 从协议配置解析
        const auto& config = device.protocolConfig;
        ctx.byteOrder = parseByteOrder(config.get("byteOrder", "BIG_ENDIAN").asString());
        ctx.readInterval = config.get("readInterval", DEFAULT_READ_INTERVAL).asInt();
        if (ctx.readInterval < 1) ctx.readInterval = 1;

        // 解析寄存器列表
        if (config.isMember("registers") && config["registers"].isArray()) {
            for (const auto& reg : config["registers"]) {
                RegisterDef def;
                def.id = reg.get("id", "").asString();
                def.name = reg.get("name", "").asString();
                def.registerType = parseRegisterType(reg.get("registerType", "HOLDING_REGISTER").asString());
                def.address = static_cast<uint16_t>(reg.get("address", 0).asUInt());
                def.dataType = parseDataType(reg.get("dataType", "UINT16").asString());
                def.quantity = static_cast<uint16_t>(reg.get("quantity", 1).asUInt());
                def.unit = reg.get("unit", "").asString();
                def.remark = reg.get("remark", "").asString();
                if (reg.isMember("dictConfig") && reg["dictConfig"].isObject()) {
                    def.dictConfig = reg["dictConfig"];
                }
                ctx.registers.push_back(std::move(def));
            }
        }

        // 合并寄存器
        ctx.readGroups = ModbusUtils::mergeRegisters(ctx.registers, MERGE_GAP);

        return ctx;
    }

    // ==================== 轮询管理 ====================

    /**
     * @brief 为已运行的链路启动轮询（初始化/重载时调用）
     */
    void startPollingForRunningLinks() {
        std::lock_guard<std::mutex> ctxLock(contextMutex_);
        std::lock_guard<std::mutex> timerLock(timerMutex_);

        std::set<int> checkedLinks;
        for (const auto& [deviceId, ctx] : deviceContexts_) {
            if (checkedLinks.count(ctx.linkId)) continue;
            checkedLinks.insert(ctx.linkId);

            if (TcpLinkManager::instance().isRunning(ctx.linkId)) {
                LOG_INFO << "[Modbus] Link " << ctx.linkId << " already running, starting polling";
                startPollTimersForLink(ctx.linkId);
            }
        }
    }

    /**
     * @brief 为指定链路启动轮询定时器（连接建立时调用）
     */
    void startPollingForLink(int linkId) {
        std::lock_guard<std::mutex> ctxLock(contextMutex_);
        std::lock_guard<std::mutex> timerLock(timerMutex_);
        startPollTimersForLink(linkId);
    }

    /**
     * @brief 内部方法：为指定链路创建轮询定时器（需持有两把锁）
     */
    void startPollTimersForLink(int linkId) {
        auto* loop = TcpLinkManager::instance().getLinkLoop(linkId);
        if (!loop) return;

        for (const auto& [deviceId, ctx] : deviceContexts_) {
            if (ctx.linkId != linkId) continue;
            if (pollTimers_.count(deviceId)) {
                LOG_DEBUG << "[Modbus] Device " << ctx.deviceName << " already polling, skip";
                continue;
            }

            double interval = static_cast<double>(ctx.readInterval);
            int devId = deviceId;

            auto timerId = loop->runEvery(interval, [this, devId]() {
                pollDevice(devId);
            });

            pollTimers_[deviceId] = {timerId, loop};

            LOG_INFO << "[Modbus] Poll start: " << ctx.deviceName
                     << " every " << ctx.readInterval << "s"
                     << " groups=" << ctx.readGroups.size()
                     << " byteOrder=" << byteOrderToString(ctx.byteOrder);
        }
    }

    /**
     * @brief 停止指定链路上设备的轮询定时器（连接断开时调用）
     */
    void stopPollingForLink(int linkId) {
        std::lock_guard<std::mutex> ctxLock(contextMutex_);
        std::lock_guard<std::mutex> timerLock(timerMutex_);

        std::vector<int> toRemove;
        for (const auto& [deviceId, ctx] : deviceContexts_) {
            if (ctx.linkId != linkId) continue;

            auto it = pollTimers_.find(deviceId);
            if (it != pollTimers_.end()) {
                if (it->second.loop) it->second.loop->invalidateTimer(it->second.timerId);
                toRemove.push_back(deviceId);

                LOG_INFO << "[Modbus] Poll stop: " << ctx.deviceName;
            }
        }

        for (int id : toRemove) {
            pollTimers_.erase(id);
        }
    }

    /**
     * @brief 停止所有轮询定时器
     */
    void stopAllPolling() {
        std::lock_guard<std::mutex> lock(timerMutex_);

        for (auto& [deviceId, entry] : pollTimers_) {
            if (entry.loop) entry.loop->invalidateTimer(entry.timerId);
        }

        int count = static_cast<int>(pollTimers_.size());
        pollTimers_.clear();

        if (count > 0) {
            LOG_INFO << "[Modbus] Stopped " << count << " polling timer(s)";
        }
    }

    /**
     * @brief 单个设备的轮询回调
     */
    void pollDevice(int deviceId) {
        DeviceContext ctx;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            auto it = deviceContexts_.find(deviceId);
            if (it == deviceContexts_.end()) return;
            ctx = it->second;  // 拷贝以避免持锁过长
        }

        // 检查链路是否运行中
        if (!TcpLinkManager::instance().isRunning(ctx.linkId)) {
            LOG_DEBUG << "[Modbus] Link " << ctx.linkId << " not running, skip poll for " << ctx.deviceName;
            return;
        }

        for (const auto& group : ctx.readGroups) {
            sendReadRequest(ctx, group);
        }
    }

    /**
     * @brief 发送一组读取请求
     */
    void sendReadRequest(const DeviceContext& ctx, const ReadGroup& group) {
        ModbusRequest req;
        req.slaveId = ctx.slaveId;
        req.functionCode = group.functionCode;
        req.startAddress = group.startAddress;
        req.quantity = group.totalQuantity;
        req.transactionId = transactionCounter_.fetch_add(1, std::memory_order_relaxed);

        auto frame = ModbusUtils::buildRequest(ctx.frameMode, req);

        // 发送
        bool sent = false;
        if (ctx.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
            // TCP Client 模式：直接发送
            std::string data(frame.begin(), frame.end());
            sent = TcpLinkManager::instance().sendData(ctx.linkId, data);
            if (!sent) {
                LOG_WARN << "[Modbus] sendData failed: linkId=" << ctx.linkId << " device=" << ctx.deviceName;
            }
        } else {
            // TCP Server 模式：通过设备连接映射定向发送
            std::string connKey = "modbus_" + std::to_string(ctx.slaveId);
            auto connOpt = DeviceConnectionCache::instance().getConnection(connKey);
            if (connOpt) {
                std::string data(frame.begin(), frame.end());
                sent = TcpLinkManager::instance().sendToClient(connOpt->linkId, connOpt->clientAddr, data);
                if (!sent) {
                    LOG_WARN << "[Modbus] sendToClient failed: linkId=" << connOpt->linkId
                             << " client=" << connOpt->clientAddr << " device=" << ctx.deviceName;
                }
            } else {
                LOG_WARN << "[Modbus] No connection mapping for " << connKey
                         << ", device=" << ctx.deviceName << " (waiting for device to connect)";
            }
        }

        if (sent) {
            LOG_DEBUG << "[Modbus] TX " << ctx.deviceName
                      << " slave=" << static_cast<int>(ctx.slaveId)
                      << " " << registerTypeToString(group.registerType)
                      << " addr=" << group.startAddress
                      << " qty=" << group.totalQuantity
                      << " | " << ModbusUtils::toHexString(frame);

            // 记录待应答请求
            std::string pendingKey = std::to_string(ctx.linkId) + ":"
                + std::to_string(ctx.slaveId) + ":"
                + std::to_string(group.functionCode);

            PendingRequest pending;
            pending.deviceId = ctx.deviceId;
            pending.group = group;
            pending.sentTime = std::chrono::steady_clock::now();
            pending.transactionId = req.transactionId;

            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingRequests_[pendingKey] = std::move(pending);
        }
    }

    // ==================== 响应处理 ====================

    /**
     * @brief 处理解析后的 Modbus 响应
     */
    Task<void> processResponse(int linkId, [[maybe_unused]] const std::string& clientAddr, const ModbusResponse& response) {
        // 查找对应的 PendingRequest
        std::string pendingKey = std::to_string(linkId) + ":"
            + std::to_string(response.slaveId) + ":"
            + std::to_string(response.functionCode);

        PendingRequest pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingRequests_.find(pendingKey);
            if (it == pendingRequests_.end()) {
                LOG_WARN << "[Modbus] No pending request for key=" << pendingKey
                         << " (unexpected response or already timed out)";
                co_return;
            }
            pending = std::move(it->second);
            pendingRequests_.erase(it);
        }

        // 获取设备上下文
        DeviceContext ctx;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            auto it = deviceContexts_.find(pending.deviceId);
            if (it == deviceContexts_.end()) co_return;
            ctx = it->second;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pending.sentTime).count();

        auto registerValues = extractRegisterValues(ctx, pending.group, response.data);

        if (registerValues.empty()) {
            LOG_WARN << "[Modbus] No values: " << ctx.deviceName << " latency=" << elapsed << "ms";
            co_return;
        }

        LOG_DEBUG << "[Modbus] RX " << ctx.deviceName
                  << " slave=" << static_cast<int>(ctx.slaveId)
                  << " " << registerTypeToString(pending.group.registerType)
                  << " addr=" << pending.group.startAddress
                  << " qty=" << pending.group.totalQuantity
                  << " byteOrder=" << byteOrderToString(ctx.byteOrder)
                  << " | " << registerValues.size() << " values"
                  << " latency=" << elapsed << "ms"
                  << " data=[" << ModbusUtils::toHexString(response.data) << "]"
                  << " → " << formatRegisterValues(registerValues);
        co_await saveData(ctx, registerValues);
    }

    /**
     * @brief 从响应数据中提取寄存器值
     * @return key (registerType_address) → { name, value, unit }
     */
    std::map<std::string, Json::Value> extractRegisterValues(
        const DeviceContext& ctx,
        const ReadGroup& group,
        const std::vector<uint8_t>& responseData
    ) {
        std::map<std::string, Json::Value> result;

        for (const auto* reg : group.registers) {
            std::string key = registerTypeToString(reg->registerType)
                + "_" + std::to_string(reg->address);

            Json::Value elem;
            elem["name"] = reg->name;
            if (!reg->unit.empty()) elem["unit"] = reg->unit;

            if (isBitRegister(reg->registerType)) {
                // 线圈/离散输入：从位数据中提取
                uint16_t bitOffset = reg->address - group.startAddress;
                if (bitOffset / 8 < responseData.size()) {
                    bool value = ModbusUtils::extractBit(responseData.data(), bitOffset);
                    elem["value"] = value ? 1 : 0;
                } else {
                    elem["value"] = Json::nullValue;
                    LOG_WARN << "[Modbus]   " << key << " offset out of range";
                }
            } else {
                // 保持寄存器/输入寄存器：从字节数据中提取
                size_t byteOffset = static_cast<size_t>(reg->address - group.startAddress) * 2;
                size_t byteSize = dataTypeToByteSize(reg->dataType);

                if (byteOffset + byteSize <= responseData.size()) {
                    double value = ModbusUtils::extractValue(
                        responseData.data() + byteOffset,
                        reg->dataType,
                        ctx.byteOrder
                    );

                    // 整数类型保留为整数
                    if (reg->dataType == DataType::BOOL || reg->dataType == DataType::INT16
                        || reg->dataType == DataType::UINT16 || reg->dataType == DataType::INT32
                        || reg->dataType == DataType::UINT32 || reg->dataType == DataType::INT64
                        || reg->dataType == DataType::UINT64) {
                        elem["value"] = static_cast<int64_t>(value);
                    } else {
                        // FLOAT32 / DOUBLE 保留小数
                        elem["value"] = value;
                    }
                } else {
                    elem["value"] = Json::nullValue;
                    LOG_WARN << "[Modbus]   " << key << " byteOffset=" << byteOffset
                             << " + byteSize=" << byteSize << " > responseLen=" << responseData.size();
                }
            }

            result[key] = elem;
        }

        return result;
    }

    /**
     * @brief 保存数据到数据库 + 缓存
     */
    Task<void> saveData(
        const DeviceContext& ctx,
        const std::map<std::string, Json::Value>& registerValues
    ) {
        try {
            // 构建 JSONB 数据（与 DeviceDataTransformer::parseModbusRegisters 的 key 格式对齐）
            Json::Value data;
            data["funcCode"] = FUNC_READ;
            data["direction"] = "UP";

            Json::Value dataObj(Json::objectValue);
            for (const auto& [key, elem] : registerValues) {
                dataObj[key] = elem;
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
            std::string reportTime = timeOss.str();

            // 序列化 JSON
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            writer["emitUTF8"] = true;
            std::string jsonStr = Json::writeString(writer, data);

            // 插入 device_data
            DatabaseService dbService;
            co_await dbService.execSqlCoro(R"(
                INSERT INTO device_data (device_id, link_id, protocol, data, report_time)
                VALUES (?, ?, 'MODBUS', ?::jsonb, ?::timestamptz)
            )", {
                std::to_string(ctx.deviceId),
                std::to_string(ctx.linkId),
                jsonStr,
                reportTime
            });

            // 更新实时数据缓存
            RealtimeDataCache::instance().update(ctx.deviceId, FUNC_READ, data, reportTime);

            // 更新资源版本号
            ResourceVersion::instance().incrementVersion("device");

            LOG_DEBUG << "[Modbus] Saved: " << ctx.deviceName
                      << " regs=" << registerValues.size();

        } catch (const std::exception& e) {
            LOG_ERROR << "[Modbus] Save data failed for device=" << ctx.deviceName << ": " << e.what();
        }
    }

    // ==================== 离线标记 ====================

    /**
     * @brief 标记链路上所有 Modbus 设备为离线（TCP Client 断连时调用）
     */
    void markDevicesOfflineForLink(int linkId) {
        std::lock_guard<std::mutex> lock(contextMutex_);
        int count = 0;
        for (const auto& [deviceId, ctx] : deviceContexts_) {
            if (ctx.linkId == linkId) {
                RealtimeDataCache::instance().clearLatestTime(deviceId);
                count++;
            }
        }
        if (count > 0) {
            LOG_INFO << "[Modbus] Marked " << count << " device(s) offline, link=" << linkId;
            ResourceVersion::instance().incrementVersion("device");
        }
    }

    /**
     * @brief 通过客户端地址查找并标记对应设备为离线（TCP Server 断连时调用）
     * 在 removeByClient 之前调用，此时 DeviceConnectionCache 仍有映射
     */
    void markDeviceOfflineByClient(int linkId, const std::string& clientAddr) {
        std::lock_guard<std::mutex> lock(contextMutex_);
        int count = 0;
        for (const auto& [deviceId, ctx] : deviceContexts_) {
            if (ctx.linkId != linkId) continue;

            std::string connKey = "modbus_" + std::to_string(ctx.slaveId);
            auto connOpt = DeviceConnectionCache::instance().getConnection(connKey);
            if (connOpt && connOpt->linkId == linkId && connOpt->clientAddr == clientAddr) {
                RealtimeDataCache::instance().clearLatestTime(deviceId);
                count++;
            }
        }
        if (count > 0) {
            LOG_INFO << "[Modbus] Marked " << count << " device(s) offline, client=" << clientAddr;
            ResourceVersion::instance().incrementVersion("device");
        }
    }

    // ==================== 辅助方法 ====================

    /**
     * @brief 格式化寄存器解析值用于日志输出
     * 输出格式: {name=value unit, name=value unit, ...}
     */
    static std::string formatRegisterValues(const std::map<std::string, Json::Value>& values) {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [key, elem] : values) {
            if (!first) oss << ", ";
            first = false;
            oss << elem.get("name", key).asString() << "=";
            const auto& val = elem["value"];
            if (val.isDouble()) {
                oss << val.asDouble();
            } else if (val.isInt64()) {
                oss << val.asInt64();
            } else {
                oss << "null";
            }
            std::string unit = elem.get("unit", "").asString();
            if (!unit.empty()) oss << unit;
        }
        oss << "}";
        return oss.str();
    }

    /**
     * @brief 获取链路上设备的帧模式
     */
    FrameMode getLinkFrameMode(int linkId) {
        std::lock_guard<std::mutex> lock(contextMutex_);
        for (const auto& [id, ctx] : deviceContexts_) {
            if (ctx.linkId == linkId) {
                return ctx.frameMode;
            }
        }
        return FrameMode::TCP;  // 默认
    }

    // ==================== 成员变量 ====================

    /** 待应答请求 */
    struct PendingRequest {
        int deviceId;
        ReadGroup group;
        std::chrono::steady_clock::time_point sentTime;
        uint16_t transactionId;
    };

    // 设备上下文: deviceId → DeviceContext
    std::map<int, DeviceContext> deviceContexts_;
    std::mutex contextMutex_;

    // 轮询定时器: deviceId → TimerEntry
    struct TimerEntry {
        trantor::TimerId timerId;
        trantor::EventLoop* loop;
    };
    std::map<int, TimerEntry> pollTimers_;
    std::mutex timerMutex_;

    // 接收缓冲区: linkId → buffer
    std::map<int, std::vector<uint8_t>> buffers_;
    std::mutex bufferMutex_;

    // 待应答请求: "linkId:slaveId:fc" → PendingRequest
    std::map<std::string, PendingRequest> pendingRequests_;
    std::mutex pendingMutex_;

    // Transaction ID 计数器
    std::atomic<uint16_t> transactionCounter_{0};
};

}  // namespace modbus
