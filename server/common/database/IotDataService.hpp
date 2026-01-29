#pragma once

#include <drogon/drogon.h>
#include <json/json.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include "DatabaseService.hpp"

using namespace drogon;

/**
 * @brief TimescaleDB 配置
 */
struct TimescaleConfig {
    std::string clientName = "default";

    static TimescaleConfig& instance() {
        static TimescaleConfig config;
        return config;
    }

    static bool& enabled() {
        static bool enabled = false;
        return enabled;
    }
};

/**
 * @brief 统一 IoT 数据服务
 * 使用 JSONB 存储灵活的设备数据
 */
class IotDataService {
public:
    IotDataService() = default;

    /**
     * @brief 获取数据库客户端
     */
    orm::DbClientPtr getClient() {
        const auto& clientName = TimescaleConfig::instance().clientName;
        return AppDbConfig::useFast()
            ? app().getFastDbClient(clientName)
            : app().getDbClient(clientName);
    }

    /**
     * @brief 初始化超表
     */
    Task<void> initialize() {
        auto db = getClient();

        // 创建 TimescaleDB 扩展
        co_await db->execSqlCoro("CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE");

        // 创建统一数据表
        co_await db->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS iot_data (
                ts TIMESTAMPTZ NOT NULL,
                device_code VARCHAR(50) NOT NULL,
                link_id INT NOT NULL,
                protocol_code VARCHAR(50) NOT NULL,
                data_type VARCHAR(20) DEFAULT 'telemetry',
                func_code VARCHAR(20),
                serial_number VARCHAR(20),
                report_time TIMESTAMPTZ,
                values JSONB NOT NULL DEFAULT '{}',
                meta JSONB DEFAULT '{}',
                tags JSONB DEFAULT '{}',
                raw_data BYTEA
            )
        )");

        // 转换为超表
        try {
            co_await db->execSqlCoro(
                "SELECT create_hypertable('iot_data', 'ts', if_not_exists => TRUE)"
            );
        } catch (const std::exception& e) {
            LOG_WARN << "Hypertable may already exist: " << e.what();
        }

