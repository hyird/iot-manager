#pragma once

#include "S7.SessionTypes.hpp"
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

namespace s7 {

class DtuRegistry {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    Task<void> reload();
    bool empty() const;
    std::vector<S7DtuDefinition> getDefinitionsByLink(int linkId) const;
    std::vector<S7DtuDefinition> getAllDefinitions() const;
    std::optional<S7DtuDefinition> findByDtuKey(const std::string& dtuKey) const;
    std::optional<S7DtuDefinition> findByRegistration(
        int linkId,
        const std::vector<uint8_t>& registrationBytes) const;
    std::optional<S7DtuDefinition> findDtuByDevice(int deviceId) const;

private:
    std::map<std::string, S7DtuDefinition> definitionsByKey_;
    std::map<int, std::vector<std::string>> linkToDtuKeys_;
    std::map<int, std::string> deviceToDtuKey_;
    mutable std::mutex mutex_;
};

namespace detail {

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

inline S7DtuDefinition buildDefinition(const DeviceCache::CachedDevice& device, const std::string& dtuKey) {
    S7DtuDefinition def;
    def.deviceId = device.id;
    def.dtuKey = dtuKey;
    def.linkId = device.linkId;
    def.linkMode = device.linkMode;
    def.name = device.linkName.empty() ? device.name : device.linkName;
    def.registrationBytes = device.registrationBytes;
    const bool hasRegistration = !device.registrationBytes.empty();
    def.supportsStandaloneRegistration = hasRegistration;
    def.supportsPrefixedPayloadRegistration = hasRegistration;
    return def;
}

}  // namespace detail

inline DtuRegistry::Task<void> DtuRegistry::reload() {
    auto cachedDevices = co_await DeviceCache::instance().getDevices();

    std::map<std::string, S7DtuDefinition> newDefinitions;
    std::map<int, std::vector<std::string>> newLinkToDtuKeys;
    std::map<int, std::string> newDeviceToDtuKey;

    for (const auto& device : cachedDevices) {
        if (device.protocolType != Constants::PROTOCOL_S7) continue;
        if (device.status != Constants::USER_STATUS_ENABLED) continue;
        if (device.linkMode != Constants::LINK_MODE_TCP_SERVER) continue;

        const std::string dtuKey = detail::makeDtuKey(device);
        auto definition = detail::buildDefinition(device, dtuKey);

        auto [it, inserted] = newDefinitions.try_emplace(dtuKey, std::move(definition));
        if (!inserted) {
            if (it->second.linkId != device.linkId) {
                throw ConflictException("S7 DTU 聚合异常：同 dtuKey 出现跨链路设备");
            }
            if (!it->second.registrationBytes.empty() && !device.registrationBytes.empty()
                && it->second.registrationBytes != device.registrationBytes) {
                throw ConflictException("S7 DTU 聚合异常：同 dtuKey 出现不同注册码");
            }
            continue;
        }

        newLinkToDtuKeys[device.linkId].push_back(dtuKey);
        newDeviceToDtuKey[device.id] = dtuKey;
    }

    const auto dtuCount = newDefinitions.size();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        definitionsByKey_ = std::move(newDefinitions);
        linkToDtuKeys_ = std::move(newLinkToDtuKeys);
        deviceToDtuKey_ = std::move(newDeviceToDtuKey);
    }

    LOG_INFO << "[S7][DtuRegistry] Loaded " << dtuCount << " DTU definition(s)";
}

inline bool DtuRegistry::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return definitionsByKey_.empty();
}

inline std::vector<S7DtuDefinition> DtuRegistry::getDefinitionsByLink(int linkId) const {
    std::vector<S7DtuDefinition> result;

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

inline std::vector<S7DtuDefinition> DtuRegistry::getAllDefinitions() const {
    std::vector<S7DtuDefinition> result;

    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(definitionsByKey_.size());
    for (const auto& [dtuKey, definition] : definitionsByKey_) {
        (void)dtuKey;
        result.push_back(definition);
    }
    return result;
}

inline std::optional<S7DtuDefinition> DtuRegistry::findByDtuKey(const std::string& dtuKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = definitionsByKey_.find(dtuKey);
    if (it == definitionsByKey_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<S7DtuDefinition> DtuRegistry::findByRegistration(
    int linkId,
    const std::vector<uint8_t>& registrationBytes) const {
    if (registrationBytes.empty()) return std::nullopt;

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

inline std::optional<S7DtuDefinition> DtuRegistry::findDtuByDevice(int deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keyIt = deviceToDtuKey_.find(deviceId);
    if (keyIt == deviceToDtuKey_.end()) return std::nullopt;

    auto defIt = definitionsByKey_.find(keyIt->second);
    if (defIt == definitionsByKey_.end()) return std::nullopt;
    return defIt->second;
}

}  // namespace s7
