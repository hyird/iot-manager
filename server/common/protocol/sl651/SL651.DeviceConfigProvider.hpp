#pragma once

#include "SL651.Types.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/database/DatabaseService.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

namespace sl651 {

class SL651DeviceConfigProvider {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    std::optional<DeviceConfig> buildFromCache(int linkId, const std::string& remoteCode) const {
        auto cachedOpt = DeviceCache::instance().findByLinkAndCodeSync(linkId, remoteCode);
        if (!cachedOpt) return std::nullopt;
        return buildDeviceConfig(*cachedOpt);
    }

    Task<std::optional<DeviceConfig>> getAsync(int linkId, const std::string& remoteCode) const {
        try {
            auto dbClient = AppDbConfig::useFast()
                ? drogon::app().getFastDbClient("default")
                : drogon::app().getDbClient("default");

            auto result = co_await dbClient->execSqlCoro(R"(
                SELECT d.id, d.name, d.protocol_params, d.protocol_config_id, d.link_id,
                       pc.config
                FROM device d
                JOIN protocol_config pc ON d.protocol_config_id = pc.id
                WHERE d.link_id = $1 AND d.protocol_params->>'device_code' = $2
                  AND d.deleted_at IS NULL AND pc.deleted_at IS NULL
            )", std::to_string(linkId), remoteCode);

            if (result.empty()) {
                co_return std::nullopt;
            }

            DeviceConfig config;
            config.deviceId = result[0]["id"].as<int>();
            config.deviceName = result[0]["name"].as<std::string>();
            config.deviceCode = remoteCode;
            config.protocolConfigId = result[0]["protocol_config_id"].as<int>();
            config.linkId = result[0]["link_id"].as<int>();

            auto protocolParams = parseJsonValue(
                result[0]["protocol_params"].isNull()
                    ? ""
                    : result[0]["protocol_params"].as<std::string>());
            if (protocolParams) {
                config.deviceCode = protocolParams->get("device_code", "").asString();
                config.timezone = protocolParams->get("timezone", "+08:00").asString();
            }

            auto protocolConfig = parseJsonValue(
                result[0]["config"].isNull()
                    ? ""
                    : result[0]["config"].as<std::string>());
            if (protocolConfig) {
                applyCompiledProtocolConfig(
                    config,
                    getOrCompileProtocolConfig(config.protocolConfigId, *protocolConfig));
            }

            co_return config;
        } catch (const std::exception& e) {
            LOG_ERROR << "[SL651][ConfigProvider] Device config load failed: " << e.what();
            co_return std::nullopt;
        }
    }

    std::vector<ElementDef> getElements(const DeviceConfig& config, const std::string& funcCode) const {
        auto it = config.elementsByFunc.find(funcCode);
        return it != config.elementsByFunc.end() ? it->second : std::vector<ElementDef>{};
    }

    void invalidateProtocolConfig(int protocolConfigId) const {
        if (protocolConfigId <= 0) return;

        std::unique_lock lock(compiledMutex_);
        compiledConfigs_.erase(protocolConfigId);
    }

    void clear() const {
        std::unique_lock lock(compiledMutex_);
        compiledConfigs_.clear();
    }

private:
    struct CompiledProtocolConfig {
        std::map<std::string, std::vector<ElementDef>> elementsByFunc;
        std::map<std::string, std::vector<ElementDef>> responseElementsByFunc;
        std::map<std::string, std::string> funcNames;
        std::map<std::string, Direction> funcDirections;
    };

    std::optional<DeviceConfig> buildDeviceConfig(const DeviceCache::CachedDevice& cached) const {
        DeviceConfig config;
        config.deviceId = cached.id;
        config.deviceName = cached.name;
        config.deviceCode = cached.deviceCode;
        config.protocolConfigId = cached.protocolConfigId;
        config.linkId = cached.linkId;
        config.timezone = cached.timezone;

        applyCompiledProtocolConfig(
            config,
            getOrCompileProtocolConfig(cached.protocolConfigId, cached.protocolConfig));

        return config;
    }

    static void applyCompiledProtocolConfig(
        DeviceConfig& deviceConfig,
        const std::shared_ptr<const CompiledProtocolConfig>& compiledConfig) {
        if (!compiledConfig) return;

        deviceConfig.elementsByFunc = compiledConfig->elementsByFunc;
        deviceConfig.responseElementsByFunc = compiledConfig->responseElementsByFunc;
        deviceConfig.funcNames = compiledConfig->funcNames;
        deviceConfig.funcDirections = compiledConfig->funcDirections;
    }

