#pragma once

#include "common/edgenode/AgentProtocol.hpp"
#include "common/protocol/FrameResult.hpp"
#include "common/protocol/s7/S7.Client.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class EdgeNodeS7Poller {
public:
    using DataCallback = std::function<void(std::vector<ParsedFrameResult>&&)>;

    void setDataCallback(DataCallback cb) {
        dataCallback_ = std::move(cb);
    }

    void loadConfig(const std::vector<agent::DeviceEndpoint>& s7Endpoints) {
        std::lock_guard lock(mutex_);
        devices_.clear();

        for (const auto& ep : s7Endpoints) {
            if (ep.transport != "ethernet" || ep.ip.empty()) {
                continue;
            }

            for (const auto& dev : ep.devices) {
                DeviceRuntime runtime;
                runtime.deviceId = dev.id;
                runtime.deviceName = dev.name;
                runtime.endpointId = ep.id;
                runtime.host = ep.ip;
                runtime.port = static_cast<std::uint16_t>(ep.port > 0 ? ep.port : 102);
                runtime.connection = parseConnection(dev.protocolConfig);
                runtime.areas = parseAreas(dev.protocolConfig);
                runtime.nextPollAt = std::chrono::steady_clock::now();

                if (runtime.deviceId <= 0 || runtime.areas.empty()) {
                    continue;
                }

                std::cout << "[EdgeNodeS7Poller] device: id=" << runtime.deviceId
                          << ", name=" << runtime.deviceName
                          << ", host=" << runtime.host << ":" << runtime.port
                          << ", mode=" << runtime.connection.mode
                          << ", areas=" << runtime.areas.size()
                          << ", interval=" << runtime.connection.pollIntervalSec << "s" << std::endl;

                devices_[runtime.deviceId] = std::move(runtime);
            }
        }

        std::cout << "[EdgeNodeS7Poller] Loaded " << devices_.size() << " S7 device(s)" << std::endl;
    }

    void tick() {
        std::vector<ParsedFrameResult> batch;
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(mutex_);
        for (auto& [deviceId, runtime] : devices_) {
            (void)deviceId;
            if (now < runtime.nextPollAt) {
                continue;
            }

            if (!ensureConnected(runtime, now)) {
                runtime.nextPollAt = now + std::chrono::milliseconds(runtime.connection.retryDelayMs);
                continue;
            }

            auto one = pollDevice(runtime);
            if (one.has_value()) {
                batch.push_back(std::move(*one));
                runtime.nextPollAt = now + std::chrono::seconds(runtime.connection.pollIntervalSec);
            } else {
                // 失败后按 retryDelay 快速重试；失败分支里已经断开连接
                runtime.nextPollAt = now + std::chrono::milliseconds(runtime.connection.retryDelayMs);
            }
        }

        if (!batch.empty() && dataCallback_) {
            dataCallback_(std::move(batch));
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        for (auto& [_, runtime] : devices_) {
            disconnect(runtime);
        }
        devices_.clear();
    }

private:
    static constexpr const char* FUNC_READ = "S7_READ";

    struct S7ConnectionConfig {
        std::string mode = "RACK_SLOT";  // RACK_SLOT / TSAP
        int rack = 0;
        int slot = 1;
        std::uint16_t localTSAP = 0x0100;
        std::uint16_t remoteTSAP = 0x0100;
        std::string connectionType = "PG";  // PG / OP / S7_BASIC
        int pingTimeoutMs = s7::kDefaultTimeoutMs;
        int sendTimeoutMs = s7::kDefaultTimeoutMs;
        int recvTimeoutMs = s7::kDefaultTimeoutMs;
        int retryDelayMs = 1000;
        int pollIntervalSec = 5;
    };

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
        int decimals = -1;
        std::string remark;
    };

    struct DeviceRuntime {
        int deviceId = 0;
        std::string deviceName;
        std::string endpointId;
        std::string host;
        std::uint16_t port = 102;
        S7ConnectionConfig connection;
        std::vector<S7AreaDefinition> areas;
        std::unique_ptr<s7::Client> client;
        bool connected = false;
        std::chrono::steady_clock::time_point nextPollAt{};
        std::chrono::steady_clock::time_point lastConnectAttempt{};
    };

    static std::string toUpper(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return value;
    }

    static std::string normalizeDataType(std::string value) {
        value = toUpper(std::move(value));
        if (value == "DOUBLE") return "LREAL";
        if (value == "FLOAT32") return "FLOAT";
        if (value == "BOOL" || value == "INT8" || value == "UINT8" || value == "INT16" ||
            value == "UINT16" || value == "INT32" || value == "UINT32" ||
            value == "FLOAT" || value == "LREAL" || value == "STRING") {
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
        return std::max(1, static_cast<int>(size));
    }

    static int resolvedDbNumber(const S7AreaDefinition& area) {
        return toUpper(area.area) == "V" ? 1 : area.dbNumber;
    }

    static std::optional<std::uint16_t> parseTsapHex(const Json::Value& value) {
        if (value.isNull()) return std::nullopt;
        if (value.isInt() || value.isUInt()) {
            const unsigned int raw = value.asUInt();
            if (raw > 0xFFFF) return std::nullopt;
            return static_cast<std::uint16_t>(raw);
        }
        if (!value.isString()) return std::nullopt;

        std::string text = value.asString();
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
            return std::isspace(c) || c == '.' || c == ':' || c == '-' || c == '_';
        }), text.end());
        text = toUpper(std::move(text));
        if (text.rfind("0X", 0) == 0) {
            text.erase(0, 2);
        }
        if (text.empty() || text.size() > 4) return std::nullopt;
        if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::stoul(text, nullptr, 16));
    }

    static std::uint16_t connectionTypeToCode(std::string value) {
        value = toUpper(std::move(value));
        if (value == "OP") return s7::kConnTypeOp;
        if (value == "S7_BASIC" || value == "BASIC") return s7::kConnTypeBasic;
        return s7::kConnTypePg;
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

    static std::uint16_t readU16BE(const std::vector<std::uint8_t>& buffer) {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(buffer[0]) << 8) | buffer[1]);
    }

    static std::uint32_t readU32BE(const std::vector<std::uint8_t>& buffer) {
        return (static_cast<std::uint32_t>(buffer[0]) << 24)
            | (static_cast<std::uint32_t>(buffer[1]) << 16)
            | (static_cast<std::uint32_t>(buffer[2]) << 8)
            | static_cast<std::uint32_t>(buffer[3]);
    }

    static std::uint64_t readU64BE(const std::vector<std::uint8_t>& buffer) {
        return (static_cast<std::uint64_t>(buffer[0]) << 56)
            | (static_cast<std::uint64_t>(buffer[1]) << 48)
            | (static_cast<std::uint64_t>(buffer[2]) << 40)
            | (static_cast<std::uint64_t>(buffer[3]) << 32)
            | (static_cast<std::uint64_t>(buffer[4]) << 24)
            | (static_cast<std::uint64_t>(buffer[5]) << 16)
            | (static_cast<std::uint64_t>(buffer[6]) << 8)
            | static_cast<std::uint64_t>(buffer[7]);
    }

    static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return {};
        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) oss << ' ';
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }

    static S7ConnectionConfig parseConnection(const Json::Value& config) {
        S7ConnectionConfig connection;
        const std::string plcModel = toUpper(config.get("plcModel", "").asString());
        if (plcModel == "S7-200") {
            connection.mode = "TSAP";
            connection.localTSAP = 0x4D57;
            connection.remoteTSAP = 0x4D57;
        } else if (plcModel == "S7-300") {
            connection.mode = "RACK_SLOT";
            connection.rack = 0;
            connection.slot = 2;
        } else if (plcModel == "S7-400") {
            connection.mode = "RACK_SLOT";
            connection.rack = 0;
            connection.slot = 3;
        } else {
            connection.mode = "RACK_SLOT";
            connection.rack = 0;
            connection.slot = 1;
        }

        if (config.isMember("connection") && config["connection"].isObject()) {
            const auto& conn = config["connection"];
            const auto mode = toUpper(conn.get("mode", "").asString());
            if (plcModel == "S7-200") {
                connection.mode = "TSAP";
            } else if (mode == "TSAP") {
                connection.mode = "TSAP";
            } else if (mode == "RACK_SLOT") {
                connection.mode = "RACK_SLOT";
            }

            connection.rack = conn.get("rack", connection.rack).asInt();
            connection.slot = conn.get("slot", connection.slot).asInt();
            connection.connectionType = toUpper(conn.get("connectionType", "PG").asString());
            if (auto localTsap = parseTsapHex(conn["localTSAP"])) {
                connection.localTSAP = *localTsap;
            }
            if (auto remoteTsap = parseTsapHex(conn["remoteTSAP"])) {
                connection.remoteTSAP = *remoteTsap;
            }

            connection.pingTimeoutMs = conn.get("pingTimeout", connection.pingTimeoutMs).asInt();
            connection.sendTimeoutMs = conn.get("sendTimeout", connection.sendTimeoutMs).asInt();
            connection.recvTimeoutMs = conn.get("recvTimeout", connection.recvTimeoutMs).asInt();
            connection.retryDelayMs = conn.get("retryDelay", connection.retryDelayMs).asInt();
        }

        connection.pollIntervalSec = config.get("pollInterval", 5).asInt();
        if (connection.pollIntervalSec < 1) connection.pollIntervalSec = 1;
        if (connection.retryDelayMs < 200) connection.retryDelayMs = 200;
        if (connection.pingTimeoutMs <= 0) connection.pingTimeoutMs = s7::kDefaultTimeoutMs;
        if (connection.sendTimeoutMs <= 0) connection.sendTimeoutMs = s7::kDefaultTimeoutMs;
        if (connection.recvTimeoutMs <= 0) connection.recvTimeoutMs = s7::kDefaultTimeoutMs;
        return connection;
    }

    static std::vector<S7AreaDefinition> parseAreas(const Json::Value& config) {
        const Json::Value* areas = nullptr;
        if (config.isMember("areas") && config["areas"].isArray()) {
            areas = &config["areas"];
        } else if (config.isMember("poll") && config["poll"].isObject()
            && config["poll"].isMember("areas") && config["poll"]["areas"].isArray()) {
            areas = &config["poll"]["areas"];
        }

        std::vector<S7AreaDefinition> result;
        if (!areas) return result;

        const std::string plcModel = toUpper(config.get("plcModel", "").asString());
        const bool isS7_200 = plcModel == "S7-200";
        for (const auto& area : *areas) {
            if (!area.isObject()) continue;

            S7AreaDefinition def;
            def.id = area.get("id", "").asString();
            if (def.id.empty()) continue;

            const std::string areaType = toUpper(area.get("area", "DB").asString());
            def.name = area.get("name", def.id).asString();
            def.area = areaType;
            def.dataType = normalizeDataType(area.get("dataType", "INT16").asString());
            if (areaType == "CT" || areaType == "TM") {
                def.dataType = "UINT16";
            }
            def.dbNumber = (areaType == "V" || (isS7_200 && areaType == "DB")) ? 1 : area.get("dbNumber", 0).asInt();
            def.start = area.get("start", 0).asInt();
            def.startBit = std::clamp(area.get("startBit", 0).asInt(), 0, 7);
            def.size = std::max(1, area.get("size", 1).asInt());
            def.writable = area.get("writable", false).asBool();
            def.decimals = area.get("decimals", -1).asInt();
            def.remark = area.get("remark", "").asString();

            result.push_back(std::move(def));
        }
        return result;
    }

    static Json::Value decodeAreaValue(const S7AreaDefinition& area, const std::vector<std::uint8_t>& buffer) {
        const std::string dataType = normalizeDataType(area.dataType);

        if ((toUpper(area.area) == "CT" || toUpper(area.area) == "TM") && buffer.size() >= 2) {
            return Json::Value(static_cast<Json::UInt64>(readU16BE(buffer)));
        }

        if (dataType == "BOOL") {
            if (buffer.empty()) return Json::nullValue;
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
            float value = 0.0F;
            std::uint32_t raw = readU32BE(buffer);
            std::memcpy(&value, &raw, sizeof(value));
            return Json::Value(value);
        }
        if (dataType == "LREAL") {
            if (buffer.size() < 8) return Json::nullValue;
            double value = 0.0;
            std::uint64_t raw = readU64BE(buffer);
            std::memcpy(&value, &raw, sizeof(value));
            return Json::Value(value);
        }
        if (dataType == "STRING") {
            std::string value(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            size_t zero = value.find('\0');
            if (zero != std::string::npos) value.resize(zero);
            return Json::Value(value);
        }
        return Json::Value(bytesToHex(buffer));
    }

    static Json::Value buildReadElement(const S7AreaDefinition& area, const std::vector<std::uint8_t>& buffer) {
        Json::Value element(Json::objectValue);
        element["name"] = area.name;
        element["value"] = decodeAreaValue(area, buffer);
        element["hex"] = bytesToHex(buffer);
        element["area"] = area.area;
        element["dataType"] = normalizeDataType(area.dataType);
        element["dbNumber"] = resolvedDbNumber(area);
        element["start"] = area.start;
        element["size"] = area.size;
        if (normalizeDataType(area.dataType) == "BOOL") {
            element["startBit"] = area.startBit;
        }
        if (area.decimals >= 0 && element["value"].isNumeric()) {
            const double value = element["value"].asDouble();
            const double factor = std::pow(10.0, static_cast<double>(area.decimals));
            element["value"] = std::round(value * factor) / factor;
        }
        if (area.writable) element["writable"] = true;
        if (!area.remark.empty()) element["remark"] = area.remark;
        return element;
    }

    static ParsedFrameResult buildPollReadResult(int deviceId, Json::Value data) {
        ParsedFrameResult result;
        result.deviceId = deviceId;
        result.linkId = 0;
        result.protocol = Constants::PROTOCOL_S7;
        result.funcCode = FUNC_READ;
        result.reportTime = makeUtcNowString();

        Json::Value payload(Json::objectValue);
        payload["funcCode"] = FUNC_READ;
        payload["funcName"] = "S7采集";
        payload["direction"] = "UP";
        payload["data"] = std::move(data);
        result.data = std::move(payload);
        return result;
    }

    static bool ensureConnected(DeviceRuntime& runtime, std::chrono::steady_clock::time_point now) {
        if (runtime.connected && runtime.client && runtime.client->connected()) {
            return true;
        }
        if (runtime.lastConnectAttempt != std::chrono::steady_clock::time_point{}
            && std::chrono::duration_cast<std::chrono::milliseconds>(now - runtime.lastConnectAttempt).count()
                < runtime.connection.retryDelayMs) {
            return false;
        }

        disconnect(runtime);
        runtime.lastConnectAttempt = now;
        runtime.client = std::make_unique<s7::Client>();
        if (!runtime.client) {
            return false;
        }

        const std::int32_t pingTimeout = runtime.connection.pingTimeoutMs;
        const std::int32_t sendTimeout = runtime.connection.sendTimeoutMs;
        const std::int32_t recvTimeout = runtime.connection.recvTimeoutMs;
        runtime.client->setParam(p_i32_PingTimeout, &pingTimeout);
        runtime.client->setParam(p_i32_SendTimeout, &sendTimeout);
        runtime.client->setParam(p_i32_RecvTimeout, &recvTimeout);
        runtime.client->setParam(p_u16_RemotePort, &runtime.port);

        int rc = -1;
        if (runtime.connection.mode == "TSAP") {
            runtime.client->setConnectionParams(
                runtime.host.c_str(), runtime.connection.localTSAP, runtime.connection.remoteTSAP);
            rc = runtime.client->connect();
        } else {
            runtime.client->setConnectionType(connectionTypeToCode(runtime.connection.connectionType));
            rc = runtime.client->connectTo(runtime.host.c_str(), runtime.connection.rack, runtime.connection.slot);
        }

        runtime.connected = (rc == s7::kS7Ok) && runtime.client->connected();
        if (!runtime.connected) {
            std::cout << "[WARN] [EdgeNodeS7Poller] connect failed: device=" << runtime.deviceId
                      << ", host=" << runtime.host << ":" << runtime.port
                      << ", rc=" << rc << std::endl;
            disconnect(runtime);
        } else {
            std::cout << "[EdgeNodeS7Poller] connected: device=" << runtime.deviceId
                      << ", host=" << runtime.host << ":" << runtime.port << std::endl;
        }
        return runtime.connected;
    }

    static void disconnect(DeviceRuntime& runtime) {
        runtime.connected = false;
        if (runtime.client) {
            runtime.client->disconnect();
            runtime.client.reset();
        }
    }

    static std::optional<ParsedFrameResult> pollDevice(DeviceRuntime& runtime) {
        if (!runtime.client || !runtime.client->connected()) {
            runtime.connected = false;
            return std::nullopt;
        }

        Json::Value aggregatedData(Json::objectValue);
        bool anyAreaSucceeded = false;
        for (const auto& area : runtime.areas) {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::max(1, area.size)), 0);
            const int rc = runtime.client->readArea(
                areaToCode(area.area),
                resolvedDbNumber(area),
                area.start,
                transferAmount(area.area, buffer.size()),
                areaWordLen(area.area),
                buffer.data());

            if (rc != s7::kS7Ok) {
                std::cout << "[WARN] [EdgeNodeS7Poller] read failed, skip area and continue: device="
                          << runtime.deviceId << ", area=" << area.area
                          << ", start=" << area.start << ", size=" << area.size
                          << ", rc=" << rc << std::endl;
                continue;
            }

            aggregatedData[area.id] = buildReadElement(area, buffer);
            anyAreaSucceeded = true;
        }

        if (!anyAreaSucceeded) {
            disconnect(runtime);  // 全失败时再重连，避免一直使用坏连接
            return std::nullopt;
        }

        if (aggregatedData.empty()) {
            return std::nullopt;
        }
        return buildPollReadResult(runtime.deviceId, std::move(aggregatedData));
    }

    std::mutex mutex_;
    std::map<int, DeviceRuntime> devices_;
    DataCallback dataCallback_;
};

using AgentS7Poller = EdgeNodeS7Poller;
