#pragma once

#include <drogon/drogon.h>
#include <set>
#include <chrono>
#include <ctime>
#include "domain/Device.hpp"
#include "DeviceDataTransformer.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/database/IotDataService.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/Constants.hpp"

using namespace drogon;

// 类型别名，简化代码
using ElementData = DeviceDataTransformer::ElementData;

/**
 * @brief 设备服务（领域驱动重构版）
 *
 * CRUD 操作使用领域模型，复杂查询使用 DeviceDataTransformer 进行数据转换。
 *
 * 职责划分：
 * - DeviceService: 业务逻辑协调、数据库查询
 * - DeviceDataTransformer: 数据格式转换（单一职责）
 * - Device（领域模型）: CRUD 业务规则
 */
class DeviceService {
private:
    DatabaseService dbService_;

public:
    // ==================== CRUD 操作（领域驱动）====================

    /**
     * @brief 设备列表（分页）
     */
    Task<std::tuple<Json::Value, int>> list(
        const Pagination& page,
        int linkId = 0,
        int protocolConfigId = 0,
        const std::string& status = ""
    ) {
        auto result = co_await Device::list(page, linkId, protocolConfigId, status);

        Json::Value items(Json::arrayValue);
        for (const auto& device : result.items) {
            items.append(device.toJson());
        }

        co_return std::make_tuple(items, result.total);
    }

    /**
     * @brief 设备详情
     */
    Task<Json::Value> detail(int id) {
        auto device = co_await Device::of(id);
        co_return device.toJson();
    }

    /**
     * @brief 设备选项（下拉选择用）
     */
    Task<Json::Value> options() {
        co_return co_await Device::options();
    }

    /**
     * @brief 创建设备
     */
    Task<Json::Value> create(const Json::Value& data) {
        auto device = Device::create(data);

        device.require(Device::nameUnique)
              .require(Device::codeUnique)
              .require(Device::linkExists)
              .require(Device::protocolConfigExists);

        co_await device.save();

        // 返回创建后的详情
        co_return co_await detail(device.id());
    }

    /**
     * @brief 更新设备
     */
    Task<void> update(int id, const Json::Value& data) {
        auto device = co_await Device::of(id);

        // 根据修改的字段添加约束
        if (data.isMember("name")) {
            device.require(Device::nameUnique);
        }
        if (data.isMember("device_code")) {
            device.require(Device::codeUnique);
        }
        if (data.isMember("link_id")) {
            device.require(Device::linkExists);
        }
        if (data.isMember("protocol_config_id")) {
            device.require(Device::protocolConfigExists);
        }

        device.update(data);

        co_await device.save();
    }

    /**
     * @brief 删除设备
     */
    Task<void> remove(int id) {
        auto device = co_await Device::of(id);
        co_await device.remove()
            .save();
    }

    // ==================== 复杂查询（保留原有实现）====================

    /**
     * @brief 获取设备列表（包含实时数据和管理信息）
     * 优化版：使用缓存获取设备基本信息，只查询实时数据
     */
    Task<Json::Value> listWithRealtime() {
        // 1. 从缓存获取设备基本信息（包含协议配置）
        auto cachedDevices = co_await DeviceCache::instance().getDevices();

        if (cachedDevices.empty()) {
            co_return Json::Value(Json::arrayValue);
        }

        // 2. 构建设备 ID 列表，批量查询实时数据
        std::ostringstream idListBuilder;
        for (size_t i = 0; i < cachedDevices.size(); ++i) {
            if (i > 0) idListBuilder << ",";
            idListBuilder << cachedDevices[i].id;
        }
        std::string idList = idListBuilder.str();

        // 3. 优化的 SQL：使用 DISTINCT ON 获取每个设备每个功能码的最新数据
        std::string sql = R"(
            SELECT device_id, data, report_time, data->>'funcCode' as func_code
            FROM (
                SELECT DISTINCT ON (device_id, data->>'funcCode')
                       device_id, data, report_time
                FROM device_data
                WHERE device_id IN ()" + idList + R"()
                  AND report_time >= NOW() - INTERVAL ')" + std::to_string(Constants::DEVICE_DATA_LOOKBACK_DAYS) + R"( days'
                ORDER BY device_id, data->>'funcCode', report_time DESC NULLS LAST
            ) sub
        )";

