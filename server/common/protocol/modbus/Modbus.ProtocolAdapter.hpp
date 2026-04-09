#pragma once

#include "DtuRegistry.hpp"
#include "DtuSessionManager.hpp"
#include "Modbus.Utils.hpp"
#include "ModbusPollScheduler.hpp"
#include "ModbusSessionEngine.hpp"
#include "RegistrationNormalizer.hpp"
#include "common/edgenode/AgentBridgeManager.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/utils/Constants.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace modbus {

/**
 * @brief Modbus 协议适配器
 *
 * 运行模型是“DTU 聚合 + 多 slave 轮询”：
 * - 同一 link + registration 聚合成一个逻辑 DTU
 * - 通过 session 绑定恢复当前运行态
 * - 调度器负责具体轮询，reload 后需要重放已绑定 session
 */
class ModbusProtocolAdapter final : public ProtocolAdapter {
public:
    using ModbusStats = ModbusSessionEngine::ModbusStats;

    explicit ModbusProtocolAdapter(ProtocolRuntimeContext runtimeContext)
        : ProtocolAdapter(std::move(runtimeContext)) {
        dtuRegistry_ = std::make_unique<DtuRegistry>();
        sessionManager_ = std::make_unique<DtuSessionManager>();
        registrationNormalizer_ = std::make_unique<RegistrationNormalizer>(*dtuRegistry_, *sessionManager_);
        sessionEngine_ = std::make_unique<ModbusSessionEngine>(*dtuRegistry_, *sessionManager_);
        pollScheduler_ = std::make_unique<ModbusPollScheduler>();

        sessionEngine_->setCommandCompletionCallback(
            [this](const std::string& commandKey, const std::string& responseCode,
                   bool success, int64_t responseRecordId, int deviceId) {
                if (runtimeContext_.notifyCommandCompletion) {
                    runtimeContext_.notifyCommandCompletion(commandKey, responseCode, success, responseRecordId);
                }
                if (success && pollScheduler_) {
                    pollScheduler_->triggerNow(deviceId);
                }
            }
        );
        sessionEngine_->setReadCompletionCallback(
            [this](int deviceId, size_t readGroupIndex, bool success) {
                if (pollScheduler_) {
                    pollScheduler_->onReadCompleted(deviceId, readGroupIndex, success);
                }
            }
        );
        pollScheduler_->setEnqueueReadCallback(
            [this](int deviceId, size_t readGroupIndex) {
                return sessionEngine_ && sessionEngine_->enqueuePoll(deviceId, readGroupIndex);
            }
        );
        sessionManager_->setOldSessionDisplacedCallback(
            [this](int linkId, const std::string& clientAddr) {
                if (pollScheduler_) {
                    // 通知 PollScheduler 旧绑定关系已解除（通过 dtuKey 查找）
                    auto sessionOpt = sessionManager_->getSession(linkId, clientAddr);
                    if (sessionOpt && !sessionOpt->dtuKey.empty()) {
                        pollScheduler_->onSessionUnbound(sessionOpt->dtuKey);
                    }
                }
                // 关闭旧的 TCP 连接
                LinkTransportFacade::instance().disconnectServerClient(linkId, clientAddr);
            }
        );
    }

    std::string_view protocol() const override {
        return Constants::PROTOCOL_MODBUS;
    }

    Task<> initializeAsync() override {
        cleanupLegacyConnectionCacheMappings();
        if (dtuRegistry_) {
            co_await dtuRegistry_->reload();
        }
        if (pollScheduler_ && dtuRegistry_) {
            pollScheduler_->reload(*dtuRegistry_);
        }
        co_return;
    }

    Task<> reloadAsync() override {
        cleanupLegacyConnectionCacheMappings();
        if (dtuRegistry_) {
            co_await dtuRegistry_->reload();
        }
        // 配置重载后旧的读组索引已失效，需要先清理 session 的 in-flight 和 poll 队列
        if (sessionManager_) {
            sessionManager_->clearInflightAndPollQueues();
        }
        if (sessionEngine_) {
            // 清除残留的轮询周期聚合数据
            sessionEngine_->clearAllPollCycles();
        }
        if (pollScheduler_ && dtuRegistry_) {
            pollScheduler_->reload(*dtuRegistry_);
        }

        refreshBoundDtuRuntime();
        co_return;
    }

