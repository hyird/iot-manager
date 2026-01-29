#pragma once

#include <chrono>
#include <string>
#include <ctime>

/**
 * @brief 时间戳助手
 */
class TimestampHelper {
public:
    static std::string now() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now;

        #ifdef _WIN32
        localtime_s(&tm_now, &time_t_now);
        #else
        localtime_r(&time_t_now, &tm_now);
        #endif

        char buffer[64];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_now);
        return std::string(buffer);
    }
};
