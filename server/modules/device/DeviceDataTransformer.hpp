#pragma once

#include <json/json.h>
#include <map>
#include <set>
#include <string>
#include <optional>
#include "common/cache/DeviceCache.hpp"

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
                std::string guideHex = extractGuideHex(fullKey);

                auto it = realtimeValues.find(guideHex);
                if (it == realtimeValues.end() || reportTime > it->second.reportTime) {
                    realtimeValues[guideHex] = {
                        elemData.get("name", "").asString(),
                        elemData.get("value", Json::nullValue),
                        elemData.get("unit", "").asString(),
                        reportTime
                    };
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

        // 兼容旧字段
        item["code"] = device.deviceCode;
        item["deviceName"] = device.name;
        item["typeName"] = device.protocolName.empty() ? "未知协议" : device.protocolName;
        item["linkId"] = device.linkId;

        // 管理字段
        item["id"] = device.id;
        item["name"] = device.name;
        item["device_code"] = device.deviceCode;
        item["link_id"] = device.linkId;
        item["protocol_config_id"] = device.protocolConfigId;
        item["status"] = device.status;
        item["online_timeout"] = device.onlineTimeout;
        item["remote_control"] = device.remoteControl;
        item["remark"] = device.remark;
        item["created_at"] = device.createdAt;
        item["link_name"] = device.linkName;
        item["link_mode"] = device.linkMode;
        item["protocol_name"] = device.protocolName;
        item["protocol_type"] = device.protocolType;

        return item;
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

        if (device.protocolConfig.isNull() || device.protocolType != "SL651") {
            return;
        }

        const auto& config = device.protocolConfig;
        if (!config.isMember("funcs") || !config["funcs"].isArray()) {
            return;
        }

        std::set<std::string> addedGuideHex;

        for (const auto& func : config["funcs"]) {
            std::string dir = func.get("dir", "").asString();
            std::string funcCode = func.get("funcCode", "").asString();

            if (!func.isMember("elements") || !func["elements"].isArray()) continue;

            if (dir == "UP") {
                if (hasJpegElement(func)) {
                    Json::Value imageFunc = parseImageFuncBase(func);
                    // 查找图片数据
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

        if (device.protocolConfig.isNull() || device.protocolType != "SL651") {
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
};