    void onConnectionChanged(int linkId, const std::string& clientAddr, bool connected) override {
        std::optional<DtuSession> previousSession;
        if (!connected && sessionManager_) {
            previousSession = sessionManager_->getSession(linkId, clientAddr);
        }

        if (sessionManager_) {
            if (connected) {
                if (sessionEngine_) {
                    sessionEngine_->onConnected(linkId, clientAddr);
                } else {
                    sessionManager_->onConnected(linkId, clientAddr);
                }
            } else {
                if (sessionEngine_) {
                    sessionEngine_->onDisconnected(linkId, clientAddr);
                } else {
                    sessionManager_->onDisconnected(linkId, clientAddr);
                }
            }
        }

        if (!connected && pollScheduler_ && previousSession
            && previousSession->bindState == SessionBindState::Bound
            && !previousSession->dtuKey.empty()) {
            pollScheduler_->onSessionUnbound(previousSession->dtuKey);
        }
        if (connected && sessionEngine_) {
            sessionEngine_->triggerDiscovery(linkId, clientAddr);
        }
    }

    void onDataReceived(int linkId, const std::string& clientAddr, std::vector<uint8_t> bytes) override {
        if (handleHeartbeat(linkId, clientAddr, bytes)) {
            return;
        }

        if (registrationNormalizer_) {
            auto normalized = registrationNormalizer_->normalize(linkId, clientAddr, bytes);
            if (normalized.kind == RegistrationMatchKind::Conflict) {
                LOG_WARN << "[Modbus][Adapter] Registration conflict: linkId="
                         << linkId << ", client=" << clientAddr;
                return;
            }

            logRegistrationMatch(linkId, clientAddr, normalized);

            if (normalized.sessionBound && !normalized.dtuKey.empty()) {
                if (normalized.kind == RegistrationMatchKind::StandaloneFrame && sessionEngine_) {
                    sessionEngine_->cancelDiscovery(linkId, clientAddr);
                }
                if (pollScheduler_ && dtuRegistry_) {
                    auto dtuOpt = dtuRegistry_->findByDtuKey(normalized.dtuKey);
                    if (dtuOpt) {
                        activateBoundDtu(*dtuOpt);
                        if (normalized.kind == RegistrationMatchKind::StandaloneFrame) {
                            triggerDtuDevicesNow(*dtuOpt);
                        } else if (normalized.kind == RegistrationMatchKind::PrefixedPayload) {
                            const int discoveryDeviceId = dtuOpt->discoveryPlan.enabled
                                ? dtuOpt->discoveryPlan.deviceId
                                : 0;
                            triggerDtuDevicesNow(*dtuOpt, discoveryDeviceId);
                        }
                    }
                }
            }

            bool bound = false;
            if (sessionManager_) {
                auto sessionOpt = sessionManager_->getSession(linkId, clientAddr);
                bound = sessionOpt && sessionOpt->bindState == SessionBindState::Bound;
            }

            if (!bound) {
                if (!normalized.payload.empty()) {
                    LOG_DEBUG << "[Modbus][Adapter] Drop payload from unbound DTU: linkId="
                              << linkId << ", client=" << clientAddr
                              << ", bytes=" << normalized.payload.size();
                }
                return;
            }

            bytes = std::move(normalized.payload);
            if (bytes.empty()) return;
        }

        auto engineResult = sessionEngine_->onPayload(linkId, clientAddr, bytes);
        if (!engineResult.parsedResults.empty() && runtimeContext_.submitParsedResults) {
            runtimeContext_.submitParsedResults(std::move(engineResult.parsedResults));
        }
    }

    void onMaintenanceTick() override {
        driveDiscovery();
    }

