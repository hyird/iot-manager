#pragma once

#include "common/utils/AppException.hpp"
#include "common/utils/JsonHelper.hpp"

namespace OpenAccess {

inline constexpr const char* WEBHOOK_EVENT_DEVICE_DATA = "device.data.reported";
inline constexpr const char* WEBHOOK_EVENT_DEVICE_IMAGE = "device.image.reported";
inline constexpr const char* WEBHOOK_EVENT_DEVICE_COMMAND_DISPATCHED = "device.command.dispatched";
inline constexpr const char* WEBHOOK_EVENT_DEVICE_COMMAND_RESPONSE = "device.command.responded";
inline constexpr const char* WEBHOOK_EVENT_DEVICE_ALERT_TRIGGERED = "device.alert.triggered";
inline constexpr const char* WEBHOOK_EVENT_DEVICE_ALERT_RESOLVED = "device.alert.resolved";

struct AccessKeySession {
    int id = 0;
    std::string name;
    bool allowRealtime = false;
    bool allowHistory = false;
    bool allowCommand = false;
    bool allowAlert = false;
    std::set<int> deviceIds;

    bool canAccessDevice(int deviceId) const {
        return deviceIds.find(deviceId) != deviceIds.end();
    }
};

struct WebhookTarget {
    int id = 0;
    int accessKeyId = 0;
    std::string accessKeyName;
    std::string name;
    std::string url;
    std::string secret;
    int timeoutSeconds = 5;
    Json::Value headers{Json::objectValue};
    std::set<int> deviceIds;
    std::set<std::string> eventTypes;

    bool supportsEvent(const std::string& eventType) const {
        return eventTypes.empty() || eventTypes.find(eventType) != eventTypes.end();
    }
};

struct ParsedUrl {
    std::string baseUrl;
    std::string path;
};

inline std::string bytesToHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

inline std::string randomHex(size_t byteCount) {
    std::vector<unsigned char> bytes(byteCount);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return bytesToHex(bytes.data(), bytes.size());
}

inline std::string generateAccessKey() {
    return "ak_" + randomHex(24);
}

inline std::string sha256Hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    if (EVP_Digest(
            input.data(),
            input.size(),
            digest,
            &digestLen,
            EVP_sha256(),
            nullptr) != 1) {
        throw std::runtime_error("EVP_Digest failed");
    }

    return bytesToHex(digest, digestLen);
}

inline std::string hmacSha256Hex(const std::string& secret, const std::string& payload) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size(),
        digest,
        &digestLen
    );

    return bytesToHex(digest, digestLen);
}

inline Json::Value parseJsonOrDefault(const std::string& raw, const Json::Value& defaultValue) {
    if (raw.empty()) return defaultValue;
    try {
        return JsonHelper::parse(raw);
    } catch (...) {
        return defaultValue;
    }
}

inline std::vector<int> normalizeDeviceIds(const Json::Value& value) {
    if (!value.isArray()) {
        throw ValidationException("deviceIds 必须是数组");
    }

    std::set<int> uniqueIds;
    for (const auto& item : value) {
        if (!item.isNumeric()) {
            throw ValidationException("deviceIds 只能包含正整数");
        }
        int id = item.asInt();
        if (id <= 0) {
            throw ValidationException("deviceIds 只能包含正整数");
        }
        uniqueIds.insert(id);
    }

    return {uniqueIds.begin(), uniqueIds.end()};
}

inline Json::Value toJsonArray(const std::vector<int>& values) {
    Json::Value arr(Json::arrayValue);
    for (int value : values) {
        arr.append(value);
    }
    return arr;
}

inline std::string boolToSql(bool value) {
    return value ? "true" : "false";
}

inline std::string buildPlaceholders(size_t count) {
    std::string placeholders;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) placeholders += ", ";
        placeholders += "?";
    }
    return placeholders;
}

inline std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

