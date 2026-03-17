#pragma once

#include "common/utils/JsonHelper.hpp"
#include "common/utils/Constants.hpp"

#include <functional>
#include <sstream>
#include <iomanip>
#include <zlib.h>

namespace agent {

// ==================== 二进制帧类型（首字节） ====================
//
// Agent ↔ Server 所有 WebSocket 消息统一走二进制帧：
//   0x00      shell 原始输出
//   0x01      shell zlib 压缩输出
//   0x10      控制消息 JSON 原始
//   0x11      控制消息 JSON zlib 压缩
//
// 判断逻辑：首字节 < 0x10 → shell 数据；>= 0x10 → 控制消息

inline constexpr uint8_t FRAME_SHELL_RAW  = 0x00;
inline constexpr uint8_t FRAME_SHELL_ZLIB = 0x01;
inline constexpr uint8_t FRAME_MSG_RAW    = 0x10;
inline constexpr uint8_t FRAME_MSG_ZLIB   = 0x11;

inline bool isShellFrame(uint8_t ch) { return ch < 0x10; }

// ==================== 消息类型 ====================

inline constexpr const char* MESSAGE_HELLO = "hello";
inline constexpr const char* MESSAGE_HELLO_ACK = "hello:ack";
inline constexpr const char* MESSAGE_HEARTBEAT = "heartbeat";
inline constexpr const char* MESSAGE_PING = "ping";
inline constexpr const char* MESSAGE_PONG = "pong";
inline constexpr const char* MESSAGE_CONFIG_SYNC = "config:sync";
inline constexpr const char* MESSAGE_CONFIG_APPLIED = "config:applied";
inline constexpr const char* MESSAGE_CONFIG_APPLY_FAILED = "config:apply_failed";
inline constexpr const char* MESSAGE_DEVICE_DATA = "device:data";
inline constexpr const char* MESSAGE_DEVICE_PARSED = "device:parsed";
inline constexpr const char* MESSAGE_DEVICE_PARSED_ACK = "device:parsed:ack";
inline constexpr const char* MESSAGE_DEVICE_SEND = "device:send";
inline constexpr const char* MESSAGE_ENDPOINT_CONNECTION = "endpoint:connection";
inline constexpr const char* MESSAGE_ENDPOINT_STATUS = "endpoint:status";
inline constexpr const char* MESSAGE_ENDPOINT_DISCONNECT = "endpoint:disconnect";
inline constexpr const char* MESSAGE_NETWORK_CONFIG = "network:config";
inline constexpr const char* MESSAGE_NETWORK_CONFIG_APPLIED = "network:config:applied";
inline constexpr const char* MESSAGE_NETWORK_CONFIG_FAILED = "network:config:failed";
inline constexpr const char* MESSAGE_DEVICE_COMMAND = "device:command";
inline constexpr const char* MESSAGE_DEVICE_COMMAND_RESULT = "device:command:result";
inline constexpr const char* MESSAGE_SHELL_OPEN = "shell:open";
inline constexpr const char* MESSAGE_SHELL_OPENED = "shell:opened";
inline constexpr const char* MESSAGE_SHELL_DATA = "shell:data";
inline constexpr const char* MESSAGE_SHELL_RESIZE = "shell:resize";
inline constexpr const char* MESSAGE_SHELL_CLOSE = "shell:close";
inline constexpr const char* MESSAGE_SHELL_CLOSED = "shell:closed";
inline constexpr const char* MESSAGE_ERROR = "error";

// Agent 认证密钥（运行时从配置加载，禁止硬编码）
inline std::string& agentSharedSecret() {
    static std::string secret;
    return secret;
}

inline void setAgentSharedSecret(const std::string& secret) {
    agentSharedSecret() = secret;
}

inline const std::string& getAgentSharedSecret() {
    return agentSharedSecret();
}

using ConfigVersion = int64_t;

// ==================== 端点内设备描述 ====================

struct EndpointDevice {
    int id = 0;
    std::string name;
    std::string deviceCode;   // SL651 设备编码
    int slaveId = 0;          // Modbus 从站地址
    std::string modbusMode;   // Modbus 模式 (TCP/RTU)
    Json::Value heartbeat;    // 心跳包配置
    Json::Value registration; // 注册包配置
    int protocolConfigId = 0; // 协议配置 ID
    Json::Value protocolConfig; // 协议配置（protocol_config.config JSONB）
    std::string timezone = "+08:00"; // 设备时区

