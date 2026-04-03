#pragma once

#include "common/edgenode/AgentProtocol.hpp"
#include "common/protocol/FrameResult.hpp"
#include "common/protocol/sl651/SL651.Parser.hpp"
#include "common/protocol/sl651/SL651.Types.hpp"
#include "common/utils/Constants.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Agent 端协议引擎
 *
 * 从 config:sync 加载协议配置，复用 server 端的 SL651Parser 进行本地解析。
 * Modbus 暂由 Step 6 实现。
 */
class EdgeNodeProtocolEngine {
public:
    /**
     * @brief 从 config:sync 下发的端点列表加载协议配置
     */
    void loadConfig(const std::vector<agent::DeviceEndpoint>& endpoints) {
        std::lock_guard lock(mutex_);

        sl651Parsers_.clear();
        sl651ConfigsByEndpoint_.clear();
        modbusEndpoints_.clear();
        s7Endpoints_.clear();
        endpointProtocols_.clear();

        for (const auto& ep : endpoints) {
            endpointProtocols_[ep.id] = ep.protocol;

            if (ep.protocol == Constants::PROTOCOL_SL651) {
                loadSl651Config(ep);
            } else if (ep.protocol == Constants::PROTOCOL_MODBUS) {
                // 记录 Modbus 端点信息，供 Step 6 使用
                modbusEndpoints_[ep.id] = ep;
            } else if (ep.protocol == Constants::PROTOCOL_S7) {
                s7Endpoints_[ep.id] = ep;
            }
        }

        std::cout << "[EdgeNodeProtocolEngine] Loaded config: "
                  << sl651ConfigsByEndpoint_.size() << " SL651 endpoint(s), "
                  << modbusEndpoints_.size() << " Modbus endpoint(s), "
                  << s7Endpoints_.size() << " S7 endpoint(s)" << std::endl;
    }

    /**
     * @brief 解析 TCP 数据
     *
     * @param endpointId 端点 ID
     * @param clientAddr 客户端地址
     * @param data 原始 TCP 数据（二进制字符串）
     * @return 解析结果列表（可能为空）
     */
    std::vector<ParsedFrameResult> parseData(
        const std::string& endpointId,
        const std::string& clientAddr,
        const std::string& data) {

        std::lock_guard lock(mutex_);

        auto protoIt = endpointProtocols_.find(endpointId);
        if (protoIt == endpointProtocols_.end()) {
            return {};  // 无协议配置，fallback 到透传
        }

        if (protoIt->second == Constants::PROTOCOL_SL651) {
            return parseSl651Data(endpointId, clientAddr, data);
        }

        // Modbus 数据由轮询引擎在 Step 6 处理
        return {};
    }

    /**
     * @brief 是否有指定端点的协议配置
     */
    bool hasConfig(const std::string& endpointId) const {
        std::lock_guard lock(mutex_);
        return endpointProtocols_.count(endpointId) > 0;
    }

    /**
     * @brief 获取端点的协议类型
     */
    std::string getProtocol(const std::string& endpointId) const {
        std::lock_guard lock(mutex_);
        auto it = endpointProtocols_.find(endpointId);
        return it != endpointProtocols_.end() ? it->second : "";
    }

    /**
     * @brief 获取 Modbus 端点定义（供 Step 6 轮询引擎使用）
     */
    std::vector<agent::DeviceEndpoint> getModbusEndpoints() const {
        std::lock_guard lock(mutex_);
        std::vector<agent::DeviceEndpoint> result;
        result.reserve(modbusEndpoints_.size());
        for (const auto& [_, ep] : modbusEndpoints_) {
            result.push_back(ep);
        }
        return result;
    }

    std::vector<agent::DeviceEndpoint> getS7Endpoints() const {
        std::lock_guard lock(mutex_);
        std::vector<agent::DeviceEndpoint> result;
        result.reserve(s7Endpoints_.size());
        for (const auto& [_, ep] : s7Endpoints_) {
            result.push_back(ep);
        }
        return result;
    }

private:
    // ==================== SL651 配置加载 ====================

    /**
     * @brief 加载 SL651 端点的设备配置
     *
     * 对每个设备，从 protocolConfig JSON 编译出 DeviceConfig，
     * 以 deviceCode 为 key 存储。
     */
    void loadSl651Config(const agent::DeviceEndpoint& ep) {
        // 创建该端点的 SL651Parser 实例
        auto parser = std::make_unique<sl651::SL651Parser>(
            // DeviceConfigGetter（协程版本，Agent 不使用）
            [](int, const std::string&) -> sl651::SL651Parser::Task<std::optional<sl651::DeviceConfig>> {
                co_return std::nullopt;
            },
            // ElementsGetter
            [](const sl651::DeviceConfig& config, const std::string& funcCode) -> std::vector<sl651::ElementDef> {
                auto it = config.elementsByFunc.find(funcCode);
                return it != config.elementsByFunc.end() ? it->second : std::vector<sl651::ElementDef>{};
            }
        );
        sl651Parsers_[ep.id] = std::move(parser);

        // 对该端点的每个设备，编译协议配置
        auto& configMap = sl651ConfigsByEndpoint_[ep.id];
        for (const auto& dev : ep.devices) {
            if (dev.deviceCode.empty()) continue;

            sl651::DeviceConfig config;
            config.deviceId = dev.id;
            config.deviceName = dev.name;
            config.deviceCode = dev.deviceCode;
            config.protocolConfigId = dev.protocolConfigId;
            config.linkId = 0;  // Agent 模式无 linkId
            config.timezone = dev.timezone;

            // 从 protocolConfig JSON 编译要素定义
            compileSl651ProtocolConfig(config, dev.protocolConfig);

            configMap[dev.deviceCode] = std::move(config);
        }
    }

