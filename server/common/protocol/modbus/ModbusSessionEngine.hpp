#pragma once

#include "RegistrationNormalizer.hpp"
#include "common/protocol/ParsedResult.hpp"
#include "common/network/LinkTransportFacade.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace modbus {

/**
 * @brief Session 粒度的 Modbus 引擎
 *
 * 设计约束：
 * - 调度单位是 DTU session，不是 slaveId
 * - 一个 session 任意时刻只允许一个 in-flight 请求
 * - 未绑定 session 只允许 discovery 查询
 */
class ModbusSessionEngine {
public:
    inline static constexpr const char* FUNC_READ = "MODBUS_READ";
    inline static constexpr const char* FUNC_WRITE = "MODBUS_WRITE";
    inline static constexpr auto REQUEST_TIMEOUT = std::chrono::milliseconds(5000);
    inline static constexpr auto DISCOVERY_RETRY_DELAY = std::chrono::seconds(1);

    struct ProcessResult {
        std::vector<ParsedFrameResult> parsedResults;
        bool handledInflight = false;
    };

    struct PreparedWrite {
        int linkId = 0;
        std::string clientAddr;
        std::vector<ModbusJob> jobs;
        std::string frameHex;

        bool empty() const {
            return linkId <= 0 || clientAddr.empty() || jobs.empty();
        }
    };

    struct ModbusStats {
        int64_t totalResponses = 0;
        double avgLatencyMs = 0.0;
        int64_t timeouts = 0;
        int64_t crcErrors = 0;
        int64_t exceptions = 0;
    };

    struct PollCycleAggregate {
        std::string reportTime;
        Json::Value data = Json::Value(Json::objectValue);
    };

    using CommandCompletionCallback = std::function<void(
        const std::string& commandKey, const std::string& responseCode, bool success, int64_t responseRecordId, int deviceId)>;
    using ReadCompletionCallback = std::function<void(int deviceId, size_t readGroupIndex, bool success)>;

    ModbusSessionEngine(DtuRegistry& registry, DtuSessionManager& sessions)
        : registry_(registry), sessions_(sessions), normalizer_(registry, sessions) {}

    void setCommandCompletionCallback(CommandCompletionCallback cb) {
        commandCompletionCallback_ = std::move(cb);
    }

    void setReadCompletionCallback(ReadCompletionCallback cb) {
        readCompletionCallback_ = std::move(cb);
    }

    /** 连接事件 */
    void onConnected(int linkId, const std::string& clientAddr);
    void onDisconnected(int linkId, const std::string& clientAddr);

    /** 接收数据：先归一化注册码，再解析纯 Modbus payload */
    ProcessResult onBytes(
        int linkId,
        const std::string& clientAddr,
        const std::vector<uint8_t>& bytes);

    /** 接收已经完成注册码归一化的纯 Modbus payload */
    ProcessResult onPayload(
        int linkId,
        const std::string& clientAddr,
        const std::vector<uint8_t>& payload);

    /** 向 session 投递轮询读任务 */
    bool enqueuePoll(int deviceId, size_t readGroupIndex = 0);

    /** 为未绑定 session 触发一条真实 discovery 查询 */
    bool triggerDiscovery(int linkId, const std::string& clientAddr);

    /** 处理 session 内请求超时 */
    ProcessResult processTimeouts();

    /** 清除所有设备的 poll cycle 聚合数据（配置热重载时调用） */
    void clearAllPollCycles();

    /** 独立注册码已完成绑定时，取消残留的 discovery 请求 */
    void cancelDiscovery(int linkId, const std::string& clientAddr);

    /** 预构建写任务，但不发送 */
    std::optional<PreparedWrite> prepareWrite(
        int deviceId,
        const std::string& commandKey,
        const Json::Value& elements);

    /** 提交已预构建的写任务并尝试发送 */
    bool submitPreparedWrite(PreparedWrite prepared);

    /** 向 session 投递写任务 */
    bool enqueueWrite(int deviceId, const std::string& commandKey, const Json::Value& elements);

    ModbusStats getModbusStats() const {
        auto responses = totalResponses_.load(std::memory_order_relaxed);
        auto latency = totalLatencyMs_.load(std::memory_order_relaxed);

        ModbusStats stats;
        stats.totalResponses = responses;
        stats.avgLatencyMs = responses > 0
            ? static_cast<double>(latency) / static_cast<double>(responses)
            : 0.0;
        stats.timeouts = totalTimeouts_.load(std::memory_order_relaxed);
        stats.crcErrors = totalCrcErrors_.load(std::memory_order_relaxed);
        stats.exceptions = totalExceptions_.load(std::memory_order_relaxed);
        return stats;
    }

private:
    static bool isWriteFunctionCode(uint8_t fc) {
        return fc == FuncCodes::WRITE_SINGLE_COIL
            || fc == FuncCodes::WRITE_SINGLE_REGISTER
            || fc == FuncCodes::WRITE_MULTIPLE_REGISTERS;
    }

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

    static std::string makeUtcNowString() {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{now - dp};

        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << static_cast<int>(ymd.year()) << "-"
            << std::setw(2) << static_cast<unsigned>(ymd.month()) << "-"
            << std::setw(2) << static_cast<unsigned>(ymd.day()) << "T"
            << std::setw(2) << hms.hours().count() << ":"
            << std::setw(2) << hms.minutes().count() << ":"
            << std::setw(2) << hms.seconds().count() << "Z";
        return oss.str();
    }

