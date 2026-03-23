#pragma once

#include "S7.Snap7Compat.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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
    int dbNumber = 0;
    int start = 0;
    int size = 1;
    bool writable = false;
    std::string remark;
};

struct S7ConnectionConfig {
    std::string host;  // 解析后的连接目标，优先取链路 IP，兼容旧配置
    int rack = 0;
    int slot = 1;
    int pollIntervalSec = 5;
    std::string connectionType = "PG";
};

struct S7DeviceRuntime {
    int deviceId = 0;
    int linkId = 0;
    std::string deviceCode;
    S7ConnectionConfig connection;
    std::vector<S7AreaDefinition> areas;
    S7Object client = nullptr;
    bool connected = false;
    std::chrono::steady_clock::time_point lastConnectAttempt{};
    std::chrono::steady_clock::time_point lastPoll{};
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

    static int areaToCode(const std::string& area) {
        const std::string upper = toUpper(area);
        if (upper == "DB") return S7AreaDB;
        if (upper == "MK") return S7AreaMK;
        if (upper == "PA") return S7AreaPA;
        if (upper == "PE") return S7AreaPE;
        if (upper == "CT") return S7AreaCT;
        if (upper == "TM") return S7AreaTM;
        return S7AreaDB;
    }

    static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
        std::ostringstream oss;
        for (uint8_t b : bytes) {
            oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        return oss.str();
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

    static S7ConnectionConfig parseConnection(const Json::Value& config) {
        S7ConnectionConfig connection;
        if (config.isMember("connection") && config["connection"].isObject()) {
            const auto& conn = config["connection"];
            connection.host = conn.get("host", "").asString();
            connection.rack = conn.get("rack", 0).asInt();
            connection.slot = conn.get("slot", 1).asInt();
            connection.connectionType = conn.get("connectionType", "PG").asString();
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
        for (const auto& area : *areas) {
            if (!area.isObject()) continue;
            S7AreaDefinition def;
            def.id = area.get("id", "").asString();
            def.name = area.get("name", def.id).asString();
            def.area = toUpper(area.get("area", "DB").asString());
            def.dbNumber = area.get("dbNumber", 0).asInt();
            def.start = area.get("start", 0).asInt();
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
        runtime->connection = parseConnection(device.protocolConfig);
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
        if (runtime.connected && runtime.client && Cli_GetConnected(runtime.client)) {
            return true;
        }

        if (runtime.client) {
            Cli_Disconnect(runtime.client);
            Cli_Destroy(runtime.client);
            runtime.client = nullptr;
        }
        runtime.client = Cli_Create();
        if (!runtime.client) {
            runtime.connected = false;
            return false;
        }

        runtime.lastConnectAttempt = now;
        int rc = Cli_ConnectTo(runtime.client, runtime.connection.host.c_str(), runtime.connection.rack, runtime.connection.slot);
        runtime.connected = (rc == 0) && Cli_GetConnected(runtime.client);
        return runtime.connected;
    }

    void pollDevice(S7DeviceRuntime& runtime, std::vector<ParsedFrameResult>& results) {
        for (const auto& area : runtime.areas) {
            if (!area.writable && area.size <= 0) continue;

            std::vector<uint8_t> buffer(static_cast<size_t>(area.size), 0);
            int rc = readArea(runtime, area.area, area.dbNumber, area.start, buffer);
            if (rc != 0) {
                runtime.connected = false;
                if (runtime.client) {
                    Cli_Disconnect(runtime.client);
                }
                break;
            }

            ParsedFrameResult result;
            result.deviceId = runtime.deviceId;
            result.linkId = runtime.linkId;
            result.protocol = Constants::PROTOCOL_S7;
            result.funcCode = area.id;
            result.reportTime = "";
            result.data["area"] = area.area;
            result.data["dbNumber"] = area.dbNumber;
            result.data["start"] = area.start;
            result.data["size"] = area.size;
            result.data["name"] = area.name;
            result.data["remark"] = area.remark;
            result.data["hex"] = bytesToHex(buffer);
            results.push_back(std::move(result));
        }
    }

    int readArea(const S7DeviceRuntime& runtime, const std::string& area, int dbNumber,
                 int start, std::vector<uint8_t>& buffer) const {
            if (!runtime.client) {
                return -1;
            }
        return Cli_ReadArea(runtime.client, areaToCode(area), dbNumber, start,
            static_cast<int>(buffer.size()), S7WLByte, buffer.data());
    }

    int writeArea(const S7DeviceRuntime& runtime, const std::string& area, int dbNumber,
                  int start, const std::vector<uint8_t>& buffer) const {
        if (!runtime.client) {
            return -1;
        }
        auto writable = buffer;
        return Cli_WriteArea(runtime.client, areaToCode(area), dbNumber, start,
            static_cast<int>(writable.size()), S7WLByte, writable.data());
    }

    mutable std::mutex devicesMutex_;
    std::unordered_map<int, S7DeviceRuntime> devices_;
};

}  // namespace s7
