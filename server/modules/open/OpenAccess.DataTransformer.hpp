#pragma once

#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

class OpenAccessDataTransformer {
public:
    struct PointTemplate {
        std::string id;
        std::string name;
        std::string unit;
        std::vector<std::string> aliases;
    };

    static Json::Value buildDeviceRef(int id, const std::string& code, const std::string& name) {
        Json::Value device(Json::objectValue);
        device["id"] = id;
        if (!code.empty()) {
            device["code"] = code;
        }
        device["name"] = name;
        return device;
    }

    static Json::Value buildDeviceRef(const DeviceCache::CachedDevice& device) {
        return buildDeviceRef(device.id, device.deviceCode, device.name);
    }

    static Json::Value buildDeviceListItem(const DeviceCache::CachedDevice& device) {
        return buildDeviceRef(device);
    }

    static Json::Value buildDataItem(
        const DeviceCache::CachedDevice& device,
        const RealtimeDataCache::DeviceRealtimeData& deviceData
    ) {
        Json::Value item(Json::objectValue);
        item["device"] = buildDeviceRef(device);
        item["points"] = buildPointsFromRealtime(device, deviceData);
        return item;
    }

    static Json::Value buildDataItem(
        const DeviceCache::CachedDevice& device,
        const Json::Value& elementsData,
        const std::string& reportTime
    ) {
        Json::Value item(Json::objectValue);
        item["device"] = buildDeviceRef(device);
        item["points"] = buildPointsFromElements(device, elementsData, reportTime);
        return item;
    }

    static Json::Value buildAlertItem(
        int64_t id,
        int deviceId,
        const std::string& deviceCode,
        const std::string& deviceName,
        int ruleId,
        const std::string& severity,
        const std::string& status,
        const std::string& message,
        const std::string& time
    ) {
        Json::Value item(Json::objectValue);
        item["id"] = static_cast<Json::Int64>(id);
        item["device"] = buildDeviceRef(deviceId, deviceCode, deviceName);
        item["ruleId"] = ruleId;
        item["severity"] = severity;
        item["status"] = status;
        item["message"] = message;
        item["time"] = time;
        return item;
    }

private:
    static bool isImageElement(const Json::Value& element) {
        return element.get("encode", "").asString() == "JPEG";
    }

    static std::string stableIdFromKey(const std::string& key) {
        if (key.empty()) return "";

        const auto pos = key.find('_');
        if (pos != std::string::npos && pos + 1 < key.size()) {
            const auto prefix = key.substr(0, pos);
            if (prefix.size() <= 4) {
                return key.substr(pos + 1);
            }
        }
        return key;
    }

    static void addAlias(PointTemplate& point, const std::string& alias) {
        if (alias.empty()) return;
        if (std::find(point.aliases.begin(), point.aliases.end(), alias) == point.aliases.end()) {
            point.aliases.push_back(alias);
        }
    }

    static std::vector<PointTemplate> configuredPoints(const DeviceCache::CachedDevice& device) {
        std::vector<PointTemplate> points;
        const auto& config = device.protocolConfig;
        if (!config.isObject()) return points;

        if (device.protocolType == Constants::PROTOCOL_SL651) {
            if (!config.isMember("funcs") || !config["funcs"].isArray()) return points;

            std::set<std::string> seen;
            for (const auto& func : config["funcs"]) {
                if (func.get("dir", "").asString() != "UP") continue;
                const std::string funcCode = func.get("funcCode", "").asString();
                if (!func.isMember("elements") || !func["elements"].isArray()) continue;

                for (const auto& element : func["elements"]) {
                    if (isImageElement(element)) continue;
                    const std::string guideHex = element.get("guideHex", "").asString();
                    std::string id = element.get("id", "").asString();
                    if (id.empty()) id = guideHex;
                    if (id.empty()) id = element.get("name", "").asString();
                    if (id.empty() || !seen.insert(id).second) continue;

                    PointTemplate point;
                    point.id = id;
                    point.name = element.get("name", id).asString();
                    point.unit = element.get("unit", "").asString();
                    addAlias(point, id);
                    addAlias(point, guideHex);
                    if (!funcCode.empty() && !guideHex.empty()) {
                        addAlias(point, funcCode + "_" + guideHex);
                    }
                    points.push_back(std::move(point));
                }
            }
            return points;
        }

        if (device.protocolType == Constants::PROTOCOL_MODBUS) {
            if (!config.isMember("registers") || !config["registers"].isArray()) return points;

            std::set<std::string> seen;
            for (const auto& reg : config["registers"]) {
                const std::string regKey = reg.get("registerType", "").asString()
                    + "_" + std::to_string(reg.get("address", 0).asInt());
                std::string id = reg.get("id", "").asString();
                if (id.empty()) id = regKey;
                if (id.empty() || !seen.insert(id).second) continue;

                PointTemplate point;
                point.id = id;
                point.name = reg.get("name", id).asString();
                point.unit = reg.get("unit", "").asString();
                addAlias(point, id);
                addAlias(point, regKey);
                points.push_back(std::move(point));
            }
            return points;
        }

        if (device.protocolType == Constants::PROTOCOL_S7) {
            const Json::Value* areas = nullptr;
            if (config.isMember("areas") && config["areas"].isArray()) {
                areas = &config["areas"];
            } else if (config.isMember("poll") && config["poll"].isObject()
                && config["poll"].isMember("areas") && config["poll"]["areas"].isArray()) {
                areas = &config["poll"]["areas"];
            }
            if (!areas) return points;

            std::set<std::string> seen;
            for (const auto& area : *areas) {
                std::string id = area.get("id", "").asString();
                if (id.empty()) {
                    id = area.get("area", "").asString() + "_" + std::to_string(area.get("start", 0).asInt());
                }
                if (id.empty() || !seen.insert(id).second) continue;

                PointTemplate point;
                point.id = id;
                point.name = area.get("name", id).asString();
                point.unit = area.get("unit", "").asString();
                addAlias(point, id);
                points.push_back(std::move(point));
            }
        }

        return points;
    }