    Json::Value toJson() const {
        Json::Value json(Json::objectValue);
        json["id"] = id;
        json["name"] = name;
        if (!deviceCode.empty()) json["deviceCode"] = deviceCode;
        if (slaveId > 0) json["slaveId"] = slaveId;
        if (!modbusMode.empty()) json["modbusMode"] = modbusMode;
        if (heartbeat.isObject() && heartbeat.get("mode", "OFF").asString() != "OFF") {
            json["heartbeat"] = heartbeat;
        }
        if (registration.isObject() && registration.get("mode", "OFF").asString() != "OFF") {
            json["registration"] = registration;
        }
        if (protocolConfigId > 0) json["protocolConfigId"] = protocolConfigId;
        if (protocolConfig.isObject() && !protocolConfig.empty()) {
            json["protocolConfig"] = protocolConfig;
        }
        if (timezone != "+08:00") json["timezone"] = timezone;
        return json;
    }

    static std::optional<EndpointDevice> fromJson(const Json::Value& json) {
        if (!json.isObject()) return std::nullopt;
        EndpointDevice d;
        d.id = json.get("id", 0).asInt();
        d.name = json.get("name", "").asString();
        d.deviceCode = json.get("deviceCode", "").asString();
        d.slaveId = json.get("slaveId", 0).asInt();
        d.modbusMode = json.get("modbusMode", "").asString();
        d.heartbeat = json.get("heartbeat", Json::objectValue);
        d.registration = json.get("registration", Json::objectValue);
        d.protocolConfigId = json.get("protocolConfigId", 0).asInt();
        d.protocolConfig = json.get("protocolConfig", Json::objectValue);
        d.timezone = json.get("timezone", "+08:00").asString();
        if (d.id <= 0) return std::nullopt;
        return d;
    }
};

// ==================== 设备端点（取代 LinkAssignment）====================

struct DeviceEndpoint {
    std::string id;                        // 端点 ID（agent_endpoint.id 的字符串形式）
    std::string name;                      // 显示名称
    std::string transport;                 // "ethernet" / "serial"
    std::string mode;                      // "TCP Server" / "TCP Client" (ethernet)
    std::string protocol = Constants::PROTOCOL_SL651;
    std::string ip;                        // 监听/目标 IP (ethernet)
    int port = 0;                          // 端口 (ethernet)
    std::string channel;                   // 串口通道 (serial)
    int baudRate = 0;                      // 波特率 (serial)
    std::vector<EndpointDevice> devices;   // 该端点下的设备列表

    Json::Value toJson() const {
        Json::Value json(Json::objectValue);
        json["id"] = id;
        json["name"] = name;
        json["transport"] = transport;
        json["mode"] = mode;
        json["protocol"] = protocol;
        json["ip"] = ip;
        json["port"] = port;
        json["channel"] = channel;
        json["baudRate"] = baudRate;
        Json::Value devArr(Json::arrayValue);
        for (const auto& d : devices) {
            devArr.append(d.toJson());
        }
        json["devices"] = devArr;
        return json;
    }

