#pragma once

#include "Modbus.SessionTypes.hpp"
#include "Modbus.Utils.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace modbus {

/**
 * @brief DTU 配置注册表
 *
 * 从现有 DeviceCache 聚合出逻辑 DTU：
 * - 同一 link 下，相同注册码的设备归属同一个 DTU
 * - 同一 DTU 下，slaveId 唯一
 */
class DtuRegistry {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    /** 重新从 DeviceCache 聚合 DTU 定义 */
    Task<void> reload();

    /** 是否存在 DTU 定义 */
    bool empty() const;

    /** 获取链路下的 DTU 定义快照 */
    std::vector<DtuDefinition> getDefinitionsByLink(int linkId) const;

    /** 获取所有 DTU 定义快照 */
    std::vector<DtuDefinition> getAllDefinitions() const;

    /** 通过 dtuKey 获取定义 */
    std::optional<DtuDefinition> findByDtuKey(const std::string& dtuKey) const;

    /** 通过注册码匹配 DTU */
    std::optional<DtuDefinition> findByRegistration(
        int linkId,
        const std::vector<uint8_t>& registrationBytes) const;

    /** 通过设备 ID 获取静态设备定义 */
    std::optional<ModbusDeviceDef> findDevice(int deviceId) const;

    /** 通过设备 ID 获取所属 DTU 定义 */
    std::optional<DtuDefinition> findDtuByDevice(int deviceId) const;

private:
    std::map<std::string, DtuDefinition> definitionsByKey_;
    std::map<int, std::vector<std::string>> linkToDtuKeys_;
    std::map<int, std::string> deviceToDtuKey_;
    std::map<int, ModbusDeviceDef> devicesById_;
    mutable std::mutex mutex_;
};

