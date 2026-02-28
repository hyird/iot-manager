#pragma once

#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/utils/Constants.hpp"

/**
 * @brief 设备数据转换器
 *
 * 单一职责：负责设备数据的格式转换和解析
 * 从 DeviceService 中提取，遵循单一职责原则
 */
class DeviceDataTransformer {
public:
    /**
     * @brief 实时要素数据结构
     */
    struct ElementData {
        std::string name;
        Json::Value value;
        std::string unit;
        std::string reportTime;
    };

    /**
     * @brief 从完整键名提取 guideHex
     * 例如："34_F1F1" -> "F1F1"
     */
    static std::string extractGuideHex(const std::string& fullKey) {
        size_t pos = fullKey.find('_');
        return (pos != std::string::npos) ? fullKey.substr(pos + 1) : fullKey;
    }

    /**
     * @brief 解析实时数据值（从多个功能码数据中提取最新值）
     * @param funcDataMap 功能码 -> {数据对象, 上报时间}
     * @return guideHex -> ElementData
     */
    static std::map<std::string, ElementData> parseRealtimeValues(
        const std::map<std::string, std::pair<Json::Value, std::string>>& funcDataMap
    ) {
        std::map<std::string, ElementData> realtimeValues;

        for (const auto& [funcCode, dataPair] : funcDataMap) {
            const auto& dataObj = dataPair.first;
            const auto& reportTime = dataPair.second;

            if (!dataObj.isMember("data") || !dataObj["data"].isObject()) continue;

            const auto& elementsObj = dataObj["data"];
            for (const auto& fullKey : elementsObj.getMemberNames()) {
                const auto& elemData = elementsObj[fullKey];

                ElementData ed = {
                    elemData.get("name", "").asString(),
                    elemData.get("value", Json::nullValue),
                    elemData.get("unit", "").asString(),
                    reportTime
                };

                // 按 guideHex 存储（SL651: "34_F1F1" → "F1F1"）
                std::string guideHex = extractGuideHex(fullKey);
                auto it = realtimeValues.find(guideHex);
                if (it == realtimeValues.end() || reportTime > it->second.reportTime) {
                    realtimeValues[guideHex] = ed;
                }

                // 同时按 fullKey 存储（Modbus: "HOLDING_REGISTER_0" 保持完整）
                if (fullKey != guideHex) {
                    auto it2 = realtimeValues.find(fullKey);
                    if (it2 == realtimeValues.end() || reportTime > it2->second.reportTime) {
                        realtimeValues[fullKey] = ed;
                    }
                }
            }
        }

        return realtimeValues;
    }

    /**
     * @brief 检查功能是否包含 JPEG 编码的要素
     */
    static bool hasJpegElement(const Json::Value& func) {
        if (!func.isMember("elements") || !func["elements"].isArray()) return false;
        for (const auto& el : func["elements"]) {
            if (el.get("encode", "").asString() == "JPEG") return true;
        }
        return false;
    }

    /**
     * @brief 解析下行功能
     */
    static Json::Value parseDownFunc(const Json::Value& func) {
        Json::Value downFunc;
        downFunc["funcCode"] = func.get("funcCode", "").asString();
        downFunc["name"] = func.get("name", "").asString();

        Json::Value funcElements(Json::arrayValue);
        for (const auto& el : func["elements"]) {
            Json::Value funcEl;
            funcEl["elementId"] = el.get("id", "").asString();
            funcEl["name"] = el.get("name", "").asString();
            funcEl["value"] = "";
            std::string unit = el.get("unit", "").asString();
            if (!unit.empty()) {
                funcEl["unit"] = unit;
            }
            if (el.isMember("options") && el["options"].isArray() && !el["options"].empty()) {
                funcEl["options"] = el["options"];
            }
            funcElements.append(funcEl);
        }
        downFunc["elements"] = funcElements;
        return downFunc;
    }

    /**
     * @brief 解析图片功能（不含图片数据）
     */
    static Json::Value parseImageFuncBase(const Json::Value& func) {
        Json::Value imageFunc;
        imageFunc["funcCode"] = func.get("funcCode", "").asString();
        imageFunc["name"] = func.get("name", "").asString();

        Json::Value funcElements(Json::arrayValue);
        for (const auto& el : func["elements"]) {
            Json::Value funcEl;
            funcEl["elementId"] = el.get("id", "").asString();
            funcEl["name"] = el.get("name", "").asString();
            funcEl["encode"] = el.get("encode", "").asString();
            std::string unit = el.get("unit", "").asString();
            if (!unit.empty()) {
                funcEl["unit"] = unit;
            }
            funcElements.append(funcEl);
        }
        imageFunc["elements"] = funcElements;
        return imageFunc;
    }