    std::optional<FrameMode> getSessionFrameMode(const DtuSession& session) const;
    std::vector<ModbusResponse> appendAndParseSessionFrames(
        DtuSession& session,
        FrameMode mode,
        const std::vector<uint8_t>& payload) const;
    std::optional<InflightRequest> takeMatchingInflight(
        int linkId,
        const std::string& clientAddr,
        const ModbusResponse& response);
    bool tryDispatchNext(int linkId, const std::string& clientAddr);
    std::optional<ModbusJob> buildDiscoveryJob(
        const DtuDefinition& dtu,
        uint16_t transactionId) const;
    bool triggerDiscoveryForSession(const DtuSession& sessionSnapshot);
    std::vector<ModbusJob> buildWriteJobs(
        const ModbusDeviceDef& device,
        const std::string& commandKey,
        const Json::Value& elements);
    std::map<std::string, Json::Value> extractRegisterValues(
        const ModbusDeviceDef& device,
        const ReadGroupSnapshot& group,
        const std::vector<uint8_t>& responseData) const;
    Json::Value buildReadDataObject(const std::map<std::string, Json::Value>& values) const;
    ParsedFrameResult buildReadResult(
        const ModbusDeviceDef& device,
        const Json::Value& values,
        const std::string& reportTime) const;
    void startPollCycle(int deviceId);
    void appendPollCycleValues(int deviceId, const std::map<std::string, Json::Value>& values);
    std::optional<ParsedFrameResult> finishPollCycle(const ModbusDeviceDef& device);
    void clearPollCycle(int deviceId);
    bool sendFrame(const DtuSession& session, const ModbusDeviceDef& device, const std::vector<uint8_t>& frame, ModbusJobKind jobKind) const;

    mutable std::atomic<int64_t> totalResponses_{0};
    mutable std::atomic<int64_t> totalLatencyMs_{0};
    mutable std::atomic<int64_t> totalTimeouts_{0};
    mutable std::atomic<int64_t> totalCrcErrors_{0};
    mutable std::atomic<int64_t> totalExceptions_{0};
    DtuRegistry& registry_;
    DtuSessionManager& sessions_;
    RegistrationNormalizer normalizer_;
    CommandCompletionCallback commandCompletionCallback_;
    ReadCompletionCallback readCompletionCallback_;
    std::atomic<uint16_t> transactionCounter_{1};
    mutable std::mutex pollCycleMutex_;
    std::map<int, PollCycleAggregate> pollCycleAggregates_;
};

inline std::optional<FrameMode> ModbusSessionEngine::getSessionFrameMode(const DtuSession& session) const {
    if (session.dtuKey.empty()) return std::nullopt;

    auto dtuOpt = registry_.findByDtuKey(session.dtuKey);
    if (!dtuOpt || dtuOpt->devicesBySlave.empty()) return std::nullopt;
    return dtuOpt->devicesBySlave.begin()->second.frameMode;
}

inline std::vector<ModbusResponse> ModbusSessionEngine::appendAndParseSessionFrames(
    DtuSession& session,
    FrameMode mode,
    const std::vector<uint8_t>& payload) const {

    std::vector<ModbusResponse> parsedFrames;

    // 防止 rxBuffer 无限增长（正常 Modbus 响应不超过 260 字节）
    static constexpr size_t MAX_RX_BUFFER_SIZE = 4096;
    if (session.rxBuffer.size() + payload.size() > MAX_RX_BUFFER_SIZE) {
        LOG_WARN << "[Modbus][SessionEngine] rxBuffer overflow for session "
                 << session.clientAddr << ", clearing ("
                 << session.rxBuffer.size() + payload.size() << " bytes)";
        session.rxBuffer.clear();
    }

    session.rxBuffer.insert(session.rxBuffer.end(), payload.begin(), payload.end());

    while (!session.rxBuffer.empty()) {
        if (mode == FrameMode::RTU) {
            size_t skip = ModbusUtils::skipNonRtuData(session.rxBuffer);
            if (skip > 0) {
                session.rxBuffer.erase(
                    session.rxBuffer.begin(),
                    session.rxBuffer.begin() + static_cast<ptrdiff_t>(skip));
                continue;
            }
        } else {
            size_t skip = ModbusUtils::skipInvalidMbapData(session.rxBuffer);
            if (skip > 0) {
                session.rxBuffer.erase(
                    session.rxBuffer.begin(),
                    session.rxBuffer.begin() + static_cast<ptrdiff_t>(skip));
                continue;
            }
        }

        ModbusResponse response;
        size_t consumed = ModbusUtils::parseResponse(mode, session.rxBuffer, response);
        if (consumed == ModbusUtils::FRAME_CORRUPT) {
            totalCrcErrors_.fetch_add(1, std::memory_order_relaxed);
            session.rxBuffer.erase(session.rxBuffer.begin());
            continue;
        }
        if (consumed == 0) {
            break;
        }

        session.rxBuffer.erase(
            session.rxBuffer.begin(),
            session.rxBuffer.begin() + static_cast<ptrdiff_t>(consumed));
        parsedFrames.push_back(std::move(response));
    }

    return parsedFrames;
}

