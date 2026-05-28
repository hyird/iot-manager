#include "config/AppConfig.h"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

boost::json::object requireObject(const boost::json::object& parent, const char* key) {
    const auto* value = parent.if_contains(key);
    if (value == nullptr || !value->is_object()) {
        throw std::runtime_error(std::string("Missing JSON object: ") + key);
    }
    return value->as_object();
}

std::string getString(const boost::json::object& object, const char* key, std::string fallback = {}) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (!value->is_string()) {
        throw std::runtime_error(std::string("Expected string for JSON key: ") + key);
    }
    return std::string(value->as_string());
}

uint16_t getUInt16(const boost::json::object& object, const char* key, uint16_t fallback) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (!value->is_int64()) {
        throw std::runtime_error(std::string("Expected integer for JSON key: ") + key);
    }
    const auto number = value->as_int64();
    if (number < 0 || number > 65535) {
        throw std::runtime_error(std::string("Integer out of uint16 range for JSON key: ") + key);
    }
    return static_cast<uint16_t>(number);
}

unsigned int getUInt(const boost::json::object& object, const char* key, unsigned int fallback) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (!value->is_int64()) {
        throw std::runtime_error(std::string("Expected integer for JSON key: ") + key);
    }
    const auto number = value->as_int64();
    if (number < 0) {
        throw std::runtime_error(std::string("Expected non-negative integer for JSON key: ") + key);
    }
    return static_cast<unsigned int>(number);
}

Json::Value requireJsonObject(const Json::Value& parent, const char* key) {
    if (!parent.isMember(key) || !parent[key].isObject()) {
        throw std::runtime_error(std::string("Missing JSON object: ") + key);
    }
    return parent[key];
}

std::string getJsonString(const Json::Value& object, const char* key, std::string fallback = {}) {
    if (!object.isMember(key) || object[key].isNull()) {
        return fallback;
    }
    if (!object[key].isString()) {
        throw std::runtime_error(std::string("Expected string for JSON key: ") + key);
    }
    return object[key].asString();
}

bool getJsonBool(const Json::Value& object, const char* key, bool fallback = false) {
    if (!object.isMember(key) || object[key].isNull()) {
        return fallback;
    }
    if (!object[key].isBool()) {
        throw std::runtime_error(std::string("Expected bool for JSON key: ") + key);
    }
    return object[key].asBool();
}

uint16_t getJsonUInt16(const Json::Value& object, const char* key, uint16_t fallback) {
    if (!object.isMember(key) || object[key].isNull()) {
        return fallback;
    }
    if (!object[key].isIntegral()) {
        throw std::runtime_error(std::string("Expected integer for JSON key: ") + key);
    }
    const auto number = object[key].asInt64();
    if (number < 0 || number > 65535) {
        throw std::runtime_error(std::string("Integer out of uint16 range for JSON key: ") + key);
    }
    return static_cast<uint16_t>(number);
}

unsigned int getJsonUInt(const Json::Value& object, const char* key, unsigned int fallback) {
    if (!object.isMember(key) || object[key].isNull()) {
        return fallback;
    }
    if (!object[key].isIntegral()) {
        throw std::runtime_error(std::string("Expected integer for JSON key: ") + key);
    }
    const auto number = object[key].asInt64();
    if (number < 0) {
        throw std::runtime_error(std::string("Expected non-negative integer for JSON key: ") + key);
    }
    return static_cast<unsigned int>(number);
}

} // namespace