    /**
     * @brief 从 protocolConfig JSON 编译 SL651 DeviceConfig
     *
     * 复用与 SL651DeviceConfigProvider::compileProtocolConfig 相同的逻辑
     */
    static void compileSl651ProtocolConfig(sl651::DeviceConfig& config, const Json::Value& protocolConfig) {
        if (!protocolConfig.isObject() || !protocolConfig.isMember("funcs") || !protocolConfig["funcs"].isArray()) {
            return;
        }

        for (const auto& func : protocolConfig["funcs"]) {
            const std::string funcCode = func.get("funcCode", "").asString();
            if (funcCode.empty()) continue;

            std::string funcName = func.get("name", "").asString();
            if (!funcName.empty()) {
                config.funcNames[funcCode] = funcName;
            }

            std::string dir = func.get("dir", "UP").asString();
            config.funcDirections[funcCode] = sl651::parseDirection(dir);
            config.elementsByFunc[funcCode] = parseElementArray(func, "elements", funcCode);
            config.responseElementsByFunc[funcCode] = parseElementArray(func, "responseElements", funcCode);
        }
    }

    static std::vector<sl651::ElementDef> parseElementArray(
        const Json::Value& func,
        const char* fieldName,
        const std::string& funcCode) {
        std::vector<sl651::ElementDef> elements;
        if (!func.isMember(fieldName) || !func[fieldName].isArray()) {
            return elements;
        }

        for (const auto& elem : func[fieldName]) {
            sl651::ElementDef def;
            def.id = elem.get("id", "").asString();
            def.name = elem.get("name", "").asString();
            def.funcCode = funcCode;
            def.guideHex = elem.get("guideHex", "").asString();
            def.encode = sl651::parseEncode(elem.get("encode", "BCD").asString());
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

    static sl651::DictConfig parseDictConfig(const Json::Value& dictConfigJson) {
        sl651::DictConfig dictConfig;
        dictConfig.mapType = sl651::parseDictMapType(dictConfigJson.get("mapType", "VALUE").asString());

        if (!dictConfigJson.isMember("items") || !dictConfigJson["items"].isArray()) {
            return dictConfig;
        }

        for (const auto& item : dictConfigJson["items"]) {
            sl651::DictMapItem mapItem;
            mapItem.key = item.get("key", "").asString();
            mapItem.label = item.get("label", "").asString();
            mapItem.value = item.get("value", "1").asString();

            if (item.isMember("dependsOn") && item["dependsOn"].isObject()) {
                sl651::DictDependsOn dependsOn;
                const auto& dependsOnJson = item["dependsOn"];
                dependsOn.op = sl651::parseDictDependencyOperator(
                    dependsOnJson.get("operator", "AND").asString());

                if (dependsOnJson.isMember("conditions") && dependsOnJson["conditions"].isArray()) {
                    for (const auto& cond : dependsOnJson["conditions"]) {
                        sl651::DictDependency dependency;
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

    // ==================== SL651 数据解析 ====================

    std::vector<ParsedFrameResult> parseSl651Data(
        const std::string& endpointId,
        const std::string& clientAddr,
        const std::string& data) {

        auto parserIt = sl651Parsers_.find(endpointId);
        if (parserIt == sl651Parsers_.end() || !parserIt->second) {
            return {};
        }

        auto configIt = sl651ConfigsByEndpoint_.find(endpointId);
        // 构建 DeviceConfigGetterSync：从本地内存查找（同步调用，引用安全）
        const auto* configMapPtr = (configIt != sl651ConfigsByEndpoint_.end())
            ? &configIt->second : nullptr;
        sl651::SL651Parser::DeviceConfigGetterSync configGetter =
            [configMapPtr](int /*linkId*/, const std::string& remoteCode)
                -> std::optional<sl651::DeviceConfig> {
            if (!configMapPtr) return std::nullopt;
            auto it = configMapPtr->find(remoteCode);
            if (it != configMapPtr->end()) return it->second;
            return std::nullopt;
        };

        // 将 string 转为 vector<uint8_t>
        std::vector<uint8_t> bytes(data.begin(), data.end());

        // 使用端点 ID 的 hash 作为虚拟 linkId（SL651Parser 用 linkId 管理缓冲区）
        int virtualLinkId = static_cast<int>(std::hash<std::string>{}(endpointId) & 0x7FFFFFFF);

        return parserIt->second->parseDataSync(virtualLinkId, clientAddr, bytes, configGetter);
    }

    // ==================== 成员变量 ====================

    mutable std::mutex mutex_;

    // 端点协议映射
    std::unordered_map<std::string, std::string> endpointProtocols_;

    // SL651: endpointId → Parser 实例
    std::unordered_map<std::string, std::unique_ptr<sl651::SL651Parser>> sl651Parsers_;

    // SL651: endpointId → (deviceCode → DeviceConfig)
    std::unordered_map<std::string, std::map<std::string, sl651::DeviceConfig>> sl651ConfigsByEndpoint_;

    // Modbus: endpointId → DeviceEndpoint（Step 6 使用）
    std::unordered_map<std::string, agent::DeviceEndpoint> modbusEndpoints_;

    // S7: endpointId → DeviceEndpoint
    std::unordered_map<std::string, agent::DeviceEndpoint> s7Endpoints_;
};

using AgentProtocolEngine = EdgeNodeProtocolEngine;
