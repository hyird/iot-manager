#pragma once

#include "S7.DtuRegistry.hpp"
#include "S7.DtuSessionManager.hpp"
#include "S7.RegistrationNormalizer.hpp"
#include "S7.Client.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/network/LinkTransportFacade.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace s7 {

struct S7AreaDefinition {
    std::string id;
    std::string name;
    std::string area = "DB";
    std::string dataType = "INT16";
    int dbNumber = 0;
    int start = 0;
    int startBit = 0;
    int size = 1;
    bool writable = false;
    std::string remark;
};

struct S7ConnectionConfig {
    std::string host;  // 解析后的连接目标，优先取链路 IP，兼容旧配置
    std::string mode = "RACK_SLOT";
    int rack = 0;
    int slot = 1;
    std::uint16_t localTSAP = 0x0100;
    std::uint16_t remoteTSAP = 0x0100;
    int pingTimeoutMs = kDefaultTimeoutMs;
    int sendTimeoutMs = kDefaultTimeoutMs;
    int recvTimeoutMs = kDefaultTimeoutMs;
    int pollIntervalSec = 5;
    std::string connectionType = "PG";
};

struct S7ConnectionPreset {
    std::string mode = "RACK_SLOT";
    int rack = 0;
    int slot = 1;
    std::uint16_t localTSAP = 0x0100;
    std::uint16_t remoteTSAP = 0x0100;
};

struct S7DeviceRuntime {
    mutable std::mutex mutex;
    mutable std::mutex clientMutex;
    mutable std::mutex bridgeIoMutex;
    std::condition_variable bridgeIoCv;
    int deviceId = 0;
    int linkId = 0;
    std::string deviceCode;
    std::string dtuKey;
    std::string bridgeClientAddr;
    int bridgeLinkId = 0;
    S7ConnectionConfig connection;
    std::vector<S7AreaDefinition> areas;
    std::deque<std::vector<uint8_t>> pendingBridgeToDtu;
    std::deque<uint8_t> bridgeRxBuffer;
    std::unique_ptr<Client> client;
    bool connected = false;
    bool bridgeMode = false;
    bool bridgeBound = false;
    bool bridgeTransportOpen = false;
    bool bridgeDiscoveryInFlight = false;
    bool connectInProgress = false;
    std::uint64_t connectGeneration = 0;
    std::chrono::steady_clock::time_point bridgeDiscoveryStartedAt{};
    std::chrono::steady_clock::time_point lastConnectAttempt{};
    std::chrono::steady_clock::time_point lastPoll{};

    S7DeviceRuntime() = default;

    ~S7DeviceRuntime() {
        resetClient();
    }

    S7DeviceRuntime(const S7DeviceRuntime&) = delete;
    S7DeviceRuntime& operator=(const S7DeviceRuntime&) = delete;

    S7DeviceRuntime(S7DeviceRuntime&& other) noexcept
        : deviceId(other.deviceId)
        , linkId(other.linkId)
        , deviceCode(std::move(other.deviceCode))
        , dtuKey(std::move(other.dtuKey))
        , bridgeClientAddr(std::move(other.bridgeClientAddr))
        , bridgeLinkId(other.bridgeLinkId)
        , connection(std::move(other.connection))
        , areas(std::move(other.areas))
        , pendingBridgeToDtu(std::move(other.pendingBridgeToDtu))
        , bridgeRxBuffer(std::move(other.bridgeRxBuffer))
        , client(std::move(other.client))
        , connected(other.connected)
        , bridgeMode(other.bridgeMode)
        , bridgeBound(other.bridgeBound)
        , bridgeTransportOpen(other.bridgeTransportOpen)
        , bridgeDiscoveryInFlight(other.bridgeDiscoveryInFlight)
        , connectInProgress(other.connectInProgress)
        , connectGeneration(other.connectGeneration)
        , bridgeDiscoveryStartedAt(other.bridgeDiscoveryStartedAt)
        , lastConnectAttempt(other.lastConnectAttempt)
        , lastPoll(other.lastPoll) {
        other.connected = false;
    }

    S7DeviceRuntime& operator=(S7DeviceRuntime&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        resetClient();

        deviceId = other.deviceId;
        linkId = other.linkId;
        deviceCode = std::move(other.deviceCode);
        dtuKey = std::move(other.dtuKey);
        bridgeClientAddr = std::move(other.bridgeClientAddr);
        bridgeLinkId = other.bridgeLinkId;
        connection = std::move(other.connection);
        areas = std::move(other.areas);
        pendingBridgeToDtu = std::move(other.pendingBridgeToDtu);
        bridgeRxBuffer = std::move(other.bridgeRxBuffer);
        client = std::move(other.client);
        connected = other.connected;
        bridgeMode = other.bridgeMode;
        bridgeBound = other.bridgeBound;
        bridgeTransportOpen = other.bridgeTransportOpen;
        bridgeDiscoveryInFlight = other.bridgeDiscoveryInFlight;
        connectInProgress = other.connectInProgress;
        connectGeneration = other.connectGeneration;
        bridgeDiscoveryStartedAt = other.bridgeDiscoveryStartedAt;
        lastConnectAttempt = other.lastConnectAttempt;
        lastPoll = other.lastPoll;
        other.connected = false;
        return *this;
    }

    void resetClient() {
        if (client) {
            client->disconnect();
            client.reset();
        }
        {
            std::lock_guard bridgeIoLock(bridgeIoMutex);
            bridgeTransportOpen = false;
            bridgeRxBuffer.clear();
        }
        bridgeIoCv.notify_all();
        connected = false;
    }
};

class S7ProtocolAdapter final : public ProtocolAdapter {
public:
    explicit S7ProtocolAdapter(ProtocolRuntimeContext runtimeContext)
        : ProtocolAdapter(std::move(runtimeContext)) {
        bridgeRegistry_ = std::make_unique<DtuRegistry>();
        bridgeSessionManager_ = std::make_unique<DtuSessionManager>();
        bridgeNormalizer_ = std::make_unique<RegistrationNormalizer>(*bridgeRegistry_, *bridgeSessionManager_);
        bridgeSessionManager_->setOldSessionDisplacedCallback(
            [](int linkId, const std::string& clientAddr) {
                LinkTransportFacade::instance().disconnectServerClient(linkId, clientAddr);
            }
        );
    }

    std::string_view protocol() const override {
        return Constants::PROTOCOL_S7;
    }

    Task<> initializeAsync() override {
        co_await refreshDevices();
        co_return;
    }

    Task<> reloadAsync() override {
        co_await refreshDevices();
        co_return;
    }

    void onConnectionChanged(int linkId, const std::string& clientAddr, bool connected) override {
        if (!bridgeRegistry_ || bridgeRegistry_->empty()) {
            return;
        }

        auto definitions = bridgeRegistry_->getDefinitionsByLink(linkId);
        if (definitions.empty()) {
            return;
        }

        LOG_INFO << "[S7][Adapter] DTU link " << linkId
                 << (connected ? " connected: " : " disconnected: ")
                 << clientAddr;

        std::optional<S7DtuSession> previousSession;
        if (bridgeSessionManager_) {
            previousSession = bridgeSessionManager_->getSession(linkId, clientAddr);
        }

        if (bridgeSessionManager_) {
            if (connected) {
                bridgeSessionManager_->onConnected(linkId, clientAddr);
            } else {
                bridgeSessionManager_->onDisconnected(linkId, clientAddr);
            }
        }

        if (!connected && previousSession && previousSession->bindState == SessionBindState::Bound
            && previousSession->deviceId > 0) {
            closeRuntimeClient(previousSession->deviceId);
        }
    }