inline std::optional<InflightRequest> ModbusSessionEngine::takeMatchingInflight(
    int linkId,
    const std::string& clientAddr,
    const ModbusResponse& response) {

    std::optional<InflightRequest> inflight;
    sessions_.mutateSession(linkId, clientAddr, [&](DtuSession& session) {
        if (!session.inflight) return;

        bool match = session.inflight->job.slaveId == response.slaveId
            && session.inflight->functionCode == response.functionCode;
        if (match && response.transactionId > 0 && session.inflight->transactionId > 0) {
            match = response.transactionId == session.inflight->transactionId;
        }
        if (!match) return;

        inflight = std::move(session.inflight);
        session.inflight.reset();
    });
    return inflight;
}

inline bool ModbusSessionEngine::sendFrame(
    const DtuSession& session,
    const ModbusDeviceDef& device,
    const std::vector<uint8_t>& frame,
    ModbusJobKind jobKind) const {

    const std::string data(frame.begin(), frame.end());
    bool ok;
    if (device.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
        ok = LinkTransportFacade::instance().sendData(device.linkId, data);
    } else {
        ok = LinkTransportFacade::instance().sendToClient(device.linkId, session.clientAddr, data);
    }

    const char* opLabel = (jobKind == ModbusJobKind::WriteRegisters) ? "控制" : "查询";

    if (ok) {
        std::ostringstream hex;
        for (size_t i = 0; i < frame.size(); ++i) {
            if (i > 0) hex << ' ';
            hex << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(frame[i]);
        }
        LOG_DEBUG << "[Modbus][" << opLabel << "] TX " << device.deviceName
                  << "(id=" << device.deviceId << ",slave=" << static_cast<int>(device.slaveId)
                  << ") " << frame.size() << "B to "
                  << session.clientAddr << " | " << hex.str();
    } else {
        LOG_WARN << "[Modbus][" << opLabel << "] TX failed " << device.deviceName
                 << "(id=" << device.deviceId << ") to " << session.clientAddr
                 << ", " << frame.size() << "B";
        if (device.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
            LinkTransportFacade::instance().forceDisconnectClient(device.linkId);
        } else {
            LinkTransportFacade::instance().forceDisconnectServerClient(device.linkId, session.clientAddr);
        }
    }

    return ok;
}

inline bool ModbusSessionEngine::tryDispatchNext(int linkId, const std::string& clientAddr) {
    bool dispatched = false;
    std::optional<ModbusJob> failedReadJob;
    sessions_.mutateSession(linkId, clientAddr, [&](DtuSession& session) {
        if (session.inflight) return;

        auto* queue = !session.highQueue.empty() ? &session.highQueue : &session.normalQueue;
        if (!queue || queue->empty()) return;

        auto job = queue->front();
        auto deviceOpt = registry_.findDevice(job.deviceId);
        if (!deviceOpt) {
            if (job.kind == ModbusJobKind::PollRead) {
                failedReadJob = job;
            }
            if (job.kind == ModbusJobKind::DiscoveryRead) {
                session.discoveryRequested = false;
                if (session.bindState != SessionBindState::Bound) {
                    session.bindState = SessionBindState::Unknown;
                }
                session.nextDiscoveryTime = std::chrono::steady_clock::now() + DISCOVERY_RETRY_DELAY;
            }
            queue->pop_front();
            return;
        }

        if (job.requestFrame.empty()) {
            queue->pop_front();
            if (job.kind == ModbusJobKind::PollRead) {
                failedReadJob = job;
            }
            if (job.kind == ModbusJobKind::WriteRegisters && commandCompletionCallback_) {
                commandCompletionCallback_(job.commandKey, FUNC_WRITE, false, 0, job.deviceId);
            }
            if (job.kind == ModbusJobKind::DiscoveryRead) {
                session.discoveryRequested = false;
                if (session.bindState != SessionBindState::Bound) {
                    session.bindState = SessionBindState::Unknown;
                }
                session.nextDiscoveryTime = std::chrono::steady_clock::now() + DISCOVERY_RETRY_DELAY;
            }
            return;
        }

        if (!sendFrame(session, *deviceOpt, job.requestFrame, job.kind)) {
            if (job.kind == ModbusJobKind::PollRead) {
                failedReadJob = job;
            }
            if (job.kind == ModbusJobKind::WriteRegisters && commandCompletionCallback_) {
                commandCompletionCallback_(job.commandKey, FUNC_WRITE, false, 0, job.deviceId);
            }
            if (job.kind == ModbusJobKind::DiscoveryRead) {
                session.discoveryRequested = false;
                if (session.bindState != SessionBindState::Bound) {
                    session.bindState = SessionBindState::Unknown;
                }
                session.nextDiscoveryTime = std::chrono::steady_clock::now() + DISCOVERY_RETRY_DELAY;
            }
            queue->pop_front();
            return;
        }

        InflightRequest inflight;
        inflight.job = job;
        inflight.functionCode = job.requestFunctionCode;
        inflight.transactionId = job.transactionId;
        inflight.sentTime = std::chrono::steady_clock::now();

        session.inflight = std::move(inflight);
        queue->pop_front();
        dispatched = true;
    });

    if (failedReadJob) {
        clearPollCycle(failedReadJob->deviceId);
        if (readCompletionCallback_) {
            readCompletionCallback_(
                failedReadJob->deviceId,
                failedReadJob->readGroupIndex,
                false);
        }
    }

    return dispatched;
}

