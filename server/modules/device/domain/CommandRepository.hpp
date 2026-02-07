#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/utils/JsonHelper.hpp"

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

    /** 批量写入条目 */
    struct SaveItem {
        int deviceId = 0;
        int linkId = 0;
        std::string protocol;
        Json::Value data;
        std::string reportTime;
    };

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
        DatabaseService dbService;

        std::string jsonStr = JsonHelper::serialize(data);

        auto result = co_await dbService.execSqlCoro(R"(
            INSERT INTO device_data (device_id, link_id, protocol, data, report_time)
            VALUES (?, ?, ?, ?::jsonb, ?::timestamptz)
            RETURNING id
        )", {std::to_string(deviceId), std::to_string(linkId), protocol, jsonStr, reportTime});

        int64_t id = 0;
        if (!result.empty()) {
            id = result[0]["id"].as<int64_t>();
        }

        LOG_TRACE << "[CommandRepository] Saved: id=" << id << ", deviceId=" << deviceId;
        co_return id;
    }

    /**
     * @brief 批量保存记录（多值 INSERT，单次 DB 往返）
     * @param items 待保存条目列表
     * @return 各记录的 ID（与 items 顺序对应）
     */
    static Task<std::vector<int64_t>> saveBatch(const std::vector<SaveItem>& items) {
        if (items.empty()) co_return {};

        DatabaseService dbService;

        // 构建多值 INSERT: VALUES (?,?,?,?::jsonb,?::timestamptz), (...), ...
        std::ostringstream sql;
        sql << "INSERT INTO device_data (device_id, link_id, protocol, data, report_time) VALUES ";

        std::vector<std::string> params;
        params.reserve(items.size() * 5);

        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) sql << ", ";
            sql << "(?, ?, ?, ?::jsonb, ?::timestamptz)";

            params.push_back(std::to_string(items[i].deviceId));
            params.push_back(std::to_string(items[i].linkId));
            params.push_back(items[i].protocol);
            params.push_back(JsonHelper::serialize(items[i].data));
            params.push_back(items[i].reportTime);
        }

        sql << " RETURNING id";

        auto result = co_await dbService.execSqlCoro(sql.str(), params);

        std::vector<int64_t> ids;
        ids.reserve(result.size());
        for (const auto& row : result) {
            ids.push_back(row["id"].as<int64_t>());
        }

        LOG_TRACE << "[CommandRepository] Batch saved: " << ids.size() << " records";
        co_return ids;
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

            std::string updateJson = JsonHelper::serialize(updateFields);

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