namespace detail {

// DTU 链路往返开销大（4G/GPRS 延迟 200-500ms），适当放宽合并间距
// 同时仍受 Modbus 协议最大读取量约束（字寄存器 125，位寄存器 2000）
inline constexpr int DTU_MERGE_GAP = 100;
inline constexpr int DTU_DEFAULT_READ_INTERVAL = 1;

inline std::string makeRegistrationToken(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return "NO_REG";

    std::ostringstream oss;
    for (uint8_t b : bytes) {
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(b);
    }
    return oss.str();
}

inline std::string makeDtuKey(const DeviceCache::CachedDevice& device) {
    if (device.registrationBytes.empty()) {
        return std::to_string(device.linkId) + ":NO_REG:" + std::to_string(device.id);
    }
    return std::to_string(device.linkId) + ":" + makeRegistrationToken(device.registrationBytes);
}

inline ReadGroupSnapshot toReadGroupSnapshot(const ReadGroup& group) {
    ReadGroupSnapshot snapshot;
    snapshot.registerType = group.registerType;
    snapshot.functionCode = group.functionCode;
    snapshot.startAddress = group.startAddress;
    snapshot.totalQuantity = group.totalQuantity;
    snapshot.registers.reserve(group.registers.size());
    for (const auto* reg : group.registers) {
        if (reg) snapshot.registers.push_back(*reg);
    }
    return snapshot;
}

inline std::vector<ReadGroupSnapshot> buildReadGroups(const std::vector<RegisterDef>& registers) {
    std::vector<ReadGroupSnapshot> groups;
    auto merged = ModbusUtils::mergeRegisters(registers, DTU_MERGE_GAP);
    groups.reserve(merged.size());
    for (const auto& group : merged) {
        groups.push_back(toReadGroupSnapshot(group));
    }
    return groups;
}

inline ModbusDeviceDef buildDeviceDef(const DeviceCache::CachedDevice& device, const std::string& dtuKey) {
    ModbusDeviceDef def;
    def.deviceId = device.id;
    def.deviceCode = device.deviceCode;
    def.deviceName = device.name;
    def.linkId = device.linkId;
    def.linkMode = device.linkMode;
    def.dtuKey = dtuKey;
    def.slaveId = device.slaveId;

    if (device.linkMode == Constants::LINK_MODE_TCP_CLIENT) {
        def.frameMode = parseFrameMode(device.modbusMode);
    } else {
        def.frameMode = (device.modbusMode == "TCP") ? FrameMode::TCP : FrameMode::RTU;
    }

    const auto& config = device.protocolConfig;
    def.byteOrder = parseByteOrder(config.get("byteOrder", "BIG_ENDIAN").asString());
    def.readInterval = config.get("readInterval", DTU_DEFAULT_READ_INTERVAL).asInt();
    if (def.readInterval < 1 || def.readInterval > 3600) {
        def.readInterval = DTU_DEFAULT_READ_INTERVAL;
    }

    if (config.isMember("registers") && config["registers"].isArray()) {
        for (const auto& reg : config["registers"]) {
            RegisterDef item;
            item.id = reg.get("id", "").asString();
            item.name = reg.get("name", "").asString();
            item.registerType = parseRegisterType(reg.get("registerType", "HOLDING_REGISTER").asString());
            item.address = static_cast<uint16_t>(reg.get("address", 0).asUInt());
            item.dataType = parseDataType(reg.get("dataType", "UINT16").asString());
            item.quantity = static_cast<uint16_t>(reg.get("quantity", 1).asUInt());
            item.unit = reg.get("unit", "").asString();
            item.remark = reg.get("remark", "").asString();
            item.decimals = reg.get("decimals", -1).asInt();
            if (reg.isMember("dictConfig") && reg["dictConfig"].isObject()) {
                item.dictConfig = reg["dictConfig"];
            }

            uint16_t maxQuantity =
                (item.registerType == RegisterType::COIL || item.registerType == RegisterType::DISCRETE_INPUT)
                    ? 2000
                    : 125;
            if (item.quantity < 1 || item.quantity > maxQuantity) {
                item.quantity = std::clamp(item.quantity, static_cast<uint16_t>(1), maxQuantity);
            }

            def.registers.push_back(std::move(item));
        }
    }

    def.readGroups = buildReadGroups(def.registers);
    return def;
}

inline void updateDiscoveryPlan(DtuDefinition& dtu, const ModbusDeviceDef& device) {
    if (dtu.discoveryPlan.enabled || device.readGroups.empty()) return;

    dtu.discoveryPlan.enabled = true;
    dtu.discoveryPlan.deviceId = device.deviceId;
    dtu.discoveryPlan.slaveId = device.slaveId;
    dtu.discoveryPlan.readGroupIndex = 0;
    dtu.discoveryPlan.group = device.readGroups.front();
}

}  // namespace detail