        // 创建索引
        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_data_device_ts ON iot_data (device_code, ts DESC)
        )");
        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_data_link_ts ON iot_data (link_id, ts DESC)
        )");
        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_data_protocol_ts ON iot_data (protocol_code, ts DESC)
        )");
        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_data_type_ts ON iot_data (data_type, ts DESC)
        )");

        // JSONB GIN 索引
        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_data_values_gin ON iot_data USING GIN (values)
        )");
        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_data_values_path ON iot_data USING GIN (values jsonb_path_ops)
        )");

        // 创建二进制数据表
        co_await db->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS iot_blob (
                ts TIMESTAMPTZ NOT NULL,
                device_code VARCHAR(50) NOT NULL,
                link_id INT NOT NULL,
                protocol_code VARCHAR(50) NOT NULL,
                blob_type VARCHAR(20) NOT NULL,
                content_type VARCHAR(100),
                file_name VARCHAR(255),
                file_size INT,
                blob_data BYTEA NOT NULL,
                meta JSONB DEFAULT '{}'
            )
        )");

        try {
            co_await db->execSqlCoro(
                "SELECT create_hypertable('iot_blob', 'ts', if_not_exists => TRUE)"
            );
        } catch (const std::exception&) {}

        co_await db->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_iot_blob_device_ts ON iot_blob (device_code, ts DESC)
        )");

        LOG_INFO << "IoT TimescaleDB tables initialized";
    }

    /**
     * @brief 插入设备数据（JSONB 格式）
     */
    Task<void> insertData(
        const std::string& deviceCode,
        int linkId,
        const std::string& protocolCode,
        const std::string& dataType,
        const std::string& funcCode,
        const std::string& serialNumber,
        const std::string& reportTime,
        const Json::Value& values,
        const Json::Value& meta = Json::Value(Json::objectValue),
        const Json::Value& tags = Json::Value(Json::objectValue),
        const std::vector<uint8_t>& rawData = {}
    ) {
        auto db = getClient();

        std::string ts = reportTime.empty() ? trantor::Date::now().toDbStringLocal() : reportTime;

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::string valuesJson = Json::writeString(writer, values);
        std::string metaJson = Json::writeString(writer, meta);
        std::string tagsJson = Json::writeString(writer, tags);

        if (rawData.empty()) {
            co_await db->execSqlCoro(
                "INSERT INTO iot_data "
                "(ts, device_code, link_id, protocol_code, data_type, func_code, serial_number, report_time, values, meta, tags) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb, $11::jsonb)",
                ts, deviceCode, linkId, protocolCode, dataType, funcCode, serialNumber, ts,
                valuesJson, metaJson, tagsJson
            );
        } else {
            std::string hexData = bytesToHex(rawData);
            co_await db->execSqlCoro(
                "INSERT INTO iot_data "
                "(ts, device_code, link_id, protocol_code, data_type, func_code, serial_number, report_time, values, meta, tags, raw_data) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb, $11::jsonb, decode($12, 'hex'))",
                ts, deviceCode, linkId, protocolCode, dataType, funcCode, serialNumber, ts,
                valuesJson, metaJson, tagsJson, hexData
            );
        }
    }

    /**
     * @brief 批量插入设备数据
     */
    Task<void> insertDataBatch(
        const std::string& deviceCode,
        int linkId,
        const std::string& protocolCode,
        const std::vector<std::tuple<
            std::string,    // dataType
            std::string,    // funcCode
            std::string,    // serialNumber
            std::string,    // reportTime
            Json::Value,    // values
            Json::Value     // meta
        >>& records
    ) {
        if (records.empty()) co_return;

        auto db = getClient();

        {
            auto trans = co_await db->newTransactionCoro();

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";

            for (const auto& [dataType, funcCode, serialNumber, reportTime, values, meta] : records) {
                std::string ts = reportTime.empty() ? trantor::Date::now().toDbStringLocal() : reportTime;
                std::string valuesJson = Json::writeString(writer, values);
                std::string metaJson = Json::writeString(writer, meta);

                co_await trans->execSqlCoro(
                    "INSERT INTO iot_data "
                    "(ts, device_code, link_id, protocol_code, data_type, func_code, serial_number, report_time, values, meta) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb)",
                    ts, deviceCode, linkId, protocolCode, dataType, funcCode, serialNumber, ts,
                    valuesJson, metaJson
                );
            }
        }
    }

    /**
     * @brief 查询设备数据
     */
    Task<Json::Value> queryData(
        const std::string& deviceCode,
        const std::string& protocolCode,
        int linkId,
        const std::string& startTime,
        const std::string& endTime,
        int limit = 1000,
        int offset = 0
    ) {
        auto db = getClient();

        std::ostringstream sql;
        std::vector<std::string> params;

        sql << "SELECT ts, device_code, link_id, protocol_code, data_type, "
            << "func_code, serial_number, report_time, values, meta "
            << "FROM iot_data WHERE 1=1";

        if (!deviceCode.empty()) {
            params.push_back(deviceCode);
            sql << " AND device_code = $" << params.size();
        }
        if (!protocolCode.empty()) {
            params.push_back(protocolCode);
            sql << " AND protocol_code = $" << params.size();
        }
        if (linkId > 0) {
            params.push_back(std::to_string(linkId));
            sql << " AND link_id = $" << params.size();
        }
        if (!startTime.empty()) {
            params.push_back(startTime);
            sql << " AND ts >= $" << params.size();
        }
        if (!endTime.empty()) {
            params.push_back(endTime);
            sql << " AND ts <= $" << params.size();
        }

        sql << " ORDER BY ts DESC LIMIT " << limit << " OFFSET " << offset;

        auto result = co_await execWithParams(db, sql.str(), params);

        Json::Value rows(Json::arrayValue);
        Json::CharReaderBuilder readerBuilder;

        for (const auto& row : result) {
            Json::Value obj;
            obj["ts"] = row["ts"].as<std::string>();
            obj["deviceCode"] = row["device_code"].as<std::string>();
            obj["linkId"] = row["link_id"].as<int>();
            obj["protocolCode"] = row["protocol_code"].as<std::string>();
            obj["dataType"] = row["data_type"].isNull() ? "telemetry" : row["data_type"].as<std::string>();
            obj["funcCode"] = row["func_code"].isNull() ? "" : row["func_code"].as<std::string>();
            obj["serialNumber"] = row["serial_number"].isNull() ? "" : row["serial_number"].as<std::string>();

            // 解析 JSONB values
            if (!row["values"].isNull()) {
                std::string valuesStr = row["values"].as<std::string>();
                Json::Value parsedValues;
                std::istringstream stream(valuesStr);
                if (Json::parseFromStream(readerBuilder, stream, &parsedValues, nullptr)) {
                    obj["values"] = parsedValues;
                } else {
                    obj["values"] = Json::Value(Json::objectValue);
                }
            }

            // 解析 JSONB meta
            if (!row["meta"].isNull()) {
                std::string metaStr = row["meta"].as<std::string>();
                Json::Value parsedMeta;
                std::istringstream stream(metaStr);
                if (Json::parseFromStream(readerBuilder, stream, &parsedMeta, nullptr)) {
                    obj["meta"] = parsedMeta;
                }
            }

            rows.append(obj);
        }

        co_return rows;
    }

    /**
     * @brief 查询设备最新数据
     */
    Task<Json::Value> queryLatestData(const std::string& deviceCode, int limit = 1) {
        auto db = getClient();

        auto result = co_await db->execSqlCoro(
            "SELECT ts, device_code, link_id, protocol_code, data_type, "
            "func_code, values, meta "
            "FROM iot_data "
            "WHERE device_code = $1 "
            "ORDER BY ts DESC "
            "LIMIT $2",
            deviceCode, limit
        );

        Json::Value rows(Json::arrayValue);
        Json::CharReaderBuilder readerBuilder;

        for (const auto& row : result) {
            Json::Value obj;
            obj["ts"] = row["ts"].as<std::string>();
            obj["deviceCode"] = row["device_code"].as<std::string>();
            obj["linkId"] = row["link_id"].as<int>();
            obj["protocolCode"] = row["protocol_code"].as<std::string>();
            obj["dataType"] = row["data_type"].isNull() ? "telemetry" : row["data_type"].as<std::string>();
            obj["funcCode"] = row["func_code"].isNull() ? "" : row["func_code"].as<std::string>();

            if (!row["values"].isNull()) {
                std::string valuesStr = row["values"].as<std::string>();
                Json::Value parsedValues;
                std::istringstream stream(valuesStr);
                if (Json::parseFromStream(readerBuilder, stream, &parsedValues, nullptr)) {
                    obj["values"] = parsedValues;
                }
            }

            rows.append(obj);
        }

        co_return rows;
    }

    /**
     * @brief 统计查询（按时间桶聚合 JSONB 中的数值字段）
     */
    Task<Json::Value> queryStatistics(
        const std::string& deviceCode,
        const std::string& fieldName,
        const std::string& startTime,
        const std::string& endTime,
        const std::string& interval = "1 hour"
    ) {
        auto db = getClient();

        std::string sql =
            "SELECT time_bucket($1::interval, ts) AS bucket, "
            "AVG((values->>$2)::numeric) AS avg_value, "
            "MAX((values->>$2)::numeric) AS max_value, "
            "MIN((values->>$2)::numeric) AS min_value, "
            "COUNT(*) AS count "
            "FROM iot_data "
            "WHERE device_code = $3 AND values ? $4";

        std::vector<std::string> params = {interval, fieldName, deviceCode, fieldName};

        if (!startTime.empty()) {
            params.push_back(startTime);
            sql += " AND ts >= $" + std::to_string(params.size());
        }
        if (!endTime.empty()) {
            params.push_back(endTime);
            sql += " AND ts <= $" + std::to_string(params.size());
        }

        sql += " GROUP BY bucket ORDER BY bucket DESC";

        auto result = co_await execWithParams(db, sql, params);

        Json::Value rows(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value obj;
            obj["ts"] = row["bucket"].as<std::string>();
            obj["avgValue"] = row["avg_value"].isNull() ? 0.0 : row["avg_value"].as<double>();
            obj["maxValue"] = row["max_value"].isNull() ? 0.0 : row["max_value"].as<double>();
            obj["minValue"] = row["min_value"].isNull() ? 0.0 : row["min_value"].as<double>();
            obj["count"] = row["count"].as<int64_t>();
            rows.append(obj);
        }

        co_return rows;
    }

    /**
     * @brief 获取数据总数
     */
    Task<int64_t> countData(
        const std::string& deviceCode,
        const std::string& protocolCode,
        int linkId,
        const std::string& startTime,
        const std::string& endTime
    ) {
        auto db = getClient();

        std::ostringstream sql;
        std::vector<std::string> params;

        sql << "SELECT COUNT(*) as cnt FROM iot_data WHERE 1=1";

        if (!deviceCode.empty()) {
            params.push_back(deviceCode);
            sql << " AND device_code = $" << params.size();
        }
        if (!protocolCode.empty()) {
            params.push_back(protocolCode);
            sql << " AND protocol_code = $" << params.size();
        }
        if (linkId > 0) {
            params.push_back(std::to_string(linkId));
            sql << " AND link_id = $" << params.size();
        }
        if (!startTime.empty()) {
            params.push_back(startTime);
            sql << " AND ts >= $" << params.size();
        }
        if (!endTime.empty()) {
            params.push_back(endTime);
            sql << " AND ts <= $" << params.size();
        }

        auto result = co_await execWithParams(db, sql.str(), params);

        if (!result.empty()) {
            co_return result[0]["cnt"].as<int64_t>();
        }
        co_return 0;
    }

    /**
     * @brief 插入二进制数据（图片等）
     */
    Task<void> insertBlob(
        const std::string& deviceCode,
        int linkId,
        const std::string& protocolCode,
        const std::string& blobType,
        const std::string& contentType,
        const std::string& fileName,
        const std::vector<uint8_t>& blobData,
        const Json::Value& meta = Json::Value(Json::objectValue)
    ) {
        auto db = getClient();

        std::string ts = trantor::Date::now().toDbStringLocal();
        std::string hexData = bytesToHex(blobData);
        int fileSize = static_cast<int>(blobData.size());

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::string metaJson = Json::writeString(writer, meta);

        co_await db->execSqlCoro(
            "INSERT INTO iot_blob "
            "(ts, device_code, link_id, protocol_code, blob_type, content_type, file_name, file_size, blob_data, meta) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, decode($9, 'hex'), $10::jsonb)",
            ts, deviceCode, linkId, protocolCode, blobType, contentType, fileName, fileSize, hexData, metaJson
        );
    }

    /**
     * @brief 查询二进制数据列表
     */
    Task<Json::Value> queryBlobs(
        const std::string& deviceCode,
        const std::string& blobType,
        const std::string& startTime,
        const std::string& endTime,
        int limit = 100
    ) {
        auto db = getClient();

        std::ostringstream sql;
        std::vector<std::string> params;

        sql << "SELECT ts, device_code, link_id, protocol_code, blob_type, "
            << "content_type, file_name, file_size, meta "
            << "FROM iot_blob WHERE 1=1";

        if (!deviceCode.empty()) {
            params.push_back(deviceCode);
            sql << " AND device_code = $" << params.size();
        }
        if (!blobType.empty()) {
            params.push_back(blobType);
            sql << " AND blob_type = $" << params.size();
        }
        if (!startTime.empty()) {
            params.push_back(startTime);
            sql << " AND ts >= $" << params.size();
        }
        if (!endTime.empty()) {
            params.push_back(endTime);
            sql << " AND ts <= $" << params.size();
        }

        sql << " ORDER BY ts DESC LIMIT " << limit;

        auto result = co_await execWithParams(db, sql.str(), params);

        Json::Value rows(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value obj;
            obj["ts"] = row["ts"].as<std::string>();
            obj["deviceCode"] = row["device_code"].as<std::string>();
            obj["linkId"] = row["link_id"].as<int>();
            obj["protocolCode"] = row["protocol_code"].as<std::string>();
            obj["blobType"] = row["blob_type"].as<std::string>();
            obj["contentType"] = row["content_type"].isNull() ? "" : row["content_type"].as<std::string>();
            obj["fileName"] = row["file_name"].isNull() ? "" : row["file_name"].as<std::string>();
            obj["fileSize"] = row["file_size"].isNull() ? 0 : row["file_size"].as<int>();
            rows.append(obj);
        }

        co_return rows;
    }

private:
    /**
     * @brief 执行带参数的 SQL
     */
    Task<orm::Result> execWithParams(
        const orm::DbClientPtr& db,
        const std::string& sql,
        const std::vector<std::string>& params
    ) {
        switch (params.size()) {
            case 0:
                co_return co_await db->execSqlCoro(sql);
            case 1:
                co_return co_await db->execSqlCoro(sql, params[0]);
            case 2:
                co_return co_await db->execSqlCoro(sql, params[0], params[1]);
            case 3:
                co_return co_await db->execSqlCoro(sql, params[0], params[1], params[2]);
            case 4:
                co_return co_await db->execSqlCoro(sql, params[0], params[1], params[2], params[3]);
            case 5:
                co_return co_await db->execSqlCoro(sql, params[0], params[1], params[2], params[3], params[4]);
            case 6:
                co_return co_await db->execSqlCoro(sql, params[0], params[1], params[2], params[3], params[4], params[5]);
            default:
                co_return co_await db->execSqlCoro(sql);
        }
    }

    static std::string bytesToHex(const std::vector<uint8_t>& data) {
        std::ostringstream oss;
        for (uint8_t b : data) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        return oss.str();
    }
};