    void onDataReceived(int linkId, const std::string& clientAddr, std::vector<uint8_t> bytes) override {
        if (!bridgeNormalizer_ || !bridgeRegistry_ || !bridgeSessionManager_) {
            return;
        }

        auto definitions = bridgeRegistry_->getDefinitionsByLink(linkId);
        if (definitions.empty()) {
            return;
        }

        auto normalized = bridgeNormalizer_->normalize(linkId, clientAddr, bytes);
        if (normalized.kind == RegistrationMatchKind::Conflict) {
            LOG_WARN << "[S7][Adapter] Registration conflict: linkId="
                     << linkId << ", client=" << clientAddr
                     << ", bytes=" << bytes.size();
            return;
        }

        logRegistrationMatch(linkId, clientAddr, normalized);

        int deviceId = 0;
        if (!normalized.dtuKey.empty()) {
            auto dtuOpt = bridgeRegistry_->findByDtuKey(normalized.dtuKey);
            if (dtuOpt) {
                deviceId = dtuOpt->deviceId;
            }
        }

        auto sessionOpt = bridgeSessionManager_->getSession(linkId, clientAddr);
        if (deviceId <= 0 && sessionOpt && sessionOpt->bindState == SessionBindState::Bound) {
            deviceId = sessionOpt->deviceId;
        }

        if (deviceId > 0 && sessionOpt && sessionOpt->bindState == SessionBindState::Bound) {
            const std::string dtuKey = normalized.dtuKey.empty() ? sessionOpt->dtuKey : normalized.dtuKey;
            attachBridgeSession(deviceId, linkId, clientAddr, dtuKey);
        }

        if (!normalized.payload.empty() && deviceId > 0) {
            forwardDtuPayload(deviceId, std::move(normalized.payload));
            return;
        }

        if (!normalized.payload.empty()) {
            LOG_DEBUG << "[S7][Adapter] Drop unbound payload: linkId=" << linkId
                      << ", client=" << clientAddr
                      << ", bytes=" << normalized.payload.size()
                      << ", kind=" << registrationMatchKindToString(normalized.kind);
        }
    }

    void onMaintenanceTick() override {
        std::vector<ParsedFrameResult> results;
        const auto now = std::chrono::steady_clock::now();

        auto runtimes = snapshotRuntimesLocked();
        for (const auto& runtime : runtimes) {
            if (!ensureConnected(runtime, now)) {
                continue;
            }

            int deviceId = 0;
            bool bridgeMode = false;
            bool connected = false;
            bool bridgeBound = false;
            std::string bridgeClientAddr;
            bool shouldPoll = true;

            {
                std::lock_guard runtimeLock(runtime->mutex);
                deviceId = runtime->deviceId;
                bridgeMode = runtime->bridgeMode;
                connected = runtime->connected;
                bridgeBound = runtime->bridgeBound;
                bridgeClientAddr = runtime->bridgeClientAddr;

                if (runtime->areas.empty()) {
                    shouldPoll = false;
                } else if (runtime->connection.pollIntervalSec > 0
                    && runtime->lastPoll != std::chrono::steady_clock::time_point{}
                    && std::chrono::duration_cast<std::chrono::seconds>(now - runtime->lastPoll).count()
                        < runtime->connection.pollIntervalSec) {
                    shouldPoll = false;
                } else {
                    runtime->lastPoll = now;
                }
            }

            if (bridgeMode) {
                LOG_TRACE << "[S7][Adapter] Poll bridge device " << deviceId
                          << " connected=" << connected
                          << " bound=" << bridgeBound
                          << " dtu=" << (bridgeClientAddr.empty() ? "<unbound>" : bridgeClientAddr);
            }

            if (!shouldPoll) {
                continue;
            }

            pollDevice(runtime, results);
        }

        if (!results.empty() && runtimeContext_.submitParsedResults) {
            runtimeContext_.submitParsedResults(std::move(results));
        }
    }

    ProtocolAdapterMetrics getMetrics() const override {
        ProtocolAdapterMetrics metrics;
        metrics.available = true;
        auto runtimes = snapshotRuntimesLocked();
        metrics.stats["deviceCount"] = static_cast<Json::Int64>(runtimes.size());
        Json::Int64 connectedCount = 0;
        Json::Int64 areaCount = 0;
        Json::Int64 bridgeDeviceCount = 0;
        Json::Int64 bridgeReadyCount = 0;
        for (const auto& runtime : runtimes) {
            std::lock_guard runtimeLock(runtime->mutex);
            if (runtime->bridgeMode) {
                ++bridgeDeviceCount;
            }
            if (runtime->connected && (!runtime->bridgeMode || isBridgeReadyLocked(*runtime))) {
                ++connectedCount;
            }
            areaCount += static_cast<Json::Int64>(runtime->areas.size());
            if (runtime->bridgeMode && isBridgeReadyLocked(*runtime)) {
                ++bridgeReadyCount;
            }
        }
        metrics.stats["connectedCount"] = connectedCount;
        metrics.stats["areaCount"] = areaCount;
        metrics.stats["bridgeDeviceCount"] = bridgeDeviceCount;
        metrics.stats["bridgeReadyCount"] = bridgeReadyCount;
        return metrics;
    }