inline std::vector<ModbusJob> ModbusSessionEngine::buildWriteJobs(
    const ModbusDeviceDef& device,
    const std::string& commandKey,
    const Json::Value& elements) {

    std::vector<ModbusJob> jobs;
    if (!elements.isArray()) return jobs;

    std::map<std::string, const RegisterDef*> regsById;
    for (const auto& reg : device.registers) {
        regsById.emplace(reg.id, &reg);
    }

    for (const auto& elem : elements) {
        const std::string registerId = elem.get("elementId", "").asString();
        const std::string valueStr = elem.get("value", "").asString();

        auto regIt = regsById.find(registerId);
        if (regIt == regsById.end()) {
            throw NotFoundException("寄存器未找到: " + registerId);
        }

        const RegisterDef& reg = *regIt->second;
        if (!isWritable(reg.registerType)) {
            throw ForbiddenException("寄存器「" + reg.name + "」不可写");
        }

        double value = 0.0;
        try {
            value = std::stod(valueStr);
        } catch (...) {
            throw ValidationException("寄存器「" + reg.name + "」的值不是有效数字: " + valueStr);
        }

        validateModbusValue(value, reg.dataType, reg.name);

        ModbusWriteRequest request;
        request.slaveId = device.slaveId;
        request.address = reg.address;
        request.transactionId = transactionCounter_.fetch_add(1, std::memory_order_relaxed);

        if (reg.registerType == RegisterType::COIL) {
            request.functionCode = FuncCodes::WRITE_SINGLE_COIL;
            request.quantity = 1;
            request.data = {static_cast<uint8_t>(value != 0.0 ? 0xFF : 0x00), 0x00};
        } else {
            request.data = ModbusUtils::encodeValue(value, reg.dataType, device.byteOrder);
            request.quantity = dataTypeToQuantity(reg.dataType);
            request.functionCode = (request.quantity == 1)
                ? FuncCodes::WRITE_SINGLE_REGISTER
                : FuncCodes::WRITE_MULTIPLE_REGISTERS;
        }

        ModbusJob job;
        job.kind = ModbusJobKind::WriteRegisters;
        job.deviceId = device.deviceId;
        job.slaveId = device.slaveId;
        job.commandKey = commandKey;
        job.requestFrame = ModbusUtils::buildWriteRequest(device.frameMode, request);
        job.requestFunctionCode = request.functionCode;
        job.transactionId = request.transactionId;

        Json::Value single(Json::arrayValue);
        single.append(elem);
        job.writeElements = single;
        jobs.push_back(std::move(job));
    }

    return jobs;
}

inline std::map<std::string, Json::Value> ModbusSessionEngine::extractRegisterValues(
    const ModbusDeviceDef& device,
    const ReadGroupSnapshot& group,
    const std::vector<uint8_t>& responseData) const {

    std::map<std::string, Json::Value> result;
    for (const auto& reg : group.registers) {
        std::string key = registerTypeToString(reg.registerType) + "_" + std::to_string(reg.address);

        Json::Value elem;
        elem["name"] = reg.name;
        if (!reg.unit.empty()) elem["unit"] = reg.unit;

        if (isBitRegister(reg.registerType)) {
            uint16_t bitOffset = reg.address - group.startAddress;
            if (bitOffset / 8 < responseData.size()) {
                bool value = ModbusUtils::extractBit(responseData.data(), bitOffset, responseData.size());
                elem["value"] = value ? 1 : 0;
            } else {
                elem["value"] = Json::nullValue;
            }
        } else {
            size_t byteOffset = static_cast<size_t>(reg.address - group.startAddress) * 2;
            size_t byteSize = dataTypeToByteSize(reg.dataType);
            if (byteOffset + byteSize <= responseData.size()) {
                double value = ModbusUtils::extractValue(
                    responseData.data() + byteOffset, reg.dataType, device.byteOrder);

                if (reg.dataType == DataType::BOOL
                    || reg.dataType == DataType::INT16
                    || reg.dataType == DataType::UINT16
                    || reg.dataType == DataType::INT32
                    || reg.dataType == DataType::UINT32
                    || reg.dataType == DataType::INT64
                    || reg.dataType == DataType::UINT64) {
                    elem["value"] = static_cast<Json::Int64>(value);
                } else {
                    if (reg.decimals >= 0) {
                        double factor = std::pow(10.0, reg.decimals);
                        value = std::round(value * factor) / factor;
                    }
                    elem["value"] = value;
                }
            } else {
                elem["value"] = Json::nullValue;
            }
        }

        result[key] = elem;
    }

    return result;
}

inline Json::Value ModbusSessionEngine::buildReadDataObject(
    const std::map<std::string, Json::Value>& values) const {
    Json::Value data(Json::objectValue);
    for (const auto& [key, value] : values) {
        data[key] = value;
    }
    return data;
}

inline ParsedFrameResult ModbusSessionEngine::buildReadResult(
    const ModbusDeviceDef& device,
    const Json::Value& values,
    const std::string& reportTime) const {

    ParsedFrameResult result;
    result.deviceId = device.deviceId;
    result.linkId = device.linkId;
    result.protocol = Constants::PROTOCOL_MODBUS;
    result.funcCode = FUNC_READ;
    result.reportTime = reportTime.empty() ? makeUtcNowString() : reportTime;

    Json::Value json;
    json["funcCode"] = FUNC_READ;
    json["funcName"] = "读寄存器";
    json["direction"] = "UP";
    json["data"] = values.isObject() ? values : Json::Value(Json::objectValue);
    result.data = json;

    return result;
}

