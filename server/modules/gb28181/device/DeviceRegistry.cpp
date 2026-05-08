#include "device/DeviceRegistry.h"

#include <chrono>
#include <utility>

void DeviceRegistry::upsertRegistration(const std::string& deviceId, const std::string& remoteAddress) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.remoteAddress = remoteAddress;
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
}

void DeviceRegistry::updateKeepalive(const std::string& deviceId, const std::string& remoteAddress) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.remoteAddress = remoteAddress;
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
}

bool DeviceRegistry::updateKeepaliveAndNeedsCatalog(const std::string& deviceId, const std::string& remoteAddress) {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    const auto needsCatalog = iter == devices_.end() || iter->second.channels.empty();

    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.remoteAddress = remoteAddress;
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
    return needsCatalog;
}

void DeviceRegistry::updateCatalog(const std::string& deviceId, std::vector<Channel> channels) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
    device.channels = std::move(channels);
}

void DeviceRegistry::updateRecords(const std::string& deviceId, std::vector<RecordItem> records) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
    device.records = std::move(records);
}

std::vector<Device> DeviceRegistry::listDevices() const {
    std::lock_guard lock(mutex_);
    std::vector<Device> result;
    result.reserve(devices_.size());
    for (const auto& [_, device] : devices_) {
        result.push_back(device);
    }
    return result;
}

std::optional<Device> DeviceRegistry::findDevice(const std::string& deviceId) const {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    if (iter == devices_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void DeviceRegistry::markOffline(const std::string& deviceId) {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    if (iter != devices_.end()) {
        iter->second.online = false;
    }
}
