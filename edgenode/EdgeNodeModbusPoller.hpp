#pragma once

#include "EdgeNodeDataStore.hpp"
#include "common/edgenode/AgentProtocol.hpp"
#include "common/protocol/FrameResult.hpp"
#include "common/protocol/modbus/Modbus.SessionTypes.hpp"
#include "common/protocol/modbus/Modbus.Types.hpp"
#include "common/protocol/modbus/Modbus.Utils.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief EdgeNode 端轻量 Modbus 轮询器
 *
 * 为每个 Modbus 端点下的设备构建轮询任务，
 * 周期性发送读取请求并解析响应。
 *
 * 与 Server 端 ModbusSessionEngine 不同：
 * - 无 DTU 发现流程（Agent 已知设备配置）
 * - 无复杂会话管理（直接轮询）
 * - 一次仅一个 in-flight 请求
 */
class EdgeNodeModbusPoller {
public:
    using DataCallback = std::function<void(std::vector<ParsedFrameResult>&&)>;
    using SendCallback = std::function<bool(int endpointKey, const std::string& data)>;

    void setDataCallback(DataCallback cb) {
        dataCallback_ = std::move(cb);
    }

    void setSendCallback(SendCallback cb) {
        sendCallback_ = std::move(cb);
    }

    /**
     * @brief 从 config:sync 加载 Modbus 端点配置
     */
    void loadConfig(const std::vector<agent::DeviceEndpoint>& modbusEndpoints,
                    const std::unordered_map<std::string, int>& endpointKeyMap) {
        std::lock_guard lock(mutex_);

        devices_.clear();
        endpointKeyMap_ = endpointKeyMap;

        for (const auto& ep : modbusEndpoints) {
            for (const auto& dev : ep.devices) {
                DevicePollContext ctx;
                ctx.deviceId = dev.id;
                ctx.deviceName = dev.name;
                ctx.endpointId = ep.id;
                ctx.endpointKey = endpointKeyMap.count(ep.id) ? endpointKeyMap.at(ep.id) : 0;
                ctx.slaveId = static_cast<uint8_t>(dev.slaveId > 0 ? dev.slaveId : 1);

                // 从 protocolConfig 解析 Modbus 配置
                const auto& config = dev.protocolConfig;
                // frameMode 由端点 transport 决定：serial→RTU, ethernet→TCP
                ctx.frameMode = (ep.transport == "serial")
                    ? modbus::FrameMode::RTU
                    : modbus::FrameMode::TCP;
                ctx.byteOrder = modbus::parseByteOrder(config.get("byteOrder", "BIG_ENDIAN").asString());
                ctx.readIntervalSec = config.get("readInterval", 5).asInt();
                if (ctx.readIntervalSec < 1) ctx.readIntervalSec = 1;
                if (ctx.readIntervalSec > 3600) ctx.readIntervalSec = 3600;
                if (config.isMember("packet") && config["packet"].isObject()) {
                    const auto& packet = config["packet"];
                    ctx.mergeGap = packet.get("mergeGap", DEFAULT_MERGE_GAP).asInt();
                    ctx.maxRegsPerRead = packet.get("maxQuantity", DEFAULT_MAX_REGS_PER_READ).asInt();
                }
                ctx.mergeGap = std::clamp(ctx.mergeGap, 0, 2000);
                ctx.maxRegsPerRead = std::clamp(ctx.maxRegsPerRead, 1, 125);

                // 解析寄存器定义
                if (config.isMember("registers") && config["registers"].isArray()) {
                    for (const auto& reg : config["registers"]) {
                        modbus::RegisterDef item;
                        item.id = reg.get("id", "").asString();
                        item.name = reg.get("name", "").asString();
                        item.registerType = modbus::parseRegisterType(
                            reg.get("registerType", "HOLDING_REGISTER").asString());
                        item.address = static_cast<uint16_t>(reg.get("address", 0).asUInt());
                        item.dataType = modbus::parseDataType(reg.get("dataType", "UINT16").asString());
                        item.quantity = static_cast<uint16_t>(reg.get("quantity", 1).asUInt());
                        item.unit = reg.get("unit", "").asString();
                        item.scale = reg.get("scale", 1.0).asDouble();
                        if (!std::isfinite(item.scale) || item.scale <= 0.0) {
                            item.scale = 1.0;
                        }
                        item.remark = reg.get("remark", "").asString();
                        item.decimals = reg.get("decimals", -1).asInt();
                        if (reg.isMember("dictConfig") && reg["dictConfig"].isObject()) {
                            item.dictConfig = reg["dictConfig"];
                        }
                        ctx.registers.push_back(std::move(item));
                    }
                }

                // 合并寄存器为读取组
                buildReadGroups(ctx);

                if (!ctx.readGroups.empty()) {
                    std::cout << "[EdgeNodeModbusPoller] device: id=" << ctx.deviceId
                              << ", name=" << ctx.deviceName
                              << ", slaveId=" << static_cast<int>(ctx.slaveId)
                              << ", registers=" << ctx.registers.size()
                              << ", readGroups=" << ctx.readGroups.size()
                              << ", mergeGap=" << ctx.mergeGap
                              << ", maxQuantity=" << ctx.maxRegsPerRead
                              << ", interval=" << ctx.readIntervalSec << "s" << std::endl;
                    devices_[dev.id] = std::move(ctx);
                } else {
                    std::cout << "[WARN] " << "[EdgeNodeModbusPoller] device " << dev.id
                              << " (" << dev.name << ") has no valid read groups, skipping" << std::endl;
                }
            }
        }

        std::cout << "[EdgeNodeModbusPoller] Loaded " << devices_.size() << " Modbus device(s)" << std::endl;
    }