        auto result = co_await dbService_.execSqlCoro(sql);

        // 4. 按设备 ID 组织实时数据
        std::map<int, std::map<std::string, std::pair<Json::Value, std::string>>> deviceDataMap;
        std::map<int, std::string> deviceLatestTime;

        Json::CharReaderBuilder readerBuilder;

        for (const auto& row : result) {
            int deviceId = FieldHelper::getInt(row["device_id"]);
            std::string funcCode = FieldHelper::getString(row["func_code"], "");
            std::string reportTime = FieldHelper::getString(row["report_time"], "");
            std::string dataStr = FieldHelper::getString(row["data"], "");

            Json::Value dataJson;
            std::string errs;
            std::istringstream iss(dataStr);
            if (Json::parseFromStream(readerBuilder, iss, &dataJson, &errs)) {
                deviceDataMap[deviceId][funcCode] = {dataJson, reportTime};

                if (deviceLatestTime.find(deviceId) == deviceLatestTime.end() ||
                    reportTime > deviceLatestTime[deviceId]) {
                    deviceLatestTime[deviceId] = reportTime;
                }
            }
        }

        // 5. 合并缓存的设备信息和实时数据
        Json::Value items(Json::arrayValue);

        for (const auto& device : cachedDevices) {
            Json::Value item = DeviceDataTransformer::buildDeviceBaseInfo(device);

            // 实时数据时间
            item["lastHeartbeatTime"] = Json::nullValue;
            auto latestIt = deviceLatestTime.find(device.id);
            item["reportTime"] = (latestIt != deviceLatestTime.end()) ? Json::Value(latestIt->second) : Json::nullValue;
            item["image"] = Json::nullValue;

            // 解析实时数据
            std::map<std::string, ElementData> realtimeValues;
            std::map<std::string, Json::Value> funcDataMap;

            auto deviceDataIt = deviceDataMap.find(device.id);
            if (deviceDataIt != deviceDataMap.end()) {
                realtimeValues = DeviceDataTransformer::parseRealtimeValues(deviceDataIt->second);
                for (const auto& [funcCode, dataPair] : deviceDataIt->second) {
                    funcDataMap[funcCode] = dataPair.first;
                }
            }

            // 使用 Transformer 解析协议配置
            Json::Value elements, downFuncs, imageFuncs;
            DeviceDataTransformer::parseProtocolFuncs(
                device, realtimeValues, funcDataMap,
                elements, downFuncs, imageFuncs
            );

            item["elements"] = elements;
            item["downFuncs"] = downFuncs;
            item["imageFuncs"] = imageFuncs;

            items.append(item);
        }

