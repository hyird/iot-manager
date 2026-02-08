#pragma once

#include "common/cache/DeviceCache.hpp"
#include "common/utils/Constants.hpp"

/**
 * @brief 构建对外统一的设备摘要
 */
class DeviceSummaryHelper {
public:
    static Json::Value build(const DeviceCache::CachedDevice& device) {
        Json::Value info(Json::objectValue);
        info["id"] = device.id;
        info["name"] = device.name;
        info["typeName"] = device.protocolName;
        info["protocolType"] = device.protocolType;
        if (device.protocolType == Constants::PROTOCOL_SL651 && !device.deviceCode.empty()) {
            info["deviceCode"] = device.deviceCode;
        }
        return info;
    }

    static Json::Value build(
        const DeviceCache::CachedDevice* device,
        int fallbackDeviceId = 0,
        const std::string& fallbackDeviceCode = ""
    ) {
        if (device) {
            return build(*device);
        }
        return buildFallback(fallbackDeviceId, fallbackDeviceCode);
    }

    static Json::Value buildFallback(
        int deviceId = 0,
        const std::string& deviceCode = "",
        const std::string& name = "",
        const std::string& typeName = "",
        const std::string& protocolType = ""
    ) {
        Json::Value info(Json::objectValue);
        if (deviceId > 0) {
            info["id"] = deviceId;
        }
        if (!name.empty()) {
            info["name"] = name;
        }
        if (!typeName.empty()) {
            info["typeName"] = typeName;
        }
        if (!protocolType.empty()) {
            info["protocolType"] = protocolType;
        }
        if (!deviceCode.empty()) {
            info["deviceCode"] = deviceCode;
        }
        return info;
    }
};