    static std::optional<DeviceEndpoint> fromJson(const Json::Value& json) {
        if (!json.isObject()) return std::nullopt;
        DeviceEndpoint ep;
        ep.id = json.get("id", "").asString();
        ep.name = json.get("name", "").asString();
        ep.transport = json.get("transport", "ethernet").asString();
        ep.mode = json.get("mode", "").asString();
        ep.protocol = json.get("protocol", Constants::PROTOCOL_SL651).asString();
        ep.ip = json.get("ip", "").asString();
        ep.port = json.get("port", 0).asInt();
        ep.channel = json.get("channel", "").asString();
        ep.baudRate = json.get("baudRate", 0).asInt();

        if (ep.id.empty()) return std::nullopt;

        // ethernet 需要 mode + ip + port；serial 需要 channel
        if (ep.transport == "ethernet" && (ep.mode.empty() || ep.ip.empty() || ep.port <= 0)) {
            return std::nullopt;
        }
        if (ep.transport == "serial" && ep.channel.empty()) {
            return std::nullopt;
        }

        const auto& devArr = json["devices"];
        if (devArr.isArray()) {
            for (const auto& item : devArr) {
                if (auto d = EndpointDevice::fromJson(item)) {
                    ep.devices.push_back(std::move(*d));
                }
            }
        }
        return ep;
    }
};

// ==================== 工具函数 ====================

inline Json::Value buildEnvelope(const std::string& type, const Json::Value& data) {
    Json::Value envelope(Json::objectValue);
    envelope["type"] = type;
    envelope["data"] = data;
    envelope["ts"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return envelope;
}

inline std::string buildMessage(const std::string& type, const Json::Value& data) {
    return JsonHelper::serialize(buildEnvelope(type, data));
}

inline std::optional<Json::Value> parseMessage(std::string_view payload) {
    try {
        return JsonHelper::parse(std::string(payload));
    } catch (...) {
        return std::nullopt;
    }
}

inline ConfigVersion parseConfigVersion(const Json::Value& data, ConfigVersion defaultValue = 0) {
    if (!data.isObject() || !data.isMember("configVersion")) {
        return defaultValue;
    }

    const auto& value = data["configVersion"];
    if (value.isInt64() || value.isUInt64() || value.isIntegral()) {
        return static_cast<ConfigVersion>(value.asLargestInt());
    }
    return defaultValue;
}

// ==================== zlib 压缩/解压 ====================

inline std::string zlibDeflate(const std::string& input) {
    z_stream zs{};
    if (::deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) return {};

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string output;
    output.resize(::deflateBound(&zs, static_cast<uLong>(input.size())));

    zs.next_out = reinterpret_cast<Bytef*>(output.data());
    zs.avail_out = static_cast<uInt>(output.size());

    int ret = ::deflate(&zs, Z_FINISH);
    ::deflateEnd(&zs);

    if (ret != Z_STREAM_END) return {};
    output.resize(zs.total_out);
    return output;
}

inline std::string zlibInflate(const char* data, size_t len) {
    if (len == 0) return {};
    z_stream zs{};
    if (::inflateInit(&zs) != Z_OK) return {};

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    zs.avail_in = static_cast<uInt>(len);

    std::string output;
    char buf[8192];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = ::inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            ::inflateEnd(&zs);
            return {};
        }
        output.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    ::inflateEnd(&zs);
    return output;
}

// ==================== 二进制帧构建/解析 ====================

/**
 * @brief 构建压缩二进制控制消息帧
 *
 * 大于 128 字节的 JSON 尝试 zlib 压缩 (FRAME_MSG_ZLIB)，
 * 小消息或压缩无效时使用原始帧 (FRAME_MSG_RAW)。
 * 返回值始终是二进制帧（首字节为帧类型）。
 */
inline std::string buildBinaryMessage(const std::string& type, const Json::Value& data) {
    auto json = buildMessage(type, data);

    if (json.size() > 128) {
        auto compressed = zlibDeflate(json);
        if (!compressed.empty() && compressed.size() < json.size()) {
            std::string frame;
            frame.reserve(1 + compressed.size());
            frame.push_back(static_cast<char>(FRAME_MSG_ZLIB));
            frame.append(compressed);
            return frame;
        }
    }

    std::string frame;
    frame.reserve(1 + json.size());
    frame.push_back(static_cast<char>(FRAME_MSG_RAW));
    frame.append(json);
    return frame;
}

/**
 * @brief 解析二进制控制消息帧（0x10 或 0x11 开头）
 *
 * @return 解析后的 JSON 信封，失败返回 nullopt
 */
inline std::optional<Json::Value> parseBinaryControlFrame(const std::string& data) {
    if (data.size() < 2) return std::nullopt;
    uint8_t ch = static_cast<uint8_t>(data[0]);

    if (ch == FRAME_MSG_ZLIB) {
        auto json = zlibInflate(data.data() + 1, data.size() - 1);
        if (json.empty()) return std::nullopt;
        return parseMessage(json);
    }
    if (ch == FRAME_MSG_RAW) {
        return parseMessage(std::string_view(data.data() + 1, data.size() - 1));
    }
    return std::nullopt;
}

}  // namespace agent