inline void ModbusSessionEngine::startPollCycle(int deviceId) {
    std::lock_guard<std::mutex> lock(pollCycleMutex_);
    auto& aggregate = pollCycleAggregates_[deviceId];
    aggregate.reportTime = makeUtcNowString();
    aggregate.data = Json::Value(Json::objectValue);
}

inline void ModbusSessionEngine::appendPollCycleValues(
    int deviceId,
    const std::map<std::string, Json::Value>& values) {
    if (values.empty()) return;

    std::lock_guard<std::mutex> lock(pollCycleMutex_);
    auto& aggregate = pollCycleAggregates_[deviceId];
    if (aggregate.reportTime.empty()) {
        aggregate.reportTime = makeUtcNowString();
    }
    if (!aggregate.data.isObject()) {
        aggregate.data = Json::Value(Json::objectValue);
    }

    for (const auto& [key, value] : values) {
        aggregate.data[key] = value;
    }
}

inline std::optional<ParsedFrameResult> ModbusSessionEngine::finishPollCycle(
    const ModbusDeviceDef& device) {
    PollCycleAggregate aggregate;
    {
        std::lock_guard<std::mutex> lock(pollCycleMutex_);
        auto it = pollCycleAggregates_.find(device.deviceId);
        if (it == pollCycleAggregates_.end()) return std::nullopt;
        aggregate = std::move(it->second);
        pollCycleAggregates_.erase(it);
    }

    if (!aggregate.data.isObject() || aggregate.data.empty()) {
        return std::nullopt;
    }

    return buildReadResult(device, aggregate.data, aggregate.reportTime);
}

inline void ModbusSessionEngine::clearPollCycle(int deviceId) {
    if (deviceId <= 0) return;

    std::lock_guard<std::mutex> lock(pollCycleMutex_);
    pollCycleAggregates_.erase(deviceId);
}

inline void ModbusSessionEngine::clearAllPollCycles() {
    std::lock_guard<std::mutex> lock(pollCycleMutex_);
    pollCycleAggregates_.clear();
}

inline void ModbusSessionEngine::onConnected(int linkId, const std::string& clientAddr) {
    sessions_.onConnected(linkId, clientAddr);
}

inline void ModbusSessionEngine::onDisconnected(int linkId, const std::string& clientAddr) {
    auto sessionOpt = sessions_.getSession(linkId, clientAddr);
    if (sessionOpt) {
        for (const auto& [slaveId, deviceId] : sessionOpt->deviceIdsBySlave) {
            (void)slaveId;
            clearPollCycle(deviceId);
        }
    }
    sessions_.onDisconnected(linkId, clientAddr);
}

inline std::optional<ModbusJob> ModbusSessionEngine::buildDiscoveryJob(
    const DtuDefinition& dtu,
    uint16_t transactionId) const {
    if (!dtu.discoveryPlan.enabled) return std::nullopt;

    auto deviceOpt = registry_.findDevice(dtu.discoveryPlan.deviceId);
    if (!deviceOpt) return std::nullopt;
    if (dtu.discoveryPlan.readGroupIndex >= deviceOpt->readGroups.size()) return std::nullopt;

    const auto& group = deviceOpt->readGroups[dtu.discoveryPlan.readGroupIndex];
    ModbusRequest request;
    request.slaveId = deviceOpt->slaveId;
    request.functionCode = group.functionCode;
    request.startAddress = group.startAddress;
    request.quantity = group.totalQuantity;
    request.transactionId = transactionId;

    ModbusJob job;
    job.kind = ModbusJobKind::DiscoveryRead;
    job.deviceId = deviceOpt->deviceId;
    job.slaveId = deviceOpt->slaveId;
    job.readGroupIndex = dtu.discoveryPlan.readGroupIndex;
    job.requestFrame = ModbusUtils::buildRequest(deviceOpt->frameMode, request);
    job.requestFunctionCode = request.functionCode;
    job.transactionId = request.transactionId;
    return job;
}

inline ModbusSessionEngine::ProcessResult ModbusSessionEngine::onBytes(
    int linkId,
    const std::string& clientAddr,
    const std::vector<uint8_t>& bytes) {

    ProcessResult output;
    auto normalized = normalizer_.normalize(linkId, clientAddr, bytes);
    if (normalized.kind == RegistrationMatchKind::Conflict) {
        LOG_WARN << "[Modbus][SessionEngine] Registration conflict on link "
                 << linkId << " client=" << clientAddr;
        return output;
    }

    if (normalized.payload.empty()) {
        return output;
    }

    return onPayload(linkId, clientAddr, normalized.payload);
}

