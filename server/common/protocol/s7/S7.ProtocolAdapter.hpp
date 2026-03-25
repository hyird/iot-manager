#pragma once

#include "S7.DtuRegistry.hpp"
#include "S7.DtuSessionManager.hpp"
#include "S7.LocalTcpProxy.hpp"
#include "S7.RegistrationNormalizer.hpp"
#include "S7.Snap7Compat.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/network/LinkTransportFacade.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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
    int deviceId = 0;
    int linkId = 0;
    std::string deviceCode;
    std::string dtuKey;
    std::string proxyClientAddr;
    std::string bridgeClientAddr;
    int bridgeLinkId = 0;
    S7ConnectionConfig connection;
    std::vector<S7AreaDefinition> areas;
    std::deque<std::vector<uint8_t>> pendingDtuToProxy;
    std::deque<std::vector<uint8_t>> pendingProxyToDtu;
    S7ClientHandle client = kS7InvalidObject;
    bool connected = false;
    bool bridgeMode = false;
    bool bridgeBound = false;
    bool bridgeDiscoveryInFlight = false;
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
        , proxyClientAddr(std::move(other.proxyClientAddr))
        , bridgeClientAddr(std::move(other.bridgeClientAddr))
        , bridgeLinkId(other.bridgeLinkId)
        , connection(std::move(other.connection))
        , areas(std::move(other.areas))
        , pendingDtuToProxy(std::move(other.pendingDtuToProxy))
        , pendingProxyToDtu(std::move(other.pendingProxyToDtu))
        , client(std::exchange(other.client, kS7InvalidObject))
        , connected(other.connected)
        , bridgeMode(other.bridgeMode)
        , bridgeBound(other.bridgeBound)
        , bridgeDiscoveryInFlight(other.bridgeDiscoveryInFlight)
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
        proxyClientAddr = std::move(other.proxyClientAddr);
        bridgeClientAddr = std::move(other.bridgeClientAddr);
        bridgeLinkId = other.bridgeLinkId;
        connection = std::move(other.connection);
        areas = std::move(other.areas);
        pendingDtuToProxy = std::move(other.pendingDtuToProxy);
        pendingProxyToDtu = std::move(other.pendingProxyToDtu);
        client = std::exchange(other.client, kS7InvalidObject);
        connected = other.connected;
        bridgeMode = other.bridgeMode;
        bridgeBound = other.bridgeBound;
        bridgeDiscoveryInFlight = other.bridgeDiscoveryInFlight;
        bridgeDiscoveryStartedAt = other.bridgeDiscoveryStartedAt;
        lastConnectAttempt = other.lastConnectAttempt;
        lastPoll = other.lastPoll;
        other.connected = false;
        return *this;
    }

    void resetClient() {
        if (s7IsValidHandle(client)) {
            s7CliDisconnect(client);
            s7CliDestroy(client);
        } else {
            client = kS7InvalidObject;
        }
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
        proxy_.setConnectionCallback(
            [this](const std::string& clientAddr, bool connected) {
                onProxyConnectionChanged(clientAddr, connected);
            }
        );
        proxy_.setDataCallback(
            [this](const std::string& clientAddr, std::vector<uint8_t> bytes) {
                onProxyDataReceived(clientAddr, std::move(bytes));
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

        std::lock_guard lock(devicesMutex_);
        for (auto& [deviceId, runtime] : devices_) {
            if (!ensureConnected(runtime, now)) {
                continue;
            }
            if (runtime.bridgeMode) {
                LOG_TRACE << "[S7][Adapter] Poll bridge device " << deviceId
                          << " connected=" << runtime.connected
                          << " bound=" << runtime.bridgeBound
                          << " proxy=" << (runtime.proxyClientAddr.empty() ? "<pending>" : runtime.proxyClientAddr)
                          << " dtu=" << (runtime.bridgeClientAddr.empty() ? "<unbound>" : runtime.bridgeClientAddr);
            }
            if (runtime.areas.empty()) {
                continue;
            }
            if (runtime.connection.pollIntervalSec > 0 && runtime.lastPoll != std::chrono::steady_clock::time_point{} &&
                std::chrono::duration_cast<std::chrono::seconds>(now - runtime.lastPoll).count() < runtime.connection.pollIntervalSec) {
                continue;
            }
            runtime.lastPoll = now;
            pollDevice(runtime, results);
        }

        if (!results.empty() && runtimeContext_.submitParsedResults) {
            runtimeContext_.submitParsedResults(std::move(results));
        }
    }

    ProtocolAdapterMetrics getMetrics() const override {
        std::lock_guard lock(devicesMutex_);
        ProtocolAdapterMetrics metrics;
        metrics.available = true;
        metrics.stats["deviceCount"] = static_cast<Json::Int64>(devices_.size());
        Json::Int64 connectedCount = 0;
        Json::Int64 areaCount = 0;
        Json::Int64 bridgeDeviceCount = 0;
        Json::Int64 bridgeReadyCount = 0;
        for (const auto& [_, runtime] : devices_) {
            if (runtime.bridgeMode) {
                ++bridgeDeviceCount;
            }
            if (runtime.connected && (!runtime.bridgeMode || isBridgeReadyLocked(runtime.deviceId))) {
                ++connectedCount;
            }
            areaCount += static_cast<Json::Int64>(runtime.areas.size());
            if (runtime.bridgeMode && isBridgeReadyLocked(runtime.deviceId)) {
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
        std::lock_guard lock(devicesMutex_);
        auto it = devices_.find(deviceId);
        return it != devices_.end()
            && it->second.connected
            && (!it->second.bridgeMode || isBridgeReadyLocked(deviceId));
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

            {
                std::lock_guard lock(devicesMutex_);
                auto runtime = findRuntimeLocked(deviceId);
                if (!runtime) {
                    co_return CommandResult::offline("S7 设备离线");
                }

                if (!ensureConnected(*runtime, std::chrono::steady_clock::now())) {
                    co_return CommandResult::offline("S7 设备离线");
                }
                if (runtime->bridgeMode && !runtime->bridgeBound) {
                    co_return CommandResult::offline("S7 桥接未就绪");
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
                std::lock_guard lock(devicesMutex_);
                auto runtime = findRuntimeLocked(deviceId);
                if (!runtime) {
                    failure = "S7 设备离线";
                } else {
                    if (!ensureConnected(*runtime, std::chrono::steady_clock::now())) {
                        failure = "S7 设备离线";
                    } else if (runtime->bridgeMode && !runtime->bridgeBound) {
                        failure = "S7 桥接未就绪";
                    } else {
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
                            int rc = writeArea(*runtime, areaName, dbNumber, start, bytes);
                            if (rc != 0) {
                                failure = "S7 写入失败，错误码=" + std::to_string(rc);
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
        std::ostringstream oss;
        for (uint8_t b : bytes) {
            oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
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

    static ParsedFrameResult buildReadResult(const S7DeviceRuntime& runtime,
                                             const S7AreaDefinition& area,
                                             const std::vector<uint8_t>& buffer) {
        ParsedFrameResult result;
        result.deviceId = runtime.deviceId;
        result.linkId = runtime.linkId;
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

    static std::unique_ptr<S7DeviceRuntime> buildRuntime(const DeviceCache::CachedDevice& device) {
        if (device.protocolType != Constants::PROTOCOL_S7) {
            return nullptr;
        }
        auto runtime = std::make_unique<S7DeviceRuntime>();
        runtime->deviceId = device.id;
        runtime->linkId = device.linkId;
        runtime->deviceCode = device.deviceCode.empty() ? ("s7_" + std::to_string(device.id)) : device.deviceCode;
        runtime->bridgeMode = device.linkMode == Constants::LINK_MODE_TCP_SERVER;
        if (runtime->bridgeMode) {
            runtime->dtuKey = std::to_string(device.linkId) + ":"
                + detail::makeRegistrationToken(device.registrationBytes);
            runtime->connection.host = "127.0.0.1";
        }
        const std::string plcModel = device.protocolConfig.get("plcModel", "").asString();
        runtime->connection = parseConnection(device.protocolConfig, plcModel);
        if (runtime->bridgeMode) {
            runtime->connection.host = "127.0.0.1";
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

        const bool hasBridgeDevices = bridgeRegistry_ && !bridgeRegistry_->empty();
        if (hasBridgeDevices) {
            const std::string proxyError = proxy_.start("127.0.0.1", 102);
            if (!proxyError.empty()) {
                LOG_ERROR << "[S7][Proxy] " << proxyError;
            } else {
                LOG_INFO << "[S7][Adapter] Local proxy ready on " << proxy_.listenHost()
                         << ":" << proxy_.listenPort();
            }
        } else {
            LOG_INFO << "[S7][Adapter] No bridge devices, stopping local proxy";
            proxy_.stop();
        }

        auto devices = co_await DeviceCache::instance().getDevices();
        std::unordered_map<int, S7DeviceRuntime> next;
        std::size_t bridgeCount = 0;
        for (const auto& device : devices) {
            if (device.protocolType != Constants::PROTOCOL_S7) continue;
            auto runtime = buildRuntime(device);
            if (!runtime) continue;
            if (runtime->bridgeMode) {
                ++bridgeCount;
            }
            next.emplace(runtime->deviceId, std::move(*runtime));
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

    bool ensureConnected(S7DeviceRuntime& runtime, std::chrono::steady_clock::time_point now) {
        if (runtime.connected && s7IsValidHandle(runtime.client) && s7CliGetConnected(runtime.client)) {
            return true;
        }

        LOG_DEBUG << "[S7][Adapter] Connecting device " << runtime.deviceId
                  << " to " << runtime.connection.host
                  << " mode=" << runtime.connection.mode
                  << (runtime.bridgeMode ? " (bridge)" : "");

        runtime.resetClient();
        runtime.client = s7CliCreate();
        if (!s7IsValidHandle(runtime.client)) {
            runtime.connected = false;
            runtime.proxyClientAddr.clear();
            return false;
        }

        runtime.lastConnectAttempt = now;
        std::unique_lock<std::mutex> connectLock;
        if (runtime.bridgeMode) {
            connectLock = std::unique_lock<std::mutex>(connectMutex_);
            if (runtime.proxyClientAddr.empty() || runtime.proxyClientAddr.rfind("PENDING#", 0) != 0) {
                runtime.proxyClientAddr = "PENDING#" + std::to_string(runtime.deviceId);
            }
        }

        int rc = -1;
        if (runtime.connection.mode == "TSAP") {
            if (s7CliSetConnectionParams(
                    runtime.client,
                    runtime.connection.host.c_str(),
                    runtime.connection.localTSAP,
                    runtime.connection.remoteTSAP) != 0) {
                LOG_WARN << "[S7][Adapter] Set TSAP params failed for device " << runtime.deviceId
                         << ", host=" << runtime.connection.host;
                runtime.resetClient();
                return false;
            }
            rc = s7CliConnect(runtime.client);
        } else {
            if (s7CliSetConnectionType(runtime.client, runtime.connection.connectionType) != 0) {
                LOG_WARN << "[S7][Adapter] Set connection type failed for device " << runtime.deviceId
                         << ", type=" << runtime.connection.connectionType;
                runtime.resetClient();
                return false;
            }
            rc = s7CliConnectTo(runtime.client, runtime.connection.host.c_str(), runtime.connection.rack, runtime.connection.slot);
        }
        runtime.connected = (rc == 0) && s7CliGetConnected(runtime.client);
        if (!runtime.connected) {
            LOG_WARN << "[S7][Adapter] Connect failed for device " << runtime.deviceId
                     << ", rc=" << rc
                     << ", host=" << runtime.connection.host
                     << ", mode=" << runtime.connection.mode;
            runtime.resetClient();
            if (runtime.bridgeMode) {
                runtime.proxyClientAddr.clear();
            }
            return false;
        }

        if (runtime.bridgeMode) {
            std::uint16_t localPort = 0;
            if (s7CliGetParam(runtime.client, p_u16_LocalPort, &localPort) == 0 && localPort != 0) {
                const std::string proxyAddr = "127.0.0.1:" + std::to_string(localPort);
                if (runtime.proxyClientAddr.empty() || runtime.proxyClientAddr.rfind("PENDING#", 0) == 0) {
                    runtime.proxyClientAddr = proxyAddr;
                }
            }
        }
        LOG_INFO << "[S7][Adapter] Connected device " << runtime.deviceId
                 << ", linkId=" << runtime.linkId
                 << ", host=" << runtime.connection.host
                 << ", mode=" << runtime.connection.mode
                 << ", bridge=" << (runtime.bridgeMode ? "yes" : "no")
                 << ", proxy=" << (runtime.proxyClientAddr.empty() ? "<pending>" : runtime.proxyClientAddr);
        return runtime.connected;
    }

    S7DeviceRuntime* findRuntimeLocked(int deviceId) {
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    const S7DeviceRuntime* findRuntimeLocked(int deviceId) const {
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    S7DeviceRuntime* findRuntimeByProxyAddrLocked(const std::string& clientAddr) {
        for (auto& [deviceId, runtime] : devices_) {
            (void)deviceId;
            if (runtime.proxyClientAddr == clientAddr) {
                return &runtime;
            }
        }
        return nullptr;
    }

    S7DeviceRuntime* findPendingRuntimeLocked() {
        for (auto& [deviceId, runtime] : devices_) {
            (void)deviceId;
            if (runtime.bridgeMode && runtime.proxyClientAddr.rfind("PENDING#", 0) == 0) {
                return &runtime;
            }
        }
        return nullptr;
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

    bool isBridgeReadyLocked(int deviceId) const {
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return false;
        }
        const auto& runtime = it->second;
        return runtime.bridgeMode
            && runtime.connected
            && runtime.bridgeBound
            && !runtime.proxyClientAddr.empty()
            && runtime.proxyClientAddr.rfind("PENDING#", 0) != 0
            && !runtime.bridgeClientAddr.empty()
            && runtime.bridgeLinkId > 0;
    }

    void attachBridgeSession(int deviceId, int linkId, const std::string& clientAddr, const std::string& dtuKey) {
        bool shouldFlush = false;
        bool shouldLog = false;
        {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (!runtime || !runtime->bridgeMode) {
                return;
            }

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
            shouldFlush = runtime->connected && !runtime->proxyClientAddr.empty()
                && runtime->proxyClientAddr.rfind("PENDING#", 0) != 0;
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
        std::string proxyClientAddr;
        {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (!runtime) {
                return;
            }

            proxyClientAddr = runtime->proxyClientAddr;
            LOG_INFO << "[S7][Adapter] Closing bridge runtime: deviceId=" << deviceId
                     << ", proxy=" << (proxyClientAddr.empty() ? "<pending>" : proxyClientAddr);
            runtime->proxyClientAddr.clear();
            runtime->bridgeBound = false;
            runtime->bridgeDiscoveryInFlight = false;
            runtime->bridgeDiscoveryStartedAt = {};
            runtime->bridgeLinkId = 0;
            runtime->bridgeClientAddr.clear();
            runtime->pendingDtuToProxy.clear();
            runtime->pendingProxyToDtu.clear();
            runtime->connected = false;
            runtime->resetClient();
        }

        if (!proxyClientAddr.empty() && proxyClientAddr.rfind("PENDING#", 0) != 0) {
            proxy_.disconnectClient(proxyClientAddr);
        }
    }

    void forwardDtuPayload(int deviceId, std::vector<uint8_t> bytes) {
        if (bytes.empty()) {
            return;
        }

        const std::size_t byteCount = bytes.size();
        std::string proxyClientAddr;
        {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (!runtime || !runtime->bridgeMode) {
                return;
            }

            if (!runtime->connected || runtime->proxyClientAddr.empty()
                || runtime->proxyClientAddr.rfind("PENDING#", 0) == 0) {
                runtime->pendingDtuToProxy.push_back(std::move(bytes));
                LOG_DEBUG << "[S7][Adapter] Queue DTU->proxy payload: deviceId=" << deviceId
                          << ", linkId=" << runtime->linkId
                          << ", bytes=" << byteCount
                          << ", pendingToProxy=" << runtime->pendingDtuToProxy.size();
                return;
            }

            proxyClientAddr = runtime->proxyClientAddr;
        }

        if (!proxy_.sendToClient(proxyClientAddr, bytes)) {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (runtime && runtime->bridgeMode) {
                runtime->pendingDtuToProxy.push_back(std::move(bytes));
                LOG_WARN << "[S7][Adapter] Failed to send DTU->proxy payload, queued for retry: deviceId="
                         << deviceId << ", proxy=" << proxyClientAddr
                         << ", bytes=" << byteCount
                         << ", pendingToProxy=" << runtime->pendingDtuToProxy.size();
            }
        }
    }

    void onProxyConnectionChanged(const std::string& clientAddr, bool connected) {
        int deviceId = 0;
        bool shouldFlush = false;

        {
            std::lock_guard lock(devicesMutex_);
            S7DeviceRuntime* runtime = findRuntimeByProxyAddrLocked(clientAddr);
            if (!runtime && connected) {
                runtime = findPendingRuntimeLocked();
            }

            if (!runtime || !runtime->bridgeMode) {
                LOG_DEBUG << "[S7][Proxy] Ignored connection state change for " << clientAddr
                          << ", connected=" << connected;
                return;
            }

            deviceId = runtime->deviceId;
            if (connected) {
                runtime->proxyClientAddr = clientAddr;
                runtime->connected = true;
                shouldFlush = runtime->bridgeBound && !runtime->bridgeClientAddr.empty()
                    && runtime->bridgeLinkId > 0;
            } else {
                runtime->connected = false;
                runtime->proxyClientAddr.clear();
                runtime->bridgeDiscoveryInFlight = false;
                runtime->bridgeDiscoveryStartedAt = {};
                runtime->pendingDtuToProxy.clear();
                runtime->pendingProxyToDtu.clear();
            }
        }

        LOG_INFO << "[S7][Proxy] " << (connected ? "Connected" : "Disconnected")
                 << " proxy client " << clientAddr
                 << ", deviceId=" << deviceId;

        if (connected && shouldFlush) {
            LOG_DEBUG << "[S7][Proxy] Proxy client ready, flushing pending payloads for device "
                      << deviceId;
            flushBridgeSession(deviceId);
        }
    }

    void onProxyDataReceived(const std::string& clientAddr, std::vector<uint8_t> bytes) {
        int deviceId = 0;
        int requestLinkId = 0;
        std::string bridgeClientAddr;
        int bridgeLinkId = 0;
        bool shouldBroadcast = false;
        std::set<std::string> excludeAddrs;
        std::string payload = bytesToString(bytes);

        {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeByProxyAddrLocked(clientAddr);
            if (!runtime || !runtime->bridgeMode) {
                LOG_DEBUG << "[S7][Proxy] Dropped proxy payload from unknown client " << clientAddr
                          << ", bytes=" << bytes.size();
                return;
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
                    if (bridgeSessionManager_) {
                        for (const auto& session : bridgeSessionManager_->listSessions()) {
                            if (session.linkId == runtime->linkId
                                && session.bindState == SessionBindState::Bound
                                && !session.clientAddr.empty()) {
                                excludeAddrs.insert(session.clientAddr);
                            }
                        }
                    }
                } else {
                    runtime->pendingProxyToDtu.push_back(std::move(bytes));
                    LOG_DEBUG << "[S7][Proxy] Queue proxy->DTU payload while discovery is in flight: deviceId="
                              << deviceId
                              << ", proxy=" << clientAddr
                              << ", bytes=" << runtime->pendingProxyToDtu.back().size()
                              << ", pendingToDtu=" << runtime->pendingProxyToDtu.size();
                    return;
                }
            }
        }

        if (shouldBroadcast) {
            const std::size_t byteCount = bytes.size();
            const bool sent = excludeAddrs.empty()
                ? LinkTransportFacade::instance().sendData(requestLinkId, payload)
                : LinkTransportFacade::instance().sendDataExcluding(requestLinkId, payload, excludeAddrs);

            if (sent) {
                LOG_INFO << "[S7][Proxy] Broadcast discovery probe: deviceId=" << deviceId
                         << ", linkId=" << requestLinkId
                         << ", proxy=" << clientAddr
                         << ", bytes=" << byteCount
                         << ", excluded=" << excludeAddrs.size();
                return;
            }

            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (runtime && runtime->bridgeMode) {
                runtime->pendingProxyToDtu.push_back(std::move(bytes));
                runtime->bridgeDiscoveryInFlight = false;
                runtime->bridgeDiscoveryStartedAt = {};
                LOG_WARN << "[S7][Proxy] Failed to forward proxy->DTU payload, queued for retry: deviceId="
                         << deviceId << ", linkId=" << requestLinkId
                         << ", proxy=" << clientAddr
                         << ", bytes=" << byteCount
                         << ", pendingToDtu=" << runtime->pendingProxyToDtu.size();
            }
            return;
        }

        const std::size_t byteCount = bytes.size();
        if (!LinkTransportFacade::instance().sendToClient(bridgeLinkId, bridgeClientAddr, payload)) {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (runtime && runtime->bridgeMode) {
                runtime->pendingProxyToDtu.push_back(std::move(bytes));
                LOG_WARN << "[S7][Proxy] Failed to forward proxy->DTU payload, queued for retry: deviceId="
                         << deviceId << ", linkId=" << bridgeLinkId
                         << ", client=" << bridgeClientAddr
                         << ", bytes=" << byteCount
                         << ", pendingToDtu=" << runtime->pendingProxyToDtu.size();
            }
        }
    }

    void flushBridgeSession(int deviceId) {
        std::deque<std::vector<uint8_t>> pendingToProxy;
        std::deque<std::vector<uint8_t>> pendingToDtu;
        std::string proxyClientAddr;
        std::string bridgeClientAddr;
        int bridgeLinkId = 0;

        {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (!runtime || !isBridgeReadyLocked(deviceId)) {
                return;
            }

            proxyClientAddr = runtime->proxyClientAddr;
            bridgeClientAddr = runtime->bridgeClientAddr;
            bridgeLinkId = runtime->bridgeLinkId;
            pendingToProxy.swap(runtime->pendingDtuToProxy);
            pendingToDtu.swap(runtime->pendingProxyToDtu);
        }

        const std::size_t queuedToProxy = pendingToProxy.size();
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

        auto remainingToProxy = sendPending(pendingToProxy, [&](const std::vector<uint8_t>& frame) {
            return proxy_.sendToClient(proxyClientAddr, frame);
        });
        auto remainingToDtu = sendPending(pendingToDtu, [&](const std::vector<uint8_t>& frame) {
            return LinkTransportFacade::instance().sendToClient(bridgeLinkId, bridgeClientAddr, bytesToString(frame));
        });

        const std::size_t flushedToProxy = queuedToProxy - remainingToProxy.size();
        const std::size_t flushedToDtu = queuedToDtu - remainingToDtu.size();
        if (flushedToProxy > 0 || flushedToDtu > 0 || !remainingToProxy.empty() || !remainingToDtu.empty()) {
            LOG_INFO << "[S7][Adapter] Flushed bridge session for device " << deviceId
                     << ": proxy=" << flushedToProxy << "/" << queuedToProxy
                     << ", dtu=" << flushedToDtu << "/" << queuedToDtu
                     << ", linkId=" << bridgeLinkId
                     << ", client=" << bridgeClientAddr;
        }

        if (!remainingToProxy.empty() || !remainingToDtu.empty()) {
            std::lock_guard lock(devicesMutex_);
            auto* runtime = findRuntimeLocked(deviceId);
            if (runtime && runtime->bridgeMode) {
                prependQueue(runtime->pendingDtuToProxy, remainingToProxy);
                prependQueue(runtime->pendingProxyToDtu, remainingToDtu);
            }
        }
    }

    void pollDevice(S7DeviceRuntime& runtime, std::vector<ParsedFrameResult>& results) {
        LOG_TRACE << "[S7][Adapter] Poll device " << runtime.deviceId
                  << ", areas=" << runtime.areas.size()
                  << ", bridge=" << (runtime.bridgeMode ? "yes" : "no")
                  << ", bound=" << (runtime.bridgeBound ? "yes" : "no");
        for (const auto& area : runtime.areas) {
            if (area.size <= 0) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(area.size), 0);
            int rc = readArea(runtime, area, buffer);
            if (rc != 0) {
                if (runtime.bridgeMode && !runtime.bridgeBound) {
                    LOG_DEBUG << "[S7][Adapter] Read area pending bridge bind: deviceId="
                              << runtime.deviceId
                              << ", area=" << area.id
                              << ", rc=" << rc;
                    if (runtime.bridgeDiscoveryInFlight) {
                        LOG_DEBUG << "[S7][Adapter] Clearing discovery probe state after read failure: deviceId="
                                  << runtime.deviceId
                                  << ", area=" << area.id;
                    }
                    runtime.bridgeDiscoveryInFlight = false;
                    runtime.bridgeDiscoveryStartedAt = {};
                } else {
                    LOG_WARN << "[S7][Adapter] Read area failed: deviceId=" << runtime.deviceId
                             << ", area=" << area.id
                             << ", rc=" << rc
                             << ", bridge=" << (runtime.bridgeMode ? "yes" : "no")
                             << ", bound=" << (runtime.bridgeBound ? "yes" : "no");
                }
                if (!runtime.bridgeMode || runtime.bridgeBound) {
                    runtime.resetClient();
                }
                break;
            }

            LOG_TRACE << "[S7][Adapter] Read area OK: deviceId=" << runtime.deviceId
                      << ", area=" << area.id
                      << ", bytes=" << buffer.size();

            results.push_back(buildReadResult(runtime, area, buffer));
        }
    }

    int readArea(const S7DeviceRuntime& runtime, const S7AreaDefinition& area,
                 std::vector<uint8_t>& buffer) const {
        if (!s7IsValidHandle(runtime.client)) {
            return -1;
        }
        const int dbNumber = area.area == "V" ? 1 : area.dbNumber;
        return s7CliReadArea(runtime.client, areaToCode(area.area), dbNumber, area.start,
            transferAmount(area.area, buffer.size()), areaWordLen(area.area), buffer.data());
    }

    int writeArea(const S7DeviceRuntime& runtime, const std::string& area, int dbNumber,
                  int start, const std::vector<uint8_t>& buffer) const {
        if (!s7IsValidHandle(runtime.client)) {
            return -1;
        }
        auto writable = buffer;
        const int resolvedDbNumber = area == "V" && dbNumber <= 0 ? 1 : dbNumber;
        return s7CliWriteArea(runtime.client, areaToCode(area), resolvedDbNumber, start,
            transferAmount(area, writable.size()), areaWordLen(area), writable.data());
    }

    mutable std::mutex devicesMutex_;
    mutable std::mutex connectMutex_;
    std::unordered_map<int, S7DeviceRuntime> devices_;
    std::unique_ptr<DtuRegistry> bridgeRegistry_;
    std::unique_ptr<DtuSessionManager> bridgeSessionManager_;
    std::unique_ptr<RegistrationNormalizer> bridgeNormalizer_;
    S7LocalTcpProxy proxy_;
};

}  // namespace s7