        co_return items;
    }

    /**
     * @brief 获取设备静态数据列表（用于 ETag 缓存）
     * 只返回设备基本信息和协议配置，不查询实时数据
     */
    Task<Json::Value> listStatic() {
        auto cachedDevices = co_await DeviceCache::instance().getDevices();

        if (cachedDevices.empty()) {
            co_return Json::Value(Json::arrayValue);
        }

        Json::Value items(Json::arrayValue);

        for (const auto& device : cachedDevices) {
            Json::Value item = DeviceDataTransformer::buildDeviceBaseInfo(device);

            // 使用 Transformer 解析静态协议配置
            Json::Value downFuncs, imageFuncs;
            DeviceDataTransformer::parseProtocolFuncsStatic(device, downFuncs, imageFuncs);

            item["downFuncs"] = downFuncs;
            item["imageFuncs"] = imageFuncs;

            items.append(item);
        }

        co_return items;
    }

    /**
     * @brief 获取设备实时数据列表
     * 只返回实时数据，不包含设备基本信息
     *
     * 优化策略：
     * - 首次访问时从数据库加载并缓存
     * - 后续通过报文解析更新缓存
     * - 实时查询直接从缓存返回，无需查询数据库
     */
    Task<Json::Value> listRealtime() {
        auto cachedDevices = co_await DeviceCache::instance().getDevices();

        if (cachedDevices.empty()) {
            co_return Json::Value(Json::arrayValue);
        }

        // 构建设备 ID 列表
        std::vector<int> deviceIds;
        deviceIds.reserve(cachedDevices.size());
        for (const auto& device : cachedDevices) {
            deviceIds.push_back(device.id);
        }

        // 如果缓存未初始化，从数据库加载
        auto& realtimeCache = RealtimeDataCache::instance();
        if (!realtimeCache.isInitialized()) {
            co_await realtimeCache.initializeFromDb(deviceIds);
        }

        // 从缓存获取实时数据
        auto deviceDataMap = co_await realtimeCache.getBatch(deviceIds);
        auto latestTimeMap = co_await realtimeCache.getLatestReportTimes(deviceIds);

        // 构建返回数据
        Json::Value items(Json::arrayValue);

        for (const auto& device : cachedDevices) {
            Json::Value item;
            item["id"] = device.id;

            // 最新上报时间
            auto latestIt = latestTimeMap.find(device.id);
            item["reportTime"] = (latestIt != latestTimeMap.end() && !latestIt->second.empty())
                ? Json::Value(latestIt->second) : Json::nullValue;
            item["lastHeartbeatTime"] = Json::nullValue;

            // 解析实时要素数据
            std::map<std::string, ElementData> realtimeValues;
            std::map<std::string, Json::Value> funcDataMap;

            auto deviceDataIt = deviceDataMap.find(device.id);
            if (deviceDataIt != deviceDataMap.end()) {
                // 转换为 parseRealtimeValues 需要的格式
                std::map<std::string, std::pair<Json::Value, std::string>> funcDataPairs;
                for (const auto& [funcCode, funcData] : deviceDataIt->second) {
                    funcDataPairs[funcCode] = {funcData.data, funcData.reportTime};
                    funcDataMap[funcCode] = funcData.data;
                }
                realtimeValues = DeviceDataTransformer::parseRealtimeValues(funcDataPairs);
            }

            // 根据协议配置构建 elements 和 image
            Json::Value elements(Json::arrayValue);
            Json::Value image = Json::nullValue;

            if (!device.protocolConfig.isNull() && device.protocolType == "SL651") {
                const auto& config = device.protocolConfig;
                if (config.isMember("funcs") && config["funcs"].isArray()) {
                    std::set<std::string> addedGuideHex;

                    for (const auto& func : config["funcs"]) {
                        std::string dir = func.get("dir", "").asString();
                        std::string funcCode = func.get("funcCode", "").asString();

                        if (dir != "UP" || !func.isMember("elements") || !func["elements"].isArray()) {
                            continue;
                        }

                        if (DeviceDataTransformer::hasJpegElement(func)) {
                            // 查找图片数据
                            auto imageData = DeviceDataTransformer::findImageData(funcCode, funcDataMap);
                            if (imageData) {
                                Json::Value latestImage;
                                latestImage["funcCode"] = funcCode;
                                latestImage["data"] = *imageData;
                                image = latestImage;
                            }
                        } else {
                            DeviceDataTransformer::parseUpElements(func, realtimeValues, addedGuideHex, elements);
                        }
                    }
                }
            }

            item["elements"] = elements;
            item["image"] = image;

            items.append(item);
        }

        co_return items;
    }

    /**
     * @brief 查询设备历史数据（从 device_data 表，支持归档数据）
     * 支持多层查询：
     * 1. 只传 code: 返回功能码列表
     * 2. 传 code + funcCode + dataType: 返回具体数据
     *
     * 优化策略（千万级数据支持）：
     * - 先通过 device_code 获取 device_id，再直接查询 device_data（避免 JOIN）
     * - 使用 idx_device_data_history 复合索引加速查询
     * - 使用 report_time 范围扫描，利用 TimescaleDB 分区剪枝
     * - 当查询时间早于归档界限时，使用 UNION ALL 同时查询主表和归档表
     */
    Task<std::tuple<Json::Value, int>> queryHistory(
        const std::string& code,
        const std::string& funcCode,
        const std::string& dataType,
        const std::string& startTime,
        const std::string& endTime,
        int page,
        int pageSize
    ) {
        // 1. 先获取 device_id（避免后续 JOIN，利用索引）
        auto deviceResult = co_await dbService_.execSqlCoro(
            "SELECT id FROM device WHERE device_code = ? AND deleted_at IS NULL",
            {code}
        );

        if (deviceResult.empty()) {
            co_return std::make_tuple(Json::Value(Json::arrayValue), 0);
        }

        int deviceId = FieldHelper::getInt(deviceResult[0]["id"]);
        std::string deviceIdStr = std::to_string(deviceId);

        // 判断是否需要查询归档数据（startTime 早于 365 天前）
        // 如果需要，使用 device_data_all 视图；否则只查询 device_data 主表
        bool needArchive = false;
        if (!startTime.empty()) {
            // 简单判断：如果 startTime 年份小于当前年份-1，或者月份差大于 12，则认为需要归档
            // 这是一个近似判断，实际可以更精确
            try {
                // 获取当前时间的归档阈值天数前
                auto now = std::chrono::system_clock::now();
                auto archiveThreshold = now - std::chrono::hours(Constants::ARCHIVE_THRESHOLD_DAYS * 24);
                auto tt = std::chrono::system_clock::to_time_t(archiveThreshold);
                std::tm tm = *std::localtime(&tt);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
                std::string thresholdStr(buf);

                // 如果 startTime 早于阈值，需要查询归档
                if (startTime < thresholdStr) {
                    needArchive = true;
                }
            } catch (...) {
                // 解析失败，保守起见不查归档
            }
        }

        // 选择查询的表：主表或合并视图
        std::string tableName = needArchive ? "device_data_all" : "device_data";

        // 默认时间范围：如果未指定 startTime，默认查询最近 N 天（利用 TimescaleDB 分区裁剪）
        std::string effectiveStartTime = startTime;
        if (effectiveStartTime.empty()) {
            auto now = std::chrono::system_clock::now();
            auto defaultStart = now - std::chrono::hours(Constants::HISTORY_DEFAULT_QUERY_DAYS * 24);
            auto tt = std::chrono::system_clock::to_time_t(defaultStart);
            std::tm tm = *std::localtime(&tt);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d 00:00:00", &tm);
            effectiveStartTime = std::string(buf);
        }

        // 构建时间条件（直接使用 device_id，无需 JOIN）
        // 始终包含 startTime 条件以利用分区裁剪
        auto buildTimeCondition = [&effectiveStartTime, &endTime](std::vector<std::string>& params) -> std::string {
            std::string condition;
            // 始终添加起始时间条件（默认或用户指定）
            condition += " AND report_time >= ?::timestamptz";
            params.push_back(effectiveStartTime);
            if (!endTime.empty()) {
                condition += " AND report_time <= ?::timestamptz";
                params.push_back(endTime);
            }
            return condition;
        };

        // 过滤空 data 的条件
        std::string filterEmptyData = " AND (data->'data' IS NOT NULL AND data->'data' <> '{}'::jsonb)";

        // 第一层：查询功能码列表
        if (funcCode.empty()) {
            std::vector<std::string> countParams = {deviceIdStr};
            std::string timeCondition = buildTimeCondition(countParams);

            // 优化：使用子查询获取不同功能码数量，避免全表 DISTINCT
            std::string countSql = R"(
                SELECT COUNT(*) as cnt FROM (
                    SELECT DISTINCT data->>'funcCode' as fc
                    FROM )" + tableName + R"(
                    WHERE device_id = ?
            )" + timeCondition + filterEmptyData + R"(
                ) sub
            )";

            std::vector<std::string> queryParams = {deviceIdStr};
            buildTimeCondition(queryParams);

            // 使用子查询先聚合，再排序（减少排序数据量）
            std::string sql = R"(
                SELECT
                    fc as func_code,
                    fn as func_name,
                    CASE WHEN fc IN ('36', 'B6') THEN 'IMAGE' ELSE 'ELEMENT' END as data_type,
                    cnt as total_records
                FROM (
                    SELECT
                        data->>'funcCode' as fc,
                        MAX(data->>'funcName') as fn,
                        COUNT(*) as cnt
                    FROM )" + tableName + R"(
                    WHERE device_id = ?
            )" + timeCondition + filterEmptyData + R"(
                    GROUP BY data->>'funcCode'
                ) sub
                ORDER BY fc
                LIMIT )" + std::to_string(pageSize) + " OFFSET " + std::to_string((page - 1) * pageSize);

            auto countResult = co_await dbService_.execSqlCoro(countSql, countParams);
            auto result = co_await dbService_.execSqlCoro(sql, queryParams);

            int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["cnt"]);

            Json::Value items(Json::arrayValue);
            for (const auto& row : result) {
                Json::Value item;
                item["code"] = code;
                item["funcCode"] = FieldHelper::getString(row["func_code"], "");
                item["funcName"] = FieldHelper::getString(row["func_name"], "");
                item["dataType"] = FieldHelper::getString(row["data_type"], "ELEMENT");
                item["totalRecords"] = row["total_records"].isNull() ? 0 : row["total_records"].as<int64_t>();
                items.append(item);
            }

            co_return std::make_tuple(items, total);
        }

        // 第二层：查询具体数据（直接使用 device_id，利用复合索引）
        bool isImage = (dataType == "IMAGE");

        // 获取协议配置以获取字典配置（dictConfig）
        std::map<std::string, Json::Value> elementDictConfigs;  // guideHex -> dictConfig
        std::map<std::string, std::string> userNameCache;  // userId -> userName

        if (!isImage) {
            // 查询设备的协议配置
            auto configResult = co_await dbService_.execSqlCoro(R"(
                SELECT pc.config FROM device d
                JOIN protocol_config pc ON d.protocol_config_id = pc.id
                WHERE d.id = ? AND d.deleted_at IS NULL
            )", {deviceIdStr});

            if (!configResult.empty() && !configResult[0]["config"].isNull()) {
                std::string configStr = configResult[0]["config"].as<std::string>();
                Json::Value config;
                std::istringstream configStream(configStr);
                std::string errs;
                Json::CharReaderBuilder readerBuilder;
                if (Json::parseFromStream(readerBuilder, configStream, &config, &errs)) {
                    // 解析功能码列表，找到当前功能码的要素定义
                    if (config.isMember("funcs") && config["funcs"].isArray()) {
                        for (const auto& func : config["funcs"]) {
                            if (func.get("funcCode", "").asString() == funcCode) {
                                // 找到对应的功能码，提取要素的 dictConfig
                                if (func.isMember("elements") && func["elements"].isArray()) {
                                    for (const auto& elem : func["elements"]) {
                                        std::string guideHex = elem.get("guideHex", "").asString();
                                        if (!guideHex.empty() && elem.isMember("dictConfig")) {
                                            elementDictConfigs[guideHex] = elem["dictConfig"];
                                        }
                                    }
                                }
                                // 也检查 responseElements（下行功能码的应答要素）
                                if (func.isMember("responseElements") && func["responseElements"].isArray()) {
                                    for (const auto& elem : func["responseElements"]) {
                                        std::string guideHex = elem.get("guideHex", "").asString();
                                        if (!guideHex.empty() && elem.isMember("dictConfig")) {
                                            elementDictConfigs[guideHex] = elem["dictConfig"];
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        std::vector<std::string> countParams = {deviceIdStr, funcCode};
        std::string timeCondition = buildTimeCondition(countParams);

        // COUNT 查询也不需要 JOIN
        std::string countSql = R"(
            SELECT COUNT(*) as cnt
            FROM )" + tableName + R"(
            WHERE device_id = ?
              AND data->>'funcCode' = ?
        )" + timeCondition + filterEmptyData;

        std::vector<std::string> queryParams = {deviceIdStr, funcCode};
        buildTimeCondition(queryParams);

        // 主查询：利用 idx_device_data_history (device_id, funcCode表达式, report_time DESC) 索引
        std::string sql = R"(
            SELECT report_time, data
            FROM )" + tableName + R"(
            WHERE device_id = ?
              AND data->>'funcCode' = ?
        )" + timeCondition + filterEmptyData + R"(
            ORDER BY report_time DESC
            LIMIT )" + std::to_string(pageSize) + " OFFSET " + std::to_string((page - 1) * pageSize);

        auto countResult = co_await dbService_.execSqlCoro(countSql, countParams);
        auto result = co_await dbService_.execSqlCoro(sql, queryParams);

        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["cnt"]);

        Json::Value items(Json::arrayValue);
        Json::CharReaderBuilder readerBuilder;

        for (const auto& row : result) {
            Json::Value item;
            item["reportTime"] = FieldHelper::getString(row["report_time"], "");

            if (isImage) {
                // 图片数据：从 data->'data' 中提取图片
                if (!row["data"].isNull()) {
                    std::string dataStr = row["data"].as<std::string>();
                    Json::Value parsedData;
                    std::istringstream stream(dataStr);
                    std::string errs;
                    if (Json::parseFromStream(readerBuilder, stream, &parsedData, &errs)) {
                        if (parsedData.isMember("image")) {
                            item["data"] = parsedData["image"].get("data", "").asString();
                            item["size"] = parsedData["image"].get("size", 0).asInt();
                        }
                    }
                }
                items.append(item);
            } else {
                // 要素数据：从 data->'data' 中提取要素
                Json::Value elements(Json::arrayValue);
                if (!row["data"].isNull()) {
                    std::string dataStr = row["data"].as<std::string>();
                    Json::Value parsedData;
                    std::istringstream stream(dataStr);
                    std::string errs;
                    if (Json::parseFromStream(readerBuilder, stream, &parsedData, &errs)) {
                        // 提取方向、应答ID、用户ID、状态、失败原因
                        if (parsedData.isMember("direction")) {
                            item["direction"] = parsedData["direction"].asString();
                        }
                        int64_t responseIdValue = 0;
                        if (parsedData.isMember("responseId")) {
                            // 兼容字符串和整数类型
                            if (parsedData["responseId"].isInt64()) {
                                responseIdValue = parsedData["responseId"].asInt64();
                                item["responseId"] = responseIdValue;
                            } else if (parsedData["responseId"].isString()) {
                                std::string responseIdStr = parsedData["responseId"].asString();
                                try {
                                    responseIdValue = std::stoll(responseIdStr);
                                    item["responseId"] = responseIdValue;
                                } catch (...) {
                                    // 解析失败，忽略
                                }
                            }
                        }
                        if (parsedData.isMember("status")) {
                            item["status"] = parsedData["status"].asString();
                        }
                        if (parsedData.isMember("failReason")) {
                            item["failReason"] = parsedData["failReason"].asString();
                        }
                        if (parsedData.isMember("userId") && parsedData["userId"].isInt()) {
                            int userId = parsedData["userId"].asInt();
                            item["userId"] = userId;
                            // 获取用户名（使用缓存避免重复查询）
                            std::string userIdStr = std::to_string(userId);
                            if (userNameCache.find(userIdStr) == userNameCache.end()) {
                                auto userResult = co_await dbService_.execSqlCoro(
                                    "SELECT username FROM sys_user WHERE id = ?", {userIdStr}
                                );
                                if (!userResult.empty()) {
                                    userNameCache[userIdStr] = FieldHelper::getString(userResult[0]["username"], "");
                                } else {
                                    userNameCache[userIdStr] = "";
                                }
                            }
                            if (!userNameCache[userIdStr].empty()) {
                                item["userName"] = userNameCache[userIdStr];
                            }
                        }

                        // 如果有应答报文，查询应答报文的解析结果
                        if (responseIdValue > 0) {
                            auto responseResult = co_await dbService_.execSqlCoro(
                                "SELECT data FROM device_data WHERE id = ?",
                                {std::to_string(responseIdValue)}
                            );
                            if (!responseResult.empty() && !responseResult[0]["data"].isNull()) {
                                std::string respDataStr = responseResult[0]["data"].as<std::string>();
                                Json::Value respParsedData;
                                std::istringstream respStream(respDataStr);
                                std::string respErrs;
                                if (Json::parseFromStream(readerBuilder, respStream, &respParsedData, &respErrs)) {
                                    if (respParsedData.isMember("data") && respParsedData["data"].isObject()) {
                                        Json::Value responseElements(Json::arrayValue);
                                        const auto& respDataObj = respParsedData["data"];
                                        for (const auto& respKey : respDataObj.getMemberNames()) {
                                            Json::Value respEl;
                                            const auto& respElemData = respDataObj[respKey];
                                            respEl["name"] = respElemData.get("name", "").asString();
                                            respEl["value"] = respElemData.get("value", Json::nullValue);
                                            if (respElemData.isMember("unit")) {
                                                respEl["unit"] = respElemData["unit"];
                                            }
                                            // 附加 dictConfig
                                            size_t respUnderscorePos = respKey.find('_');
                                            if (respUnderscorePos != std::string::npos) {
                                                std::string respGuideHex = respKey.substr(respUnderscorePos + 1);
                                                auto dictIt = elementDictConfigs.find(respGuideHex);
                                                if (dictIt != elementDictConfigs.end()) {
                                                    respEl["dictConfig"] = dictIt->second;
                                                }
                                            }
                                            responseElements.append(respEl);
                                        }
                                        item["responseElements"] = responseElements;
                                    }
                                }
                            }
                        }

                        if (parsedData.isMember("data") && parsedData["data"].isObject()) {
                            const auto& dataObj = parsedData["data"];
                            // data 格式为 { "funcCode_guideHex": { "name": ..., "value": ..., "unit": ... }, ... }
                            for (const auto& key : dataObj.getMemberNames()) {
                                Json::Value el;
                                const auto& elemData = dataObj[key];
                                el["name"] = elemData.get("name", "").asString();
                                el["value"] = elemData.get("value", Json::nullValue);
                                if (elemData.isMember("unit")) {
                                    el["unit"] = elemData["unit"];
                                }

                                // 从 key（funcCode_guideHex）中提取 guideHex，附加 dictConfig
                                size_t underscorePos = key.find('_');
                                if (underscorePos != std::string::npos) {
                                    std::string guideHex = key.substr(underscorePos + 1);
                                    auto dictIt = elementDictConfigs.find(guideHex);
                                    if (dictIt != elementDictConfigs.end()) {
                                        el["dictConfig"] = dictIt->second;
                                    }
                                }

                                elements.append(el);
                            }
                        }
                    }
                }
                item["elements"] = elements;
                items.append(item);
            }
        }

        co_return std::make_tuple(items, total);
    }
};
