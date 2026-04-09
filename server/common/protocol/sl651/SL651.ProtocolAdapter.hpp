#pragma once

#include "SL651.hpp"
#include "SL651.DeviceConfigProvider.hpp"
#include "SL651.LinkIngress.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/network/LinkTransportFacade.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/Constants.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sl651 {

/**
 * @brief SL651 协议适配器
 *
 * 运行模型是“配置缓存 + 报文解析”：
 * - 设备运行态主要依赖 DeviceConfigProvider 的缓存视图
 * - reload 主要清理和重置缓存
 * - 不依赖 DTU 聚合和轮询激活模型
 */
class SL651ProtocolAdapter final : public ProtocolAdapter {
public:
    using Sl651Stats = SL651Parser::Sl651Stats;

    SL651ProtocolAdapter(
        ProtocolRuntimeContext runtimeContext,
        const SL651DeviceConfigProvider& configProvider)
        : ProtocolAdapter(std::move(runtimeContext))
        , configProvider_(configProvider) {
        parser_ = std::make_unique<SL651Parser>(
            [this](int linkId, const std::string& remoteCode) -> Task<std::optional<DeviceConfig>> {
                co_return co_await configProvider_.getAsync(linkId, remoteCode);
            },
            [this](const DeviceConfig& config, const std::string& funcCode) -> std::vector<ElementDef> {
                return configProvider_.getElements(config, funcCode);
            }
        );

        parser_->setCommandCompletionCallback(
            [this](const std::string& commandKey, const std::string& responseCode,
                   bool success, int64_t responseRecordId) {
                if (runtimeContext_.notifyCommandCompletion) {
                    runtimeContext_.notifyCommandCompletion(commandKey, responseCode, success, responseRecordId);
                }
            }
        );
    }

    std::string_view protocol() const override {
        return Constants::PROTOCOL_SL651;
    }

    Task<> initializeAsync() override {
        clearRuntimeCaches();
        co_return;
    }

    Task<> reloadAsync() override {
        clearRuntimeCaches();
        co_return;
    }

    void onConnectionChanged(int, const std::string&, bool) override {}

    void onDataReceived(int linkId, const std::string& clientAddr, std::vector<uint8_t> bytes) override {
        auto ingress = linkIngress_.preprocess(linkId, clientAddr, std::move(bytes));
        if (!ingress.shouldParse) {
            return;
        }

        parseAndSubmit(linkId, clientAddr, std::move(ingress.payload));
    }

    void onMaintenanceTick() override {
        if (parser_) {
            parser_->performMaintenance();
        }
    }

    ProtocolAdapterMetrics getMetrics() const override {
        ProtocolAdapterMetrics metrics;
        metrics.available = parser_ != nullptr;
        if (!metrics.available) {
            return metrics;
        }

        auto stats = parser_->getSl651Stats();
        metrics.stats["framesParsed"] = static_cast<Json::Int64>(stats.framesParsed);
        metrics.stats["crcErrors"] = static_cast<Json::Int64>(stats.crcErrors);
        metrics.stats["multiPacketCompleted"] = static_cast<Json::Int64>(stats.multiPacketCompleted);
        metrics.stats["multiPacketExpired"] = static_cast<Json::Int64>(stats.multiPacketExpired);
        metrics.stats["parseErrors"] = static_cast<Json::Int64>(stats.parseErrors);
        return metrics;
    }

