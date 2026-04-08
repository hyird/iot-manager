#pragma once

#include "common/utils/Constants.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

#include <json/json.h>

/**
 * @brief 设备连接状态判断工具
 *
 * 统一设备在线/离线口径：
 * - 优先使用最近上报时间判断数据是否仍然有效
 * - 不再使用链路连接状态作为在线依据
 * - 对外只输出 online / offline，避免状态枚举分叉
 */
class DeviceConnectionStateHelper {
public:
    static std::optional<int> resolveProtocolIntervalSec(
        const std::string& protocolType,
        const Json::Value& protocolConfig
    ) {
        if (!protocolConfig.isObject()) return std::nullopt;

        if (protocolType == Constants::PROTOCOL_MODBUS
            && protocolConfig.isMember("readInterval")) {
            return std::max(1, protocolConfig.get("readInterval", 1).asInt());
        }
        if (protocolType == Constants::PROTOCOL_S7
            && protocolConfig.isMember("pollInterval")) {
            return std::max(1, protocolConfig.get("pollInterval", 5).asInt());
        }
        return std::nullopt;
    }

    static int resolveEffectiveTimeout(
        std::optional<int> intervalSec,
        int fallbackTimeoutSec = 300,
        int cycleCount = 3
    ) {
        if (intervalSec && *intervalSec > 0 && cycleCount > 0) {
            return (*intervalSec) * cycleCount;
        }
        return fallbackTimeoutSec > 0 ? fallbackTimeoutSec : 300;
    }

    static bool isReportTimeFresh(const std::string& reportTime, int onlineTimeoutSec) {
        const auto parsed = parseReportTime(reportTime);
        if (!parsed) return false;

        const int timeoutSec = onlineTimeoutSec > 0 ? onlineTimeoutSec : 300;
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - *parsed
        ).count();
        return elapsed < timeoutSec;
    }

    static const char* resolveConnectionState(
        const std::string& latestTime,
        int onlineTimeoutSec
    ) {
        return isReportTimeFresh(latestTime, onlineTimeoutSec) ? "online" : "offline";
    }

private:
    static std::optional<std::chrono::system_clock::time_point> parseReportTime(
        const std::string& reportTime
    ) {
        if (reportTime.empty()) return std::nullopt;

        std::string timePart = reportTime;
        if (timePart.size() > 1 && timePart.back() == 'Z') {
            timePart.pop_back();
        }

        int offsetSeconds = 0;
        bool hasOffset = false;
        size_t pos = timePart.find_last_of("+-");
        if (pos != std::string::npos && pos > 10) {
            std::string offsetStr = timePart.substr(pos);
            if (offsetStr.size() == 6 && offsetStr[3] == ':') {
                char sign = offsetStr[0];
                int hours = std::stoi(offsetStr.substr(1, 2));
                int mins = std::stoi(offsetStr.substr(4, 2));
                offsetSeconds = hours * 3600 + mins * 60;
                if (sign == '+') {
                    offsetSeconds = -offsetSeconds;
                }
                hasOffset = true;
                timePart = timePart.substr(0, pos);
            }
        }

        std::tm tm{};
        std::istringstream iss(timePart);
        iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (iss.fail()) return std::nullopt;

#ifdef _WIN32
        auto tt = _mkgmtime(&tm);
#else
        auto tt = timegm(&tm);
#endif
        if (tt == -1) return std::nullopt;

        auto tp = std::chrono::system_clock::from_time_t(tt);
        if (hasOffset) {
            tp -= std::chrono::seconds(offsetSeconds);
        }
        return tp;
    }
};