    ProtocolAdapterMetrics getMetrics() const override {
        ProtocolAdapterMetrics metrics;
        metrics.available = sessionEngine_ != nullptr;
        if (!metrics.available) {
            return metrics;
        }

        auto stats = sessionEngine_->getModbusStats();
        auto definitions = dtuRegistry_ ? dtuRegistry_->getAllDefinitions() : std::vector<DtuDefinition>{};
        auto sessions = sessionManager_ ? sessionManager_->listSessions() : std::vector<DtuSession>{};

        Json::Int64 totalDevices = 0;
        Json::Int64 discoveryCapableDtus = 0;
        for (const auto& dtu : definitions) {
            totalDevices += static_cast<Json::Int64>(dtu.devicesBySlave.size());
            if (dtu.discoveryPlan.enabled) {
                ++discoveryCapableDtus;
            }
        }

        Json::Int64 boundSessions = 0;
        Json::Int64 probingSessions = 0;
        Json::Int64 unknownSessions = 0;
        Json::Int64 discoveryRequestedSessions = 0;
        Json::Int64 inflightSessions = 0;
        Json::Int64 queuedJobs = 0;
        Json::Int64 onlineRoutes = 0;
        for (const auto& session : sessions) {
            switch (session.bindState) {
                case SessionBindState::Bound:
                    ++boundSessions;
                    break;
                case SessionBindState::Probing:
                    ++probingSessions;
                    break;
                case SessionBindState::Unknown:
                    ++unknownSessions;
                    break;
            }
            if (session.discoveryRequested) {
                ++discoveryRequestedSessions;
            }
            if (session.inflight.has_value()) {
                ++inflightSessions;
            }
            queuedJobs += static_cast<Json::Int64>(session.highQueue.size() + session.normalQueue.size());
            onlineRoutes += static_cast<Json::Int64>(session.deviceIdsBySlave.size());
        }

        metrics.stats["totalResponses"] = static_cast<Json::Int64>(stats.totalResponses);
        metrics.stats["avgLatencyMs"] = stats.avgLatencyMs;
        metrics.stats["timeouts"] = static_cast<Json::Int64>(stats.timeouts);
        metrics.stats["crcErrors"] = static_cast<Json::Int64>(stats.crcErrors);
        metrics.stats["exceptions"] = static_cast<Json::Int64>(stats.exceptions);
        metrics.stats["registryDtuCount"] = static_cast<Json::Int64>(definitions.size());
        metrics.stats["registryDeviceCount"] = totalDevices;
        metrics.stats["discoveryCapableDtuCount"] = discoveryCapableDtus;
        metrics.stats["sessionCount"] = static_cast<Json::Int64>(sessions.size());
        metrics.stats["boundSessionCount"] = boundSessions;
        metrics.stats["probingSessionCount"] = probingSessions;
        metrics.stats["unknownSessionCount"] = unknownSessions;
        metrics.stats["discoveryRequestedSessionCount"] = discoveryRequestedSessions;
        metrics.stats["inflightSessionCount"] = inflightSessions;
        metrics.stats["queuedJobCount"] = queuedJobs;
        metrics.stats["onlineRouteCount"] = onlineRoutes;
        metrics.stats["legacyMappingsCleanedLastReload"] =
            static_cast<Json::Int64>(legacyMappingsCleanedLast_.load(std::memory_order_relaxed));
        metrics.stats["legacyMappingsCleanedTotal"] =
            static_cast<Json::Int64>(legacyMappingsCleanedTotal_.load(std::memory_order_relaxed));
        return metrics;
    }

    bool isDeviceConnected(int deviceId) const override {
        // Server 端直连模式
        if (sessionManager_->getOnlineRouteByDevice(deviceId).has_value()) return true;
        // Agent 模式（Agent 上报 device:parsed 时注册到 DeviceConnectionCache）
        return DeviceConnectionCache::instance().getConnection(
            "modbus_" + std::to_string(deviceId)).has_value();
    }