    /**
     * @brief 定时器回调：检查是否到了轮询时间
     */
    void tick() {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        // 检查写命令超时
        if (writeState_ && writeState_->waitingWriteResponse) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - writeState_->sentAt);
            if (elapsed > REQUEST_TIMEOUT) {
                std::cout << "[WARN] " << "[AgentModbusPoller] Write response timeout for device " << writeState_->deviceId
                          << ", frame " << (writeState_->currentFrameIndex + 1)
                          << "/" << writeState_->writeFrames.size()
                          << " — proceeding (device may not echo response)" << std::endl;
                writeState_->responseBuffer.clear();
                writeState_->waitingWriteResponse = false;
                // 继续下一帧或进入回读，由回读确认写入结果
                writeState_->currentFrameIndex++;
                auto it = devices_.find(writeState_->deviceId);
                if (it != devices_.end()) {
                    dispatchNextWriteFrame(it->second);
                } else {
                    completeWriteCommand(true, "写入已发送(无响应确认)");
                }
            }
            return;  // 写命令进行中，暂停轮询
        }

        for (auto& [deviceId, ctx] : devices_) {
            if (ctx.readGroups.empty() || ctx.endpointKey <= 0) continue;

            // 检查是否有 in-flight 请求超时
            if (ctx.waitingResponse) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.requestSentAt);
                if (elapsed > REQUEST_TIMEOUT) {
                    std::cout << "[WARN] " << "[AgentModbusPoller] Timeout for device " << deviceId
                              << " readGroup=" << ctx.currentReadGroupIndex << std::endl;
                    ctx.waitingResponse = false;
                    advanceReadGroup(ctx);
                }
                continue;
            }

            // 检查轮询间隔
            auto sinceLastPoll = std::chrono::duration_cast<std::chrono::seconds>(now - ctx.lastPollAt);
            if (sinceLastPoll.count() < ctx.readIntervalSec) continue;

            // 发送下一个读取请求
            sendPollRequest(ctx);
        }
    }

    /**
     * @brief 处理 TCP 响应数据
     */
    void onData(int epKey, const std::string& /*clientAddr*/, const std::string& data) {
        std::lock_guard lock(mutex_);

        // 优先检查是否有写命令等待响应
        if (writeState_ && writeState_->waitingWriteResponse) {
            auto it = devices_.find(writeState_->deviceId);
            if (it != devices_.end() && it->second.endpointKey == epKey) {
                auto& ctx = it->second;
                std::vector<uint8_t> bytes(data.begin(), data.end());
                writeState_->responseBuffer.insert(writeState_->responseBuffer.end(), bytes.begin(), bytes.end());

                modbus::ModbusResponse resp;
                size_t consumed = modbus::ModbusUtils::parseResponse(ctx.frameMode, writeState_->responseBuffer, resp);

                if (consumed == 0) return;  // 等待更多数据
                if (consumed == modbus::ModbusUtils::FRAME_CORRUPT) {
                    completeWriteCommand(false, "写响应帧格式错误");
                    return;
                }

                writeState_->responseBuffer.clear();
                writeState_->waitingWriteResponse = false;

                if (resp.isException) {
                    completeWriteCommand(false, "设备异常: code=" + std::to_string(resp.exceptionCode));
                    return;
                }

                std::cout << "[AgentModbusPoller] Write response OK for device " << writeState_->deviceId
                          << ", frame " << (writeState_->currentFrameIndex + 1) << std::endl;

                // 下一个写帧
                writeState_->currentFrameIndex++;
                dispatchNextWriteFrame(ctx);
                return;
            }
        }

        // 查找该端点上正在等待响应的设备
        for (auto& [deviceId, ctx] : devices_) {
            if (ctx.endpointKey != epKey || !ctx.waitingResponse) continue;

            std::vector<uint8_t> bytes(data.begin(), data.end());
            ctx.responseBuffer.insert(ctx.responseBuffer.end(), bytes.begin(), bytes.end());

            modbus::ModbusResponse resp;
            size_t consumed = modbus::ModbusUtils::parseResponse(ctx.frameMode, ctx.responseBuffer, resp);

            if (consumed == 0) continue;  // 数据不完整，等待更多
            if (consumed == modbus::ModbusUtils::FRAME_CORRUPT) {
                ctx.responseBuffer.clear();
                ctx.waitingResponse = false;
                advanceReadGroup(ctx);
                continue;
            }

            // 成功解析响应
            ctx.responseBuffer.erase(ctx.responseBuffer.begin(),
                                      ctx.responseBuffer.begin() + static_cast<ptrdiff_t>(consumed));
            ctx.waitingResponse = false;

            if (resp.isException) {
                std::cout << "[WARN] " << "[AgentModbusPoller] Device " << deviceId
                          << " exception code=" << static_cast<int>(resp.exceptionCode) << std::endl;
                advanceReadGroup(ctx);
                continue;
            }

            // 从响应中提取寄存器值
            auto results = extractReadResults(ctx, resp);
            if (!results.empty() && dataCallback_) {
                dataCallback_(std::move(results));
            }

            advanceReadGroup(ctx);
            break;
        }
    }

    /**
     * @brief 执行写寄存器命令并回读确认
     *
     * 构建写帧 → 发送 → 等待写响应 → 触发 N 次立即轮询回读
     */
    void executeWriteCommand(int deviceId,
                             const Json::Value& elements,
                             int readbackCount,
                             std::function<void(bool, const std::string&)> resultCallback) {
        std::lock_guard lock(mutex_);

        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            if (resultCallback) resultCallback(false, "设备未找到");
            return;
        }

        auto& ctx = it->second;
        if (ctx.endpointKey <= 0) {
            if (resultCallback) resultCallback(false, "端点未就绪");
            return;
        }

        // 构建寄存器索引
        std::map<std::string, const modbus::RegisterDef*> regsById;
        for (const auto& reg : ctx.registers) {
            regsById[reg.id] = &reg;
        }

        // 为每个要素构建写帧
        std::vector<std::vector<uint8_t>> writeFrames;
        for (const auto& elem : elements) {
            const std::string registerId = elem.get("elementId", "").asString();
            const std::string valueStr = elem.get("value", "").asString();

            auto regIt = regsById.find(registerId);
            if (regIt == regsById.end()) {
                if (resultCallback) resultCallback(false, "寄存器未找到: " + registerId);
                return;
            }

            const auto& reg = *regIt->second;
            double value = 0.0;
            try { value = std::stod(valueStr); } catch (...) {
                if (resultCallback) resultCallback(false, "值无效: " + valueStr);
                return;
            }

            modbus::ModbusWriteRequest request;
            request.slaveId = ctx.slaveId;
            request.address = reg.address;
            request.transactionId = ctx.nextTransactionId++;

            if (reg.registerType == modbus::RegisterType::COIL) {
                request.functionCode = modbus::FuncCodes::WRITE_SINGLE_COIL;
                request.quantity = 1;
                request.data = {static_cast<uint8_t>(value != 0.0 ? 0xFF : 0x00), 0x00};
            } else {
                request.data = modbus::ModbusUtils::encodeValue(value, reg.dataType, ctx.byteOrder);
                request.quantity = modbus::dataTypeToQuantity(reg.dataType);
                request.functionCode = (request.quantity == 1)
                    ? modbus::FuncCodes::WRITE_SINGLE_REGISTER
                    : modbus::FuncCodes::WRITE_MULTIPLE_REGISTERS;
            }

            writeFrames.push_back(modbus::ModbusUtils::buildWriteRequest(ctx.frameMode, request));
        }

        // 设置写命令状态
        writeState_ = std::make_shared<WriteCommandState>();
        writeState_->deviceId = deviceId;
        writeState_->writeFrames = std::move(writeFrames);
        writeState_->readbackRemaining = readbackCount;
        writeState_->callback = std::move(resultCallback);

        // 发送第一个写帧
        dispatchNextWriteFrame(ctx);
    }

    void clear() {
        std::lock_guard lock(mutex_);
        devices_.clear();
        writeState_.reset();
    }