inline ModbusSessionEngine::ProcessResult ModbusSessionEngine::onPayload(
    int linkId,
    const std::string& clientAddr,
    const std::vector<uint8_t>& payload) {

    ProcessResult output;

    auto sessionOpt = sessions_.getSession(linkId, clientAddr);
    if (!sessionOpt || sessionOpt->bindState != SessionBindState::Bound) {
        LOG_DEBUG << "[Modbus][SessionEngine] Dropping payload from unbound session "
                  << clientAddr << " size=" << payload.size();
        return output;
    }

    auto modeOpt = getSessionFrameMode(*sessionOpt);
    if (!modeOpt) {
        LOG_WARN << "[Modbus][SessionEngine] No frame mode for session "
                 << clientAddr << " dtuKey=" << sessionOpt->dtuKey
                 << ", dropping " << payload.size() << "B payload";
        return output;
    }

    std::vector<ModbusResponse> parsed;
    sessions_.mutateSession(linkId, clientAddr, [&](DtuSession& session) {
        parsed = appendAndParseSessionFrames(session, *modeOpt, payload);
        session.lastSeen = std::chrono::steady_clock::now();
    });

    for (const auto& response : parsed) {
        auto inflightOpt = takeMatchingInflight(linkId, clientAddr, response);
        if (!inflightOpt) continue;
        output.handledInflight = true;
        if (inflightOpt->job.kind == ModbusJobKind::DiscoveryRead) {
            sessions_.mutateSession(linkId, clientAddr, [](DtuSession& session) {
                session.discoveryRequested = false;
                session.nextDiscoveryTime = std::chrono::steady_clock::time_point{};
            });
        }

        auto deviceOpt = registry_.findDevice(inflightOpt->job.deviceId);
        if (!deviceOpt) {
            if (inflightOpt->job.kind != ModbusJobKind::WriteRegisters && readCompletionCallback_) {
                clearPollCycle(inflightOpt->job.deviceId);
                readCompletionCallback_(inflightOpt->job.deviceId, inflightOpt->job.readGroupIndex, false);
            }
            tryDispatchNext(linkId, clientAddr);
            continue;
        }

        if (response.isException) {
            totalExceptions_.fetch_add(1, std::memory_order_relaxed);
        }

        if (isWriteFunctionCode(response.functionCode)) {
            bool success = !response.isException;
            char fcHex[8];
            snprintf(fcHex, sizeof(fcHex), "0x%02X", response.functionCode);
            LOG_DEBUG << "[Modbus][控制] RX " << deviceOpt->deviceName
                      << "(id=" << deviceOpt->deviceId
                      << ",slave=" << static_cast<int>(response.slaveId)
                      << ") FC=" << fcHex
                      << (success ? " SUCCESS" : " EXCEPTION");
            if (commandCompletionCallback_) {
                commandCompletionCallback_(inflightOpt->job.commandKey, FUNC_WRITE, success, 0, inflightOpt->job.deviceId);
            }
            tryDispatchNext(linkId, clientAddr);
            continue;
        }

        if (response.isException) {
            LOG_WARN << "[Modbus][查询] RX exception " << deviceOpt->deviceName
                     << "(id=" << deviceOpt->deviceId
                     << ",slave=" << static_cast<int>(response.slaveId)
                     << ") fc=" << static_cast<int>(response.functionCode)
                     << " code=" << static_cast<int>(response.exceptionCode);
            if (inflightOpt->job.kind == ModbusJobKind::PollRead) {
                // 单个读组异常时，不中断整轮轮询；
                // 允许已成功读取的其他寄存器继续保留并在最后一组结束时落库。
                if (inflightOpt->job.readGroupIndex + 1 >= deviceOpt->readGroups.size()) {
                    auto cycleResult = finishPollCycle(*deviceOpt);
                    if (cycleResult) {
                        output.parsedResults.push_back(std::move(*cycleResult));
                    }
                }
                if (readCompletionCallback_) {
                    readCompletionCallback_(
                        inflightOpt->job.deviceId,
                        inflightOpt->job.readGroupIndex,
                        true
                    );
                }
            } else if (readCompletionCallback_) {
                clearPollCycle(inflightOpt->job.deviceId);
                readCompletionCallback_(inflightOpt->job.deviceId, inflightOpt->job.readGroupIndex, false);
            }
            tryDispatchNext(linkId, clientAddr);
            continue;
        }

        if (inflightOpt->job.readGroupIndex >= deviceOpt->readGroups.size()) {
            if (inflightOpt->job.kind != ModbusJobKind::WriteRegisters && readCompletionCallback_) {
                clearPollCycle(inflightOpt->job.deviceId);
                readCompletionCallback_(inflightOpt->job.deviceId, inflightOpt->job.readGroupIndex, false);
            }
            tryDispatchNext(linkId, clientAddr);
            continue;
        }

        const auto& group = deviceOpt->readGroups[inflightOpt->job.readGroupIndex];
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - inflightOpt->sentTime).count();
        totalResponses_.fetch_add(1, std::memory_order_relaxed);
        totalLatencyMs_.fetch_add(elapsed, std::memory_order_relaxed);

        auto values = extractRegisterValues(*deviceOpt, group, response.data);
        if (!values.empty()) {
            if (trantor::Logger::logLevel() <= trantor::Logger::kDebug) {
                char fcHex2[8];
                snprintf(fcHex2, sizeof(fcHex2), "0x%02X", response.functionCode);
                std::ostringstream oss;
                oss << "[Modbus][查询] RX " << deviceOpt->deviceName
                    << "(id=" << deviceOpt->deviceId
                    << ",slave=" << static_cast<int>(response.slaveId)
                    << ") FC=" << fcHex2
                    << " latency=" << elapsed << "ms |";
                for (const auto& [key, elem] : values) {
                    oss << " " << elem.get("name", key).asString() << "=";
                    const auto& v = elem["value"];
                    if (v.isDouble()) oss << v.asDouble();
                    else if (v.isInt64()) oss << v.asInt64();
                    else if (v.isNull()) oss << "null";
                    else oss << v.asString();
                    if (elem.isMember("unit")) oss << elem["unit"].asString();
                }
                LOG_DEBUG << oss.str();
            }
        }

        if (inflightOpt->job.kind == ModbusJobKind::PollRead) {
            appendPollCycleValues(inflightOpt->job.deviceId, values);
            if (inflightOpt->job.readGroupIndex + 1 >= deviceOpt->readGroups.size()) {
                auto cycleResult = finishPollCycle(*deviceOpt);
                if (cycleResult) {
                    output.parsedResults.push_back(std::move(*cycleResult));
                }
            }
        } else if (!values.empty()) {
            output.parsedResults.push_back(
                buildReadResult(*deviceOpt, buildReadDataObject(values), makeUtcNowString())
            );
        }

        if (inflightOpt->job.kind != ModbusJobKind::WriteRegisters && readCompletionCallback_) {
            readCompletionCallback_(inflightOpt->job.deviceId, inflightOpt->job.readGroupIndex, true);
        }

        tryDispatchNext(linkId, clientAddr);
    }

    return output;
}