    ProtocolLifecycleImpact onDeviceLifecycleEvent(const DeviceLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_MODBUS) {
            return ProtocolLifecycleImpact::None;
        }
        return ProtocolLifecycleImpact::Reload;
    }

    ProtocolLifecycleImpact onProtocolConfigLifecycleEvent(const ProtocolConfigLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_MODBUS) {
            return ProtocolLifecycleImpact::None;
        }
        return ProtocolLifecycleImpact::Reload;
    }

    Task<CommandResult> sendCommand(const CommandRequest& req) override {
        auto guard = makeCommandGuard();
        int deviceId = req.deviceId;
        std::string generalError;
        int64_t downCommandId = 0;

        try {
            if (deviceId == 0) {
                auto devices = DeviceCache::instance().getDevicesByLinkIdSync(req.linkId);
                for (const auto& dev : devices) {
                    if (!req.deviceCode.empty() && dev.deviceCode == req.deviceCode) {
                        deviceId = dev.id;
                        break;
                    }
                }
            }
            if (deviceId == 0) {
                LOG_ERROR << "[Modbus][Adapter] Device not found: id=" << req.deviceId
                          << " code=" << req.deviceCode;
                co_return CommandResult::error("设备不存在");
            }

            std::string pendingKey = req.deviceCode.empty()
                ? ("modbus:" + std::to_string(deviceId))
                : req.deviceCode;

            // ========== Agent 模式：通过 WebSocket 转发给 Agent ==========
            if (req.linkId == 0) {
                auto deviceOpt = DeviceCache::instance().findByIdSync(deviceId);
                int agentId = deviceOpt ? deviceOpt->agentId : 0;
                if (agentId <= 0) {
                    co_return CommandResult::error("Agent 模式设备缺少 agent_id");
                }
                if (!AgentBridgeManager::instance().isAgentOnline(agentId)) {
                    co_return CommandResult::offline("Agent 离线");
                }

                auto elementsData = buildElementsData(req.elements, nullptr);

                if (!runtimeContext_.commandCoordinator.tryReserve(
                        pendingKey, req.funcCode, req.userId, req.timeoutMs)) {
                    co_return CommandResult::busy();
                }
                guard.key = pendingKey;

                downCommandId = co_await savePendingCommand(
                    deviceId, 0, Constants::PROTOCOL_MODBUS,
                    req.funcCode, "写寄存器", "", req.userId, elementsData);
                runtimeContext_.commandCoordinator.attachDownCommandId(pendingKey, downCommandId);

                if (!AgentBridgeManager::instance().sendDeviceCommand(
                        agentId, pendingKey, deviceId, req.elements)) {
                    co_await runtimeContext_.commandStore.updateCommandStatus(
                        downCommandId, "SEND_FAILED", "Agent 命令发送失败");
                    co_return CommandResult::sendFailed("Agent 命令发送失败");
                }

                LOG_INFO << "[Modbus][Adapter] TX command to Agent " << agentId
                         << " for device " << deviceId;

                co_return co_await awaitCommandResponse(pendingKey, req.timeoutMs, downCommandId);
            }

            // ========== 本地链路模式：直接通过 DTU 会话发送 ==========
            auto deviceDef = dtuRegistry_->findDevice(deviceId);
            auto elementsData = buildElementsData(req.elements,
                deviceDef ? &deviceDef->registers : nullptr);

            auto preparedWrite = sessionEngine_->prepareWrite(deviceId, pendingKey, req.elements);
            if (!preparedWrite) {
                co_await saveFailedCommand(
                    deviceId, req.linkId, Constants::PROTOCOL_MODBUS,
                    req.funcCode, "写寄存器", "", req.userId,
                    "设备离线", elementsData);
                co_return CommandResult::offline("设备离线，未绑定 DTU 会话");
            }

            if (!runtimeContext_.commandCoordinator.tryReserve(
                    pendingKey, req.funcCode, req.userId, req.timeoutMs)) {
                LOG_WARN << "[Modbus][Adapter] Device " << pendingKey
                         << " already has an in-flight command";
                co_await saveFailedCommand(
                    deviceId, req.linkId, Constants::PROTOCOL_MODBUS,
                    req.funcCode, "写寄存器", preparedWrite->frameHex, req.userId,
                    "设备有未完成的指令", elementsData);
                co_return CommandResult::busy();
            }
            guard.key = pendingKey;

            downCommandId = co_await savePendingCommand(
                deviceId, req.linkId, Constants::PROTOCOL_MODBUS,
                req.funcCode, "写寄存器", preparedWrite->frameHex, req.userId, elementsData);
            runtimeContext_.commandCoordinator.attachDownCommandId(pendingKey, downCommandId);

            if (!sessionEngine_->submitPreparedWrite(std::move(*preparedWrite))) {
                co_await runtimeContext_.commandStore.updateCommandStatus(
                    downCommandId, "SEND_FAILED", "DTU 会话不可用或发送失败");
                co_return CommandResult::sendFailed("DTU 会话不可用或发送失败");
            }

            co_return co_await awaitCommandResponse(pendingKey, req.timeoutMs, downCommandId);

        } catch (const std::exception& e) {
            generalError = e.what();
        }

        if (downCommandId > 0 && !generalError.empty()) {
            co_await runtimeContext_.commandStore.updateCommandStatus(
                downCommandId, "SEND_FAILED", generalError);
        }
        co_return CommandResult::error(generalError);
    }

