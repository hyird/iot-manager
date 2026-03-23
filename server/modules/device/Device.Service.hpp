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
#include "common/utils/AppException.hpp"
#include "common/utils/SqlHelper.hpp"
#include "common/utils/JsonHelper.hpp"
#include "common/edgenode/AgentBridgeManager.hpp"

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
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

private:
    DatabaseService dbService_;

    Task<DeviceCache::CachedDevice> requireCommandDevice(
        const std::string& deviceCode,
        int deviceId
    ) {
        auto cachedDevices = co_await DeviceCache::instance().getDevices();
        for (const auto& device : cachedDevices) {
            if (deviceId > 0 && device.id == deviceId) {
                co_return device;
            }
            if (deviceId <= 0 && !deviceCode.empty() && device.deviceCode == deviceCode) {
                co_return device;
            }
        }
        throw NotFoundException("设备不存在");
    }

    std::string resolveSl651CommandFuncCode(
        const DeviceCache::CachedDevice& device,
        const Json::Value& elements
    ) {
        if (device.protocolConfig.isNull()
            || !device.protocolConfig.isMember("funcs")
            || !device.protocolConfig["funcs"].isArray()) {
            throw ValidationException("设备协议配置缺失，无法推导功能码");
        }

        std::unordered_map<std::string, std::string> elementFuncCodes;
        for (const auto& func : device.protocolConfig["funcs"]) {
            if (func.get("dir", "").asString() != "DOWN") continue;

            std::string funcCode = func.get("funcCode", "").asString();
            if (funcCode.empty() || !func.isMember("elements") || !func["elements"].isArray()) {
                continue;
            }

            for (const auto& element : func["elements"]) {
                std::string elementId = element.get("id", "").asString();
                if (elementId.empty()) continue;

                auto [it, inserted] = elementFuncCodes.emplace(elementId, funcCode);
                if (!inserted && it->second != funcCode) {
                    throw ValidationException("协议配置异常：同一要素归属多个功能码");
                }
            }
        }

        std::string resolvedFuncCode;
        for (const auto& element : elements) {
            std::string elementId = element.get("elementId", "").asString();
            if (elementId.empty()) {
                throw ValidationException("要素 elementId 不能为空");
            }

            auto it = elementFuncCodes.find(elementId);
            if (it == elementFuncCodes.end()) {
                throw ValidationException("未找到要素对应的功能码: " + elementId);
            }

            if (resolvedFuncCode.empty()) {
                resolvedFuncCode = it->second;
            } else if (resolvedFuncCode != it->second) {
                throw ValidationException("一次下发只能包含同一功能码的要素");
            }
        }

        if (resolvedFuncCode.empty()) {
            throw ValidationException("无法根据要素推导功能码");
        }

        return resolvedFuncCode;
    }

    Task<std::string> resolveCommandFuncCode(
        const std::string& deviceCode,
        int deviceId,
        const Json::Value& elements
    ) {
        auto device = co_await requireCommandDevice(deviceCode, deviceId);

        if (device.protocolType == Constants::PROTOCOL_MODBUS) {
            co_return std::string("MODBUS_WRITE");
        }
        if (device.protocolType == Constants::PROTOCOL_SL651) {
            co_return resolveSl651CommandFuncCode(device, elements);
        }

        throw ValidationException("暂不支持该协议的指令下发");
    }

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
    Task<void> create(const Json::Value& data) {
        auto device = Device::create(data);

        device.require(Device::nameUnique)
              .require(Device::linkExists)
              .require(Device::agentExists)
              .require(Device::protocolConfigExists);

        // 设备编码非空时才检查唯一性（Modbus 设备无编码）
        if (data.isMember("device_code") && !data["device_code"].asString().empty()) {
            device.require(Device::codeUnique);
        }

        // Modbus 多设备链路必须配置注册包
        device.require(Device::modbusRegistrationRequired);

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

        // Modbus 多设备链路必须配置注册包
        device.require(Device::modbusRegistrationRequired);

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

    // ==================== 查询接口 ====================

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

            // 按协议类型返回各自的操作定义（协议无关命名）
            Json::Value commandOps, imageOps;
            DeviceDataTransformer::parseProtocolFuncsStatic(device, commandOps, imageOps);

            if (!commandOps.empty()) {
                item["commandOperations"] = commandOps;
            }
            if (!imageOps.empty()) {
                item["imageOperations"] = imageOps;
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

        // 检测 Redis 中缺失数据的设备（TTL 过期或从未加载），从 DB 补加载
        std::vector<int> missingIds;
        for (int id : deviceIds) {
            if (deviceDataMap.find(id) == deviceDataMap.end()) {
                missingIds.push_back(id);
            }
        }
        if (!missingIds.empty()) {
            co_await realtimeCache.loadFromDb(missingIds);
            auto extraData = co_await realtimeCache.getBatch(missingIds);
            for (auto& [id, data] : extraData) {
                deviceDataMap[id] = std::move(data);
            }
            // 离线设备不更新 latestTimeMap（保持为空，前端显示离线状态）
        }

        // 构建返回数据（复用 DeviceDataTransformer::buildRealtimeItem）
        Json::Value items(Json::arrayValue);
        RealtimeDataCache::DeviceRealtimeData emptyData;
        auto connChecker = [](int deviceId) {
            return ProtocolDispatcher::instance().isDeviceConnected(deviceId);
        };

        for (const auto& device : cachedDevices) {
            auto dataIt = deviceDataMap.find(device.id);
            auto timeIt = latestTimeMap.find(device.id);
            const auto& data = dataIt != deviceDataMap.end() ? dataIt->second : emptyData;
            std::string latestTime = timeIt != latestTimeMap.end() ? timeIt->second : "";
            items.append(DeviceDataTransformer::buildRealtimeItem(device, data, latestTime, connChecker));
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
        const std::string& dataType,
        const std::string& startTime,
        const std::string& endTime,
        int page,
        int pageSize,
        int deviceId
    ) {
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

        // 查询具体数据（直接使用 device_id，利用复合索引）
        bool isImage = (dataType == "IMAGE");

        // 获取协议配置以获取字典配置（dictConfig）
        std::map<std::string, Json::Value> elementDictConfigs;  // guideHex -> dictConfig

        if (!isImage) {
            // 查询设备的协议配置
            auto configResult = co_await dbService_.execSqlCoro(R"(
                SELECT pc.config FROM device d
                JOIN protocol_config pc ON d.protocol_config_id = pc.id AND pc.deleted_at IS NULL
                WHERE d.id = ? AND d.deleted_at IS NULL
            )", {deviceIdStr});

            if (!configResult.empty() && !configResult[0]["config"].isNull()) {
                try {
                auto config = JsonHelper::parse(configResult[0]["config"].as<std::string>());
                {
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
                    // SL651：从所有 funcs[] 中提取 dictConfig
                    if (config.isMember("funcs") && config["funcs"].isArray()) {
                        for (const auto& func : config["funcs"]) {
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
                        }
                    }
                }
                } catch (...) {}
            }
        }

        auto t0 = std::chrono::steady_clock::now();

        // 按 dataType 过滤 funcCode（IMAGE 对应 36/B6，ELEMENT 排除 36/B6）
        // ELEMENT 模式额外排除 DOWN 方向（写命令记录）
        std::string funcCodeFilter = isImage
            ? " AND data->>'funcCode' IN ('36', 'B6')"
            : " AND data->>'funcCode' NOT IN ('36', 'B6') AND COALESCE(data->>'direction', 'UP') != 'DOWN'";

        std::vector<std::string> countParams = {deviceIdStr};
        std::string timeCondition = buildTimeCondition(countParams);
        const bool isPaged = pageSize > 0;
        const int normalizedPage = page > 0 ? page : 1;
        const int effectivePageSize = isPaged ? pageSize : Constants::MAX_UNPAGED_ROWS;
        const int effectiveOffset = isPaged ? ((normalizedPage - 1) * pageSize) : 0;

        // COUNT 查询
        std::string countSql = R"(
            SELECT COUNT(*) as cnt
            FROM )" + tableName + R"(
            WHERE device_id = ?
        )" + funcCodeFilter + timeCondition + filterEmptyData;

        std::vector<std::string> queryParams = {deviceIdStr};
        buildTimeCondition(queryParams);

        // 内层查询：SQL 层提取 JSONB 标量字段
        std::string innerSql = R"(
            SELECT id, report_time,
                   data->>'direction' AS direction,
                   data->>'userId' AS user_id_str,
                   data->>'responseId' AS response_id_str,
                   data->>'status' AS status,
                   data->>'failReason' AS fail_reason,
                   data->'image'->>'data' AS image_data,
                   data->'image'->>'size' AS image_size_str,
                   data->'data' AS elements_data
            FROM )" + tableName + R"(
            WHERE device_id = ?
        )" + funcCodeFilter + timeCondition + filterEmptyData + R"(
            ORDER BY report_time DESC
        )";

        if (effectivePageSize > 0) {
            innerSql += " LIMIT " + std::to_string(effectivePageSize);
            if (effectiveOffset > 0) {
                innerSql += " OFFSET " + std::to_string(effectiveOffset);
            }
        }

        // 外层查询：JOIN sys_user/device_data + jsonb_each 展开要素，消除预扫描和全部 C++ JSON 解析
        std::string sql;
        if (isImage) {
            sql = innerSql;
        } else {
            sql = R"(
                SELECT d.id AS record_id, d.report_time, d.direction,
                       d.user_id_str, d.response_id_str, d.status, d.fail_reason,
                       u.username AS user_name,
                       resp.data->'data' AS resp_elements_data,
                       e.key AS elem_key,
                       e.value->>'name' AS elem_name,
                       e.value->'value' AS elem_value,
                       e.value->>'unit' AS elem_unit
                FROM ()" + innerSql + R"() d
                LEFT JOIN LATERAL jsonb_each(d.elements_data) AS e(key, value) ON true
                LEFT JOIN sys_user u ON d.user_id_str IS NOT NULL AND u.id = (d.user_id_str)::int
                LEFT JOIN device_data resp ON d.response_id_str IS NOT NULL AND resp.id = (d.response_id_str)::bigint
                ORDER BY d.report_time DESC, d.id DESC
            )";
        }

        // 不分页时跳过 COUNT 查询
        int total = 0;
        if (isPaged) {
            auto countResult = co_await dbService_.execSqlCoro(countSql, countParams);
            total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["cnt"]);
        }

        auto result = co_await dbService_.execSqlCoro(sql, queryParams);

        auto t1 = std::chrono::steady_clock::now();
        LOG_INFO << "[queryHistory] SQL execution: "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                 << "ms, rows=" << result.size();

        Json::Value items(Json::arrayValue);

        // JSONB 单值解析器：将 PostgreSQL 返回的 JSONB 文本转为 Json::Value（无需 JSON 库解析）
        auto parseJsonbValue = [](const std::string& v) -> Json::Value {
            if (v.empty() || v == "null") return Json::nullValue;
            if (v == "true") return Json::Value(true);
            if (v == "false") return Json::Value(false);
            if (v.front() == '"' && v.size() >= 2) return v.substr(1, v.size() - 2);
            try {
                if (v.find('.') != std::string::npos || v.find('e') != std::string::npos
                    || v.find('E') != std::string::npos) {
                    return std::stod(v);
                }
                return static_cast<Json::Int64>(std::stoll(v));
            } catch (...) {
                return Json::Value(v);
            }
        };

        auto t2 = std::chrono::steady_clock::now();

        if (isImage) {
            // 图片数据：直接从 SQL 列读取（零 JSON 解析）
            for (const auto& row : result) {
                Json::Value item;
                item["reportTime"] = FieldHelper::getString(row["report_time"], "");
                if (!row["image_data"].isNull()) {
                    item["data"] = row["image_data"].as<std::string>();
                    if (!row["image_size_str"].isNull()) {
                        try { item["size"] = std::stoi(row["image_size_str"].as<std::string>()); }
                        catch (...) { item["size"] = 0; }
                    }
                }
                items.append(item);
            }
        } else {
            // 要素数据：按 record_id 分组，全部字段从 SQL 列读取
            int64_t prevRecordId = -1;
            Json::Value item;
            Json::Value elements(Json::arrayValue);

            for (const auto& row : result) {
                int64_t recordId = row["record_id"].as<int64_t>();

                if (recordId != prevRecordId) {
                    // 写入上一条记录
                    if (prevRecordId >= 0) {
                        item["elements"] = std::move(elements);
                        items.append(std::move(item));
                        elements = Json::Value(Json::arrayValue);
                    }
                    prevRecordId = recordId;

                    // 标量字段：直接从 SQL 列读取
                    item = Json::Value();
                    item["reportTime"] = FieldHelper::getString(row["report_time"], "");
                    if (!row["direction"].isNull()) {
                        item["direction"] = row["direction"].as<std::string>();
                    }
                    int64_t responseIdValue = 0;
                    if (!row["response_id_str"].isNull()) {
                        try {
                            responseIdValue = std::stoll(row["response_id_str"].as<std::string>());
                            item["responseId"] = responseIdValue;
                        } catch (...) {}
                    }
                    if (!row["status"].isNull()) {
                        item["status"] = row["status"].as<std::string>();
                    }
                    if (!row["fail_reason"].isNull()) {
                        item["failReason"] = row["fail_reason"].as<std::string>();
                    }
                    // 用户名：直接从 JOIN 结果读取
                    if (!row["user_id_str"].isNull()) {
                        try {
                            item["userId"] = std::stoi(row["user_id_str"].as<std::string>());
                        } catch (...) {}
                    }
                    if (!row["user_name"].isNull()) {
                        item["userName"] = row["user_name"].as<std::string>();
                    }

                    // 应答报文要素：从 JOIN 的 resp_elements_data 解析（仅每条记录一次，体积小）
                    if (responseIdValue > 0 && !row["resp_elements_data"].isNull()) {
                        try {
                        auto respData = JsonHelper::parse(row["resp_elements_data"].as<std::string>());
                        if (respData.isObject()) {
                            Json::Value responseElements(Json::arrayValue);
                            for (const auto& respKey : respData.getMemberNames()) {
                                Json::Value respEl;
                                const auto& respElemData = respData[respKey];
                                respEl["name"] = respElemData.get("name", "").asString();
                                respEl["value"] = respElemData.get("value", Json::nullValue);
                                if (respElemData.isMember("unit")) {
                                    respEl["unit"] = respElemData["unit"];
                                }
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
                        } catch (...) {}
                    }
                }

                // 要素字段：每行一个要素，直接从 SQL 列读取
                if (!row["elem_key"].isNull()) {
                    Json::Value el;
                    std::string elemKey = row["elem_key"].as<std::string>();
                    el["name"] = row["elem_name"].isNull() ? "" : row["elem_name"].as<std::string>();
                    if (!row["elem_value"].isNull()) {
                        el["value"] = parseJsonbValue(row["elem_value"].as<std::string>());
                    } else {
                        el["value"] = Json::nullValue;
                    }
                    if (!row["elem_unit"].isNull()) {
                        el["unit"] = row["elem_unit"].as<std::string>();
                    }

                    // 附加 dictConfig
                    auto dictIt = elementDictConfigs.find(elemKey);
                    if (dictIt == elementDictConfigs.end()) {
                        size_t pos = elemKey.find('_');
                        if (pos != std::string::npos) {
                            dictIt = elementDictConfigs.find(elemKey.substr(pos + 1));
                        }
                    }
                    if (dictIt != elementDictConfigs.end()) {
                        el["dictConfig"] = dictIt->second;
                    }

                    elements.append(std::move(el));
                }
            }

            // 写入最后一条记录
            if (prevRecordId >= 0) {
                item["elements"] = std::move(elements);
                items.append(std::move(item));
            }
        }

        auto t3 = std::chrono::steady_clock::now();
        LOG_INFO << "[queryHistory] Format loop: "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count() << "ms";

        // 不分页时 total 用实际数量
        if (!isPaged) {
            total = static_cast<int>(items.size());
            if (total >= Constants::MAX_UNPAGED_ROWS) {
                LOG_WARN << "[queryHistory] Unpaged request hit safety limit: "
                         << Constants::MAX_UNPAGED_ROWS
                         << ", please use pagination for full-range exports";
            }
        }

        LOG_INFO << "[queryHistory] Total: "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count()
                 << "ms, items=" << items.size();

        co_return std::make_tuple(items, total);
    }

    /**
     * @brief 查询历史数据 - Raw JSONB 透传模式（零 C++ JSON 解析）
     *
     * 用于图表等非分页场景：直接从 PostgreSQL 取 JSONB 原文，
     * 通过字符串拼接构建响应体，完全跳过 jsoncpp。
     *
     * @return 完整的 JSON 响应字符串，可直接用于 Response::rawJson()
     */
    Task<std::string> queryHistoryRaw(
        const std::string& startTime,
        const std::string& endTime,
        int deviceId
    ) {
        std::string deviceIdStr = std::to_string(deviceId);

        // 判断是否需要查询归档数据
        bool needArchive = false;
        try {
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
            if (startTime < std::string(buf)) {
                needArchive = true;
            }
        } catch (...) {}

        std::string tableName = needArchive ? "device_data_all" : "device_data";

        // 极简 SQL：只取 report_time 和 data->'data'（JSONB 原样输出，排除图片功能码）
        std::vector<std::string> params = {deviceIdStr, startTime, endTime};

        std::string sql = R"(
            SELECT report_time, data->'data' AS elements_data
            FROM )" + tableName + R"(
            WHERE device_id = ?
              AND data->>'funcCode' NOT IN ('36', 'B6')
              AND report_time >= ?::timestamptz
              AND report_time <= ?::timestamptz
              AND (data->'data' IS NOT NULL AND data->'data' <> '{}'::jsonb)
            ORDER BY report_time DESC
        )";

        auto t0 = std::chrono::steady_clock::now();
        auto result = co_await dbService_.execSqlCoro(sql, params);
        auto t1 = std::chrono::steady_clock::now();

        LOG_INFO << "[queryHistoryRaw] SQL: "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                 << "ms, rows=" << result.size();

        // 直接拼接 JSON 字符串，完全跳过 jsoncpp
        std::string body;
        body.reserve(result.size() * 300 + 256);
        body += R"({"code":0,"message":"Success","data":{"list":[)";

        for (size_t i = 0; i < result.size(); ++i) {
            if (i > 0) body += ',';
            body += R"({"reportTime":")";
            body += result[i]["report_time"].as<std::string>();
            body += R"(","data":)";
            if (!result[i]["elements_data"].isNull()) {
                body += result[i]["elements_data"].as<std::string>();
            } else {
                body += "{}";
            }
            body += '}';
        }

        body += R"(],"total":)";
        body += std::to_string(result.size());
        body += "}}";

        auto t2 = std::chrono::steady_clock::now();
        LOG_INFO << "[queryHistoryRaw] String concat: "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()
                 << "ms, body=" << body.size() << " bytes";

        co_return body;
    }

    /**
     * @brief Open API 实时数据（包含设备属性、elementId、操作定义）
     */
    Task<Json::Value> listRealtimeForOpenApi() {
        auto cachedDevices = co_await DeviceCache::instance().getDevices();
        if (cachedDevices.empty()) {
            co_return Json::Value(Json::arrayValue);
        }

        std::vector<int> deviceIds;
        deviceIds.reserve(cachedDevices.size());
        for (const auto& device : cachedDevices) {
            deviceIds.push_back(device.id);
        }

        auto& realtimeCache = RealtimeDataCache::instance();
        if (!realtimeCache.isInitialized()) {
            co_await realtimeCache.initializeFromDb(deviceIds);
        }

        auto deviceDataMap = co_await realtimeCache.getBatch(deviceIds);
        auto latestTimeMap = co_await realtimeCache.getLatestReportTimes(deviceIds);

        std::vector<int> missingIds;
        for (int id : deviceIds) {
            if (deviceDataMap.find(id) == deviceDataMap.end()) {
                missingIds.push_back(id);
            }
        }
        if (!missingIds.empty()) {
            co_await realtimeCache.loadFromDb(missingIds);
            auto extraData = co_await realtimeCache.getBatch(missingIds);
            for (auto& [id, data] : extraData) {
                deviceDataMap[id] = std::move(data);
            }
        }

        Json::Value items(Json::arrayValue);
        RealtimeDataCache::DeviceRealtimeData emptyData;
        auto connChecker = [](int deviceId) {
            return ProtocolDispatcher::instance().isDeviceConnected(deviceId);
        };

        for (const auto& device : cachedDevices) {
            auto dataIt = deviceDataMap.find(device.id);
            auto timeIt = latestTimeMap.find(device.id);
            const auto& data = dataIt != deviceDataMap.end() ? dataIt->second : emptyData;
            std::string latestTime = timeIt != latestTimeMap.end() ? timeIt->second : "";
            items.append(DeviceDataTransformer::buildOpenApiRealtimeItem(device, data, latestTime, connChecker));
        }

        co_return items;
    }

    // ==================== 指令下发 ====================

    /**
     * @brief 下发设备指令
     * @param linkId 链路ID
     * @param deviceCode 设备编码
     * @param elements 要素数据
     * @param userId 操作用户ID
     * @return CommandResult 包含状态和错误信息
     */
    Task<CommandResult> sendCommand(int linkId, const std::string& deviceCode,
                                    const Json::Value& elements,
                                    int userId, int deviceId = 0) {
        std::string funcCode = co_await resolveCommandFuncCode(deviceCode, deviceId, elements);

        CommandRequest req;
        req.linkId = linkId;
        req.deviceId = deviceId;
        req.deviceCode = deviceCode;
        req.funcCode = funcCode;
        req.elements = elements;
        req.userId = userId;

        auto result = co_await ProtocolDispatcher::instance().sendCommand(req);

        // 无论成功或失败，都已保存记录到数据库，需要更新版本号以刷新 ETag 缓存
        ResourceVersion::instance().incrementVersion("device");

        co_return result;
    }
};