inline bool ModbusSessionEngine::enqueuePoll(int deviceId, size_t readGroupIndex) {
    auto deviceOpt = registry_.findDevice(deviceId);
    if (!deviceOpt) {
        clearPollCycle(deviceId);
        return false;
    }

    auto sessionOpt = sessions_.getBoundSessionByDtuKey(deviceOpt->dtuKey);
    if (!sessionOpt) {
        clearPollCycle(deviceId);
        return false;
    }
    if (readGroupIndex >= deviceOpt->readGroups.size()) {
        clearPollCycle(deviceId);
        return false;
    }

    if (readGroupIndex == 0) {
        startPollCycle(deviceId);
    }

    const auto& group = deviceOpt->readGroups[readGroupIndex];
    ModbusRequest request;
    request.slaveId = deviceOpt->slaveId;
    request.functionCode = group.functionCode;
    request.startAddress = group.startAddress;
    request.quantity = group.totalQuantity;
    request.transactionId = transactionCounter_.fetch_add(1, std::memory_order_relaxed);

    ModbusJob job;
    job.kind = ModbusJobKind::PollRead;
    job.deviceId = deviceOpt->deviceId;
    job.slaveId = deviceOpt->slaveId;
    job.readGroupIndex = readGroupIndex;
    job.requestFrame = ModbusUtils::buildRequest(deviceOpt->frameMode, request);
    job.requestFunctionCode = request.functionCode;
    job.transactionId = request.transactionId;

    bool rejected = false;
    const bool queued = sessions_.mutateSession(sessionOpt->linkId, sessionOpt->clientAddr, [&](DtuSession& session) {
        if (session.isQueueFull()) {
            rejected = true;
            return;
        }
        session.normalQueue.push_back(job);
    });
    if (!queued || rejected) {
        clearPollCycle(deviceId);
        return false;
    }

    tryDispatchNext(sessionOpt->linkId, sessionOpt->clientAddr);
    return true;
}

inline bool ModbusSessionEngine::triggerDiscoveryForSession(const DtuSession& sessionSnapshot) {
    if (sessionSnapshot.bindState != SessionBindState::Unknown) return false;
    if (sessionSnapshot.inflight) return false;
    if (sessionSnapshot.discoveryRequested) return false;

    const auto now = std::chrono::steady_clock::now();
    if (sessionSnapshot.nextDiscoveryTime != std::chrono::steady_clock::time_point{}
        && now < sessionSnapshot.nextDiscoveryTime) {
        return false;
    }

    auto definitions = registry_.getDefinitionsByLink(sessionSnapshot.linkId);
    std::vector<DtuDefinition> candidates;
    candidates.reserve(definitions.size());
    for (const auto& dtu : definitions) {
        if (dtu.discoveryPlan.enabled) {
            candidates.push_back(dtu);
        }
    }
    if (candidates.empty()) return false;

    const size_t startIndex = sessionSnapshot.discoveryCursor % candidates.size();
    std::optional<ModbusJob> discoveryJob;
    size_t nextCursor = startIndex;
    for (size_t offset = 0; offset < candidates.size(); ++offset) {
        const size_t idx = (startIndex + offset) % candidates.size();
        discoveryJob = buildDiscoveryJob(
            candidates[idx],
            transactionCounter_.fetch_add(1, std::memory_order_relaxed));
        if (discoveryJob) {
            nextCursor = (idx + 1) % candidates.size();
            break;
        }
    }
    if (!discoveryJob) return false;

    const bool queued = sessions_.mutateSession(sessionSnapshot.linkId, sessionSnapshot.clientAddr, [&](DtuSession& session) {
        if (session.bindState == SessionBindState::Bound || session.inflight) return;

        session.bindState = SessionBindState::Probing;
        session.discoveryRequested = true;
        session.discoveryCursor = nextCursor;
        session.nextDiscoveryTime = now + REQUEST_TIMEOUT;
        session.normalQueue.push_back(std::move(*discoveryJob));
    });
    if (!queued) return false;

    return tryDispatchNext(sessionSnapshot.linkId, sessionSnapshot.clientAddr);
}

inline bool ModbusSessionEngine::triggerDiscovery(int linkId, const std::string& clientAddr) {
    (void)clientAddr;
    bool dispatched = false;
    auto sessions = sessions_.listUnknownSessions(linkId);
    for (const auto& session : sessions) {
        dispatched = triggerDiscoveryForSession(session) || dispatched;
    }
    return dispatched;
}