    /**
     * @brief 从功能数据中查找图片
     */
    static std::optional<std::string> findImageData(
        const std::string& funcCode,
        const std::map<std::string, Json::Value>& funcDataMap
    ) {
        auto it = funcDataMap.find(funcCode);
        if (it == funcDataMap.end() || !it->second.isMember("data")) return std::nullopt;

        const auto& dataObj = it->second["data"];
        for (const auto& key : dataObj.getMemberNames()) {
            if (dataObj[key].get("type", "").asString() == "JPEG") {
                std::string imageData = dataObj[key].get("value", "").asString();
                if (!imageData.empty()) return imageData;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 解析上行要素（普通数据）
     */
    static void parseUpElements(
        const Json::Value& func,
        const std::map<std::string, ElementData>& realtimeValues,
        std::set<std::string>& addedGuideHex,
        Json::Value& elements
    ) {
        for (const auto& el : func["elements"]) {
            std::string guideHex = el.get("guideHex", "").asString();

            if (addedGuideHex.count(guideHex) > 0) continue;
            addedGuideHex.insert(guideHex);

            Json::Value element;
            element["name"] = el.get("name", "").asString();

            std::string unit = el.get("unit", "").asString();
            if (!unit.empty()) element["unit"] = unit;

            std::string encode = el.get("encode", "").asString();
            if (!encode.empty()) element["encode"] = encode;

            if (encode == "DICT" && el.isMember("dictConfig")) {
                element["dictConfig"] = el["dictConfig"];
            }

            auto it = realtimeValues.find(guideHex);
            element["value"] = (it != realtimeValues.end()) ? it->second.value : Json::nullValue;

            elements.append(element);
        }
    }

    /**
     * @brief 构建设备基本信息 JSON
     */
    static Json::Value buildDeviceBaseInfo(const DeviceCache::CachedDevice& device) {
        Json::Value item;

        // 通用字段
        item["id"] = device.id;
        item["name"] = device.name;
        item["link_id"] = device.linkId;
        item["protocol_config_id"] = device.protocolConfigId;
        item["group_id"] = device.groupId > 0 ? Json::Value(device.groupId) : Json::Value::null;
        item["status"] = device.status;
        // Modbus 设备：在线超时 = 2 × 采集间隔；其他协议使用配置值
        if (device.protocolType == "Modbus" && device.protocolConfig.isMember("readInterval")) {
            int readInterval = device.protocolConfig.get("readInterval", 1).asInt();
            int timeout = readInterval * 2;
            item["online_timeout"] = timeout < 10 ? 10 : timeout;  // 最小 10 秒
        } else {
            item["online_timeout"] = device.onlineTimeout;
        }
        item["remote_control"] = device.remoteControl;
        item["remark"] = device.remark;
        item["created_at"] = device.createdAt;
        item["link_name"] = device.linkName;
        item["link_mode"] = device.linkMode;
        item["protocol_name"] = device.protocolName;
        item["protocol_type"] = device.protocolType;

        // 心跳包/注册包（通用）
        if (!device.heartbeatMode.empty() && device.heartbeatMode != "OFF") {
            Json::Value hb;
            hb["mode"] = device.heartbeatMode;
            hb["content"] = device.heartbeatContent;
            item["heartbeat"] = hb;
        }
        if (!device.registrationMode.empty() && device.registrationMode != "OFF") {
            Json::Value reg;
            reg["mode"] = device.registrationMode;
            reg["content"] = device.registrationContent;
            item["registration"] = reg;
        }

        // SL651 特有字段
        if (device.protocolType == Constants::PROTOCOL_SL651) {
            item["device_code"] = device.deviceCode;
            item["timezone"] = device.timezone;
        }

        // Modbus 特有字段
        if (device.protocolType == Constants::PROTOCOL_MODBUS) {
            if (!device.modbusMode.empty()) {
                item["modbus_mode"] = device.modbusMode;
            }
            if (device.slaveId > 0) {
                item["slave_id"] = static_cast<int>(device.slaveId);
            }
        }

        return item;
    }

    /**
     * @brief 解析 Modbus 寄存器为 elements 格式
     */
    static void parseModbusRegisters(
        const DeviceCache::CachedDevice& device,
        const std::map<std::string, ElementData>& realtimeValues,
        Json::Value& outElements
    ) {
        const auto& config = device.protocolConfig;
        if (!config.isMember("registers") || !config["registers"].isArray()) return;

        for (const auto& reg : config["registers"]) {
            Json::Value element;
            element["name"] = reg.get("name", "").asString();

            std::string unit = reg.get("unit", "").asString();
            if (!unit.empty()) element["unit"] = unit;

            // Modbus dictConfig 没有 mapType，补上 "VALUE" 以兼容前端
            if (reg.isMember("dictConfig") && reg["dictConfig"].isObject()
                && reg["dictConfig"].isMember("items")) {
                Json::Value dictConfig = reg["dictConfig"];
                dictConfig["mapType"] = "VALUE";
                element["dictConfig"] = dictConfig;
            }

            // 从实时数据中匹配值（key 格式: registerType_address）
            std::string regKey = reg.get("registerType", "").asString()
                + "_" + std::to_string(reg.get("address", 0).asInt());
            auto it = realtimeValues.find(regKey);
            element["value"] = (it != realtimeValues.end()) ? it->second.value : Json::nullValue;

            outElements.append(element);
        }
    }

    /**
     * @brief 解析协议配置中的功能定义
     * @param device 缓存的设备信息
     * @param realtimeValues 实时数据（可选，用于填充 elements 的 value）
     * @param funcDataMap 功能数据映射（可选，用于查找图片）
     * @param outElements 输出的要素数组
     * @param outDownFuncs 输出的下行功能数组
     * @param outImageFuncs 输出的图片功能数组
     */
    static void parseProtocolFuncs(
        const DeviceCache::CachedDevice& device,
        const std::map<std::string, ElementData>& realtimeValues,
        const std::map<std::string, Json::Value>& funcDataMap,
        Json::Value& outElements,
        Json::Value& outDownFuncs,
        Json::Value& outImageFuncs
    ) {
        outElements = Json::Value(Json::arrayValue);
        outDownFuncs = Json::Value(Json::arrayValue);
        outImageFuncs = Json::Value(Json::arrayValue);

        if (device.protocolConfig.isNull()) return;

        if (device.protocolType == Constants::PROTOCOL_SL651) {
            const auto& config = device.protocolConfig;
            if (!config.isMember("funcs") || !config["funcs"].isArray()) return;

            std::set<std::string> addedGuideHex;

            for (const auto& func : config["funcs"]) {
                std::string dir = func.get("dir", "").asString();
                std::string funcCode = func.get("funcCode", "").asString();

                if (!func.isMember("elements") || !func["elements"].isArray()) continue;

                if (dir == "UP") {
                    if (hasJpegElement(func)) {
                        Json::Value imageFunc = parseImageFuncBase(func);
                        auto imageData = findImageData(funcCode, funcDataMap);
                        if (imageData) {
                            Json::Value latestImage;
                            latestImage["data"] = *imageData;
                            imageFunc["latestImage"] = latestImage;
                        }
                        outImageFuncs.append(imageFunc);
                    } else {
                        parseUpElements(func, realtimeValues, addedGuideHex, outElements);
                    }
                } else if (dir == "DOWN") {
                    outDownFuncs.append(parseDownFunc(func));
                }
            }
        } else if (device.protocolType == Constants::PROTOCOL_MODBUS) {
            parseModbusRegisters(device, realtimeValues, outElements);
            parseModbusDownFuncs(device, outDownFuncs);
        }
    }

    /**
     * @brief 解析协议配置中的静态功能定义（不包含实时数据）
     */
    static void parseProtocolFuncsStatic(
        const DeviceCache::CachedDevice& device,
        Json::Value& outDownFuncs,
        Json::Value& outImageFuncs
    ) {
        outDownFuncs = Json::Value(Json::arrayValue);
        outImageFuncs = Json::Value(Json::arrayValue);

        if (device.protocolConfig.isNull()) return;

        if (device.protocolType == Constants::PROTOCOL_MODBUS) {
            parseModbusDownFuncs(device, outDownFuncs);
            return;
        }

        if (device.protocolType != Constants::PROTOCOL_SL651) {
            return;
        }

        const auto& config = device.protocolConfig;
        if (!config.isMember("funcs") || !config["funcs"].isArray()) {
            return;
        }

        for (const auto& func : config["funcs"]) {
            std::string dir = func.get("dir", "").asString();

            if (!func.isMember("elements") || !func["elements"].isArray()) continue;

            if (dir == "UP" && hasJpegElement(func)) {
                outImageFuncs.append(parseImageFuncBase(func));
            } else if (dir == "DOWN") {
                outDownFuncs.append(parseDownFunc(func));
            }
        }
    }

    /**
     * @brief 从 Modbus 寄存器配置中构建下行功能（可写寄存器）
     */
    static void parseModbusDownFuncs(
        const DeviceCache::CachedDevice& device,
        Json::Value& outDownFuncs
    ) {
        const auto& config = device.protocolConfig;
        if (!config.isMember("registers") || !config["registers"].isArray()) return;

        Json::Value funcElements(Json::arrayValue);

        for (const auto& reg : config["registers"]) {
            std::string regType = reg.get("registerType", "").asString();
            // 仅 COIL 和 HOLDING_REGISTER 可写
            if (regType != "COIL" && regType != "HOLDING_REGISTER") continue;

            Json::Value funcEl;
            funcEl["elementId"] = reg.get("id", "").asString();
            funcEl["name"] = reg.get("name", "").asString();
            funcEl["value"] = "";
            funcEl["registerType"] = regType;
            funcEl["dataType"] = reg.get("dataType", "UINT16").asString();

            std::string unit = reg.get("unit", "").asString();
            if (!unit.empty()) funcEl["unit"] = unit;

            // COIL 类型添加预设值选项，优先使用寄存器配置的 0/1 值显示标签
            if (regType == "COIL") {
                std::string label1 = "ON";
                std::string label0 = "OFF";
                if (reg.isMember("dictConfig") && reg["dictConfig"].isMember("items")
                    && reg["dictConfig"]["items"].isArray()) {
                    for (const auto& item : reg["dictConfig"]["items"]) {
                        std::string key = item.get("key", "").asString();
                        std::string lbl = item.get("label", "").asString();
                        if (!lbl.empty()) {
                            if (key == "1") label1 = lbl;
                            else if (key == "0") label0 = lbl;
                        }
                    }
                }
                Json::Value options(Json::arrayValue);
                Json::Value optOn;
                optOn["label"] = label1;
                optOn["value"] = "1";
                options.append(optOn);
                Json::Value optOff;
                optOff["label"] = label0;
                optOff["value"] = "0";
                options.append(optOff);
                funcEl["options"] = options;
            }

            funcElements.append(funcEl);
        }

        if (!funcElements.empty()) {
            Json::Value downFunc;
            downFunc["funcCode"] = "MODBUS_WRITE";
            downFunc["name"] = "写寄存器";
            downFunc["elements"] = funcElements;
            outDownFuncs.append(downFunc);
        }
    }

    /**
     * @brief 构建单个设备的实时数据 JSON（供 API 和 WebSocket 共用）
     *
     * 从 RealtimeDataCache 数据 + 设备协议配置 → {id, reportTime, elements, image}
     */
    static Json::Value buildRealtimeItem(
        const DeviceCache::CachedDevice& device,
        const RealtimeDataCache::DeviceRealtimeData& deviceData,
        const std::string& latestTime
    ) {
        Json::Value item;
        item["id"] = device.id;
        item["reportTime"] = latestTime.empty() ? Json::nullValue : Json::Value(latestTime);
        item["lastHeartbeatTime"] = Json::nullValue;

        // 从 funcCode 数据中提取实时值
        std::map<std::string, std::pair<Json::Value, std::string>> funcDataPairs;
        std::map<std::string, Json::Value> funcDataMap;
        for (const auto& [funcCode, funcData] : deviceData) {
            funcDataPairs[funcCode] = {funcData.data, funcData.reportTime};
            funcDataMap[funcCode] = funcData.data;
        }
        auto realtimeValues = parseRealtimeValues(funcDataPairs);

        // 根据协议配置转换为 elements + image
        Json::Value elements(Json::arrayValue);
        Json::Value image = Json::nullValue;

        if (!device.protocolConfig.isNull()) {
            if (device.protocolType == Constants::PROTOCOL_SL651) {
                const auto& config = device.protocolConfig;
                if (config.isMember("funcs") && config["funcs"].isArray()) {
                    std::set<std::string> addedGuideHex;
                    for (const auto& func : config["funcs"]) {
                        std::string dir = func.get("dir", "").asString();
                        std::string funcCode = func.get("funcCode", "").asString();
                        if (dir != "UP" || !func.isMember("elements") || !func["elements"].isArray()) continue;

                        if (hasJpegElement(func)) {
                            auto imageData = findImageData(funcCode, funcDataMap);
                            if (imageData) {
                                Json::Value latestImage;
                                latestImage["funcCode"] = funcCode;
                                latestImage["data"] = *imageData;
                                image = latestImage;
                            }
                        } else {
                            parseUpElements(func, realtimeValues, addedGuideHex, elements);
                        }
                    }
                }
            } else if (device.protocolType == Constants::PROTOCOL_MODBUS) {
                parseModbusRegisters(device, realtimeValues, elements);
            }
        }

        item["elements"] = elements;
        item["image"] = image;
        return item;
    }
};
