#pragma once

#include "Modbus.Types.hpp"
#include "common/protocol/ParsedResult.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace modbus {

/**
 * @brief 读取组快照
 *
 * 与旧版 ReadGroup 不同，这里持有 RegisterDef 值对象，
 * 避免配置热重载后悬空指针问题。
 */
struct ReadGroupSnapshot {
    RegisterType registerType = RegisterType::HOLDING_REGISTER;
    uint8_t functionCode = 0;
    uint16_t startAddress = 0;
    uint16_t totalQuantity = 0;
    std::vector<RegisterDef> registers;
};

/** DTU 下挂载的 Modbus 设备定义 */
struct ModbusDeviceDef {
    int deviceId = 0;
    std::string deviceCode;
    std::string deviceName;
    int linkId = 0;
    std::string linkMode;
    std::string dtuKey;
    uint8_t slaveId = 1;
    FrameMode frameMode = FrameMode::TCP;
    ByteOrder byteOrder = ByteOrder::Big;
    int readInterval = 1;
    std::vector<RegisterDef> registers;
    std::vector<ReadGroupSnapshot> readGroups;
};

/** 发现计划：首次真实查询使用的设备和读组 */
struct DiscoveryPlan {
    bool enabled = false;
    int deviceId = 0;
    uint8_t slaveId = 1;
    size_t readGroupIndex = 0;
    ReadGroupSnapshot group;
};

/** 逻辑 DTU 定义：由同 link 下相同注册码的设备聚合而成 */
struct DtuDefinition {
    std::string dtuKey;
    int linkId = 0;
    std::string linkMode;
    std::string name;
    std::vector<uint8_t> registrationBytes;
    std::vector<uint8_t> heartbeatBytes;
    bool supportsStandaloneRegistration = true;
    bool supportsPrefixedPayloadRegistration = true;
    DiscoveryPlan discoveryPlan;
    std::map<uint8_t, ModbusDeviceDef> devicesBySlave;
};

/** 会话绑定状态 */
enum class SessionBindState {
    Unknown,
    Probing,
    Bound
};

/** 在线路由：clientAddr + slaveId 唯一定位设备 */
struct OnlineRoute {
    std::string sessionKey;
    std::string dtuKey;
    int linkId = 0;
    std::string clientAddr;
    uint8_t slaveId = 1;
    int deviceId = 0;
};

/** Session 内部任务类型 */
enum class ModbusJobKind {
    DiscoveryRead,
    PollRead,
    WriteRegisters
};

/** Session 任务 */
struct ModbusJob {
    ModbusJobKind kind = ModbusJobKind::PollRead;
    int deviceId = 0;
    uint8_t slaveId = 1;
    size_t readGroupIndex = 0;
    Json::Value writeElements;
    std::string commandKey;
    std::vector<uint8_t> requestFrame;
    uint8_t requestFunctionCode = 0;
    uint16_t transactionId = 0;
};

/** 当前 in-flight 请求 */
struct InflightRequest {
    ModbusJob job;
    uint8_t functionCode = 0;
    uint16_t transactionId = 0;
    std::chrono::steady_clock::time_point sentTime;
};

/** 运行态 DTU 会话 */
struct DtuSession {
    static constexpr size_t MAX_QUEUE_SIZE = 256;  // 单队列最大任务数，防止无限堆积

    int linkId = 0;
    std::string clientAddr;
    std::string sessionKey;
    SessionBindState bindState = SessionBindState::Unknown;
    std::string dtuKey;
    std::chrono::steady_clock::time_point lastSeen;
    std::vector<uint8_t> rxBuffer;
    std::deque<ModbusJob> highQueue;
    std::deque<ModbusJob> normalQueue;
    std::optional<InflightRequest> inflight;
    std::map<uint8_t, int> deviceIdsBySlave;
    bool discoveryRequested = false;
    size_t discoveryCursor = 0;
    std::chrono::steady_clock::time_point nextDiscoveryTime{};

    /** 检查队列是否已满 */
    bool isQueueFull() const {
        return highQueue.size() + normalQueue.size() >= MAX_QUEUE_SIZE;
    }
};

/** 注册归一化结果 */
enum class RegistrationMatchKind {
    None,
    StandaloneFrame,
    PrefixedPayload,
    Conflict
};

struct RegistrationMatchResult {
    RegistrationMatchKind kind = RegistrationMatchKind::None;
    bool sessionBound = false;
    std::string dtuKey;
    std::vector<uint8_t> registrationBytes;
    std::vector<uint8_t> payload;
};

inline std::string makeDtuSessionKey(int linkId, const std::string& clientAddr) {
    return std::to_string(linkId) + ":" + clientAddr;
}

}  // namespace modbus