inline void ModbusSessionEngine::cancelDiscovery(int linkId, const std::string& clientAddr) {
    bool shouldDispatch = false;
    sessions_.mutateSession(linkId, clientAddr, [&](DtuSession& session) {
        session.discoveryRequested = false;
        session.nextDiscoveryTime = std::chrono::steady_clock::time_point{};

        if (session.inflight && session.inflight->job.kind == ModbusJobKind::DiscoveryRead) {
            session.inflight.reset();
            shouldDispatch = true;
        }

        auto keepQueue = [](std::deque<ModbusJob>& queue) {
            std::deque<ModbusJob> filtered;
            for (auto& job : queue) {
                if (job.kind != ModbusJobKind::DiscoveryRead) {
                    filtered.push_back(std::move(job));
                }
            }
            queue = std::move(filtered);
        };

        keepQueue(session.highQueue);
        keepQueue(session.normalQueue);
    });

    if (shouldDispatch) {
        tryDispatchNext(linkId, clientAddr);
    }
}

inline ModbusSessionEngine::ProcessResult ModbusSessionEngine::processTimeouts() {
    ProcessResult output;
    const auto now = std::chrono::steady_clock::now();
    auto sessions = sessions_.listSessions();

    for (const auto& sessionSnapshot : sessions) {
        if (!sessionSnapshot.inflight) continue;
        if (now - sessionSnapshot.inflight->sentTime < REQUEST_TIMEOUT) continue;

        std::optional<InflightRequest> timedOut;
        const bool cleared = sessions_.mutateSession(
            sessionSnapshot.linkId,
            sessionSnapshot.clientAddr,
            [&](DtuSession& session) {
                if (!session.inflight) return;
                if (now - session.inflight->sentTime < REQUEST_TIMEOUT) return;

                timedOut = std::move(session.inflight);
                session.inflight.reset();

                if (timedOut->job.kind == ModbusJobKind::DiscoveryRead) {
                    session.discoveryRequested = false;
                    if (session.bindState != SessionBindState::Bound) {
                        session.bindState = SessionBindState::Unknown;
                    }
                    session.nextDiscoveryTime = now + DISCOVERY_RETRY_DELAY;
                }
            });

        if (!cleared || !timedOut) continue;

        totalTimeouts_.fetch_add(1, std::memory_order_relaxed);

        if (timedOut->job.kind == ModbusJobKind::WriteRegisters) {
            if (commandCompletionCallback_) {
                commandCompletionCallback_(timedOut->job.commandKey, FUNC_WRITE, false, 0, timedOut->job.deviceId);
            }
        } else if (timedOut->job.kind == ModbusJobKind::PollRead) {
            auto deviceOpt = registry_.findDevice(timedOut->job.deviceId);
            if (deviceOpt) {
                if (timedOut->job.readGroupIndex + 1 >= deviceOpt->readGroups.size()) {
                    auto cycleResult = finishPollCycle(*deviceOpt);
                    if (cycleResult) {
                        output.parsedResults.push_back(std::move(*cycleResult));
                    }
                }
                if (readCompletionCallback_) {
                    readCompletionCallback_(
                        timedOut->job.deviceId,
                        timedOut->job.readGroupIndex,
                        true);
                }
            } else {
                clearPollCycle(timedOut->job.deviceId);
                if (readCompletionCallback_) {
                    readCompletionCallback_(
                        timedOut->job.deviceId,
                        timedOut->job.readGroupIndex,
                        false);
                }
            }
        }

        tryDispatchNext(sessionSnapshot.linkId, sessionSnapshot.clientAddr);
    }

    return output;
}

inline std::optional<ModbusSessionEngine::PreparedWrite> ModbusSessionEngine::prepareWrite(
    int deviceId,
    const std::string& commandKey,
    const Json::Value& elements) {

    auto deviceOpt = registry_.findDevice(deviceId);
    if (!deviceOpt) return std::nullopt;

    auto sessionOpt = sessions_.getBoundSessionByDtuKey(deviceOpt->dtuKey);
    if (!sessionOpt) return std::nullopt;

    auto jobs = buildWriteJobs(*deviceOpt, commandKey, elements);
    if (jobs.empty()) return std::nullopt;

    PreparedWrite prepared;
    prepared.linkId = sessionOpt->linkId;
    prepared.clientAddr = sessionOpt->clientAddr;
    prepared.jobs = jobs;

    for (const auto& job : jobs) {
        if (!prepared.frameHex.empty()) prepared.frameHex += " ";
        prepared.frameHex += ModbusUtils::toHexString(job.requestFrame);
    }

    return prepared;
}

inline bool ModbusSessionEngine::submitPreparedWrite(PreparedWrite prepared) {
    if (prepared.empty()) return false;

    bool rejected = false;
    const bool queued = sessions_.mutateSession(prepared.linkId, prepared.clientAddr, [&](DtuSession& session) {
        if (session.isQueueFull()) {
            rejected = true;
            return;
        }
        for (auto& job : prepared.jobs) {
            session.highQueue.push_back(std::move(job));
        }
    });
    if (!queued || rejected) return false;

    tryDispatchNext(prepared.linkId, prepared.clientAddr);
    return true;
}

inline bool ModbusSessionEngine::enqueueWrite(
    int deviceId, const std::string& commandKey, const Json::Value& elements) {
    auto prepared = prepareWrite(deviceId, commandKey, elements);
    if (!prepared) return false;
    return submitPreparedWrite(std::move(*prepared));
}

}  // namespace modbus