    static Json::Value emptyPoint(const PointTemplate& point, const Json::Value& timeValue) {
        Json::Value result(Json::objectValue);
        result["id"] = point.id;
        result["name"] = point.name;
        result["value"] = Json::nullValue;
        result["unit"] = point.unit;
        result["time"] = timeValue;
        return result;
    }

    static Json::Value actualPoint(
        const std::string& id,
        const std::string& fallbackName,
        const std::string& fallbackUnit,
        const Json::Value& element,
        const std::string& reportTime
    ) {
        Json::Value result(Json::objectValue);
        result["id"] = id;
        result["name"] = element.get("name", fallbackName).asString();
        result["value"] = element.isMember("value") ? element["value"] : Json::nullValue;
        result["unit"] = element.get("unit", fallbackUnit).asString();
        result["time"] = reportTime.empty()
            ? Json::Value(Json::nullValue)
            : Json::Value(reportTime);
        return result;
    }

    static Json::Value buildPointsFromElements(
        const DeviceCache::CachedDevice& device,
        const Json::Value& elementsData,
        const std::string& reportTime
    ) {
        const auto templates = configuredPoints(device);
        std::map<std::string, Json::Value> pointsById;
        std::map<std::string, std::string> aliasToId;
        std::map<std::string, PointTemplate> templateById;
        const Json::Value timeValue = reportTime.empty()
            ? Json::Value(Json::nullValue)
            : Json::Value(reportTime);

        for (const auto& point : templates) {
            pointsById[point.id] = emptyPoint(point, timeValue);
            templateById[point.id] = point;
            for (const auto& alias : point.aliases) {
                aliasToId[alias] = point.id;
            }
        }

        if (elementsData.isObject()) {
            for (const auto& key : elementsData.getMemberNames()) {
                const auto& element = elementsData[key];
                if (!element.isObject()) continue;
                std::string id = element.get("elementId", "").asString();
                if (id.empty() && aliasToId.contains(key)) {
                    id = aliasToId[key];
                }
                if (id.empty()) {
                    const std::string stableId = stableIdFromKey(key);
                    id = aliasToId.contains(stableId) ? aliasToId[stableId] : stableId;
                }
                if (id.empty()) continue;

                const auto templateIt = templateById.find(id);
                const std::string fallbackName = templateIt != templateById.end()
                    ? templateIt->second.name
                    : id;
                const std::string fallbackUnit = templateIt != templateById.end()
                    ? templateIt->second.unit
                    : "";
                pointsById[id] = actualPoint(id, fallbackName, fallbackUnit, element, reportTime);
            }
        }

        Json::Value points(Json::arrayValue);
        std::set<std::string> appended;
        for (const auto& point : templates) {
            points.append(pointsById[point.id]);
            appended.insert(point.id);
        }
        for (const auto& [id, point] : pointsById) {
            if (appended.contains(id)) continue;
            points.append(point);
        }
        return points;
    }

    static Json::Value buildPointsFromRealtime(
        const DeviceCache::CachedDevice& device,
        const RealtimeDataCache::DeviceRealtimeData& deviceData
    ) {
        const auto templates = configuredPoints(device);
        std::map<std::string, Json::Value> pointsById;
        std::map<std::string, std::string> pointTimes;
        std::map<std::string, std::string> aliasToId;
        std::map<std::string, PointTemplate> templateById;

        for (const auto& point : templates) {
            pointsById[point.id] = emptyPoint(point, Json::nullValue);
            templateById[point.id] = point;
            for (const auto& alias : point.aliases) {
                aliasToId[alias] = point.id;
            }
        }

        for (const auto& [funcCode, funcData] : deviceData) {
            (void)funcCode;
            if (!funcData.data.isMember("data") || !funcData.data["data"].isObject()) continue;

            const auto& elementsData = funcData.data["data"];
            for (const auto& key : elementsData.getMemberNames()) {
                const auto& element = elementsData[key];
                if (!element.isObject()) continue;

                std::string id = element.get("elementId", "").asString();
                if (id.empty() && aliasToId.contains(key)) {
                    id = aliasToId[key];
                }
                if (id.empty()) {
                    const std::string stableId = stableIdFromKey(key);
                    id = aliasToId.contains(stableId) ? aliasToId[stableId] : stableId;
                }
                // RealtimeDataCache may still contain values from a prior protocol
                // configuration. Open webhooks must expose only active point definitions.
                if (id.empty() || !templateById.contains(id)) continue;

                auto timeIt = pointTimes.find(id);
                if (timeIt != pointTimes.end() && !funcData.reportTime.empty()
                    && funcData.reportTime < timeIt->second) {
                    continue;
                }

                const auto& point = templateById.at(id);
                pointsById[id] = actualPoint(
                    id, point.name, point.unit, element, funcData.reportTime);
                pointTimes[id] = funcData.reportTime;
            }
        }

        Json::Value points(Json::arrayValue);
        for (const auto& point : templates) {
            points.append(pointsById[point.id]);
        }
        return points;
    }
};