private:
    void cleanupLegacyConnectionCacheMappings() {
        const int removed = DeviceConnectionCache::instance().removeByMinSlaveId(1);
        legacyMappingsCleanedLast_.store(removed, std::memory_order_relaxed);
        legacyMappingsCleanedTotal_.fetch_add(removed, std::memory_order_relaxed);
        if (removed > 0) {
            LOG_INFO << "[Modbus][Adapter] Cleaned " << removed
                     << " legacy DeviceConnectionCache mapping(s)";
        }
    }

    void refreshBoundDtuRuntime() {
        if (!pollScheduler_ || !dtuRegistry_ || !sessionManager_) {
            return;
        }

        // reload 只负责刷新配置视图，这里把当前已绑定的 session 重新映射到运行态。
        replayBoundSessions(
            sessionManager_.get(),
            [](const DtuSession& session) {
                return session.bindState == SessionBindState::Bound && !session.dtuKey.empty();
            },
            [this](const DtuSession& session) {
                return dtuRegistry_->findByDtuKey(session.dtuKey);
            },
            [this](const DtuDefinition& dtu, const DtuSession&) {
                activateBoundDtu(dtu);
            }
        );
    }

    void activateBoundDtu(const DtuDefinition& dtu) {
        if (pollScheduler_) {
            pollScheduler_->onSessionBound(dtu);
        }
    }

    void triggerDtuDevicesNow(const DtuDefinition& dtu, int skipDeviceId = 0) {
        if (!pollScheduler_) {
            return;
        }

        for (const auto& [slaveId, device] : dtu.devicesBySlave) {
            (void)slaveId;
            if (skipDeviceId > 0 && device.deviceId == skipDeviceId) {
                continue;
            }
            pollScheduler_->triggerNow(device.deviceId);
        }
    }

    static Json::Value buildElementsData(const Json::Value& elements,
                                         const std::vector<modbus::RegisterDef>* registers) {
        // 构建 id → RegisterDef 索引
        std::map<std::string, const modbus::RegisterDef*> regIndex;
        if (registers) {
            for (const auto& reg : *registers) {
                regIndex[reg.id] = &reg;
            }
        }

        Json::Value dataObj(Json::objectValue);
        if (!elements.isArray()) return dataObj;

        for (const auto& elem : elements) {
            std::string elementId = elem.get("elementId", "").asString();
            auto it = regIndex.find(elementId);

            Json::Value entry(Json::objectValue);
            if (it != regIndex.end()) {
                const auto& reg = *it->second;
                // 使用寄存器类型+地址作为稳定 key（如 COIL_0）
                std::string key = modbus::registerTypeToString(reg.registerType)
                    + "_" + std::to_string(reg.address);
                entry["name"] = reg.name;
                entry["value"] = elem.get("value", "");
                entry["elementId"] = elementId;
                if (!reg.unit.empty()) entry["unit"] = reg.unit;
                dataObj[key] = std::move(entry);
            } else {
                entry["name"] = elementId;
                entry["value"] = elem.get("value", "");
                dataObj[elementId] = std::move(entry);
            }
        }
        return dataObj;
    }

    bool handleHeartbeat(int linkId, const std::string& clientAddr, const std::vector<uint8_t>& bytes) {
        if (!dtuRegistry_) return false;

        auto dtus = dtuRegistry_->getDefinitionsByLink(linkId);
        if (dtus.empty()) return false;

        for (const auto& dtu : dtus) {
            if (dtu.heartbeatBytes.empty() || bytes != dtu.heartbeatBytes) continue;

            if (sessionManager_) {
                sessionManager_->touch(linkId, clientAddr);
                auto sessionOpt = sessionManager_->getSession(linkId, clientAddr);
                if (sessionOpt && sessionOpt->bindState == SessionBindState::Bound) {
                    LOG_DEBUG << "[Modbus][Adapter] Heartbeat from bound DTU " << clientAddr;
                } else {
                    LOG_DEBUG << "[Modbus][Adapter] Heartbeat from unbound DTU "
                              << clientAddr << ", ignoring";
                }
            }
            return true;
        }
        return false;
    }

    void driveDiscovery() {
        if (!sessionEngine_ || !sessionManager_) return;

        auto timeoutResult = sessionEngine_->processTimeouts();
        if (!timeoutResult.parsedResults.empty() && runtimeContext_.submitParsedResults) {
            runtimeContext_.submitParsedResults(std::move(timeoutResult.parsedResults));
        }

        auto sessions = sessionManager_->listSessions();
        std::set<int> pendingLinks;
        for (const auto& session : sessions) {
            if (session.bindState == SessionBindState::Bound) continue;
            pendingLinks.insert(session.linkId);
        }
        for (int linkId : pendingLinks) {
            sessionEngine_->triggerDiscovery(linkId, "");
        }
    }
    static std::string registrationMatchKindToString(RegistrationMatchKind kind) {
        switch (kind) {
            case RegistrationMatchKind::StandaloneFrame:
                return "StandaloneFrame";
            case RegistrationMatchKind::PrefixedPayload:
                return "PrefixedPayload";
            case RegistrationMatchKind::Conflict:
                return "Conflict";
            case RegistrationMatchKind::None:
            default:
                return "None";
        }
    }

    static std::string bytesToAsciiString(const std::vector<uint8_t>& bytes) {
        static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(bytes.size());
        for (uint8_t byte : bytes) {
            switch (byte) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (byte >= 0x20 && byte <= 0x7E) {
                        out.push_back(static_cast<char>(byte));
                    } else {
                        out += "\\x";
                        out.push_back(HEX_DIGITS[byte >> 4]);
                        out.push_back(HEX_DIGITS[byte & 0x0F]);
                    }
                    break;
            }
        }
        return out;
    }

    static void logRegistrationMatch(
        int linkId,
        const std::string& clientAddr,
        const RegistrationMatchResult& normalized) {
        if (normalized.kind != RegistrationMatchKind::StandaloneFrame
            && normalized.kind != RegistrationMatchKind::PrefixedPayload) {
            return;
        }
        if (normalized.registrationBytes.empty()) {
            return;
        }

        LOG_DEBUG << "[Modbus][Adapter] Registration stripped: linkId=" << linkId
                  << ", client=" << clientAddr
                  << ", kind=" << registrationMatchKindToString(normalized.kind)
                  << ", hex=" << ModbusUtils::toHexString(normalized.registrationBytes)
                  << ", ascii=\"" << bytesToAsciiString(normalized.registrationBytes) << "\""
                  << ", payloadBytes=" << normalized.payload.size();
    }

    std::unique_ptr<DtuRegistry> dtuRegistry_;
    std::unique_ptr<DtuSessionManager> sessionManager_;
    std::unique_ptr<RegistrationNormalizer> registrationNormalizer_;
    std::unique_ptr<ModbusSessionEngine> sessionEngine_;
    std::unique_ptr<ModbusPollScheduler> pollScheduler_;
    std::atomic<int64_t> legacyMappingsCleanedLast_{0};
    std::atomic<int64_t> legacyMappingsCleanedTotal_{0};
};

}  // namespace modbus