inline DtuRegistry::Task<void> DtuRegistry::reload() {
    auto cachedDevices = co_await DeviceCache::instance().getDevices();

    std::map<std::string, DtuDefinition> newDefinitions;
    std::map<int, std::vector<std::string>> newLinkToDtuKeys;
    std::map<int, std::string> newDeviceToDtuKey;
    std::map<int, ModbusDeviceDef> newDevicesById;

    for (const auto& device : cachedDevices) {
        if (device.protocolType != Constants::PROTOCOL_MODBUS) continue;
        if (device.status != Constants::USER_STATUS_ENABLED) continue;

        std::string dtuKey = detail::makeDtuKey(device);
        auto deviceDef = detail::buildDeviceDef(device, dtuKey);

        auto [it, inserted] = newDefinitions.try_emplace(dtuKey);
        auto& dtu = it->second;
        if (inserted) {
            dtu.dtuKey = dtuKey;
            dtu.linkId = device.linkId;
            dtu.linkMode = device.linkMode;
            dtu.name = device.linkName.empty() ? device.name : device.linkName;
            dtu.registrationBytes = device.registrationBytes;
            dtu.heartbeatBytes = device.heartbeatBytes;
            bool hasRegistration = !device.registrationBytes.empty();
            dtu.supportsStandaloneRegistration = hasRegistration;
            dtu.supportsPrefixedPayloadRegistration = hasRegistration;
            newLinkToDtuKeys[device.linkId].push_back(dtuKey);
        } else {
            if (dtu.linkId != device.linkId) {
                throw ConflictException("DTU 聚合异常：同 dtuKey 出现跨链路设备");
            }
            if (!dtu.registrationBytes.empty() && !device.registrationBytes.empty()
                && dtu.registrationBytes != device.registrationBytes) {
                throw ConflictException("DTU 聚合异常：同 dtuKey 出现不同注册码");
            }
            if (dtu.heartbeatBytes.empty() && !device.heartbeatBytes.empty()) {
                dtu.heartbeatBytes = device.heartbeatBytes;
            }
        }

        if (dtu.devicesBySlave.count(deviceDef.slaveId) > 0) {
            throw ConflictException(
                "DTU 配置冲突：linkId=" + std::to_string(device.linkId)
                + " dtuKey=" + dtuKey
                + " slaveId=" + std::to_string(deviceDef.slaveId) + " 重复");
        }

        detail::updateDiscoveryPlan(dtu, deviceDef);
        dtu.devicesBySlave.emplace(deviceDef.slaveId, deviceDef);
        newDeviceToDtuKey[deviceDef.deviceId] = dtuKey;
        newDevicesById[deviceDef.deviceId] = std::move(deviceDef);
    }

    const auto dtuCount = newDefinitions.size();
    const auto deviceCount = newDevicesById.size();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        definitionsByKey_ = std::move(newDefinitions);
        linkToDtuKeys_ = std::move(newLinkToDtuKeys);
        deviceToDtuKey_ = std::move(newDeviceToDtuKey);
        devicesById_ = std::move(newDevicesById);
    }

    LOG_INFO << "[Modbus][DtuRegistry] Loaded " << dtuCount
             << " DTU definition(s), devices=" << deviceCount;
}

inline bool DtuRegistry::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return definitionsByKey_.empty();
}

inline std::vector<DtuDefinition> DtuRegistry::getDefinitionsByLink(int linkId) const {
    std::vector<DtuDefinition> result;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = linkToDtuKeys_.find(linkId);
    if (it == linkToDtuKeys_.end()) return result;

    result.reserve(it->second.size());
    for (const auto& dtuKey : it->second) {
        auto defIt = definitionsByKey_.find(dtuKey);
        if (defIt != definitionsByKey_.end()) {
            result.push_back(defIt->second);
        }
    }
    return result;
}

inline std::vector<DtuDefinition> DtuRegistry::getAllDefinitions() const {
    std::vector<DtuDefinition> result;

    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(definitionsByKey_.size());
    for (const auto& [dtuKey, definition] : definitionsByKey_) {
        (void)dtuKey;
        result.push_back(definition);
    }

    return result;
}

inline std::optional<DtuDefinition> DtuRegistry::findByDtuKey(const std::string& dtuKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = definitionsByKey_.find(dtuKey);
    if (it == definitionsByKey_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<DtuDefinition> DtuRegistry::findByRegistration(
    int linkId,
    const std::vector<uint8_t>& registrationBytes) const {

    std::lock_guard<std::mutex> lock(mutex_);
    auto linkIt = linkToDtuKeys_.find(linkId);
    if (linkIt == linkToDtuKeys_.end()) return std::nullopt;

    for (const auto& dtuKey : linkIt->second) {
        auto defIt = definitionsByKey_.find(dtuKey);
        if (defIt == definitionsByKey_.end()) continue;
        if (defIt->second.registrationBytes == registrationBytes) {
            return defIt->second;
        }
    }
    return std::nullopt;
}

inline std::optional<ModbusDeviceDef> DtuRegistry::findDevice(int deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devicesById_.find(deviceId);
    if (it == devicesById_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<DtuDefinition> DtuRegistry::findDtuByDevice(int deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keyIt = deviceToDtuKey_.find(deviceId);
    if (keyIt == deviceToDtuKey_.end()) return std::nullopt;

    auto defIt = definitionsByKey_.find(keyIt->second);
    if (defIt == definitionsByKey_.end()) return std::nullopt;
    return defIt->second;
}

}  // namespace modbus
