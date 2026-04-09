#pragma once

#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sl651 {

class SL651LinkIngress {
public:
    struct Result {
        bool shouldParse = false;
        std::vector<uint8_t> payload;
    };

    Result preprocess(int linkId, const std::string& clientAddr, std::vector<uint8_t> bytes) const {
        auto devices = DeviceCache::instance().getDevicesByLinkIdSync(linkId);

        auto deviceLabel = [](const DeviceCache::CachedDevice& dev) {
            if (!dev.name.empty()) return dev.name;
            if (!dev.deviceCode.empty()) return dev.deviceCode;
            return std::string{"<unknown>"};
        };

        auto registerDevice = [&](const DeviceCache::CachedDevice& dev, bool logInfo, const char* matchType) {
            if (!dev.deviceCode.empty()) {
                DeviceConnectionCache::instance().registerConnection(dev.deviceCode, linkId, clientAddr);
            }
            if (logInfo) {
                LOG_INFO << "[SL651][LinkIngress] " << matchType << " matched "
                         << deviceLabel(dev) << "(id=" << dev.id << ",code=" << dev.deviceCode << ")"
                         << " from " << clientAddr;
            } else {
                LOG_DEBUG << "[SL651][LinkIngress] " << matchType << " matched "
                          << deviceLabel(dev) << "(id=" << dev.id << ",code=" << dev.deviceCode << ")"
                          << " from " << clientAddr;
            }
        };

        const bool requiresRegistration = std::any_of(devices.begin(), devices.end(), [](const auto& dev) {
            return dev.registrationMode != "OFF" && !dev.registrationBytes.empty();
        });

        std::vector<uint8_t> matchedRegistration;  // 值拷贝，避免悬挂指针
        bool prefixedPayload = false;
        for (const auto& dev : devices) {
            if (dev.registrationMode == "OFF" || dev.registrationBytes.empty()) continue;
            if (bytes == dev.registrationBytes) {
                matchedRegistration = dev.registrationBytes;
                break;
            }
            if (bytes.size() > dev.registrationBytes.size()
                && std::equal(dev.registrationBytes.begin(), dev.registrationBytes.end(), bytes.begin())) {
                matchedRegistration = dev.registrationBytes;
                prefixedPayload = true;
                break;
            }
        }

        if (!matchedRegistration.empty()) {
            for (const auto& dev : devices) {
                if (dev.registrationMode != "OFF" && dev.registrationBytes == matchedRegistration) {
                    registerDevice(dev, true, "Registration");
                }
            }
            if (!prefixedPayload) {
                return {};
            }

            bytes.erase(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(matchedRegistration.size()));
        }

        for (const auto& dev : devices) {
            if (dev.heartbeatMode == "OFF" || dev.heartbeatBytes.empty()) continue;
            if (bytes != dev.heartbeatBytes) continue;

            if (DeviceConnectionCache::instance().isClientRegistered(linkId, clientAddr)) {
                DeviceConnectionCache::instance().refreshClient(linkId, clientAddr);
                LOG_DEBUG << "[SL651][LinkIngress] Heartbeat from " << clientAddr;
            } else {
                LOG_DEBUG << "[SL651][LinkIngress] Heartbeat from unregistered "
                          << clientAddr << ", ignoring";
            }
            return {};
        }

        if (requiresRegistration
            && !DeviceConnectionCache::instance().isClientRegistered(linkId, clientAddr)) {
            LOG_WARN << "[SL651][LinkIngress] Unregistered client " << clientAddr
                     << ", dropping " << bytes.size() << "B";
            return {};
        }

        Result result;
        result.shouldParse = true;
        result.payload = std::move(bytes);
        return result;
    }
};

}  // namespace sl651
