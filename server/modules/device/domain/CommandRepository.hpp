#pragma once

#include "common/database/DatabaseService.hpp"

/**
 * @brief 设备指令数据访问
 *
 * 封装 device_data 表的写操作，供 ProtocolDispatcher 调用。
 * 将数据库操作从协议基础设施层分离到设备领域层。
 */
class CommandRepository {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    /**
     * @brief 保存指令记录
     * @param deviceId 设备ID
     * @param linkId 链路ID
     * @param protocol 协议类型
     * @param data 指令 JSONB 数据（由调用方构建）
     * @param reportTime 时间戳
     * @return 插入记录的 ID
     */
    static Task<int64_t> save(int deviceId, int linkId, const std::string& protocol,
                              const Json::Value& data, const std::string& reportTime) {
        try {
            DatabaseService dbService;

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            writer["emitUTF8"] = true;
            std::string jsonStr = Json::writeString(writer, data);

            auto result = co_await dbService.execSqlCoro(R"(
                INSERT INTO device_data (device_id, link_id, protocol, data, report_time)
                VALUES (?, ?, ?, ?::jsonb, ?::timestamptz)
                RETURNING id
            )", {std::to_string(deviceId), std::to_string(linkId), protocol, jsonStr, reportTime});

            int64_t id = 0;
            if (!result.empty()) {
                id = result[0]["id"].as<int64_t>();
            }

            LOG_DEBUG << "[CommandRepository] 指令已保存: id=" << id << ", deviceId=" << deviceId;
            co_return id;

        } catch (const std::exception& e) {
            LOG_ERROR << "[CommandRepository] 保存指令失败: " << e.what();
            co_return 0;
        }
    }

    /**
     * @brief 关联应答报文 ID
     */
    static Task<void> linkResponse(int64_t downCommandId, int64_t responseId) {
        if (downCommandId <= 0) co_return;

        try {
            DatabaseService dbService;
            co_await dbService.execSqlCoro(R"(
                UPDATE device_data
                SET data = data || jsonb_build_object('responseId', ?::bigint)
                WHERE id = ?
            )", {std::to_string(responseId), std::to_string(downCommandId)});

            LOG_DEBUG << "[CommandRepository] 指令已关联应答: downId=" << downCommandId << ", respId=" << responseId;
        } catch (const std::exception& e) {
            LOG_ERROR << "[CommandRepository] 关联应答失败: " << e.what();
        }
    }

    /**
     * @brief 更新指令状态
     * @param status PENDING/SUCCESS/TIMEOUT/SEND_FAILED
     */
    static Task<void> updateStatus(int64_t downCommandId, const std::string& status,
                                    const std::string& failReason = "") {
        if (downCommandId <= 0) co_return;

        try {
            DatabaseService dbService;
            Json::Value updateFields;
            updateFields["status"] = status;
            if (!failReason.empty()) {
                updateFields["failReason"] = failReason;
            }

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            writer["emitUTF8"] = true;
            std::string updateJson = Json::writeString(writer, updateFields);

            co_await dbService.execSqlCoro(R"(
                UPDATE device_data
                SET data = data || ?::jsonb
                WHERE id = ?
            )", {updateJson, std::to_string(downCommandId)});

            LOG_DEBUG << "[CommandRepository] 指令状态已更新: id=" << downCommandId << ", status=" << status;
        } catch (const std::exception& e) {
            LOG_ERROR << "[CommandRepository] 更新指令状态失败: " << e.what();
        }
    }
};
