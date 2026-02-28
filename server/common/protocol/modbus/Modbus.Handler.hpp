#pragma once

#include "Modbus.Types.hpp"
#include "Modbus.Utils.hpp"

#include <deque>
#include "common/protocol/ParsedResult.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/utils/AppException.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/network/WebSocketManager.hpp"
#include "common/utils/Constants.hpp"

namespace modbus {

/** Modbus 虚拟功能码（数据存储用） */
inline constexpr const char* FUNC_READ = "MODBUS_READ";

/** Modbus 合并间距 */
inline constexpr int MERGE_GAP = 10;

/** Modbus 默认轮询间隔（秒） */
inline constexpr int DEFAULT_READ_INTERVAL = 1;

/** Modbus 请求超时（毫秒） */
inline constexpr int RESPONSE_TIMEOUT_MS = 10000;

/** 接收缓冲区上限（字节），超过则清空防止内存泄漏 */
inline constexpr size_t MAX_BUFFER_SIZE = 1024;

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
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            writeQueues_.clear();
        }
        LOG_INFO << "[Modbus] Handler shutdown";
    }

    /**
     * @brief 重新加载设备配置（设备变更时调用）
     */
    Task<void> reloadDevices() {
        LOG_INFO << "[Modbus] Reloading devices...";
        stopAllPolling();
        // 清理所有写入队列
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            writeQueues_.clear();
        }
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
        // 连接变化时清理缓冲区和待处理状态
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            buffers_.erase(linkId);
        }
        clearPendingStateForLink(linkId);

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

    // ==================== 写寄存器 ====================

    /** 写寄存器结果 */
    struct WriteResult {
        bool sent = false;
        std::string frameHex;
    };

    /** 指令应答回调 */
    using CommandResponseCallback = std::function<void(
        const std::string& deviceCode, const std::string& funcCode, bool success, int64_t responseId)>;

    void setCommandResponseCallback(CommandResponseCallback cb) {
        commandResponseCallback_ = std::move(cb);
    }

    /**
     * @brief 写入 Modbus 寄存器（由 ProtocolDispatcher 调用）
     * @param deviceId 设备 ID
     * @param deviceCode 设备编码（用于应答回调匹配）
     * @param elements 要素数据 [{ elementId: "reg-id", value: "123" }]
     * @return 发送结果
     */
    WriteResult writeRegister(int deviceId, const std::string& deviceCode, const Json::Value& elements) {
        DeviceContext ctx;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            auto it = deviceContexts_.find(deviceId);
            if (it == deviceContexts_.end()) {
                LOG_ERROR << "[Modbus] writeRegister: device not found, id=" << deviceId;
                return {};
            }
            ctx = it->second;
        }

        // ========== 第一步：解析所有要素，按寄存器类型分组 ==========

        struct WriteEntry {
            uint16_t address;
            uint16_t quantity;
            std::vector<uint8_t> data;
            RegisterType registerType;
            double value;
        };

        std::vector<WriteEntry> coilEntries;
        std::vector<WriteEntry> holdingEntries;

        for (const auto& elem : elements) {
            std::string registerId = elem.get("elementId", "").asString();
            std::string valueStr = elem.get("value", "").asString();

            const RegisterDef* regDef = nullptr;
            for (const auto& reg : ctx.registers) {
                if (reg.id == registerId) {
                    regDef = &reg;
                    break;
                }
            }
            if (!regDef) {
                throw ValidationException("寄存器未找到: " + registerId);
            }
            if (!isWritable(regDef->registerType)) {
                throw ValidationException("寄存器「" + regDef->name + "」不可写（类型: "
                    + registerTypeToString(regDef->registerType) + "）");
            }

            double value = 0.0;
            try { value = std::stod(valueStr); } catch (...) {
                throw ValidationException("寄存器「" + regDef->name + "」的值不是有效数字: " + valueStr);
            }

            // 数据类型范围检查
            validateModbusValue(value, regDef->dataType, regDef->name);

            WriteEntry entry;
            entry.address = regDef->address;
            entry.registerType = regDef->registerType;
            entry.value = value;

            if (regDef->registerType == RegisterType::COIL) {
                entry.quantity = 1;
                entry.data = {static_cast<uint8_t>(value != 0 ? 0xFF : 0x00), 0x00};
                coilEntries.push_back(std::move(entry));
            } else {
                entry.data = ModbusUtils::encodeValue(value, regDef->dataType, ctx.byteOrder);
                entry.quantity = dataTypeToQuantity(regDef->dataType);
                holdingEntries.push_back(std::move(entry));
            }
        }

        // ========== 第二步：合并连续 HOLDING_REGISTER 写入 ==========

        // 按地址排序
        std::sort(holdingEntries.begin(), holdingEntries.end(),
            [](const WriteEntry& a, const WriteEntry& b) { return a.address < b.address; });

        // 合并连续地址段
        struct MergedWrite {
            uint16_t startAddress;
            uint16_t totalQuantity;
            std::vector<uint8_t> mergedData;
        };
        std::vector<MergedWrite> mergedWrites;

        for (const auto& entry : holdingEntries) {
            if (!mergedWrites.empty()) {
                auto& last = mergedWrites.back();
                if (entry.address == last.startAddress + last.totalQuantity) {
                    // 地址连续，追加到当前组
                    last.totalQuantity += entry.quantity;
                    last.mergedData.insert(last.mergedData.end(), entry.data.begin(), entry.data.end());
                    continue;
                }
            }
            // 新的地址段
            mergedWrites.push_back({entry.address, entry.quantity, entry.data});
        }

        // ========== 第三步：构建帧并入队（串行发送，等应答后再发下一帧） ==========

        std::vector<QueuedWriteFrame> framesToQueue;

        // 合并后的 HOLDING_REGISTER 写帧
        for (const auto& mw : mergedWrites) {
            ModbusWriteRequest writeReq;
            writeReq.slaveId = ctx.slaveId;
            writeReq.address = mw.startAddress;
            writeReq.quantity = mw.totalQuantity;
            writeReq.data = mw.mergedData;
            writeReq.transactionId = transactionCounter_.fetch_add(1, std::memory_order_relaxed);
            writeReq.functionCode = (mw.totalQuantity == 1)
                ? FuncCodes::WRITE_SINGLE_REGISTER
                : FuncCodes::WRITE_MULTIPLE_REGISTERS;

            QueuedWriteFrame qf;
            qf.frame = ModbusUtils::buildWriteRequest(ctx.frameMode, writeReq);
            qf.functionCode = writeReq.functionCode;
            qf.address = writeReq.address;
            qf.transactionId = writeReq.transactionId;
            qf.deviceCode = deviceCode;
            qf.deviceId = ctx.deviceId;
            qf.linkId = ctx.linkId;
            qf.linkMode = ctx.linkMode;
            qf.slaveId = ctx.slaveId;
            qf.deviceName = ctx.deviceName;
            framesToQueue.push_back(std::move(qf));
        }

        // COIL 写帧
        for (const auto& entry : coilEntries) {
            ModbusWriteRequest writeReq;
            writeReq.slaveId = ctx.slaveId;
            writeReq.address = entry.address;
            writeReq.functionCode = FuncCodes::WRITE_SINGLE_COIL;
            writeReq.data = entry.data;
            writeReq.transactionId = transactionCounter_.fetch_add(1, std::memory_order_relaxed);

            QueuedWriteFrame qf;
            qf.frame = ModbusUtils::buildWriteRequest(ctx.frameMode, writeReq);
            qf.functionCode = writeReq.functionCode;
            qf.address = writeReq.address;
            qf.transactionId = writeReq.transactionId;
            qf.deviceCode = deviceCode;
            qf.deviceId = ctx.deviceId;
            qf.linkId = ctx.linkId;
            qf.linkMode = ctx.linkMode;
            qf.slaveId = ctx.slaveId;
            qf.deviceName = ctx.deviceName;
            framesToQueue.push_back(std::move(qf));
        }

        if (framesToQueue.empty()) return {};

        // 构建返回结果（所有帧的 hex，含排队的）
        WriteResult result;
        result.sent = true;
        for (const auto& qf : framesToQueue) {
            if (!result.frameHex.empty()) result.frameHex += " ";
            result.frameHex += ModbusUtils::toHexString(qf.frame);
        }

        // 入队并尝试发送第一帧
        enqueueWrites(makeWriteQueueKey(ctx.linkId, ctx.slaveId), std::move(framesToQueue));

        return result;
    }

    // ==================== 数据接收 ====================

    /**
     * @brief 处理接收到的数据（被 ProtocolDispatcher 调用）
     */
    Task<void> handleData(int linkId, const std::string& clientAddr, const std::vector<uint8_t>& data) {
        auto parsedFrames = appendAndParseFrames(linkId, data);

        for (auto& response : parsedFrames) {
            if (!clientAddr.empty()) {
                std::string connKey = "modbus_" + std::to_string(response.slaveId);
                DeviceConnectionCache::instance().registerConnection(connKey, linkId, clientAddr);
            }

            if (response.isException) {
                totalExceptions_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "[Modbus] Exception: slave=" << static_cast<int>(response.slaveId)
                         << " FC=" << static_cast<int>(response.functionCode)
                         << " code=" << static_cast<int>(response.exceptionCode);
                // 写异常：通知失败
                if (isWriteFunctionCode(response.functionCode)) {
                    handleWriteResponse(linkId, response, false);
                }
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

        auto parsedFrames = appendAndParseFrames(linkId, data);

        for (auto& response : parsedFrames) {
            if (!clientAddr.empty()) {
                std::string connKey = "modbus_" + std::to_string(response.slaveId);
                DeviceConnectionCache::instance().registerConnection(connKey, linkId, clientAddr);
            }

            if (response.isException) {
                totalExceptions_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "[Modbus] Exception: slave=" << static_cast<int>(response.slaveId)
                         << " FC=" << static_cast<int>(response.functionCode)
                         << " code=" << static_cast<int>(response.exceptionCode);
                // 写异常：通知失败
                if (isWriteFunctionCode(response.functionCode)) {
                    handleWriteResponse(linkId, response, false);
                }
            } else {
                auto result = processResponseSync(linkId, clientAddr, response);
                if (result) {
                    results.push_back(std::move(*result));
                }
            }
        }

        return results;
    }

    /** Modbus 性能统计 */
    struct ModbusStats {
        int64_t totalResponses;
        double avgLatencyMs;
        int64_t timeouts;
        int64_t crcErrors;
        int64_t exceptions;
    };

    ModbusStats getModbusStats() const {
        auto responses = totalResponses_.load(std::memory_order_relaxed);
        auto latency = totalLatencyMs_.load(std::memory_order_relaxed);
        return {
            responses,
            responses > 0 ? static_cast<double>(latency) / static_cast<double>(responses) : 0.0,
            totalTimeouts_.load(std::memory_order_relaxed),
            totalCrcErrors_.load(std::memory_order_relaxed),
            totalExceptions_.load(std::memory_order_relaxed)
        };
    }

private:
    // ==================== 同步响应处理 ====================

    /**
     * @brief 同步处理 Modbus 响应，返回 ParsedFrameResult（不涉及 DB 操作）
     */
    std::optional<ParsedFrameResult> processResponseSync(int linkId, const std::string& /*clientAddr*/,
                                                          const ModbusResponse& response) {
        // 写回显：通知成功，不生成 ParsedFrameResult
        if (isWriteFunctionCode(response.functionCode)) {
            handleWriteResponse(linkId, response, true);
            return std::nullopt;
        }

        std::string pendingKey = std::to_string(linkId) + ":"
            + std::to_string(response.slaveId) + ":"
            + std::to_string(response.functionCode);

        PendingRequest pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingRequests_.find(pendingKey);
            if (it == pendingRequests_.end() || it->second.empty()) {
                LOG_WARN << "[Modbus] No pending request for key=" << pendingKey
                         << " (unexpected response or already timed out)";
                return std::nullopt;
            }
            pending = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pendingRequests_.erase(it);
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
        totalResponses_.fetch_add(1, std::memory_order_relaxed);
        totalLatencyMs_.fetch_add(elapsed, std::memory_order_relaxed);

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

        // 合并到内存缓冲区（与异步路径 saveData 保持一致，解决多 ReadGroup 合并问题）
        Json::Value dataObj(Json::objectValue);
        {
            std::lock_guard<std::mutex> mlock(mergedRegsMutex_);
            auto& merged = deviceMergedRegs_[ctx.deviceId];
            for (const auto& [key, elem] : registerValues) {
                merged[key] = elem;
            }
            dataObj = merged;
        }
        jsonData["data"] = dataObj;

        result.data = jsonData;

        // 写优先：当前组响应后，先检查是否有待发写命令
        // 有写 → 记录恢复点，暂停读，等写完后由 resumeReadAfterWrite 继续
        // 无写 → 发下一个读组（或全部读完进入间隔等待）
        size_t nextIdx = pending.groupIndex + 1;
        bool allGroupsDone = (nextIdx >= ctx.readGroups.size());
        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            readResumeIdx_[ctx.deviceId] = allGroupsDone ? SIZE_MAX : nextIdx;
        }
        bool writeStarted = tryStartPendingWrite(ctx.linkId, ctx.slaveId);
        if (!writeStarted) {
            {
                std::lock_guard<std::mutex> lk(pendingMutex_);
                readResumeIdx_.erase(ctx.deviceId);
            }
            if (allGroupsDone) {
                onReadCycleComplete(ctx.deviceId);
            } else {
                sendReadRequest(ctx, ctx.readGroups[nextIdx], nextIdx);
            }
        }

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

    // ==================== 帧解析（公共） ====================

    /**
     * @brief 将数据追加到缓冲区并解析出所有完整帧
     *
     * 统一处理 TCP 流式传输的粘包/拆包：
     * - 数据追加到 linkId 对应的缓冲区
     * - while 循环持续解析，处理粘包（一次接收多帧）
     * - consumed == 0 时 break，处理拆包（帧不完整）
     * - FRAME_CORRUPT 时跳过 1 字节重新对齐
     * - 缓冲区超限时清空防止内存泄漏
     */
    std::vector<ModbusResponse> appendAndParseFrames(int linkId, const std::vector<uint8_t>& data) {
        FrameMode mode = getLinkFrameMode(linkId);
        std::vector<ModbusResponse> parsedFrames;

        std::lock_guard<std::mutex> lock(bufferMutex_);
        auto& linkBuffer = buffers_[linkId];
        linkBuffer.insert(linkBuffer.end(), data.begin(), data.end());

        while (!linkBuffer.empty()) {
            // RTU 模式：跳过非 Modbus 数据（如 DTU JSON 心跳）
            if (mode == FrameMode::RTU) {
                size_t skip = ModbusUtils::skipNonRtuData(linkBuffer);
                if (skip > 0) {
                    auto end = linkBuffer.begin() + static_cast<ptrdiff_t>((std::min)(skip, size_t(64)));
                    LOG_WARN << "[Modbus] Skipping " << skip << "B non-RTU data on link " << linkId
                             << " | " << ModbusUtils::toHexString({linkBuffer.begin(), end});
                    linkBuffer.erase(linkBuffer.begin(), linkBuffer.begin() + static_cast<ptrdiff_t>(skip));
                    continue;
                }
            }

            // TCP 模式：跳过非法 MBAP Header 数据
            if (mode == FrameMode::TCP) {
                size_t skip = ModbusUtils::skipInvalidMbapData(linkBuffer);
                if (skip > 0) {
                    auto end = linkBuffer.begin() + static_cast<ptrdiff_t>((std::min)(skip, size_t(64)));
                    LOG_WARN << "[Modbus] Skipping " << skip << "B invalid MBAP data on link " << linkId
                             << " | " << ModbusUtils::toHexString({linkBuffer.begin(), end});
                    linkBuffer.erase(linkBuffer.begin(), linkBuffer.begin() + static_cast<ptrdiff_t>(skip));
                    continue;
                }
            }

            ModbusResponse response;
            size_t consumed = ModbusUtils::parseResponse(mode, linkBuffer, response);

            if (consumed == ModbusUtils::FRAME_CORRUPT) {
                // 帧校验失败（CRC 不匹配或帧格式异常），跳过 1 字节重新对齐
                totalCrcErrors_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "[Modbus] Corrupt frame on link " << linkId
                         << ", skip 1B for resync | head="
                         << ModbusUtils::toHexString({linkBuffer.begin(),
                            linkBuffer.begin() + static_cast<ptrdiff_t>((std::min)(linkBuffer.size(), size_t(8)))});
                linkBuffer.erase(linkBuffer.begin());
                continue;
            }

            if (consumed == 0) {
                // 数据不足；缓冲区超限则清空
                if (linkBuffer.size() > MAX_BUFFER_SIZE) {
                    LOG_ERROR << "[Modbus] Parse failed, buffer overflow (" << linkBuffer.size()
                              << "B) on link " << linkId << ", clearing | "
                              << ModbusUtils::toHexString(linkBuffer);
                    linkBuffer.clear();
                } else {
                    LOG_DEBUG << "[Modbus] Incomplete frame on link " << linkId
                              << " (" << linkBuffer.size() << "B), waiting for more data";
                }
                break;
            }

            linkBuffer.erase(linkBuffer.begin(), linkBuffer.begin() + static_cast<ptrdiff_t>(consumed));
            parsedFrames.push_back(std::move(response));
        }

        return parsedFrames;
    }

private:
    // ==================== 设备上下文管理 ====================

    /**
     * @brief 从 DeviceCache 加载所有 Modbus 设备
     */
    Task<void> loadDeviceContexts() {
        auto cachedDevices = co_await DeviceCache::instance().getDevices();

        std::lock_guard<std::mutex> lock(contextMutex_);
        deviceContexts_.clear();
        {
            std::lock_guard<std::mutex> mlock(mergedRegsMutex_);
            deviceMergedRegs_.clear();
        }

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

        // 帧模式：TCP Client 和 TCP Server 均根据 modbusMode 配置
        // TCP Client：空字符串默认 TCP（parseFrameMode 行为）
        // TCP Server：显式配置 "TCP" 时使用 ModbusTCP，否则默认 RTU（兼容 DTU 串口透传设备）
        if (device.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
            ctx.frameMode = parseFrameMode(device.modbusMode);
        } else {
            ctx.frameMode = (device.modbusMode == "TCP") ? FrameMode::TCP : FrameMode::RTU;
        }

        // 从协议配置解析
        const auto& config = device.protocolConfig;
        ctx.byteOrder = parseByteOrder(config.get("byteOrder", "BIG_ENDIAN").asString());
        ctx.readInterval = config.get("readInterval", DEFAULT_READ_INTERVAL).asInt();
        if (ctx.readInterval < 1 || ctx.readInterval > 3600) {
            LOG_WARN << "[Modbus] Invalid readInterval=" << ctx.readInterval
                     << " for device " << device.name << ", using default";
            ctx.readInterval = DEFAULT_READ_INTERVAL;
        }

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

                // Modbus 协议限制：线圈/离散输入最多 2000 个，寄存器最多 125 个
                uint16_t maxQuantity = (def.registerType == RegisterType::COIL ||
                                        def.registerType == RegisterType::DISCRETE_INPUT) ? 2000 : 125;
                if (def.quantity < 1 || def.quantity > maxQuantity) {
                    LOG_WARN << "[Modbus] Invalid quantity=" << def.quantity
                             << " for register " << def.name << ", clamping to [1," << maxQuantity << "]";
                    def.quantity = std::clamp(def.quantity, static_cast<uint16_t>(1), maxQuantity);
                }

                def.unit = reg.get("unit", "").asString();
                def.remark = reg.get("remark", "").asString();
                def.decimals = reg.get("decimals", -1).asInt();
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
        std::vector<int> devicesToPoll;
        {
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

            // 收集已启动轮询的设备 ID
            for (const auto& [deviceId, _] : pollTimers_) {
                devicesToPoll.push_back(deviceId);
            }
        }

        // 释放锁后立即轮询一次
        for (int devId : devicesToPoll) {
            pollDevice(devId);
        }
    }

    /**
     * @brief 为指定链路启动轮询定时器（连接建立时调用）
     */
    void startPollingForLink(int linkId) {
        std::vector<int> devicesToPoll;
        {
            std::lock_guard<std::mutex> ctxLock(contextMutex_);
            std::lock_guard<std::mutex> timerLock(timerMutex_);
            startPollTimersForLink(linkId);

            // 收集该链路上启动轮询的设备 ID
            for (const auto& [deviceId, ctx] : deviceContexts_) {
                if (ctx.linkId == linkId && pollTimers_.count(deviceId)) {
                    devicesToPoll.push_back(deviceId);
                }
            }
        }

        // 释放锁后立即轮询一次
        for (int devId : devicesToPoll) {
            pollDevice(devId);
        }
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
                try {
                    pollDevice(devId);
                } catch (const std::exception& e) {
                    LOG_ERROR << "[Modbus] pollDevice exception (deviceId=" << devId << "): " << e.what();
                } catch (...) {
                    LOG_ERROR << "[Modbus] pollDevice unknown exception (deviceId=" << devId << ")";
                }
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

        // 清理超时的待处理请求，防止响应丢失导致内存泄漏
        cleanupTimedOutRequests(ctx.linkId);

        if (ctx.readGroups.empty()) return;

        // 串行轮询：读/写任一进行中，或距离上次周期结束未满间隔时，跳过
        {
            auto now = std::chrono::steady_clock::now();
            std::string slavePrefix = std::to_string(ctx.linkId) + ":" + std::to_string(ctx.slaveId) + ":";
            std::string writeKey = makeWriteQueueKey(ctx.linkId, ctx.slaveId);
            std::lock_guard<std::mutex> lock(pendingMutex_);

            // 上次周期完成后的等待间隔尚未到期
            auto timeIt = nextAllowedPollTime_.find(ctx.deviceId);
            if (timeIt != nextAllowedPollTime_.end() && now < timeIt->second) {
                LOG_DEBUG << "[Modbus] " << ctx.deviceName << " interval not elapsed, skip poll";
                return;
            }

            // 有待应答的读请求
            for (const auto& [key, q] : pendingRequests_) {
                if (key.compare(0, slavePrefix.size(), slavePrefix) == 0 && !q.empty()) {
                    LOG_DEBUG << "[Modbus] " << ctx.deviceName << " read in progress, skip poll";
                    return;
                }
            }

            // 有写操作正在执行或排队
            auto wit = writeQueues_.find(writeKey);
            if (wit != writeQueues_.end() && (wit->second.sending || !wit->second.frames.empty())) {
                LOG_DEBUG << "[Modbus] " << ctx.deviceName << " write pending, skip poll";
                return;
            }
        }

        // 仅发送第一组；后续组在收到响应后链式触发
        sendReadRequest(ctx, ctx.readGroups[0], 0);
    }

    /**
     * @brief 清理超时的待处理读/写请求
     */
    void cleanupTimedOutRequests(int linkId) {
        auto now = std::chrono::steady_clock::now();
        std::string prefix = std::to_string(linkId) + ":";
        std::vector<std::pair<int, uint8_t>> unstickQueues;
        std::vector<std::string> timedOutConnKeys;  // 锁内收集，锁外清理（避免锁序反转）

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);

            for (auto it = pendingRequests_.begin(); it != pendingRequests_.end(); ) {
                if (it->first.compare(0, prefix.size(), prefix) != 0) { ++it; continue; }
                auto& queue = it->second;
                while (!queue.empty()) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - queue.front().sentTime).count();
                    if (elapsed < RESPONSE_TIMEOUT_MS) break;
                    totalTimeouts_.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN << "[Modbus] Read request timed out (" << elapsed << "ms): key=" << it->first;
                    // 收集超时从站的连接 key，锁释放后清理 DTU 映射
                    auto secondColon = it->first.find(':', prefix.size());
                    if (secondColon != std::string::npos) {
                        timedOutConnKeys.push_back(
                            "modbus_" + it->first.substr(prefix.size(), secondColon - prefix.size()));
                    }
                    queue.pop_front();
                }
                if (queue.empty()) { it = pendingRequests_.erase(it); } else { ++it; }
            }

            for (auto it = pendingWriteRequests_.begin(); it != pendingWriteRequests_.end(); ) {
                if (it->first.compare(0, prefix.size(), prefix) != 0) { ++it; continue; }
                auto& queue = it->second;
                while (!queue.empty()) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - queue.front().sentTime).count();
                    if (elapsed < RESPONSE_TIMEOUT_MS) break;
                    totalTimeouts_.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN << "[Modbus] Write request timed out (" << elapsed << "ms): key=" << it->first;
                    if (commandResponseCallback_) {
                        commandResponseCallback_(queue.front().deviceCode, "MODBUS_WRITE", false, 0);
                    }
                    queue.pop_front();
                }
                if (queue.empty()) { it = pendingWriteRequests_.erase(it); } else { ++it; }
            }

            // 解除因超时丢失应答而卡住的写入队列
            for (auto& [queueKey, queue] : writeQueues_) {
                if (!queue.sending) continue;
                if (queueKey.compare(0, prefix.size(), prefix) != 0) continue;

                std::string writePrefix = queueKey + ":";
                bool hasPendingWrite = false;
                for (const auto& [pk, pq] : pendingWriteRequests_) {
                    if (pk.compare(0, writePrefix.size(), writePrefix) == 0 && !pq.empty()) {
                        hasPendingWrite = true;
                        break;
                    }
                }
                if (!hasPendingWrite) {
                    queue.sending = false;
                    auto colonPos = queueKey.find(':');
                    if (colonPos != std::string::npos) {
                        try {
                            uint8_t slaveId = static_cast<uint8_t>(std::stoi(queueKey.substr(colonPos + 1)));
                            unstickQueues.push_back({linkId, slaveId});
                            LOG_WARN << "[Modbus] Unsticking write queue: " << queueKey;
                        } catch (...) {}
                    }
                }
            }
        }

        // 锁释放后清理 DTU 连接映射：超时的从站下次轮询改为广播，等待重新注册
        for (const auto& connKey : timedOutConnKeys) {
            if (DeviceConnectionCache::instance().getConnection(connKey)) {
                DeviceConnectionCache::instance().removeConnection(connKey);
                LOG_INFO << "[Modbus] Timeout: cleared DTU mapping for " << connKey
                         << ", next poll will broadcast";
            }
        }

        // 锁释放后尝试发送队列中下一帧
        for (auto [lid, sid] : unstickQueues) {
            trySendNextWrite(lid, sid);
        }
    }

    /**
     * @brief 发送一组读取请求
     * @param groupIndex 在 readGroups 中的下标，用于串行轮询触发下一组
     */
    void sendReadRequest(const DeviceContext& ctx, const ReadGroup& group, size_t groupIndex = 0) {
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
            std::string data(frame.begin(), frame.end());
            if (connOpt) {
                // 已有映射：定向发送到已知客户端
                sent = TcpLinkManager::instance().sendToClient(connOpt->linkId, connOpt->clientAddr, data);
                if (!sent) {
                    LOG_WARN << "[Modbus] sendToClient failed: linkId=" << connOpt->linkId
                             << " client=" << connOpt->clientAddr << " device=" << ctx.deviceName;
                }
            } else {
                // RTU 从站无注册包：向该链路所有客户端广播，收到响应后自动建立映射
                sent = TcpLinkManager::instance().sendData(ctx.linkId, data);
                if (sent) {
                    LOG_DEBUG << "[Modbus] Broadcast query for slave=" << static_cast<int>(ctx.slaveId)
                              << " on link " << ctx.linkId << " (no mapping yet)";
                } else {
                    LOG_WARN << "[Modbus] No clients on link " << ctx.linkId
                             << ", device=" << ctx.deviceName;
                }
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
            pending.groupIndex = groupIndex;
            pending.sentTime = std::chrono::steady_clock::now();
            pending.transactionId = req.transactionId;

            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingRequests_[pendingKey].push_back(std::move(pending));
        }
    }

    // ==================== 响应处理 ====================

    /**
     * @brief 处理解析后的 Modbus 响应
     */
    Task<void> processResponse(int linkId, [[maybe_unused]] const std::string& clientAddr, const ModbusResponse& response) {
        // 写回显：通知成功，不保存数据
        if (isWriteFunctionCode(response.functionCode)) {
            handleWriteResponse(linkId, response, true);
            co_return;
        }

        // 查找对应的 PendingRequest（FIFO：取队列头部）
        std::string pendingKey = std::to_string(linkId) + ":"
            + std::to_string(response.slaveId) + ":"
            + std::to_string(response.functionCode);

        PendingRequest pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingRequests_.find(pendingKey);
            if (it == pendingRequests_.end() || it->second.empty()) {
                LOG_WARN << "[Modbus] No pending request for key=" << pendingKey
                         << " (unexpected response or already timed out)";
                co_return;
            }
            pending = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pendingRequests_.erase(it);
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
        totalResponses_.fetch_add(1, std::memory_order_relaxed);
        totalLatencyMs_.fetch_add(elapsed, std::memory_order_relaxed);

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

        // 写优先：当前组响应后，先检查是否有待发写命令
        size_t nextIdx = pending.groupIndex + 1;
        bool allGroupsDone = (nextIdx >= ctx.readGroups.size());
        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            readResumeIdx_[ctx.deviceId] = allGroupsDone ? SIZE_MAX : nextIdx;
        }
        bool writeStarted = tryStartPendingWrite(ctx.linkId, ctx.slaveId);
        if (!writeStarted) {
            {
                std::lock_guard<std::mutex> lk(pendingMutex_);
                readResumeIdx_.erase(ctx.deviceId);
            }
            if (allGroupsDone) {
                onReadCycleComplete(ctx.deviceId);
            } else {
                sendReadRequest(ctx, ctx.readGroups[nextIdx], nextIdx);
            }
        }
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
                    bool value = ModbusUtils::extractBit(responseData.data(), bitOffset, responseData.size());
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
                        // FLOAT32 / DOUBLE：按配置的小数位数截断
                        if (reg->decimals >= 0) {
                            double factor = std::pow(10.0, reg->decimals);
                            value = std::round(value * factor) / factor;
                        }
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

            // 合并到内存缓冲区，获取当前设备所有已接收寄存器的完整快照
            Json::Value dataObj(Json::objectValue);
            {
                std::lock_guard<std::mutex> mlock(mergedRegsMutex_);
                auto& merged = deviceMergedRegs_[ctx.deviceId];
                for (const auto& [key, elem] : registerValues) {
                    merged[key] = elem;
                }
                dataObj = merged;
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

            // 更新实时数据缓存（data 已包含所有寄存器类型的合并值）
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
        Json::Value offlineDeviceIds(Json::arrayValue);
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            for (const auto& [deviceId, ctx] : deviceContexts_) {
                if (ctx.linkId == linkId) {
                    RealtimeDataCache::instance().clearLatestTime(deviceId);
                    offlineDeviceIds.append(deviceId);
                }
            }
        }
        if (!offlineDeviceIds.empty()) {
            LOG_INFO << "[Modbus] Marked " << offlineDeviceIds.size()
                     << " device(s) offline, link=" << linkId;
            ResourceVersion::instance().incrementVersion("device");
            broadcastDeviceOffline(offlineDeviceIds);
        }
    }

    /**
     * @brief 通过客户端地址查找并标记对应设备为离线（TCP Server 断连时调用）
     * 在 removeByClient 之前调用，此时 DeviceConnectionCache 仍有映射
     */
    void markDeviceOfflineByClient(int linkId, const std::string& clientAddr) {
        Json::Value offlineDeviceIds(Json::arrayValue);
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            for (const auto& [deviceId, ctx] : deviceContexts_) {
                if (ctx.linkId != linkId) continue;

                std::string connKey = "modbus_" + std::to_string(ctx.slaveId);
                auto connOpt = DeviceConnectionCache::instance().getConnection(connKey);
                if (connOpt && connOpt->linkId == linkId && connOpt->clientAddr == clientAddr) {
                    RealtimeDataCache::instance().clearLatestTime(deviceId);
                    offlineDeviceIds.append(deviceId);
                }
            }
        }
        if (!offlineDeviceIds.empty()) {
            LOG_INFO << "[Modbus] Marked " << offlineDeviceIds.size()
                     << " device(s) offline, client=" << clientAddr;
            ResourceVersion::instance().incrementVersion("device");
            broadcastDeviceOffline(offlineDeviceIds);
        }
    }

    /**
     * @brief 推送设备离线事件到前端
     */
    void broadcastDeviceOffline(const Json::Value& deviceIds) {
        if (WebSocketManager::instance().connectionCount() > 0) {
            Json::Value payload;
            payload["deviceIds"] = deviceIds;
            WebSocketManager::instance().broadcast("device:offline", payload);
        }
    }

    // ==================== 值校验 ====================

    /**
     * @brief 校验写入值是否在数据类型允许范围内
     */
    static void validateModbusValue(double value, DataType dataType, const std::string& name) {
        switch (dataType) {
            case DataType::BOOL:
                if (value != 0.0 && value != 1.0) {
                    throw ValidationException("寄存器「" + name + "」BOOL 类型只能为 0 或 1");
                }
                break;
            case DataType::INT16:
                if (value < -32768.0 || value > 32767.0) {
                    throw ValidationException("寄存器「" + name + "」INT16 范围 -32768 ~ 32767");
                }
                break;
            case DataType::UINT16:
                if (value < 0.0 || value > 65535.0) {
                    throw ValidationException("寄存器「" + name + "」UINT16 范围 0 ~ 65535");
                }
                break;
            case DataType::INT32:
                if (value < -2147483648.0 || value > 2147483647.0) {
                    throw ValidationException("寄存器「" + name + "」INT32 范围 -2147483648 ~ 2147483647");
                }
                break;
            case DataType::UINT32:
                if (value < 0.0 || value > 4294967295.0) {
                    throw ValidationException("寄存器「" + name + "」UINT32 范围 0 ~ 4294967295");
                }
                break;
            case DataType::FLOAT32:
                if (!std::isfinite(static_cast<float>(value))) {
                    throw ValidationException("寄存器「" + name + "」FLOAT32 值超出范围");
                }
                break;
            case DataType::INT64:
            case DataType::UINT64:
                if (dataType == DataType::UINT64 && value < 0.0) {
                    throw ValidationException("寄存器「" + name + "」UINT64 不能为负数");
                }
                break;
            case DataType::DOUBLE:
                if (!std::isfinite(value)) {
                    throw ValidationException("寄存器「" + name + "」DOUBLE 值不合法");
                }
                break;
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

    // ==================== 写响应处理 ====================

    /** 判断是否为写功能码 */
    static bool isWriteFunctionCode(uint8_t fc) {
        return fc == FuncCodes::WRITE_SINGLE_COIL ||
               fc == FuncCodes::WRITE_SINGLE_REGISTER ||
               fc == FuncCodes::WRITE_MULTIPLE_REGISTERS;
    }

    /**
     * @brief 处理写回显/写异常响应
     * @param success true=写回显成功, false=写异常失败
     */
    void handleWriteResponse(int linkId, const ModbusResponse& response, bool success) {
        std::string pendingKey = std::to_string(linkId) + ":"
            + std::to_string(response.slaveId) + ":"
            + std::to_string(response.functionCode);

        PendingWriteRequest pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingWriteRequests_.find(pendingKey);
            if (it == pendingWriteRequests_.end() || it->second.empty()) {
                LOG_WARN << "[Modbus] No pending write for key=" << pendingKey;
                return;
            }
            pending = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pendingWriteRequests_.erase(it);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pending.sentTime).count();

        if (success) {
            LOG_INFO << "[Modbus] Write echo OK: slave=" << static_cast<int>(response.slaveId)
                     << " FC=" << static_cast<int>(response.functionCode)
                     << " latency=" << elapsed << "ms";
        } else {
            LOG_WARN << "[Modbus] Write FAILED: slave=" << static_cast<int>(response.slaveId)
                     << " FC=" << static_cast<int>(response.functionCode)
                     << " exception=" << static_cast<int>(response.exceptionCode)
                     << " latency=" << elapsed << "ms";
        }

        if (commandResponseCallback_) {
            commandResponseCallback_(pending.deviceCode, "MODBUS_WRITE", success, 0);
        }

        // 发送队列中的下一帧
        trySendNextWrite(linkId, response.slaveId);
    }

    // ==================== 写入队列管理 ====================

    /** 队列中待发送的写帧 */
    struct QueuedWriteFrame {
        std::vector<uint8_t> frame;
        uint8_t functionCode;
        uint16_t address;
        uint16_t transactionId;
        std::string deviceCode;
        int deviceId;
        int linkId;
        std::string linkMode;
        uint8_t slaveId;
        std::string deviceName;
    };

    /** 设备写入队列 */
    struct DeviceWriteQueue {
        std::deque<QueuedWriteFrame> frames;
        bool sending = false;  // 是否有帧在等待应答
    };

    static std::string makeWriteQueueKey(int linkId, uint8_t slaveId) {
        return std::to_string(linkId) + ":" + std::to_string(slaveId);
    }

    /**
     * @brief 将帧加入写入队列
     *
     * 若当前无读/写操作进行中则立即发送第一帧；
     * 若读操作正在进行则仅入队，等读周期最后一组完成后由 tryStartPendingWrite 触发。
     */
    void enqueueWrites(const std::string& queueKey, std::vector<QueuedWriteFrame> frames) {
        QueuedWriteFrame frameToSend;
        bool shouldSend = false;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto& queue = writeQueues_[queueKey];

            for (auto& f : frames) {
                queue.frames.push_back(std::move(f));
            }

            if (!queue.sending) {
                // 检查是否有读操作正在进行（读周期未结束，写需等待）
                std::string readPrefix = queueKey + ":";  // "linkId:slaveId:"
                bool readInProgress = false;
                for (const auto& [key, q] : pendingRequests_) {
                    if (key.compare(0, readPrefix.size(), readPrefix) == 0 && !q.empty()) {
                        readInProgress = true;
                        break;
                    }
                }

                if (!readInProgress) {
                    queue.sending = true;
                    frameToSend = std::move(queue.frames.front());
                    queue.frames.pop_front();
                    shouldSend = true;
                } else {
                    LOG_INFO << "[Modbus] Write deferred (read in progress): "
                             << queue.frames.back().deviceName
                             << " queued=" << queue.frames.size() << " frame(s)";
                }
            } else {
                LOG_INFO << "[Modbus] Write queued: " << queue.frames.back().deviceName
                         << " pending=" << queue.frames.size() << " frame(s)";
            }
        }

        if (shouldSend) {
            sendSingleWriteFrame(frameToSend);
        }
    }

    /**
     * @brief 读周期完成后检查并启动延迟的写操作
     *
     * 在读周期最后一组的响应处理完成后调用（nextIdx >= readGroups.size()），
     * 此时 pendingRequests_ 已清空，可以安全启动写队列。
     */
    // 返回 true 表示启动了写操作，false 表示无写操作待执行（读周期真正完成）
    bool tryStartPendingWrite(int linkId, uint8_t slaveId) {
        std::string queueKey = makeWriteQueueKey(linkId, slaveId);
        QueuedWriteFrame frameToSend;
        bool shouldSend = false;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = writeQueues_.find(queueKey);
            if (it == writeQueues_.end() || it->second.sending || it->second.frames.empty()) return false;

            it->second.sending = true;
            frameToSend = std::move(it->second.frames.front());
            it->second.frames.pop_front();
            shouldSend = true;
        }

        if (shouldSend) {
            LOG_INFO << "[Modbus] Starting deferred write after read cycle: " << frameToSend.deviceName;
            sendSingleWriteFrame(frameToSend);
        }
        return shouldSend;
    }

    /**
     * @brief 读周期（含写操作）全部完成时调用
     *
     * 记录"下次允许轮询的时间点"= now + readInterval，
     * 并通过 runAfter 在 readInterval 后触发下一轮。
     * runEvery 定时器在 nextAllowedPollTime_ 到期前会被 pollDevice 内的检查拦截，
     * 保证始终是"读完 → 等间隔 → 再读"的语义。
     */
    void onReadCycleComplete(int deviceId) {
        DeviceContext ctx;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            auto it = deviceContexts_.find(deviceId);
            if (it == deviceContexts_.end()) return;
            ctx = it->second;
        }

        auto* loop = TcpLinkManager::instance().getLinkLoop(ctx.linkId);
        if (!loop) return;

        auto allowedAt = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(ctx.readInterval * 1000);

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            nextAllowedPollTime_[deviceId] = allowedAt;
        }

        loop->runAfter(ctx.readInterval, [this, deviceId]() {
            pollDevice(deviceId);
        });

        LOG_DEBUG << "[Modbus] Cycle complete, next poll in " << ctx.readInterval
                  << "s: " << ctx.deviceName;
    }

    /**
     * @brief 写队列清空后，按 linkId + slaveId 恢复被打断的读周期
     *
     * 若写操作是在读周期中途触发的，readResumeIdx_ 中记录了断点索引：
     *   - resumeIdx < readGroups.size() → 从该索引继续发送读组
     *   - resumeIdx == SIZE_MAX          → 读组已全部完成，进入间隔等待
     * 若写操作是在两轮读之间触发的（readResumeIdx_ 无记录），直接进入间隔等待。
     */
    void resumeReadAfterWrite(int linkId, uint8_t slaveId) {
        std::vector<int> deviceIds;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            for (const auto& [devId, ctx] : deviceContexts_) {
                if (ctx.linkId == linkId && ctx.slaveId == slaveId) {
                    deviceIds.push_back(devId);
                }
            }
        }

        for (int devId : deviceIds) {
            size_t resumeIdx = SIZE_MAX;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto it = readResumeIdx_.find(devId);
                if (it != readResumeIdx_.end()) {
                    resumeIdx = it->second;
                    readResumeIdx_.erase(it);
                }
            }

            if (resumeIdx == SIZE_MAX) {
                // 读组已全部完成，或写发生在两轮读之间
                onReadCycleComplete(devId);
            } else {
                DeviceContext ctx;
                {
                    std::lock_guard<std::mutex> lock(contextMutex_);
                    auto it = deviceContexts_.find(devId);
                    if (it == deviceContexts_.end()) continue;
                    ctx = it->second;
                }
                if (resumeIdx < ctx.readGroups.size()) {
                    LOG_DEBUG << "[Modbus] Resuming read after write at group "
                              << resumeIdx << ": " << ctx.deviceName;
                    sendReadRequest(ctx, ctx.readGroups[resumeIdx], resumeIdx);
                } else {
                    onReadCycleComplete(devId);
                }
            }
        }
    }

    /**
     * @brief 发送单个写帧并注册待应答
     */
    void sendSingleWriteFrame(const QueuedWriteFrame& qf) {
        bool sent = false;
        if (qf.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
            std::string data(qf.frame.begin(), qf.frame.end());
            sent = TcpLinkManager::instance().sendData(qf.linkId, data);
        } else {
            std::string connKey = "modbus_" + std::to_string(qf.slaveId);
            auto connOpt = DeviceConnectionCache::instance().getConnection(connKey);
            if (connOpt) {
                std::string data(qf.frame.begin(), qf.frame.end());
                sent = TcpLinkManager::instance().sendToClient(connOpt->linkId, connOpt->clientAddr, data);
            } else {
                LOG_WARN << "[Modbus] writeRegister: no connection for slave="
                         << static_cast<int>(qf.slaveId);
            }
        }

        if (sent) {
            LOG_INFO << "[Modbus] Write TX: " << qf.deviceName
                     << " slave=" << static_cast<int>(qf.slaveId)
                     << " FC=" << static_cast<int>(qf.functionCode)
                     << " addr=" << qf.address
                     << " | " << ModbusUtils::toHexString(qf.frame);

            std::string pendingKey = std::to_string(qf.linkId) + ":"
                + std::to_string(qf.slaveId) + ":"
                + std::to_string(qf.functionCode);

            PendingWriteRequest pending;
            pending.deviceId = qf.deviceId;
            pending.deviceCode = qf.deviceCode;
            pending.sentTime = std::chrono::steady_clock::now();
            pending.transactionId = qf.transactionId;

            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingWriteRequests_[pendingKey].push_back(std::move(pending));
            }
        } else {
            LOG_ERROR << "[Modbus] Write send failed: " << qf.deviceName
                      << " FC=" << static_cast<int>(qf.functionCode)
                      << " addr=" << qf.address;
            // 发送失败，清空队列（连接可能已断开）
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                writeQueues_.erase(makeWriteQueueKey(qf.linkId, qf.slaveId));
            }
            if (commandResponseCallback_) {
                commandResponseCallback_(qf.deviceCode, "MODBUS_WRITE", false, 0);
            }
        }
    }

    /**
     * @brief 尝试从队列发送下一帧（写响应处理后调用）
     */
    void trySendNextWrite(int linkId, uint8_t slaveId) {
        std::string queueKey = makeWriteQueueKey(linkId, slaveId);
        QueuedWriteFrame nextFrame;
        bool shouldSend = false;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = writeQueues_.find(queueKey);
            if (it == writeQueues_.end()) return;

            if (!it->second.frames.empty()) {
                nextFrame = std::move(it->second.frames.front());
                it->second.frames.pop_front();
                shouldSend = true;
            } else {
                writeQueues_.erase(it);
            }
        }

        if (shouldSend) {
            sendSingleWriteFrame(nextFrame);
        } else {
            // 写队列已清空，恢复被打断的读周期（或进入间隔等待）
            resumeReadAfterWrite(linkId, slaveId);
        }
    }

    /**
     * @brief 清理指定链路的所有待处理状态（连接变化时调用）
     *
     * 包括：待应答读请求、待应答写请求（触发失败回调）、写入队列
     */
    void clearPendingStateForLink(int linkId) {
        std::string prefix = std::to_string(linkId) + ":";
        std::vector<PendingWriteRequest> failedWrites;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);

            // 清理待应答读请求
            for (auto it = pendingRequests_.begin(); it != pendingRequests_.end(); ) {
                if (it->first.compare(0, prefix.size(), prefix) == 0) {
                    it = pendingRequests_.erase(it);
                } else {
                    ++it;
                }
            }

            // 清理待应答写请求（收集失败回调数据）
            for (auto it = pendingWriteRequests_.begin(); it != pendingWriteRequests_.end(); ) {
                if (it->first.compare(0, prefix.size(), prefix) == 0) {
                    for (auto& req : it->second) {
                        failedWrites.push_back(std::move(req));
                    }
                    it = pendingWriteRequests_.erase(it);
                } else {
                    ++it;
                }
            }

            // 清理写入队列
            for (auto it = writeQueues_.begin(); it != writeQueues_.end(); ) {
                if (it->first.compare(0, prefix.size(), prefix) == 0) {
                    it = writeQueues_.erase(it);
                } else {
                    ++it;
                }
            }

        }

        // 清理读恢复点（两把锁不能同时持有，分两步查询）
        {
            std::vector<int> deviceIds;
            {
                std::lock_guard<std::mutex> lock(contextMutex_);
                for (const auto& [devId, ctx] : deviceContexts_) {
                    if (ctx.linkId == linkId) deviceIds.push_back(devId);
                }
            }
            std::lock_guard<std::mutex> lock(pendingMutex_);
            for (int devId : deviceIds) readResumeIdx_.erase(devId);
        }

        // 锁释放后触发写请求失败回调
        if (commandResponseCallback_) {
            for (const auto& req : failedWrites) {
                commandResponseCallback_(req.deviceCode, "MODBUS_WRITE", false, 0);
            }
        }

        if (!failedWrites.empty()) {
            LOG_WARN << "[Modbus] Link " << linkId << " cleared " << failedWrites.size()
                     << " pending write request(s) on disconnect";
        }
    }

    // ==================== 成员变量 ====================

    /** 待应答读请求 */
    struct PendingRequest {
        int deviceId;
        ReadGroup group;
        size_t groupIndex = 0;  // 在 readGroups 中的下标，用于串行轮询触发下一组
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

    // 待应答读请求队列: "linkId:slaveId:fc" → FIFO queue
    // 同一 key 可能有多个请求（同从站同FC的多个 ReadGroup），按发送顺序匹配响应
    std::map<std::string, std::deque<PendingRequest>> pendingRequests_;
    std::mutex pendingMutex_;

    /** 待应答写请求 */
    struct PendingWriteRequest {
        int deviceId;
        std::string deviceCode;  // 用于回调匹配 ProtocolDispatcher 的 pendingCommands_ key
        std::chrono::steady_clock::time_point sentTime;
        uint16_t transactionId;
    };

    // 待应答写请求队列: "linkId:slaveId:fc" → FIFO queue（共用 pendingMutex_）
    std::map<std::string, std::deque<PendingWriteRequest>> pendingWriteRequests_;

    // 写入队列: "linkId:slaveId" → DeviceWriteQueue（共用 pendingMutex_）
    std::map<std::string, DeviceWriteQueue> writeQueues_;

    // 下次允许轮询的时间点: deviceId → 时间点（共用 pendingMutex_）
    // 在读周期完成时设置为 now + readInterval，pollDevice 遇到未到期的时间点则跳过
    std::map<int, std::chrono::steady_clock::time_point> nextAllowedPollTime_;

    // 写优先暂停读的恢复点: deviceId → 下次应发送的 ReadGroup 索引（共用 pendingMutex_）
    // SIZE_MAX 表示全部读组已完成（写结束后直接进入 onReadCycleComplete）
    std::map<int, size_t> readResumeIdx_;

    // 指令应答回调
    CommandResponseCallback commandResponseCallback_;

    // 寄存器值合并缓冲区: deviceId → 已接收到的所有寄存器值（跨 ReadGroup 累积）
    // 解决多 ReadGroup 轮询时各批响应互相覆盖的问题
    std::map<int, Json::Value> deviceMergedRegs_;
    std::mutex mergedRegsMutex_;

    // Transaction ID 计数器
    std::atomic<uint16_t> transactionCounter_{0};

    // 性能统计计数器（原子操作，无锁）
    std::atomic<int64_t> totalResponses_{0};
    std::atomic<int64_t> totalLatencyMs_{0};
    std::atomic<int64_t> totalTimeouts_{0};
    std::atomic<int64_t> totalCrcErrors_{0};
    std::atomic<int64_t> totalExceptions_{0};
};

}  // namespace modbus
