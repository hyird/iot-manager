#pragma once

#include <cstdint>
#include <json/json.h>
#include <string>

struct ServerConfig {
    std::string host{"0.0.0.0"};
    uint16_t port{8080};
    unsigned int threads{4};
};

struct SipConfig {
    std::string domain;
    std::string id;
    std::string host{"0.0.0.0"};
    std::string publicIp;
    uint16_t port{5060};
    std::string password;
    std::string transport{"udp"};
};

struct MediaConfig {
    std::string zlmBaseUrl;
    std::string zlmPublicBaseUrl;
    std::string zlmSecret;
    std::string rtpPublicIp;
    uint16_t rtpPortRangeStart{30000};
    uint16_t rtpPortRangeEnd{30500};
};

struct LogConfig {
    std::string level{"info"};
};

struct AppConfig {
    bool enabled{false};
    bool autoFirewall{false};
    std::string apiPrefix{"/api/gb28181"};
    ServerConfig server;
    SipConfig sip;
    MediaConfig media;
    LogConfig log;

    static AppConfig loadFromFile(const std::string& path);
    static AppConfig fromJson(const Json::Value& root);
};
