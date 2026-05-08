#pragma once

#include "device/Device.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class DeviceRegistry {
public:
    void upsertRegistration(const std::string& deviceId, const std::string& remoteAddress);
    void updateKeepalive(const std::string& deviceId, const std::string& remoteAddress);
    bool updateKeepaliveAndNeedsCatalog(const std::string& deviceId, const std::string& remoteAddress);
    void updateCatalog(const std::string& deviceId, std::vector<Channel> channels);
    void updateRecords(const std::string& deviceId, std::vector<RecordItem> records);
    std::vector<Device> listDevices() const;
    std::optional<Device> findDevice(const std::string& deviceId) const;
    void markOffline(const std::string& deviceId);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Device> devices_;
};
