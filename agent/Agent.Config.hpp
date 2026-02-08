#pragma once

#include "common/agent/AgentProtocol.hpp"
#include "common/utils/JsonHelper.hpp"

namespace agent_app {

namespace fs = std::filesystem;

struct AgentConfig {
    std::string code;
    std::string name;
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

class AgentConfigLoader {
public:
    static std::optional<AgentConfig> load(const std::string& explicitPath = {},
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
                setError(error, "未找到 Agent 配置文件");
                return std::nullopt;
            }
            resolvedPath = *found;
        }

        std::cout << "[Agent] loading config: " << resolvedPath << std::endl;

        std::ifstream ifs(resolvedPath);
        if (!ifs) {
            setError(error, "无法打开 Agent 配置文件: " + resolvedPath);
            return std::nullopt;
        }

        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errs;
        if (!Json::parseFromStream(reader, ifs, &root, &errs)) {
            setError(error, "Agent 配置 JSON 解析失败: " + errs);
            return std::nullopt;
        }

        const auto& agent = root["agent"];
        if (!agent.isObject()) {
            setError(error, "Agent 配置缺少 agent 节点");
            return std::nullopt;
        }

        AgentConfig config;
        config.code = toUpperCode(readDeviceTreeSerial().value_or(
            agent.get("code", "").asString()));
        config.name = agent.get("name", config.code).asString();
        config.platformHost = normalizeToWsHost(agent.get("platform_url", "").asString());

        if (config.code.empty() || config.platformHost.empty()) {
            setError(error, "Agent 配置缺少 code/platform_url");
            return std::nullopt;
        }

        std::cout << "[Agent] code=" << config.code
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
        // 去除首尾空白
        serial.erase(0, serial.find_first_not_of(" \t\r\n"));
        serial.erase(serial.find_last_not_of(" \t\r\n") + 1);
        if (serial.empty()) return std::nullopt;
        std::cout << "[Agent] code from /proc/device-tree/serial-number: " << serial << std::endl;
        return serial;
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

}  // namespace agent_app
