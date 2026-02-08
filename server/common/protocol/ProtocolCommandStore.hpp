#pragma once

#include "modules/device/domain/CommandRepository.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

class ProtocolCommandStore {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    struct CommandRecord {
        int deviceId = 0;
        int linkId = 0;
        std::string protocol;
        Json::Value data = Json::Value(Json::objectValue);
        std::string reportTime;

        /**
         * @brief 构建下行命令记录（公共信封 + 协议特有的要素数据）
         * @param elementsData 协议特有的要素数据（作为 data["data"]）
         */
        static CommandRecord create(
            int deviceId, int linkId, const std::string& protocol,
            const std::string& funcCode, const std::string& funcName,
            const std::string& rawHex, int userId,
            const std::string& status, const std::string& failReason,
            Json::Value elementsData = Json::Value(Json::objectValue)) {
            CommandRecord record;
            record.deviceId = deviceId;
            record.linkId = linkId;
            record.protocol = protocol;

            Json::Value d(Json::objectValue);
            d["funcCode"] = funcCode;
            if (!funcName.empty()) d["funcName"] = funcName;
            d["direction"] = "DOWN";
            d["userId"] = userId;
            d["status"] = status;
            if (!failReason.empty()) d["failReason"] = failReason;

            Json::Value rawArr(Json::arrayValue);
            rawArr.append(rawHex);
            d["raw"] = rawArr;
            d["data"] = std::move(elementsData);

            record.data = std::move(d);
            return record;
        }
    };

    Task<int64_t> saveCommand(CommandRecord record) const {
        if (record.reportTime.empty()) {
            record.reportTime = makeUtcNowString();
        }
        co_return co_await CommandRepository::save(
            record.deviceId, record.linkId, record.protocol, record.data, record.reportTime);
    }

    Task<void> updateCommandStatus(
        int64_t downCommandId,
        const std::string& status,
        const std::string& failReason = "") const {
        co_await CommandRepository::updateStatus(downCommandId, status, failReason);
    }

    /**
     * @brief 安全更新命令状态（吞掉异常）
     * 用于异常清理路径，避免二次异常导致状态永久停留在 PENDING
     */
    Task<void> tryUpdateCommandStatus(
        int64_t downCommandId,
        const std::string& status,
        const std::string& failReason = "") const {
        if (downCommandId <= 0) co_return;
        try {
            co_await CommandRepository::updateStatus(downCommandId, status, failReason);
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolCommandStore] tryUpdateCommandStatus failed: id="
                      << downCommandId << ", status=" << status << ", error=" << e.what();
        }
    }

private:
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
};