    std::shared_ptr<const CompiledProtocolConfig> getOrCompileProtocolConfig(
        int protocolConfigId,
        const Json::Value& protocolConfig) const {
        if (protocolConfigId > 0) {
            std::shared_lock lock(compiledMutex_);
            auto it = compiledConfigs_.find(protocolConfigId);
            if (it != compiledConfigs_.end()) {
                return it->second;
            }
        }

        auto compiled = compileProtocolConfig(protocolConfig);
        if (!compiled || protocolConfigId <= 0) {
            return compiled;
        }

        std::unique_lock lock(compiledMutex_);
        auto [it, inserted] = compiledConfigs_.emplace(protocolConfigId, compiled);
        if (!inserted) {
            return it->second;
        }
        return compiled;
    }

    static std::shared_ptr<const CompiledProtocolConfig> compileProtocolConfig(
        const Json::Value& protocolConfig) {
        if (protocolConfig.isNull() || !protocolConfig.isObject()) {
            return nullptr;
        }

        auto compiled = std::make_shared<CompiledProtocolConfig>();
        try {
            if (!protocolConfig.isMember("funcs") || !protocolConfig["funcs"].isArray()) {
                return compiled;
            }

            for (const auto& func : protocolConfig["funcs"]) {
                const std::string funcCode = func.get("funcCode", "").asString();
                if (funcCode.empty()) continue;

                std::string funcName = func.get("name", "").asString();
                if (!funcName.empty()) {
                    compiled->funcNames[funcCode] = funcName;
                }

                std::string dir = func.get("dir", "UP").asString();
                compiled->funcDirections[funcCode] = parseDirection(dir);
                compiled->elementsByFunc[funcCode] = parseElementArray(func, "elements", funcCode);
                compiled->responseElementsByFunc[funcCode] =
                    parseElementArray(func, "responseElements", funcCode);
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "[SL651][ConfigProvider] Protocol config compile failed: " << e.what();
            return nullptr;
        }

        return compiled;
    }

    static std::optional<Json::Value> parseJsonValue(const std::string& json) {
        if (json.empty()) return std::nullopt;

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream iss(json);
        if (!Json::parseFromStream(builder, iss, &root, &errs)) {
            LOG_ERROR << "[SL651][ConfigProvider] Config JSON parse failed: " << errs;
            return std::nullopt;
        }
        return root;
    }

    static std::vector<ElementDef> parseElementArray(
        const Json::Value& func,
        const char* fieldName,
        const std::string& funcCode) {
        std::vector<ElementDef> elements;
        if (!func.isMember(fieldName) || !func[fieldName].isArray()) {
            return elements;
        }

        for (const auto& elem : func[fieldName]) {
            ElementDef def;
            def.id = elem.get("id", "").asString();
            def.name = elem.get("name", "").asString();
            def.funcCode = funcCode;
            def.guideHex = elem.get("guideHex", "").asString();
            def.encode = parseEncode(elem.get("encode", "BCD").asString());
            def.length = elem.get("length", 0).asInt();
            def.digits = elem.get("digits", 0).asInt();
            def.unit = elem.get("unit", "").asString();
            def.remark = elem.get("remark", "").asString();

            if (elem.isMember("dictConfig") && elem["dictConfig"].isObject()) {
                def.dictConfig = parseDictConfig(elem["dictConfig"]);
            }

            elements.push_back(std::move(def));
        }

        return elements;
    }

    static DictConfig parseDictConfig(const Json::Value& dictConfigJson) {
        DictConfig dictConfig;
        dictConfig.mapType = parseDictMapType(dictConfigJson.get("mapType", "VALUE").asString());

        if (!dictConfigJson.isMember("items") || !dictConfigJson["items"].isArray()) {
            return dictConfig;
        }

        for (const auto& item : dictConfigJson["items"]) {
            DictMapItem mapItem;
            mapItem.key = item.get("key", "").asString();
            mapItem.label = item.get("label", "").asString();
            mapItem.value = item.get("value", "1").asString();

            if (item.isMember("dependsOn") && item["dependsOn"].isObject()) {
                DictDependsOn dependsOn;
                const auto& dependsOnJson = item["dependsOn"];
                dependsOn.op = parseDictDependencyOperator(
                    dependsOnJson.get("operator", "AND").asString());

                if (dependsOnJson.isMember("conditions") && dependsOnJson["conditions"].isArray()) {
                    for (const auto& cond : dependsOnJson["conditions"]) {
                        DictDependency dependency;
                        dependency.bitIndex = cond.get("bitIndex", "").asString();
                        dependency.bitValue = cond.get("bitValue", "1").asString();
                        dependsOn.conditions.push_back(std::move(dependency));
                    }
                }

                mapItem.dependsOn = std::move(dependsOn);
            }

            dictConfig.items.push_back(std::move(mapItem));
        }

        return dictConfig;
    }

    mutable std::map<int, std::shared_ptr<const CompiledProtocolConfig>> compiledConfigs_;
    mutable std::shared_mutex compiledMutex_;
};

}  // namespace sl651
