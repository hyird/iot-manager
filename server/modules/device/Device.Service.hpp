#pragma once

#include "domain/Device.hpp"
#include "DeviceDataTransformer.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/protocol/ProtocolDispatcher.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/utils/SqlHelper.hpp"

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
    template<typename T = void>
    using Task = drogon::Task<T>;

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
    Task<void> create(const Json::Value& data) {
        auto device = Device::create(data);

        device.require(Device::nameUnique)
              .require(Device::linkExists)
              .require(Device::protocolConfigExists);

        // 设备编码非空时才检查唯一性（Modbus 设备无编码）
        if (data.isMember("device_code") && !data["device_code"].asString().empty()) {
            device.require(Device::codeUnique);
        }

        co_await device.save();
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
        std::vector<int> deviceIds;
        deviceIds.reserve(cachedDevices.size());
        for (const auto& d : cachedDevices) {
            deviceIds.push_back(d.id);
        }
        std::string idList = SqlHelper::buildInClause(deviceIds);

        // 3. 优化的 SQL：使用 DISTINCT ON 获取每个设备每个功能码的最新数据
        std::string sql =
            "SELECT device_id, data, report_time, data->>'funcCode' as func_code "
            "FROM ( "
            "  SELECT DISTINCT ON (device_id, data->>'funcCode') "
            "         device_id, data, report_time "
            "  FROM device_data "
            "  WHERE device_id IN (" + idList + ") "
            "    AND report_time >= NOW() - INTERVAL '" + std::to_string(Constants::DEVICE_DATA_LOOKBACK_DAYS) + " days' "
            "  ORDER BY device_id, data->>'funcCode', report_time DESC NULLS LAST "
            ") sub";

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

            // 按协议类型返回各自的配置字段
            if (device.protocolType == Constants::PROTOCOL_SL651) {
                item["downFuncs"] = downFuncs;
                item["imageFuncs"] = imageFuncs;
            } else if (device.protocolType == Constants::PROTOCOL_MODBUS) {
                item["downFuncs"] = downFuncs;
            }

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

            // 按协议类型返回各自的配置字段
            Json::Value downFuncs, imageFuncs;
            DeviceDataTransformer::parseProtocolFuncsStatic(device, downFuncs, imageFuncs);

            if (device.protocolType == Constants::PROTOCOL_SL651) {
                item["downFuncs"] = downFuncs;
                item["imageFuncs"] = imageFuncs;
            } else if (device.protocolType == Constants::PROTOCOL_MODBUS) {
                item["downFuncs"] = downFuncs;  // Modbus 也有下行功能码（写寄存器）
            }

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

        // 构建返回数据（复用 DeviceDataTransformer::buildRealtimeItem）
        Json::Value items(Json::arrayValue);
        RealtimeDataCache::DeviceRealtimeData emptyData;

        for (const auto& device : cachedDevices) {
            auto dataIt = deviceDataMap.find(device.id);
            auto timeIt = latestTimeMap.find(device.id);
            const auto& data = dataIt != deviceDataMap.end() ? dataIt->second : emptyData;
            std::string latestTime = timeIt != latestTimeMap.end() ? timeIt->second : "";
            items.append(DeviceDataTransformer::buildRealtimeItem(device, data, latestTime));
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
        int pageSize,
        int deviceIdParam = 0
    ) {
        int deviceId = deviceIdParam;

        if (deviceId <= 0) {
            // 通过 device_code 查找 device_id（SL651 设备）
            auto deviceResult = co_await dbService_.execSqlCoro(
                "SELECT id FROM device WHERE protocol_params->>'device_code' = ? AND deleted_at IS NULL",
                {code}
            );

            if (deviceResult.empty()) {
                co_return std::make_tuple(Json::Value(Json::arrayValue), 0);
            }

            deviceId = FieldHelper::getInt(deviceResult[0]["id"]);
        }

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
                std::tm tm{};
                #ifdef _WIN32
                gmtime_s(&tm, &tt);
                #else
                gmtime_r(&tt, &tm);
                #endif
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
            std::tm tm{};
            #ifdef _WIN32
            gmtime_s(&tm, &tt);
            #else
            gmtime_r(&tt, &tm);
            #endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT00:00:00Z", &tm);
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
                    // Modbus：从 registers[] 中按 "registerType_address" 构建 dictConfig 映射
                    if (config.isMember("registers") && config["registers"].isArray()) {
                        for (const auto& reg : config["registers"]) {
                            if (reg.isMember("dictConfig") && reg["dictConfig"].isObject()) {
                                std::string regType = reg.get("registerType", "").asString();
                                int addr = reg.get("address", 0).asInt();
                                std::string fullKey = regType + "_" + std::to_string(addr);
                                elementDictConfigs[fullKey] = reg["dictConfig"];
                            }
                        }
                    }
                    // SL651：从 funcs[] 中按功能码匹配提取 dictConfig
                    if (config.isMember("funcs") && config["funcs"].isArray()) {
                        for (const auto& func : config["funcs"]) {
                            if (func.get("funcCode", "").asString() == funcCode) {
                                if (func.isMember("elements") && func["elements"].isArray()) {
                                    for (const auto& elem : func["elements"]) {
                                        std::string guideHex = elem.get("guideHex", "").asString();
                                        if (!guideHex.empty() && elem.isMember("dictConfig")) {
                                            elementDictConfigs[guideHex] = elem["dictConfig"];
                                        }
                                    }
                                }
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

        // 预扫描：收集所有 userId 和 responseId，批量查询（避免 N+1）
        std::map<int64_t, std::string> responseDataCache;  // responseId -> data JSON string
        if (!isImage) {
            std::set<int> userIdSet;
            std::set<int64_t> responseIdSet;
            for (const auto& row : result) {
                if (row["data"].isNull()) continue;
                std::string dataStr = row["data"].as<std::string>();
                Json::Value parsedData;
                std::istringstream iss(dataStr);
                std::string errs;
                if (Json::parseFromStream(readerBuilder, iss, &parsedData, &errs)) {
                    if (parsedData.isMember("userId") && parsedData["userId"].isInt()) {
                        userIdSet.insert(parsedData["userId"].asInt());
                    }
                    // 收集 responseId 用于批量预加载
                    if (parsedData.isMember("responseId")) {
                        int64_t rid = 0;
                        if (parsedData["responseId"].isInt64()) {
                            rid = parsedData["responseId"].asInt64();
                        } else if (parsedData["responseId"].isString()) {
                            try { rid = std::stoll(parsedData["responseId"].asString()); } catch (...) {}
                        }
                        if (rid > 0) responseIdSet.insert(rid);
                    }
                }
            }
            if (!userIdSet.empty()) {
                std::vector<int> userIds(userIdSet.begin(), userIdSet.end());
                std::string userIdList = SqlHelper::buildInClause(userIds);
                auto userResult = co_await dbService_.execSqlCoro(
                    "SELECT id, username FROM sys_user WHERE id IN (" + userIdList + ")"
                );
                for (const auto& uRow : userResult) {
                    std::string uid = std::to_string(FieldHelper::getInt(uRow["id"]));
                    userNameCache[uid] = FieldHelper::getString(uRow["username"], "");
                }
            }
            // 批量预加载所有应答报文数据（N+1 → 1 次查询）
            if (!responseIdSet.empty()) {
                std::vector<int64_t> rids(responseIdSet.begin(), responseIdSet.end());
                std::string ridList = SqlHelper::buildInClause(rids);
                auto respResult = co_await dbService_.execSqlCoro(
                    "SELECT id, data FROM device_data WHERE id IN (" + ridList + ")"
                );
                for (const auto& rRow : respResult) {
                    int64_t rid = rRow["id"].as<int64_t>();
                    if (!rRow["data"].isNull()) {
                        responseDataCache[rid] = rRow["data"].as<std::string>();
                    }
                }
            }
        }

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
                            // 从预加载的缓存中查找用户名
                            std::string userIdStr = std::to_string(userId);
                            auto nameIt = userNameCache.find(userIdStr);
                            if (nameIt != userNameCache.end() && !nameIt->second.empty()) {
                                item["userName"] = nameIt->second;
                            }
                        }

                        // 如果有应答报文，从预加载缓存中获取（批量查询已在上方完成）
                        if (responseIdValue > 0) {
                            auto respCacheIt = responseDataCache.find(responseIdValue);
                            if (respCacheIt != responseDataCache.end()) {
                                std::string respDataStr = respCacheIt->second;
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
                            }  // end respCacheIt
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

                                // 附加 dictConfig：先尝试完整 key（Modbus: HOLDING_REGISTER_100）
                                auto dictIt = elementDictConfigs.find(key);
                                if (dictIt == elementDictConfigs.end()) {
                                    // 回退：从 key（SL651: funcCode_guideHex）中提取 guideHex
                                    size_t underscorePos = key.find('_');
                                    if (underscorePos != std::string::npos) {
                                        dictIt = elementDictConfigs.find(key.substr(underscorePos + 1));
                                    }
                                }
                                if (dictIt != elementDictConfigs.end()) {
                                    el["dictConfig"] = dictIt->second;
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

    // ==================== 指令下发 ====================

    /**
     * @brief 下发设备指令
     * @param linkId 链路ID
     * @param deviceCode 设备编码
     * @param funcCode 功能码
     * @param elements 要素数据
     * @param userId 操作用户ID
     * @return 设备应答成功返回 true
     */
    Task<bool> sendCommand(int linkId, const std::string& deviceCode,
                           const std::string& funcCode, const Json::Value& elements,
                           int userId, int deviceId = 0) {
        bool success = co_await ProtocolDispatcher::instance().sendCommand(
            linkId, deviceCode, funcCode, elements, userId, deviceId
        );

        // 无论成功或失败，都已保存记录到数据库，需要更新版本号以刷新 ETag 缓存
        ResourceVersion::instance().incrementVersion("device");

        co_return success;
    }
};
