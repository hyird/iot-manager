#pragma once

#include "common/edgenode/AgentProtocol.hpp"
#include "common/utils/JsonHelper.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <vector>

namespace agent_app {

namespace fs = std::filesystem;

struct EdgeNodeConfig {
    std::string sn;
    std::string model;
    std::string platformHost;
    std::string logLevel = "INFO";
    int ioThreads = 2;
    int heartbeatIntervalSec = 30;
    bool validateCert = false;
};

inline std::string toUpperCode(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

class EdgeNodeConfigLoader {
public:
    static std::optional<EdgeNodeConfig> load(const std::string& explicitPath = {},
                                           std::string* error = nullptr) {
        std::string resolvedPath;
        if (!explicitPath.empty()) {
            if (!fs::exists(explicitPath)) {
                setError(error, "指定的配置文件不存在: " + explicitPath);
                return std::nullopt;
            }
            resolvedPath = explicitPath;
        } else {
            auto found = findConfigPath();
            if (!found) {
                setError(error, "未找到 EdgeNode 配置文件");
                return std::nullopt;
            }
            resolvedPath = *found;
        }

        std::cout << "[EdgeNode] loading config: " << resolvedPath << std::endl;

        std::ifstream ifs(resolvedPath);
        if (!ifs) {
            setError(error, "无法打开 EdgeNode 配置文件: " + resolvedPath);
            return std::nullopt;
        }

        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errs;
        if (!Json::parseFromStream(reader, ifs, &root, &errs)) {
            setError(error, "EdgeNode 配置 JSON 解析失败: " + errs);
            return std::nullopt;
        }

        const auto& agent = root["agent"];
        if (!agent.isObject()) {
            setError(error, "EdgeNode 配置缺少 agent 节点");
            return std::nullopt;
        }

        EdgeNodeConfig config;
        config.sn = readDeviceTreeSerial().value_or(toUpperCode(agent.get("code", "").asString()));
        config.model = readDeviceTreeModel().value_or(agent.get("model", "").asString());
        config.platformHost = normalizeToWsHost(agent.get("platform_url", "").asString());

        if (config.sn.empty() || config.platformHost.empty()) {
            setError(error, "EdgeNode 配置缺少 sn/platform_url");
            return std::nullopt;
        }
        if (config.model.empty()) {
            config.model = "unknown-model";
        }

        std::cout << "[EdgeNode] sn=" << config.sn
                  << ", model=" << config.model
                  << ", platform=" << config.platformHost << "/agent/ws" << std::endl;

        return config;
    }

private:
    static std::optional<std::string> readDeviceTreeSerial() {
#ifdef __linux__
        constexpr auto path = "/proc/device-tree/serial-number";
        if (!fs::exists(path)) return std::nullopt;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return std::nullopt;
        std::string serial;
        std::getline(ifs, serial, '\0');
        trim(serial);
        if (serial.empty()) return std::nullopt;
        serial = toUpperCode(serial);
        std::cout << "[EdgeNode] sn from /proc/device-tree/serial-number: " << serial << std::endl;
        return serial;
#else
        return std::nullopt;
#endif
    }

    static std::optional<std::string> readDeviceTreeModel() {
#ifdef __linux__
        constexpr auto path = "/proc/device-tree/model";
        if (!fs::exists(path)) return std::nullopt;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return std::nullopt;
        std::string model;
        std::getline(ifs, model, '\0');
        trim(model);
        if (model.empty()) return std::nullopt;
        std::cout << "[EdgeNode] model from /proc/device-tree/model: " << model << std::endl;
        return model;
#else
        return std::nullopt;
#endif
    }

    static std::optional<std::string> findConfigPath() {
        static const std::vector<std::string> paths = {
            "./config/agent.local.json",
            "../config/agent.local.json",
            "../../config/agent.local.json",
            "./config/agent.json",
            "../config/agent.json",
            "../../config/agent.json",
            "./config/agent.example.json",
            "../config/agent.example.json",
            "../../config/agent.example.json"
        };

        for (const auto& path : paths) {
            if (fs::exists(path)) {
                return path;
            }
        }
        return std::nullopt;
    }

    static void trim(std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            value.clear();
            return;
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
    }

    /**
     * @brief 从 URL 提取 WebSocket 地址
     *
     * 支持 http/https/ws/wss 输入，自动转换：
     *   http://  → ws://
     *   https:// → wss://
     * 并去掉路径部分，只保留 scheme://host:port
     */
    static std::string normalizeToWsHost(const std::string& url) {
        std::string result = url;

        // http(s) → ws(s)
        if (result.substr(0, 8) == "https://") {
            result = "wss://" + result.substr(8);
        } else if (result.substr(0, 7) == "http://") {
            result = "ws://" + result.substr(7);
        }

        // 去掉路径部分
        auto schemeEnd = result.find("://");
        if (schemeEnd == std::string::npos) return result;
        auto pathStart = result.find('/', schemeEnd + 3);
        return pathStart == std::string::npos ? result : result.substr(0, pathStart);
    }

    static void setError(std::string* error, const std::string& message) {
        if (error) {
            *error = message;
        }
    }
};

using AgentConfig = EdgeNodeConfig;
using AgentConfigLoader = EdgeNodeConfigLoader;

}  // namespace agent_app