    ProtocolLifecycleImpact onDeviceLifecycleEvent(const DeviceLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_S7) {
            return ProtocolLifecycleImpact::None;
        }
        return ProtocolLifecycleImpact::Reload;
    }

    ProtocolLifecycleImpact onProtocolConfigLifecycleEvent(const ProtocolConfigLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_S7) {
            return ProtocolLifecycleImpact::None;
        }
        return ProtocolLifecycleImpact::Reload;
    }

    bool isDeviceConnected(int deviceId) const override {
        auto runtime = const_cast<S7ProtocolAdapter*>(this)->findRuntimeLocked(deviceId);
        if (!runtime) {
            return false;
        }

        std::lock_guard runtimeLock(runtime->mutex);
        return runtime->connected
            && (!runtime->bridgeMode || isBridgeReadyLocked(*runtime));
    }

    Task<CommandResult> sendCommand(const CommandRequest& req) override {
        auto guard = makeCommandGuard();

        try {
            auto deviceOpt = req.deviceId > 0
                ? DeviceCache::instance().findByIdSync(req.deviceId)
                : std::optional<DeviceCache::CachedDevice>{};
            if (!deviceOpt && !req.deviceCode.empty()) {
                deviceOpt = DeviceCache::instance().findByCodeSync(req.deviceCode);
            }
            if (!deviceOpt || deviceOpt->protocolType != Constants::PROTOCOL_S7) {
                co_return CommandResult::error("S7 设备不存在");
            }

            const int deviceId = deviceOpt->id;
            int linkId = deviceOpt->linkId;
            std::string deviceCode = deviceOpt->deviceCode.empty()
                ? std::to_string(deviceOpt->id)
                : deviceOpt->deviceCode;

            auto runtime = findRuntimeLocked(deviceId);
            if (!runtime) {
                co_return CommandResult::offline("S7 设备离线");
            }

            if (!ensureConnected(runtime, std::chrono::steady_clock::now())) {
                co_return CommandResult::offline("S7 设备离线");
            }

            {
                std::lock_guard runtimeLock(runtime->mutex);
                if (runtime->bridgeMode && !runtime->bridgeBound) {
                    co_return CommandResult::offline("S7 会话未就绪");
                }

                linkId = runtime->linkId;
                if (!runtime->deviceCode.empty()) {
                    deviceCode = runtime->deviceCode;
                }
            }

            if (!req.elements.isArray() || req.elements.empty()) {
                co_return CommandResult::error("S7 指令缺少 elements");
            }

            int64_t downCommandId = co_await savePendingCommand(
                deviceId, linkId, Constants::PROTOCOL_S7,
                req.funcCode, "S7 写入", "", req.userId, req.elements);

            std::string failure;
            {
                if (!ensureConnected(runtime, std::chrono::steady_clock::now())) {
                    failure = "S7 设备离线";
                } else {
                    {
                        std::lock_guard runtimeLock(runtime->mutex);
                        if (runtime->bridgeMode && !runtime->bridgeBound) {
                            failure = "S7 会话未就绪";
                        }
                    }

                    if (failure.empty()) {
                        for (const auto& elem : req.elements) {
                            if (!elem.isObject()) continue;
                            std::string areaName = toUpper(elem.get("area", "DB").asString());
                            int dbNumber = elem.get("dbNumber", 0).asInt();
                            if (areaName == "V" && dbNumber <= 0) {
                                dbNumber = 1;
                            }
                            int start = elem.get("start", 0).asInt();
                            auto bytes = parseBytes(elem);
                            if (bytes.empty()) {
                                failure = "S7 指令缺少可写入的数据";
                                break;
                            }

                            int rc = 0;
                            {
                                std::lock_guard clientLock(runtime->clientMutex);
                                rc = writeArea(*runtime, areaName, dbNumber, start, bytes);
                            }
                            if (rc != 0) {
                                failure = "S7 写入失败，错误码=" + std::to_string(rc)
                                    + " (" + explainClientRc(rc) + ")";
                                break;
                            }
                        }
                    }
                }
            }

            if (!failure.empty()) {
                co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SEND_FAILED", failure);
                co_return CommandResult::sendFailed(failure);
            }

            co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SUCCESS", "S7 写入成功");
            if (runtimeContext_.notifyCommandCompletion) {
                runtimeContext_.notifyCommandCompletion(deviceCode,
                    "SUCCESS", true, downCommandId);
            }
            guard.release();
            co_return CommandResult::success();
        } catch (const std::exception& e) {
            co_return CommandResult::error(e.what());
        }
    }