AppConfig AppConfig::loadFromFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Could not open config file: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    boost::system::error_code error;
    const auto parsed = boost::json::parse(buffer.str(), error);
    if (error) {
        throw std::runtime_error("Invalid JSON config: " + error.message());
    }

    if (!parsed.is_object()) {
        throw std::runtime_error("Config root must be a JSON object");
    }

    const auto root = parsed.as_object();
    const auto server = requireObject(root, "server");
    const auto sip = requireObject(root, "sip");
    const auto media = requireObject(root, "media");
    AppConfig config;
    config.server.host = getString(server, "host", config.server.host);
    config.server.port = getUInt16(server, "port", config.server.port);
    config.server.threads = getUInt(server, "threads", config.server.threads);

    config.sip.domain = getString(sip, "domain");
    config.sip.id = getString(sip, "id");
    config.sip.host = getString(sip, "host", config.sip.host);
    config.sip.publicIp = getString(sip, "public_ip");
    config.sip.port = getUInt16(sip, "port", config.sip.port);
    config.sip.password = getString(sip, "password");
    config.sip.transport = getString(sip, "transport", config.sip.transport);
    if (const auto* loggingValue = root.if_contains("logging");
        loggingValue != nullptr && !loggingValue->is_null()) {
        if (!loggingValue->is_bool()) {
            throw std::runtime_error("Expected bool for JSON key: logging");
        }
        config.sip.logging = loggingValue->as_bool();
    }

    config.media.zlmBaseUrl = getString(media, "zlm_base_url");
    config.media.zlmPublicBaseUrl = getString(media, "zlm_public_base_url", config.media.zlmBaseUrl);
    config.media.zlmSecret = getString(media, "zlm_secret");
    config.media.rtpPublicIp = getString(media, "rtp_public_ip", config.sip.publicIp);
    config.media.rtpPortRangeStart = getUInt16(media, "rtp_port_range_start", config.media.rtpPortRangeStart);
    config.media.rtpPortRangeEnd = getUInt16(media, "rtp_port_range_end", config.media.rtpPortRangeEnd);

    return config;
}

AppConfig AppConfig::fromJson(const Json::Value& root) {
    if (!root.isObject()) {
        throw std::runtime_error("GB28181 config root must be a JSON object");
    }

    AppConfig config;
    config.enabled = getJsonBool(root, "enabled", config.enabled);
    config.autoFirewall = getJsonBool(root, "auto_firewall", config.autoFirewall);
    config.apiPrefix = getJsonString(root, "api_prefix", config.apiPrefix);

    if (root.isMember("server") && root["server"].isObject()) {
        const auto& server = root["server"];
        config.server.host = getJsonString(server, "host", config.server.host);
        config.server.port = getJsonUInt16(server, "port", config.server.port);
        config.server.threads = getJsonUInt(server, "threads", config.server.threads);
    }

    const auto sip = requireJsonObject(root, "sip");
    config.sip.domain = getJsonString(sip, "domain");
    config.sip.id = getJsonString(sip, "id");
    config.sip.host = getJsonString(sip, "host", config.sip.host);
    config.sip.publicIp = getJsonString(sip, "public_ip");
    config.sip.port = getJsonUInt16(sip, "port", config.sip.port);
    config.sip.password = getJsonString(sip, "password");
    config.sip.transport = getJsonString(sip, "transport", config.sip.transport);

    if (root.isMember("logging") && !root["logging"].isNull()) {
        if (!root["logging"].isBool()) {
            throw std::runtime_error("Expected bool for JSON key: logging");
        }
        config.sip.logging = root["logging"].asBool();
    }

    const auto media = requireJsonObject(root, "media");
    config.media.zlmBaseUrl = getJsonString(media, "zlm_base_url");
    config.media.zlmPublicBaseUrl = getJsonString(media, "zlm_public_base_url", config.media.zlmBaseUrl);
    config.media.zlmSecret = getJsonString(media, "zlm_secret");
    config.media.rtpPublicIp = getJsonString(media, "rtp_public_ip", config.sip.publicIp);
    config.media.rtpPortRangeStart = getJsonUInt16(media, "rtp_port_range_start", config.media.rtpPortRangeStart);
    config.media.rtpPortRangeEnd = getJsonUInt16(media, "rtp_port_range_end", config.media.rtpPortRangeEnd);

    return config;
}