private:
    static constexpr auto REQUEST_TIMEOUT = std::chrono::milliseconds(5000);
    static constexpr auto READBACK_INTERVAL = std::chrono::seconds(5);
    static constexpr int DEFAULT_MERGE_GAP = 100;
    static constexpr int DEFAULT_MAX_REGS_PER_READ = 125;

    struct DevicePollContext {
        int deviceId = 0;
        std::string deviceName;
        std::string endpointId;
        int endpointKey = 0;
        uint8_t slaveId = 1;
        modbus::FrameMode frameMode = modbus::FrameMode::TCP;
        modbus::ByteOrder byteOrder = modbus::ByteOrder::Big;
        int readIntervalSec = 5;
        int mergeGap = DEFAULT_MERGE_GAP;
        int maxRegsPerRead = DEFAULT_MAX_REGS_PER_READ;

        std::vector<modbus::RegisterDef> registers;
        std::vector<modbus::ReadGroupSnapshot> readGroups;

        // 轮询状态
        size_t currentReadGroupIndex = 0;
        bool waitingResponse = false;
        std::chrono::steady_clock::time_point requestSentAt;
        std::chrono::steady_clock::time_point lastPollAt;
        std::vector<uint8_t> responseBuffer;
        uint16_t nextTransactionId = 1;

        // poll cycle 聚合（一轮所有 readGroup 完成后上报）
        Json::Value pollCycleData = Json::Value(Json::objectValue);
        bool pollCycleStarted = false;
    };

    struct WriteCommandState {
        int deviceId = 0;
        std::vector<std::vector<uint8_t>> writeFrames;
        size_t currentFrameIndex = 0;
        bool waitingWriteResponse = false;
        std::chrono::steady_clock::time_point sentAt;
        std::vector<uint8_t> responseBuffer;
        int readbackRemaining = 0;
        bool firstReadbackDone = false;
        std::function<void(bool, const std::string&)> callback;
    };

    void dispatchNextWriteFrame(DevicePollContext& ctx) {
        if (!writeState_ || writeState_->currentFrameIndex >= writeState_->writeFrames.size()) {
            // 所有写帧已发送完，立即回调成功（不等回读）
            if (writeState_ && writeState_->callback) {
                auto cb = std::move(writeState_->callback);
                cb(true, "写入成功");
            }
            // 继续异步回读（更新数据展示），但不阻塞结果上报
            startWriteReadback(ctx);
            return;
        }

        const auto& frame = writeState_->writeFrames[writeState_->currentFrameIndex];
        std::string payload(frame.begin(), frame.end());
        if (sendCallback_) { (void)sendCallback_(ctx.endpointKey, payload); }

        writeState_->waitingWriteResponse = true;
        writeState_->sentAt = std::chrono::steady_clock::now();
        writeState_->responseBuffer.clear();

        std::cout << "[AgentModbusPoller] Write frame " << (writeState_->currentFrameIndex + 1)
                  << "/" << writeState_->writeFrames.size()
                  << " sent to device " << writeState_->deviceId << std::endl;
    }

    void startWriteReadback(DevicePollContext& ctx) {
        if (!writeState_ || writeState_->readbackRemaining <= 0) {
            // 回读完成，清理写状态（回调已在写响应确认后立即触发）
            writeState_.reset();
            return;
        }

        // 第一次回读立即执行，后续按 READBACK_INTERVAL 间隔
        // 如果设备本身轮询间隔 <= READBACK_INTERVAL，只做一次立即回读即可
        bool immediate = !writeState_->firstReadbackDone;
        if (immediate) {
            writeState_->firstReadbackDone = true;
            if (ctx.readIntervalSec <= static_cast<int>(READBACK_INTERVAL.count())) {
                // 轮询间隔本身就很短，一次立即回读后恢复正常轮询
                writeState_->readbackRemaining = 0;
            }
        }

        auto delay = immediate ? std::chrono::seconds(0) : READBACK_INTERVAL;
        std::cout << "[AgentModbusPoller] Write readback: device=" << ctx.deviceId
                  << ", remaining=" << writeState_->readbackRemaining
                  << (immediate ? ", immediate" : ", next in " + std::to_string(READBACK_INTERVAL.count()) + "s")
                  << std::endl;

        ctx.lastPollAt = std::chrono::steady_clock::now()
            - std::chrono::seconds(ctx.readIntervalSec)
            + delay;
        ctx.currentReadGroupIndex = 0;
        writeState_->readbackRemaining--;
    }

    void completeWriteCommand(bool success, const std::string& message) {
        if (!writeState_) return;
        auto cb = std::move(writeState_->callback);
        writeState_.reset();
        if (cb) cb(success, message);
    }

    void buildReadGroups(DevicePollContext& ctx) {
        auto merged = modbus::ModbusUtils::mergeRegisters(
            ctx.registers,
            ctx.mergeGap,
            ctx.maxRegsPerRead
        );
        ctx.readGroups.clear();
        ctx.readGroups.reserve(merged.size());

        for (const auto& group : merged) {
            modbus::ReadGroupSnapshot snapshot;
            snapshot.registerType = group.registerType;
            snapshot.functionCode = group.functionCode;
            snapshot.startAddress = group.startAddress;
            snapshot.totalQuantity = group.totalQuantity;
            for (const auto* reg : group.registers) {
                if (reg) snapshot.registers.push_back(*reg);
            }
            ctx.readGroups.push_back(std::move(snapshot));
        }
    }

    void sendPollRequest(DevicePollContext& ctx) {
        if (ctx.currentReadGroupIndex >= ctx.readGroups.size()) {
            ctx.currentReadGroupIndex = 0;
        }

        const auto& group = ctx.readGroups[ctx.currentReadGroupIndex];

        std::cout << "[AgentModbusPoller] poll: device=" << ctx.deviceId
                  << ", slave=" << static_cast<int>(ctx.slaveId)
                  << ", fc=" << static_cast<int>(group.functionCode)
                  << ", addr=" << group.startAddress
                  << ", qty=" << group.totalQuantity
                  << ", group=" << (ctx.currentReadGroupIndex + 1) << "/" << ctx.readGroups.size() << std::endl;

        modbus::ModbusRequest req;
        req.slaveId = ctx.slaveId;
        req.functionCode = group.functionCode;
        req.startAddress = group.startAddress;
        req.quantity = group.totalQuantity;
        req.transactionId = ctx.nextTransactionId++;

        auto frame = modbus::ModbusUtils::buildRequest(ctx.frameMode, req);
        std::string payload(frame.begin(), frame.end());

        if (sendCallback_) { (void)sendCallback_(ctx.endpointKey, payload); }

        ctx.waitingResponse = true;
        ctx.requestSentAt = std::chrono::steady_clock::now();
        ctx.responseBuffer.clear();

        if (!ctx.pollCycleStarted) {
            ctx.pollCycleStarted = true;
            ctx.pollCycleData = Json::Value(Json::objectValue);
        }
    }

    void advanceReadGroup(DevicePollContext& ctx) {
        ctx.currentReadGroupIndex++;

        if (ctx.currentReadGroupIndex >= ctx.readGroups.size()) {
            // 一轮轮询完成，重置
            ctx.currentReadGroupIndex = 0;
            ctx.lastPollAt = std::chrono::steady_clock::now();

            // 如果有聚合数据，生成最终结果
            if (ctx.pollCycleStarted && !ctx.pollCycleData.empty()) {
                flushPollCycle(ctx);
            }
            ctx.pollCycleStarted = false;

            // 写命令回读阶段：一轮读完后继续下一轮或结束
            if (writeState_ && !writeState_->waitingWriteResponse
                && ctx.deviceId == writeState_->deviceId) {
                startWriteReadback(ctx);
            }
        }
    }

    std::vector<ParsedFrameResult> extractReadResults(DevicePollContext& ctx,
                                                       const modbus::ModbusResponse& resp) {
        std::vector<ParsedFrameResult> results;

        if (ctx.currentReadGroupIndex >= ctx.readGroups.size()) return results;
        const auto& group = ctx.readGroups[ctx.currentReadGroupIndex];

        // 将各寄存器值提取到 pollCycleData
        for (const auto& reg : group.registers) {
            double value = 0.0;

            if (modbus::isBitRegister(reg.registerType)) {
                uint16_t bitOffset = reg.address - group.startAddress;
                value = modbus::ModbusUtils::extractBit(resp.data.data(), bitOffset, resp.data.size())
                        ? 1.0 : 0.0;
            } else {
                uint16_t byteOffset = (reg.address - group.startAddress) * 2;
                if (byteOffset + modbus::dataTypeToByteSize(reg.dataType) <= resp.data.size()) {
                    value = modbus::ModbusUtils::extractValue(
                        resp.data.data() + byteOffset, reg.dataType, ctx.byteOrder);
                    if (std::isfinite(reg.scale) && reg.scale > 0.0) {
                        value *= reg.scale;
                    }
                }
            }

            // 限制小数位
            if (reg.decimals >= 0) {
                double factor = std::pow(10.0, reg.decimals);
                value = std::round(value * factor) / factor;
            }

            Json::Value entry(Json::objectValue);
            entry["value"] = value;
            entry["name"] = reg.name;
            if (!reg.unit.empty()) entry["unit"] = reg.unit;
            if (!reg.id.empty()) entry["elementId"] = reg.id;

            // key 格式与 Server 端 ModbusSessionEngine 一致: registerType_address
            std::string key = modbus::registerTypeToString(reg.registerType)
                + "_" + std::to_string(reg.address);
            ctx.pollCycleData[key] = entry;
        }

        return results;  // 结果在 flushPollCycle 中生成
    }

    void flushPollCycle(DevicePollContext& ctx) {
        std::cout << "[EdgeNodeModbusPoller] poll cycle complete: device=" << ctx.deviceId
                  << " (" << ctx.deviceName << ")"
                  << ", values=" << ctx.pollCycleData.size() << std::endl;

        ParsedFrameResult result;
        result.deviceId = ctx.deviceId;
        result.linkId = 0;
        result.protocol = Constants::PROTOCOL_MODBUS;
        result.funcCode = "MODBUS_READ";
        // 与 Server 端 ModbusSessionEngine::buildReadResult 格式一致
        Json::Value json(Json::objectValue);
        json["funcCode"] = "MODBUS_READ";
        json["funcName"] = "\xe8\xaf\xbb\xe5\xaf\x84\xe5\xad\x98\xe5\x99\xa8";  // "读寄存器"
        json["direction"] = "UP";
        json["data"] = std::move(ctx.pollCycleData);
        result.data = std::move(json);
        ctx.pollCycleData = Json::Value(Json::objectValue);

        // 生成 UTC 时间（chrono 提取的是 UTC 分量，标记为 +00:00）
        auto now = std::chrono::system_clock::now();
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::seconds>(now) - dp};
        std::ostringstream oss;
        oss << static_cast<int>(ymd.year()) << "-"
            << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ymd.month()) << "-"
            << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ymd.day()) << "T"
            << std::setw(2) << std::setfill('0') << hms.hours().count() << ":"
            << std::setw(2) << std::setfill('0') << hms.minutes().count() << ":"
            << std::setw(2) << std::setfill('0') << hms.seconds().count() << "+00:00";
        result.reportTime = oss.str();

        if (dataCallback_) {
            std::vector<ParsedFrameResult> results;
            results.push_back(std::move(result));
            dataCallback_(std::move(results));
        }
    }

    std::mutex mutex_;
    std::map<int, DevicePollContext> devices_;
    std::unordered_map<std::string, int> endpointKeyMap_;
    DataCallback dataCallback_;
    SendCallback sendCallback_;
    std::shared_ptr<WriteCommandState> writeState_;
};

using AgentModbusPoller = EdgeNodeModbusPoller;