inline std::string sanitizeError(const std::string& value, size_t maxLength = 500) {
    std::string sanitized;
    sanitized.reserve(std::min(value.size(), maxLength));
    for (char ch : value) {
        if (ch == '\r' || ch == '\n') {
            sanitized.push_back(' ');
        } else {
            sanitized.push_back(ch);
        }
        if (sanitized.size() >= maxLength) break;
    }
    return trim(sanitized);
}

inline std::string nowIso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

inline std::string extractAccessKey(const drogon::HttpRequestPtr& req) {
    std::string accessKey = trim(req->getHeader("X-Access-Key"));
    if (!accessKey.empty()) return accessKey;

    std::string authorization = trim(req->getHeader("Authorization"));
    if (!authorization.empty()) {
        constexpr std::string_view prefix = "AccessKey ";
        if (authorization.size() > prefix.size() &&
            authorization.compare(0, prefix.size(), prefix) == 0) {
            return trim(authorization.substr(prefix.size()));
        }
    }

    // 不从 URL query 参数读取 AccessKey（防止日志泄露）
    return "";
}

inline std::string resolveClientIp(const drogon::HttpRequestPtr& req) {
    // 优先使用 X-Real-IP（由可信反向代理设置，不可被客户端伪造）
    std::string realIp = trim(req->getHeader("X-Real-IP"));
    if (!realIp.empty()) return realIp;

    // X-Forwarded-For: 取最后一个 IP（代理追加的可信 IP），而非第一个（客户端可伪造）
    std::string forwarded = req->getHeader("X-Forwarded-For");
    if (!forwarded.empty()) {
        auto lastComma = forwarded.rfind(',');
        if (lastComma != std::string::npos) {
            return trim(forwarded.substr(lastComma + 1));
        }
        return trim(forwarded);
    }

    return req->peerAddr().toIpPort();
}

/** 检查主机名是否为内网/回环地址（SSRF 防护） */
inline bool isPrivateHost(const std::string& host) {
    // 回环地址
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "0.0.0.0") {
        return true;
    }
    // 云元数据地址
    if (host == "169.254.169.254" || host == "metadata.google.internal") {
        return true;
    }
    // RFC 1918 内网段简单检查
    if (host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0) {
        return true;
    }
    // 172.16.0.0/12
    if (host.rfind("172.", 0) == 0) {
        try {
            int second = std::stoi(host.substr(4, host.find('.', 4) - 4));
            if (second >= 16 && second <= 31) return true;
        } catch (...) {}
    }
    // 链路本地
    if (host.rfind("169.254.", 0) == 0) return true;
    return false;
}

inline ParsedUrl parseWebhookUrl(const std::string& url) {
    if (!(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
        throw ValidationException("Webhook 地址必须以 http:// 或 https:// 开头");
    }

    // 提取主机名并检查 SSRF
    size_t schemeEnd = url.find("://") + 3;
    size_t pathPos = url.find('/', schemeEnd);
    size_t portPos = url.find(':', schemeEnd);
    size_t hostEnd = std::min({pathPos, portPos, url.size()});
    std::string host = url.substr(schemeEnd, hostEnd - schemeEnd);

    if (isPrivateHost(host)) {
        throw ValidationException("Webhook 地址不允许指向内网或回环地址");
    }

    if (pathPos == std::string::npos) {
        return {url, "/"};
    }

    return {url.substr(0, pathPos), url.substr(pathPos)};
}

inline const std::set<std::string>& supportedWebhookEvents() {
    static const std::set<std::string> events = {
        WEBHOOK_EVENT_DEVICE_DATA,
        WEBHOOK_EVENT_DEVICE_IMAGE,
        WEBHOOK_EVENT_DEVICE_COMMAND_DISPATCHED,
        WEBHOOK_EVENT_DEVICE_COMMAND_RESPONSE,
        WEBHOOK_EVENT_DEVICE_ALERT_TRIGGERED,
        WEBHOOK_EVENT_DEVICE_ALERT_RESOLVED,
    };
    return events;
}

}  // namespace OpenAccess