    ProtocolLifecycleImpact onDeviceLifecycleEvent(const DeviceLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_SL651) {
            return ProtocolLifecycleImpact::None;
        }
        if (event.action == DeviceLifecycleAction::Deleted && !event.deviceCode.empty()) {
            DeviceConnectionCache::instance().removeConnection(event.deviceCode);
        }
        return ProtocolLifecycleImpact::None;
    }

    ProtocolLifecycleImpact onProtocolConfigLifecycleEvent(
        const ProtocolConfigLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_SL651) {
            return ProtocolLifecycleImpact::None;
        }

        configProvider_.invalidateProtocolConfig(event.configId);
        return ProtocolLifecycleImpact::None;
    }

    Task<CommandResult> sendCommand(const CommandRequest& req) override {
        auto guard = makeCommandGuard();

        try {
            auto configOpt = configProvider_.buildFromCache(req.linkId, req.deviceCode);
            if (!configOpt) {
                LOG_ERROR << "[SL651][Adapter] Device not found: code=" << req.deviceCode;
                co_return CommandResult::error("设备不存在");
            }

            const std::string deviceLabel = configOpt->deviceName.empty()
                ? (configOpt->deviceCode.empty() ? req.deviceCode : configOpt->deviceCode)
                : configOpt->deviceName;

            std::string funcName;
            auto funcNameIt = configOpt->funcNames.find(req.funcCode);
            if (funcNameIt != configOpt->funcNames.end()) {
                funcName = funcNameIt->second;
            }
            auto elementsData = buildElementsData(*configOpt, req.funcCode, req.elements);

            std::string data = parser_->buildCommand(req.deviceCode, req.funcCode, req.elements, *configOpt);
            if (data.empty()) {
                co_await saveFailedCommand(
                    configOpt->deviceId, req.linkId, Constants::PROTOCOL_SL651,
                    req.funcCode, funcName, "", req.userId, "构建报文失败", elementsData);
                co_return CommandResult::sendFailed("构建报文失败");
            }

            auto connOpt = DeviceConnectionCache::instance().getConnection(req.deviceCode);
            if (!connOpt) {
                co_await saveFailedCommand(
                    configOpt->deviceId, req.linkId, Constants::PROTOCOL_SL651,
                    req.funcCode, funcName, toHexString(data), req.userId,
                    "设备离线", elementsData);
                co_return CommandResult::offline("设备离线，未找到连接映射");
            }

            if (!runtimeContext_.commandCoordinator.tryReserve(
                    req.deviceCode,
                    req.funcCode,
                    req.userId,
                    req.timeoutMs,
                    0,
                    [](const std::string& sentFuncCode, const std::string& responseFuncCode) {
                        return sentFuncCode == responseFuncCode
                            || responseFuncCode == FuncCodes::ACK_OK
                            || responseFuncCode == FuncCodes::ACK_ERR;
                    })) {
                LOG_WARN << "[SL651][Adapter] Device " << deviceLabel
                         << "(id=" << configOpt->deviceId << ",code=" << req.deviceCode << ")"
                         << " already has an in-flight command";
                co_await saveFailedCommand(
                    configOpt->deviceId, req.linkId, Constants::PROTOCOL_SL651,
                    req.funcCode, funcName, toHexString(data), req.userId,
                    "设备有未完成的指令", elementsData);
                co_return CommandResult::busy();
            }
            guard.key = req.deviceCode;

            int64_t downCommandId = co_await savePendingCommand(
                configOpt->deviceId, req.linkId, Constants::PROTOCOL_SL651,
                req.funcCode, funcName, toHexString(data), req.userId, elementsData);
            runtimeContext_.commandCoordinator.attachDownCommandId(req.deviceCode, downCommandId);

            bool sent = LinkTransportFacade::instance().sendToClient(
                connOpt->linkId, connOpt->clientAddr, data);
            if (!sent) {
                LinkTransportFacade::instance().forceDisconnectServerClient(
                    connOpt->linkId, connOpt->clientAddr);
                co_await runtimeContext_.commandStore.updateCommandStatus(
                    downCommandId, "SEND_FAILED", "TCP发送失败");
                co_return CommandResult::sendFailed("TCP发送失败");
            }

            LOG_DEBUG << "[SL651][Adapter] TX command: " << deviceLabel
                      << "(id=" << configOpt->deviceId << ",code=" << req.deviceCode << ")"
                      << " -> " << connOpt->clientAddr
                      << ", funcCode=" << req.funcCode;

            co_return co_await awaitCommandResponse(req.deviceCode, req.timeoutMs, downCommandId);

        } catch (const ValidationException&) {
            throw;
        } catch (const std::exception& e) {
            co_return CommandResult::error(e.what());
        }
    }

    bool isDeviceConnected(int deviceId) const override {
        auto deviceOpt = DeviceCache::instance().findByIdSync(deviceId);
        if (!deviceOpt || deviceOpt->deviceCode.empty()) return false;
        return DeviceConnectionCache::instance().getConnection(deviceOpt->deviceCode).has_value();
    }

private:
    /**
     * @brief 清理协议运行时缓存
     */
    void clearRuntimeCaches() {
        configProvider_.clear();
    }

    /**
     * @brief 解析有效载荷并提交解析结果
     */
    void parseAndSubmit(int linkId, const std::string& clientAddr, std::vector<uint8_t> payload) {
        if (!parser_) {
            return;
        }

        auto results = parser_->parseDataSync(
            linkId,
            clientAddr,
            std::move(payload),
            [this](int lookupLinkId, const std::string& remoteCode) {
                return configProvider_.buildFromCache(lookupLinkId, remoteCode);
            });
        if (!results.empty() && runtimeContext_.submitParsedResults) {
            runtimeContext_.submitParsedResults(std::move(results));
        }
    }

    static Json::Value buildElementsData(
        const DeviceConfig& config,
        const std::string& funcCode,
        const Json::Value& elements) {
        Json::Value dataObj(Json::objectValue);
        if (!elements.isArray()) return dataObj;

        auto funcElemIt = config.elementsByFunc.find(funcCode);
        for (const auto& elem : elements) {
            std::string elementId = elem.get("elementId", "").asString();
            std::string value = elem.get("value", "").asString();

            const ElementDef* matchedDef = nullptr;
            if (funcElemIt != config.elementsByFunc.end()) {
                for (const auto& elemDef : funcElemIt->second) {
                    if (elemDef.id == elementId) {
                        matchedDef = &elemDef;
                        break;
                    }
                }
            }

            Json::Value entry(Json::objectValue);
            entry["name"] = matchedDef ? matchedDef->name : "";
            entry["value"] = value;
            entry["elementId"] = elementId;
            if (matchedDef && !matchedDef->unit.empty()) {
                entry["unit"] = matchedDef->unit;
            }

            std::string key = matchedDef
                ? (funcCode + "_" + matchedDef->guideHex)
                : elementId;
            dataObj[key] = std::move(entry);
        }
        return dataObj;
    }

    static std::string toHexString(const std::string& rawData) {
        std::ostringstream oss;
        for (unsigned char c : rawData) {
            oss << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(c);
        }
        return oss.str();
    }

    const SL651DeviceConfigProvider& configProvider_;
    SL651LinkIngress linkIngress_;
    std::unique_ptr<SL651Parser> parser_;
};

}  // namespace sl651
