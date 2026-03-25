#pragma once

#include "S7.Snap7Compat.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
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
    S7ConnectionConfig connection;
    std::vector<S7AreaDefinition> areas;
    S7ClientHandle client = kS7InvalidObject;
    bool connected = false;
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
        , connection(std::move(other.connection))
        , areas(std::move(other.areas))
        , client(std::exchange(other.client, kS7InvalidObject))
        , connected(other.connected)
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
        connection = std::move(other.connection);
        areas = std::move(other.areas);
        client = std::exchange(other.client, kS7InvalidObject);
        connected = other.connected;
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
        : ProtocolAdapter(std::move(runtimeContext)) {}

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

    void onConnectionChanged(int, const std::string&, bool) override {}

    void onDataReceived(int, const std::string&, std::vector<uint8_t>) override {}

    void onMaintenanceTick() override {
        std::vector<ParsedFrameResult> results;
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(devicesMutex_);
        for (auto& [deviceId, runtime] : devices_) {
            if (!ensureConnected(runtime, now)) {
                continue;
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
        for (const auto& [_, runtime] : devices_) {
            if (runtime.connected) {
                ++connectedCount;
            }
            areaCount += static_cast<Json::Int64>(runtime.areas.size());
        }
        metrics.stats["connectedCount"] = connectedCount;
        metrics.stats["areaCount"] = areaCount;
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
        return it != devices_.end() && it->second.connected;
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

            auto runtime = buildRuntime(*deviceOpt);
            if (!runtime) {
                co_return CommandResult::error("S7 设备配置无效");
            }

            if (!ensureConnected(*runtime, std::chrono::steady_clock::now())) {
                co_return CommandResult::offline("S7 设备离线");
            }

            if (!req.elements.isArray() || req.elements.empty()) {
                co_return CommandResult::error("S7 指令缺少 elements");
            }

            int64_t downCommandId = co_await savePendingCommand(
                runtime->deviceId, runtime->linkId, Constants::PROTOCOL_S7,
                req.funcCode, "S7 写入", "", req.userId, req.elements);

            std::string failure;
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

            if (!failure.empty()) {
                co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SEND_FAILED", failure);
                co_return CommandResult::sendFailed(failure);
            }

            co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SUCCESS", "S7 写入成功");
            if (runtimeContext_.notifyCommandCompletion) {
                runtimeContext_.notifyCommandCompletion(req.deviceCode.empty() ? std::to_string(req.deviceId) : req.deviceCode,
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
        connection.mode = preset.mode;
        connection.rack = preset.rack;
        connection.slot = preset.slot;
        connection.localTSAP = preset.localTSAP;
        connection.remoteTSAP = preset.remoteTSAP;

        if (config.isMember("connection") && config["connection"].isObject()) {
            const auto& conn = config["connection"];
            connection.host = conn.get("host", "").asString();
            connection.connectionType = toUpper(conn.get("connectionType", "PG").asString());
            if (conn.isMember("mode")) {
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
        const std::string plcModel = device.protocolConfig.get("plcModel", "").asString();
        runtime->connection = parseConnection(device.protocolConfig, plcModel);
        if (!device.linkIp.empty()) {
            runtime->connection.host = device.linkIp;
        }
        if (runtime->connection.host.empty()) {
            return nullptr;
        }
        runtime->areas = parseAreas(device.protocolConfig);
        return runtime;
    }

    Task<> refreshDevices() {
        auto devices = co_await DeviceCache::instance().getDevices();
        std::unordered_map<int, S7DeviceRuntime> next;
        for (const auto& device : devices) {
            if (device.protocolType != Constants::PROTOCOL_S7) continue;
            auto runtime = buildRuntime(device);
            if (!runtime) continue;
            next.emplace(runtime->deviceId, std::move(*runtime));
        }

        std::lock_guard lock(devicesMutex_);
        devices_ = std::move(next);
        co_return;
    }

    bool ensureConnected(S7DeviceRuntime& runtime, std::chrono::steady_clock::time_point now) {
        if (runtime.connected && s7IsValidHandle(runtime.client) && s7CliGetConnected(runtime.client)) {
            return true;
        }

        runtime.resetClient();
        runtime.client = s7CliCreate();
        if (!s7IsValidHandle(runtime.client)) {
            runtime.connected = false;
            return false;
        }

        runtime.lastConnectAttempt = now;
        int rc = -1;
        if (runtime.connection.mode == "TSAP") {
            if (s7CliSetConnectionParams(
                    runtime.client,
                    runtime.connection.host.c_str(),
                    runtime.connection.localTSAP,
                    runtime.connection.remoteTSAP) != 0) {
                runtime.resetClient();
                return false;
            }
            rc = s7CliConnect(runtime.client);
        } else {
            if (s7CliSetConnectionType(runtime.client, runtime.connection.connectionType) != 0) {
                runtime.resetClient();
                return false;
            }
            rc = s7CliConnectTo(runtime.client, runtime.connection.host.c_str(), runtime.connection.rack, runtime.connection.slot);
        }
        runtime.connected = (rc == 0) && s7CliGetConnected(runtime.client);
        if (!runtime.connected) {
            runtime.resetClient();
        }
        return runtime.connected;
    }

    void pollDevice(S7DeviceRuntime& runtime, std::vector<ParsedFrameResult>& results) {
        for (const auto& area : runtime.areas) {
            if (area.size <= 0) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(area.size), 0);
            int rc = readArea(runtime, area, buffer);
            if (rc != 0) {
                runtime.resetClient();
                break;
            }

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
    std::unordered_map<int, S7DeviceRuntime> devices_;
};

}  // namespace s7