private:
    static std::string toUpper(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return value;
    }

    static std::string normalizeDataType(std::string value) {
        value = toUpper(std::move(value));
        if (value == "DOUBLE") return "LREAL";
        if (value == "BOOL" || value == "INT8" || value == "UINT8" || value == "INT16" ||
            value == "UINT16" || value == "INT32" || value == "UINT32" || value == "FLOAT" ||
            value == "LREAL" || value == "STRING") {
            return value;
        }
        return "INT16";
    }

    static int areaToCode(const std::string& area) {
        const std::string upper = toUpper(area);
        if (upper == "DB") return S7AreaDB;
        if (upper == "V") return S7AreaDB;
        if (upper == "MK") return S7AreaMK;
        if (upper == "PA") return S7AreaPA;
        if (upper == "PE") return S7AreaPE;
        if (upper == "CT") return S7AreaCT;
        if (upper == "TM") return S7AreaTM;
        return S7AreaDB;
    }

    static int areaWordLen(const std::string& area) {
        const std::string upper = toUpper(area);
        if (upper == "CT") return S7WLCounter;
        if (upper == "TM") return S7WLTimer;
        return S7WLByte;
    }

    static int transferAmount(const std::string& area, size_t size) {
        const std::string upper = toUpper(area);
        if (upper == "CT" || upper == "TM") {
            return std::max(1, static_cast<int>((size + 1) / 2));
        }
        return static_cast<int>(size);
    }

    static std::string normalizeConnectionMode(std::string value) {
        value = toUpper(std::move(value));
        if (value == "TSAP") {
            return "TSAP";
        }
        return "RACK_SLOT";
    }

    static std::uint16_t connectionTypeToCode(std::string value) {
        value = toUpper(std::move(value));
        if (value == "OP") {
            return kConnTypeOp;
        }
        if (value == "S7_BASIC" || value == "BASIC") {
            return kConnTypeBasic;
        }
        return kConnTypePg;
    }

    static std::optional<std::uint16_t> parseTsapHex(const Json::Value& value) {
        if (value.isNull()) {
            return std::nullopt;
        }
        if (value.isInt() || value.isUInt()) {
            const unsigned int raw = value.asUInt();
            if (raw > 0xFFFF) {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>(raw);
        }
        if (!value.isString()) {
            return std::nullopt;
        }

        std::string text = value.asString();
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
            return std::isspace(c) || c == '.' || c == ':' || c == '-' || c == '_';
        }), text.end());
        text = toUpper(std::move(text));
        if (text.rfind("0X", 0) == 0) {
            text.erase(0, 2);
        }
        if (text.empty() || text.size() > 4) {
            return std::nullopt;
        }
        if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::stoul(text, nullptr, 16));
    }

    static std::optional<S7ConnectionPreset> resolvePreset(const std::string& plcModel) {
        if (plcModel == "S7-200") return S7ConnectionPreset{.mode = "TSAP", .rack = 0, .slot = 1, .localTSAP = 0x4D57, .remoteTSAP = 0x4D57};
        if (plcModel == "S7-300") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 2};
        if (plcModel == "S7-400") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 3};
        if (plcModel == "S7-1200" || plcModel == "S7-1500") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 1};
        return std::nullopt;
    }

    static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) {
            return {};
        }

        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) {
                oss << ' ';
            }
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
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

    static std::string explainClientRc(int rc) {
        switch (rc) {
            case kS7Ok:
                return "OK";
            case kS7ErrInvalidHandle:
                return "Invalid handle";
            case kS7ErrInvalidParams:
                return "Invalid params";
            case kS7ErrSocketInit:
                return "Socket init failed";
            case kS7ErrResolveFailed:
                return "Resolve failed";
            case kS7ErrConnectFailed:
                return "Connect failed";
            case kS7ErrTimeout:
                return "Timeout";
            case kS7ErrSocketIo:
                return "Socket IO error";
            case kS7ErrProtocol:
                return "Protocol error";
            case kS7ErrNotConnected:
                return "Not connected";
            case kS7ErrPduNegotiation:
                return "PDU negotiation failed";
            case kS7ErrUnsupported:
                return "Unsupported";
            case kS7ErrResponseTooShort:
                return "Response too short";
            default:
                return rc < 0 ? "Unknown client error" : "PLC/S7 error";
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

        LOG_DEBUG << "[S7][Adapter] Registration stripped: linkId=" << linkId
                  << ", client=" << clientAddr
                  << ", kind=" << registrationMatchKindToString(normalized.kind)
                  << ", hex=" << bytesToHex(normalized.registrationBytes)
                  << ", ascii=\"" << bytesToAsciiString(normalized.registrationBytes) << "\""
                  << ", payloadBytes=" << normalized.payload.size();
    }

    static std::string makeUtcNowString() {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{now - dp};

        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << static_cast<int>(ymd.year()) << "-"
            << std::setw(2) << static_cast<unsigned>(ymd.month()) << "-"
            << std::setw(2) << static_cast<unsigned>(ymd.day()) << "T"
            << std::setw(2) << hms.hours().count() << ":"
            << std::setw(2) << hms.minutes().count() << ":"
            << std::setw(2) << hms.seconds().count() << "Z";
        return oss.str();
    }

    static uint16_t readU16BE(const std::vector<uint8_t>& buffer) {
        return static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) | buffer[1]);
    }

    static uint32_t readU32BE(const std::vector<uint8_t>& buffer) {
        return (static_cast<uint32_t>(buffer[0]) << 24)
            | (static_cast<uint32_t>(buffer[1]) << 16)
            | (static_cast<uint32_t>(buffer[2]) << 8)
            | static_cast<uint32_t>(buffer[3]);
    }

    static uint64_t readU64BE(const std::vector<uint8_t>& buffer) {
        return (static_cast<uint64_t>(buffer[0]) << 56)
            | (static_cast<uint64_t>(buffer[1]) << 48)
            | (static_cast<uint64_t>(buffer[2]) << 40)
            | (static_cast<uint64_t>(buffer[3]) << 32)
            | (static_cast<uint64_t>(buffer[4]) << 24)
            | (static_cast<uint64_t>(buffer[5]) << 16)
            | (static_cast<uint64_t>(buffer[6]) << 8)
            | static_cast<uint64_t>(buffer[7]);
    }

    static Json::Value decodeAreaValue(const S7AreaDefinition& area, const std::vector<uint8_t>& buffer) {
        const std::string dataType = normalizeDataType(area.dataType);

        if ((area.area == "CT" || area.area == "TM") && buffer.size() >= 2) {
            return Json::Value(static_cast<Json::UInt64>(readU16BE(buffer)));
        }

        if (dataType == "BOOL") {
            if (buffer.empty()) {
                return Json::nullValue;
            }
            const int bit = std::clamp(area.startBit, 0, 7);
            return Json::Value((buffer[0] >> bit) & 0x01);
        }

        if (dataType == "INT8") {
            if (buffer.empty()) return Json::nullValue;
            return Json::Value(static_cast<Json::Int64>(static_cast<int8_t>(buffer[0])));
        }
        if (dataType == "UINT8") {
            if (buffer.empty()) return Json::nullValue;
            return Json::Value(static_cast<Json::UInt64>(buffer[0]));
        }
        if (dataType == "INT16") {
            if (buffer.size() < 2) return Json::nullValue;
            return Json::Value(static_cast<Json::Int64>(static_cast<int16_t>(readU16BE(buffer))));
        }
        if (dataType == "UINT16") {
            if (buffer.size() < 2) return Json::nullValue;
            return Json::Value(static_cast<Json::UInt64>(readU16BE(buffer)));
        }
        if (dataType == "INT32") {
            if (buffer.size() < 4) return Json::nullValue;
            return Json::Value(static_cast<Json::Int64>(static_cast<int32_t>(readU32BE(buffer))));
        }
        if (dataType == "UINT32") {
            if (buffer.size() < 4) return Json::nullValue;
            return Json::Value(static_cast<Json::UInt64>(readU32BE(buffer)));
        }
        if (dataType == "FLOAT") {
            if (buffer.size() < 4) return Json::nullValue;
            float value = 0.0f;
            uint32_t raw = readU32BE(buffer);
            std::memcpy(&value, &raw, sizeof(value));
            return Json::Value(value);
        }
        if (dataType == "LREAL") {
            if (buffer.size() < 8) return Json::nullValue;
            double value = 0.0;
            uint64_t raw = readU64BE(buffer);
            std::memcpy(&value, &raw, sizeof(value));
            return Json::Value(value);
        }
        if (dataType == "STRING") {
            std::string value(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            size_t zero = value.find('\0');
            if (zero != std::string::npos) {
                value.resize(zero);
            }
            return Json::Value(value);
        }
        return Json::Value(bytesToHex(buffer));
    }

    static ParsedFrameResult buildReadResult(int deviceId,
                                             int linkId,
                                             const S7AreaDefinition& area,
                                             const std::vector<uint8_t>& buffer) {
        ParsedFrameResult result;
        result.deviceId = deviceId;
        result.linkId = linkId;
        result.protocol = Constants::PROTOCOL_S7;
        result.funcCode = area.id;
        result.reportTime = makeUtcNowString();

        Json::Value payload(Json::objectValue);
        payload["funcCode"] = area.id;
        payload["funcName"] = area.name;
        payload["direction"] = "UP";

        Json::Value data(Json::objectValue);
        Json::Value element(Json::objectValue);
        const int dbNumber = area.area == "V" ? 1 : area.dbNumber;
        element["name"] = area.name;
        element["value"] = decodeAreaValue(area, buffer);
        element["hex"] = bytesToHex(buffer);
        element["area"] = area.area;
        element["dataType"] = normalizeDataType(area.dataType);
        element["dbNumber"] = dbNumber;
        element["start"] = area.start;
        element["size"] = area.size;
        if (normalizeDataType(area.dataType) == "BOOL") {
            element["startBit"] = area.startBit;
        }
        if (area.writable) {
            element["writable"] = true;
        }
        if (!area.remark.empty()) {
            element["remark"] = area.remark;
        }

        data[area.id] = std::move(element);
        payload["data"] = std::move(data);
        result.data = std::move(payload);
        return result;
    }

    static std::vector<uint8_t> parseBytes(const Json::Value& element) {
        std::string hex = element.get("valueHex", "").asString();
        if (hex.empty()) {
            hex = element.get("value", "").asString();
        }
        std::vector<uint8_t> bytes;
        if (hex.empty()) {
            return bytes;
        }
        if (hex.size() % 2 != 0) {
            hex = "0" + hex;
        }
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
            bytes.push_back(byte);
        }
        return bytes;
    }

    static S7ConnectionConfig parseConnection(const Json::Value& config, const std::string& plcModel) {
        S7ConnectionConfig connection;
        const auto preset = resolvePreset(plcModel).value_or(S7ConnectionPreset{});
        const bool isS7_200 = toUpper(plcModel) == "S7-200";
        connection.mode = preset.mode;
        connection.rack = preset.rack;
        connection.slot = preset.slot;
        connection.localTSAP = preset.localTSAP;
        connection.remoteTSAP = preset.remoteTSAP;

        if (config.isMember("connection") && config["connection"].isObject()) {
            const auto& conn = config["connection"];
            connection.host = conn.get("host", "").asString();
            connection.connectionType = toUpper(conn.get("connectionType", "PG").asString());
            if (isS7_200) {
                connection.mode = "TSAP";
            } else if (conn.isMember("mode")) {
                connection.mode = normalizeConnectionMode(conn.get("mode", "").asString());
            } else if (conn.isMember("localTSAP") || conn.isMember("remoteTSAP")) {
                connection.mode = "TSAP";
            }
            connection.rack = conn.get("rack", connection.rack).asInt();
            connection.slot = conn.get("slot", connection.slot).asInt();
            if (auto localTSAP = parseTsapHex(conn["localTSAP"])) {
                connection.localTSAP = *localTSAP;
            }
            if (auto remoteTSAP = parseTsapHex(conn["remoteTSAP"])) {
                connection.remoteTSAP = *remoteTSAP;
            }
            connection.pingTimeoutMs = conn.get("pingTimeout", connection.pingTimeoutMs).asInt();
            connection.sendTimeoutMs = conn.get("sendTimeout", connection.sendTimeoutMs).asInt();
            connection.recvTimeoutMs = conn.get("recvTimeout", connection.recvTimeoutMs).asInt();
            if (connection.pingTimeoutMs <= 0) connection.pingTimeoutMs = kDefaultTimeoutMs;
            if (connection.sendTimeoutMs <= 0) connection.sendTimeoutMs = kDefaultTimeoutMs;
            if (connection.recvTimeoutMs <= 0) connection.recvTimeoutMs = kDefaultTimeoutMs;
            if (connection.recvTimeoutMs < connection.sendTimeoutMs) {
                connection.recvTimeoutMs = connection.sendTimeoutMs;
            }
            if (connection.pingTimeoutMs < connection.recvTimeoutMs) {
                connection.pingTimeoutMs = connection.recvTimeoutMs;
            }
        }
        if (isS7_200) {
            connection.mode = "TSAP";
        }
        connection.pollIntervalSec = config.get("pollInterval", 5).asInt();
        if (connection.pollIntervalSec < 1) connection.pollIntervalSec = 1;
        return connection;
    }

    static std::vector<S7AreaDefinition> parseAreas(const Json::Value& config) {
        const Json::Value* areas = nullptr;
        if (config.isMember("areas") && config["areas"].isArray()) {
            areas = &config["areas"];
        } else if (config.isMember("poll") && config["poll"].isObject() && config["poll"].isMember("areas") && config["poll"]["areas"].isArray()) {
            areas = &config["poll"]["areas"];
        }

        std::vector<S7AreaDefinition> result;
        if (!areas) return result;
        const std::string plcModel = toUpper(config.get("plcModel", "").asString());
        const bool isS7_200 = plcModel == "S7-200";
        for (const auto& area : *areas) {
            if (!area.isObject()) continue;
            S7AreaDefinition def;
            const std::string areaType = toUpper(area.get("area", "DB").asString());
            def.id = area.get("id", "").asString();
            def.name = area.get("name", def.id).asString();
            def.area = areaType;
            def.dataType = normalizeDataType(area.get("dataType", (areaType == "CT" || areaType == "TM") ? "UINT16" : "INT16").asString());
            if (areaType == "CT" || areaType == "TM") {
                def.dataType = "UINT16";
            }
            def.dbNumber = (areaType == "V" || (isS7_200 && areaType == "DB")) ? 1 : area.get("dbNumber", 0).asInt();
            def.start = area.get("start", 0).asInt();
            def.startBit = std::clamp(area.get("startBit", 0).asInt(), 0, 7);
            def.size = std::max(1, area.get("size", 1).asInt());
            def.writable = area.get("writable", false).asBool();
            def.remark = area.get("remark", "").asString();
            if (!def.id.empty()) {
                result.push_back(std::move(def));
            }
        }
        return result;
    }

    static std::shared_ptr<S7DeviceRuntime> buildRuntime(const DeviceCache::CachedDevice& device) {
        if (device.protocolType != Constants::PROTOCOL_S7) {
            return nullptr;
        }
        auto runtime = std::make_shared<S7DeviceRuntime>();
        runtime->deviceId = device.id;
        runtime->linkId = device.linkId;
        runtime->deviceCode = device.deviceCode.empty() ? ("s7_" + std::to_string(device.id)) : device.deviceCode;
        runtime->bridgeMode = device.linkMode == Constants::LINK_MODE_TCP_SERVER;
        if (runtime->bridgeMode) {
            runtime->dtuKey = std::to_string(device.linkId) + ":"
                + detail::makeRegistrationToken(device.registrationBytes);
        }
        const std::string plcModel = device.protocolConfig.get("plcModel", "").asString();
        runtime->connection = parseConnection(device.protocolConfig, plcModel);
        if (runtime->bridgeMode) {
            if (runtime->connection.host.empty()) {
                runtime->connection.host = device.linkIp.empty() ? "tcpserver" : device.linkIp;
            }
        } else if (!device.linkIp.empty()) {
            runtime->connection.host = device.linkIp;
        }
        if (runtime->connection.host.empty()) {
            return nullptr;
        }
        runtime->areas = parseAreas(device.protocolConfig);
        return runtime;
    }

    Task<> refreshDevices() {
        if (bridgeRegistry_) {
            co_await bridgeRegistry_->reload();
        }

        if (bridgeSessionManager_) {
            auto sessions = bridgeSessionManager_->listSessions();
            for (const auto& session : sessions) {
                if (session.linkId > 0 && !session.clientAddr.empty()) {
                    LinkTransportFacade::instance().disconnectServerClient(session.linkId, session.clientAddr);
                }
            }
            bridgeSessionManager_->clearAllSessions();
        }

        auto devices = co_await DeviceCache::instance().getDevices();
        std::unordered_map<int, std::shared_ptr<S7DeviceRuntime>> next;
        std::size_t bridgeCount = 0;
        for (const auto& device : devices) {
            if (device.protocolType != Constants::PROTOCOL_S7) continue;
            auto runtime = buildRuntime(device);
            if (!runtime) continue;
            if (runtime->bridgeMode) {
                ++bridgeCount;
            }
            next.emplace(runtime->deviceId, runtime);
        }

        std::size_t deviceCount = 0;
        {
            std::lock_guard lock(devicesMutex_);
            devices_ = std::move(next);
            deviceCount = devices_.size();
        }
        LOG_INFO << "[S7][Adapter] Reloaded " << deviceCount
                 << " S7 device(s), bridge devices=" << bridgeCount;
        co_return;
    }

    bool ensureConnected(const std::shared_ptr<S7DeviceRuntime>& runtime,
                         std::chrono::steady_clock::time_point now) {
        if (!runtime) {
            return false;
        }

        const auto destroyClient = [](std::unique_ptr<Client>& client) {
            if (client) {
                client->disconnect();
                client.reset();
            }
        };

        int deviceId = 0;
        int linkId = 0;
        bool bridgeMode = false;
        S7ConnectionConfig connection;
        std::uint64_t attemptId = 0;

        {
            std::scoped_lock runtimeLock(runtime->clientMutex, runtime->mutex);
            if (runtime->connected && runtime->client && runtime->client->connected()) {
                return true;
            }
            if (runtime->connectInProgress) {
                return false;
            }

            deviceId = runtime->deviceId;
            linkId = runtime->linkId;
            bridgeMode = runtime->bridgeMode;
            connection = runtime->connection;

            LOG_DEBUG << "[S7][Adapter] Connecting device " << deviceId
                      << " to " << connection.host
                      << " mode=" << connection.mode
                      << (bridgeMode ? " (bridge)" : "")
                      << ", pingTimeout=" << connection.pingTimeoutMs << "ms"
                      << ", sendTimeout=" << connection.sendTimeoutMs << "ms"
                      << ", recvTimeout=" << connection.recvTimeoutMs << "ms";

            runtime->resetClient();
            runtime->lastConnectAttempt = now;
            runtime->connectInProgress = true;
            attemptId = ++runtime->connectGeneration;
        }

        auto client = std::unique_ptr<Client>(new (std::nothrow) Client());
        if (!client) {
            std::lock_guard runtimeLock(runtime->mutex);
            if (runtime->connectGeneration == attemptId) {
                runtime->connectInProgress = false;
                runtime->connected = false;
            }
            return false;
        }

        applyConnectionTimeouts(*client, connection);
        client->setTraceCallback(
            [deviceId](std::string_view stage, bool outbound, const std::vector<std::uint8_t>& frame) {
                LOG_DEBUG << "[S7][Packet] deviceId=" << deviceId
                          << " " << (outbound ? "TX " : "RX ")
                          << stage
                          << " len=" << frame.size()
                          << " hex=" << bytesToHex(frame);
            }
        );
        if (bridgeMode) {
            client->setTransportHooks({
                .open = [runtime](const std::string&, std::uint16_t, int, int, int, std::uint16_t& localPort) {
                    return openBridgeTransport(runtime, localPort);
                },
                .close = [runtime]() {
                    closeBridgeTransport(runtime);
                },
                .connected = [runtime]() {
                    return isBridgeTransportOpen(runtime);
                },
                .send = [this, runtime](const std::uint8_t* data, std::size_t size) {
                    return sendBridgePayload(runtime, data, size);
                },
                .recv = [runtime](std::uint8_t* data, std::size_t size, int timeoutMs) {
                    return recvBridgePayload(runtime, data, size, timeoutMs);
                }
            });
        }

        int rc = -1;
        if (connection.mode == "TSAP") {
            if (client->setConnectionParams(
                    connection.host.c_str(),
                    connection.localTSAP,
                    connection.remoteTSAP) != 0) {
                LOG_WARN << "[S7][Adapter] Set TSAP params failed for device " << deviceId
                         << ", host=" << connection.host;
                destroyClient(client);
                std::lock_guard runtimeLock(runtime->mutex);
                if (runtime->connectGeneration == attemptId) {
                    runtime->connectInProgress = false;
                    runtime->connected = false;
                }
                return false;
            }
            rc = client->connect();
        } else {
            if (client->setConnectionType(connectionTypeToCode(connection.connectionType)) != 0) {
                LOG_WARN << "[S7][Adapter] Set connection type failed for device " << deviceId
                         << ", type=" << connection.connectionType;
                destroyClient(client);
                std::lock_guard runtimeLock(runtime->mutex);
                if (runtime->connectGeneration == attemptId) {
                    runtime->connectInProgress = false;
                    runtime->connected = false;
                }
                return false;
            }
            rc = client->connectTo(connection.host.c_str(), connection.rack, connection.slot);
        }

        const bool connected = (rc == 0) && client->connected();
        {
            std::scoped_lock runtimeLock(runtime->clientMutex, runtime->mutex);
            if (runtime->connectGeneration != attemptId) {
                destroyClient(client);
                return runtime->connected && runtime->client && runtime->client->connected();
            }

            runtime->connectInProgress = false;
            runtime->connected = connected;

            if (connected) {
                runtime->client = std::move(client);
            }
        }

        if (!connected) {
            LOG_WARN << "[S7][Adapter] Connect failed for device " << deviceId
                     << ", rc=" << rc
                     << " (" << explainClientRc(rc) << ")"
                     << ", host=" << connection.host
                     << ", mode=" << connection.mode;
            destroyClient(client);
            return false;
        }

        LOG_INFO << "[S7][Adapter] Connected device " << deviceId
                 << ", linkId=" << linkId
                 << ", host=" << connection.host
                 << ", mode=" << connection.mode
                 << ", bridge=" << (bridgeMode ? "yes" : "no");
        return true;
    }

    std::shared_ptr<S7DeviceRuntime> findRuntimeLocked(int deviceId) {
        std::lock_guard lock(devicesMutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return nullptr;
        }
        return it->second;
    }

    std::vector<std::shared_ptr<S7DeviceRuntime>> snapshotRuntimesLocked() const {
        std::vector<std::shared_ptr<S7DeviceRuntime>> runtimes;
        std::lock_guard lock(devicesMutex_);
        runtimes.reserve(devices_.size());
        for (const auto& [_, runtime] : devices_) {
            runtimes.push_back(runtime);
        }
        return runtimes;
    }

    static void prependQueue(std::deque<std::vector<uint8_t>>& target, std::deque<std::vector<uint8_t>>& source) {
        while (!source.empty()) {
            target.push_front(std::move(source.back()));
            source.pop_back();
        }
    }

    static std::string bytesToString(const std::vector<uint8_t>& bytes) {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    static void applyConnectionTimeouts(Client& client, const S7ConnectionConfig& connection) {
        const auto setTimeout = [&](int param, int value, const char* name) {
            if (value <= 0) {
                return;
            }
            int32_t timeout = value;
            if (client.setParam(param, &timeout) != 0) {
                LOG_DEBUG << "[S7][Adapter] Failed to set " << name
                          << " for host=" << connection.host
                          << ", value=" << value;
            }
        };

        setTimeout(p_i32_PingTimeout, connection.pingTimeoutMs, "PingTimeout");
        setTimeout(p_i32_SendTimeout, connection.sendTimeoutMs, "SendTimeout");
        setTimeout(p_i32_RecvTimeout, connection.recvTimeoutMs, "RecvTimeout");
    }

    static bool isBridgeReadyLocked(const S7DeviceRuntime& runtime) {
        return runtime.bridgeMode
            && runtime.connected
            && !runtime.connectInProgress
            && runtime.bridgeBound
            && !runtime.bridgeClientAddr.empty()
            && runtime.bridgeLinkId > 0;
    }

    static int openBridgeTransport(const std::shared_ptr<S7DeviceRuntime>& runtime,
                                   std::uint16_t& localPort) {
        if (!runtime) {
            return kS7ErrInvalidHandle;
        }

        {
            std::lock_guard bridgeIoLock(runtime->bridgeIoMutex);
            runtime->bridgeTransportOpen = true;
            runtime->bridgeRxBuffer.clear();
        }
        runtime->bridgeIoCv.notify_all();
        localPort = 0;
        return kS7Ok;
    }

    static void closeBridgeTransport(const std::shared_ptr<S7DeviceRuntime>& runtime) {
        if (!runtime) {
            return;
        }

        {
            std::lock_guard bridgeIoLock(runtime->bridgeIoMutex);
            runtime->bridgeTransportOpen = false;
            runtime->bridgeRxBuffer.clear();
        }
        runtime->bridgeIoCv.notify_all();
    }

    static bool isBridgeTransportOpen(const std::shared_ptr<S7DeviceRuntime>& runtime) {
        if (!runtime) {
            return false;
        }

        std::lock_guard bridgeIoLock(runtime->bridgeIoMutex);
        return runtime->bridgeTransportOpen;
    }

    int sendBridgePayload(const std::shared_ptr<S7DeviceRuntime>& runtime,
                          const std::uint8_t* data,
                          std::size_t size) {
        if (!runtime) {
            return kS7ErrInvalidHandle;
        }
        if (!data || size == 0) {
            return kS7Ok;
        }

        std::vector<uint8_t> bytes(data, data + size);
        const std::string payload = bytesToString(bytes);

        int deviceId = 0;
        int requestLinkId = 0;
        std::string bridgeClientAddr;
        int bridgeLinkId = 0;
        bool shouldBroadcast = false;
        std::set<std::string> excludeAddrs;

        {
            std::lock_guard runtimeLock(runtime->mutex);
            if (!runtime->bridgeMode) {
                return kS7ErrInvalidHandle;
            }

            deviceId = runtime->deviceId;
            requestLinkId = runtime->linkId;
            bridgeClientAddr = runtime->bridgeClientAddr;
            bridgeLinkId = runtime->bridgeLinkId;

            if (!runtime->bridgeBound || bridgeClientAddr.empty() || bridgeLinkId <= 0) {
                if (!runtime->bridgeDiscoveryInFlight) {
                    runtime->bridgeDiscoveryInFlight = true;
                    runtime->bridgeDiscoveryStartedAt = std::chrono::steady_clock::now();
                    shouldBroadcast = true;
                } else {
                    runtime->pendingBridgeToDtu.push_back(std::move(bytes));
                    LOG_DEBUG << "[S7][Direct] Queue payload while discovery is in flight: deviceId="
                              << deviceId
                              << ", bytes=" << runtime->pendingBridgeToDtu.back().size()
                              << ", pendingToDtu=" << runtime->pendingBridgeToDtu.size();
                    return kS7Ok;
                }
            }
        }

        if (shouldBroadcast) {
            if (bridgeSessionManager_) {
                for (const auto& session : bridgeSessionManager_->listSessions()) {
                    if (session.linkId == requestLinkId
                        && session.bindState == SessionBindState::Bound
                        && !session.clientAddr.empty()) {
                        excludeAddrs.insert(session.clientAddr);
                    }
                }
            }

            LOG_DEBUG << "[S7][Adapter] TX probe to DTU linkId=" << requestLinkId
                      << ", deviceId=" << deviceId
                      << ", bytes=" << bytes.size()
                      << ", excluded=" << excludeAddrs.size()
                      << ", hex=" << bytesToHex(bytes);
            const bool sent = excludeAddrs.empty()
                ? LinkTransportFacade::instance().sendData(requestLinkId, payload)
                : LinkTransportFacade::instance().sendDataExcluding(requestLinkId, payload, excludeAddrs);

            if (sent) {
                LOG_INFO << "[S7][Direct] Broadcast discovery probe: deviceId=" << deviceId
                         << ", linkId=" << requestLinkId
                         << ", bytes=" << bytes.size()
                         << ", excluded=" << excludeAddrs.size();
                return kS7Ok;
            }

            std::lock_guard runtimeLock(runtime->mutex);
            runtime->pendingBridgeToDtu.push_back(std::move(bytes));
            runtime->bridgeDiscoveryInFlight = false;
            runtime->bridgeDiscoveryStartedAt = {};
            LOG_WARN << "[S7][Direct] Failed to forward discovery payload, queued for retry: deviceId="
                     << deviceId << ", linkId=" << requestLinkId
                     << ", bytes=" << runtime->pendingBridgeToDtu.back().size()
                     << ", pendingToDtu=" << runtime->pendingBridgeToDtu.size();
            return kS7Ok;
        }

        LOG_DEBUG << "[S7][Adapter] TX DTU client=" << bridgeClientAddr
                  << ", linkId=" << bridgeLinkId
                  << ", bytes=" << bytes.size()
                  << ", hex=" << bytesToHex(bytes);
        if (LinkTransportFacade::instance().sendToClient(bridgeLinkId, bridgeClientAddr, payload)) {
            return kS7Ok;
        }

        std::lock_guard runtimeLock(runtime->mutex);
        runtime->pendingBridgeToDtu.push_back(std::move(bytes));
        LOG_WARN << "[S7][Direct] Failed to forward payload, queued for retry: deviceId="
                 << deviceId << ", linkId=" << bridgeLinkId
                 << ", client=" << bridgeClientAddr
                 << ", bytes=" << runtime->pendingBridgeToDtu.back().size()
                 << ", pendingToDtu=" << runtime->pendingBridgeToDtu.size();
        return kS7Ok;
    }

    static int recvBridgePayload(const std::shared_ptr<S7DeviceRuntime>& runtime,
                                 std::uint8_t* data,
                                 std::size_t size,
                                 int timeoutMs) {
        if (!runtime || (!data && size > 0)) {
            return kS7ErrInvalidParams;
        }
        if (size == 0) {
            return kS7Ok;
        }

        std::unique_lock bridgeIoLock(runtime->bridgeIoMutex);
        const auto ready = [&]() {
            return runtime->bridgeRxBuffer.size() >= size || !runtime->bridgeTransportOpen;
        };

        if (timeoutMs > 0) {
            if (!runtime->bridgeIoCv.wait_for(
                    bridgeIoLock,
                    std::chrono::milliseconds(timeoutMs),
                    ready)) {
                return kS7ErrTimeout;
            }
        } else {
            runtime->bridgeIoCv.wait(bridgeIoLock, ready);
        }

        if (runtime->bridgeRxBuffer.size() < size) {
            return runtime->bridgeTransportOpen ? kS7ErrTimeout : kS7ErrSocketIo;
        }

        for (std::size_t i = 0; i < size; ++i) {
            data[i] = runtime->bridgeRxBuffer.front();
            runtime->bridgeRxBuffer.pop_front();
        }
        return kS7Ok;
    }

    void attachBridgeSession(int deviceId, int linkId, const std::string& clientAddr, const std::string& dtuKey) {
        bool shouldFlush = false;
        bool shouldLog = false;
        {
            auto runtime = findRuntimeLocked(deviceId);
            if (!runtime || !runtime->bridgeMode) {
                return;
            }

            std::lock_guard runtimeLock(runtime->mutex);
            shouldLog = !runtime->bridgeBound
                || runtime->bridgeLinkId != linkId
                || runtime->bridgeClientAddr != clientAddr
                || (!dtuKey.empty() && runtime->dtuKey != dtuKey);
            runtime->bridgeBound = true;
            runtime->bridgeDiscoveryInFlight = false;
            runtime->bridgeDiscoveryStartedAt = {};
            runtime->bridgeLinkId = linkId;
            runtime->bridgeClientAddr = clientAddr;
            if (!dtuKey.empty()) {
                runtime->dtuKey = dtuKey;
            }
            shouldFlush = runtime->bridgeTransportOpen;
        }

        if (shouldLog) {
            LOG_INFO << "[S7][Adapter] Bridge session bound: deviceId=" << deviceId
                     << ", linkId=" << linkId
                     << ", client=" << clientAddr
                     << ", dtuKey=" << (dtuKey.empty() ? "<unknown>" : dtuKey);
        }

        if (shouldFlush) {
            LOG_DEBUG << "[S7][Adapter] Bridge session ready, flushing pending payloads for device "
                      << deviceId;
            flushBridgeSession(deviceId);
        }
    }

    void closeRuntimeClient(int deviceId) {
        auto runtime = findRuntimeLocked(deviceId);
        if (!runtime) {
            return;
        }

        {
            std::lock_guard runtimeLock(runtime->mutex);
            LOG_INFO << "[S7][Adapter] Closing bridge runtime: deviceId=" << deviceId
                     << ", client=" << (runtime->bridgeClientAddr.empty() ? "<unbound>" : runtime->bridgeClientAddr);
            runtime->bridgeBound = false;
            runtime->bridgeDiscoveryInFlight = false;
            runtime->connectInProgress = false;
            ++runtime->connectGeneration;
            runtime->bridgeDiscoveryStartedAt = {};
            runtime->bridgeLinkId = 0;
            runtime->bridgeClientAddr.clear();
            runtime->pendingBridgeToDtu.clear();
            runtime->connected = false;
        }

        {
            std::lock_guard clientLock(runtime->clientMutex);
            runtime->resetClient();
        }
    }

    void forwardDtuPayload(int deviceId, std::vector<uint8_t> bytes) {
        if (bytes.empty()) {
            return;
        }

        const std::size_t byteCount = bytes.size();
        auto runtime = findRuntimeLocked(deviceId);
        if (!runtime || !runtime->bridgeMode) {
            return;
        }

        int linkId = 0;
        std::string bridgeClientAddr;
        bool accepted = false;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            linkId = runtime->linkId;
            bridgeClientAddr = runtime->bridgeClientAddr;
        }
        {
            std::lock_guard bridgeIoLock(runtime->bridgeIoMutex);
            if (runtime->bridgeTransportOpen) {
                runtime->bridgeRxBuffer.insert(runtime->bridgeRxBuffer.end(), bytes.begin(), bytes.end());
                accepted = true;
            }
        }
        if (accepted) {
            runtime->bridgeIoCv.notify_all();
        }

        LOG_DEBUG << "[S7][Adapter] RX DTU payload: deviceId=" << deviceId
                  << ", linkId=" << linkId
                  << ", client=" << (bridgeClientAddr.empty() ? "<unbound>" : bridgeClientAddr)
                  << ", bytes=" << byteCount
                  << ", hex=" << bytesToHex(bytes);

        if (!accepted) {
            LOG_DEBUG << "[S7][Direct] Drop DTU payload because bridge transport is not open: deviceId="
                      << deviceId << ", linkId=" << linkId
                      << ", bytes=" << byteCount;
        }
    }

    void flushBridgeSession(int deviceId) {
        std::deque<std::vector<uint8_t>> pendingToDtu;
        std::string bridgeClientAddr;
        int bridgeLinkId = 0;

        {
            auto runtime = findRuntimeLocked(deviceId);
            if (!runtime) {
                return;
            }

            std::lock_guard runtimeLock(runtime->mutex);
            if (!isBridgeReadyLocked(*runtime)) {
                return;
            }
            bridgeClientAddr = runtime->bridgeClientAddr;
            bridgeLinkId = runtime->bridgeLinkId;
            pendingToDtu.swap(runtime->pendingBridgeToDtu);
        }

        const std::size_t queuedToDtu = pendingToDtu.size();

        auto sendPending = [&](std::deque<std::vector<uint8_t>>& queue, auto&& sender) {
            std::deque<std::vector<uint8_t>> remaining;
            while (!queue.empty()) {
                auto frame = std::move(queue.front());
                queue.pop_front();
                if (!sender(frame)) {
                    remaining.push_back(std::move(frame));
                    break;
                }
            }
            while (!queue.empty()) {
                remaining.push_back(std::move(queue.front()));
                queue.pop_front();
            }
            return remaining;
        };

        auto remainingToDtu = sendPending(pendingToDtu, [&](const std::vector<uint8_t>& frame) {
            LOG_DEBUG << "[S7][Adapter] TX DTU client=" << bridgeClientAddr
                      << ", linkId=" << bridgeLinkId
                      << ", bytes=" << frame.size()
                      << ", hex=" << bytesToHex(frame);
            return LinkTransportFacade::instance().sendToClient(bridgeLinkId, bridgeClientAddr, bytesToString(frame));
        });

        const std::size_t flushedToDtu = queuedToDtu - remainingToDtu.size();
        if (flushedToDtu > 0 || !remainingToDtu.empty()) {
            LOG_INFO << "[S7][Adapter] Flushed bridge session for device " << deviceId
                     << ", dtu=" << flushedToDtu << "/" << queuedToDtu
                     << ", linkId=" << bridgeLinkId
                     << ", client=" << bridgeClientAddr;
        }

        if (!remainingToDtu.empty()) {
            auto runtime = findRuntimeLocked(deviceId);
            if (runtime && runtime->bridgeMode) {
                std::lock_guard runtimeLock(runtime->mutex);
                prependQueue(runtime->pendingBridgeToDtu, remainingToDtu);
            }
        }
    }

    void pollDevice(const std::shared_ptr<S7DeviceRuntime>& runtime, std::vector<ParsedFrameResult>& results) {
        if (!runtime) {
            return;
        }

        int deviceId = 0;
        int linkId = 0;
        bool bridgeMode = false;
        bool bridgeBound = false;
        std::vector<S7AreaDefinition> areas;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            deviceId = runtime->deviceId;
            linkId = runtime->linkId;
            bridgeMode = runtime->bridgeMode;
            bridgeBound = runtime->bridgeBound;
            areas = runtime->areas;
        }

        LOG_TRACE << "[S7][Adapter] Poll device " << deviceId
                  << ", areas=" << areas.size()
                  << ", bridge=" << (bridgeMode ? "yes" : "no")
                  << ", bound=" << (bridgeBound ? "yes" : "no");
        for (const auto& area : areas) {
            if (area.size <= 0) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(area.size), 0);
            int rc = 0;
            {
                std::lock_guard clientLock(runtime->clientMutex);
                rc = readArea(*runtime, area, buffer);
            }
            if (rc != 0) {
                bool shouldResetClient = false;
                {
                    std::lock_guard runtimeLock(runtime->mutex);
                    if (runtime->bridgeMode && !runtime->bridgeBound) {
                        LOG_DEBUG << "[S7][Adapter] Read area pending bridge bind: deviceId="
                                  << runtime->deviceId
                                  << ", area=" << area.id
                                  << ", rc=" << rc;
                        if (runtime->bridgeDiscoveryInFlight) {
                            LOG_DEBUG << "[S7][Adapter] Clearing discovery probe state after read failure: deviceId="
                                      << runtime->deviceId
                                      << ", area=" << area.id;
                        }
                        runtime->bridgeDiscoveryInFlight = false;
                        runtime->bridgeDiscoveryStartedAt = {};
                    } else {
                        LOG_WARN << "[S7][Adapter] Read area failed: deviceId=" << runtime->deviceId
                                 << ", area=" << area.id
                                 << ", rc=" << rc
                                 << " (" << explainClientRc(rc) << ")"
                                 << ", bridge=" << (runtime->bridgeMode ? "yes" : "no")
                                 << ", bound=" << (runtime->bridgeBound ? "yes" : "no");
                    }
                    shouldResetClient = !runtime->bridgeMode || runtime->bridgeBound;
                }
                if (shouldResetClient) {
                    std::lock_guard clientLock(runtime->clientMutex);
                    runtime->resetClient();
                }
                break;
            }

            LOG_TRACE << "[S7][Adapter] Read area OK: deviceId=" << deviceId
                      << ", area=" << area.id
                      << ", bytes=" << buffer.size();

            results.push_back(buildReadResult(deviceId, linkId, area, buffer));
        }
    }

    int readArea(const S7DeviceRuntime& runtime, const S7AreaDefinition& area,
                 std::vector<uint8_t>& buffer) const {
        if (!runtime.client) {
            return -1;
        }
        const int dbNumber = area.area == "V" ? 1 : area.dbNumber;
        return runtime.client->readArea(areaToCode(area.area), dbNumber, area.start,
            transferAmount(area.area, buffer.size()), areaWordLen(area.area), buffer.data());
    }

    int writeArea(const S7DeviceRuntime& runtime, const std::string& area, int dbNumber,
                  int start, const std::vector<uint8_t>& buffer) const {
        if (!runtime.client) {
            return -1;
        }
        auto writable = buffer;
        const int resolvedDbNumber = area == "V" && dbNumber <= 0 ? 1 : dbNumber;
        return runtime.client->writeArea(areaToCode(area), resolvedDbNumber, start,
            transferAmount(area, writable.size()), areaWordLen(area), writable.data());
    }

    mutable std::mutex devicesMutex_;
    std::unordered_map<int, std::shared_ptr<S7DeviceRuntime>> devices_;
    std::unique_ptr<DtuRegistry> bridgeRegistry_;
    std::unique_ptr<DtuSessionManager> bridgeSessionManager_;
    std::unique_ptr<RegistrationNormalizer> bridgeNormalizer_;
};

}  // namespace s7
