#pragma once

#include "EdgeNode.Config.hpp"
#include "EdgeNodeCapabilities.hpp"
#include "EdgeNodeDataStore.hpp"
#include "EdgeNodeModbusPoller.hpp"
#include "EdgeNodeNetworkConfigurator.hpp"
#include "EdgeNodeProtocolEngine.hpp"
#include "EdgeNodeS7Poller.hpp"
#include "EdgeNodeShellManager.hpp"
#include "EdgeNodeL2ConfigBridge.hpp"
#include "EdgeNodeSerialLinkManager.hpp"
#include "common/edgenode/AgentProtocol.hpp"
#include "common/network/TcpLinkManager.hpp"

#include <drogon/WebSocketClient.h>

#include <iostream>

namespace agent_app {

inline constexpr const char* IOT_AGENT_VERSION = "0.1.0";

class EdgeNodeRuntime {
public:
    explicit EdgeNodeRuntime(EdgeNodeConfig config)
        : config_(std::move(config)) {}

    void run() {
        std::cout << "[EdgeNode] initializing..." << std::endl;

        // 检查主线程是否已存在 EventLoop（trantor 不允许同一线程创建两个）
        auto* existingLoop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (existingLoop) {
            std::cout << "[EdgeNode] reusing existing EventLoop on main thread" << std::endl;
            loopOwned_.reset();
            loop_ = existingLoop;
        } else {
            loopOwned_ = std::make_unique<trantor::EventLoop>();
            loop_ = loopOwned_.get();
            std::cout << "[EdgeNode] event loop created" << std::endl;
        }

        // 初始化本地 SQLite 缓存
        if (!dataStore_.initialize("./data/agent_cache.db")) {
            std::cerr << "[EdgeNode] WARNING: Failed to initialize local data store" << std::endl;
        }

        TcpLinkManager::instance().initialize(static_cast<size_t>(config_.ioThreads));
        TcpLinkManager::instance().setDataCallbackWithClient(
            [this](int epKey, const std::string& clientAddr, const std::string& data) {
                onEndpointData(epKey, clientAddr, data);
            }
        );
        TcpLinkManager::instance().setConnectionCallback(
            [this](int epKey, const std::string& clientAddr, bool connected) {
                onEndpointConnection(epKey, clientAddr, connected);
            }
        );

        // 串口链路回调
        EdgeNodeSerialLinkManager::instance().setDataCallback(
            [this](int epKey, const std::string& data) {
                onEndpointData(epKey, /*clientAddr=*/"serial", data);
            }
        );
        EdgeNodeSerialLinkManager::instance().setConnectionCallback(
            [this](int epKey, bool connected) {
                onEndpointConnection(epKey, "serial", connected);
            }
        );

        // Modbus 轮询器发送路由：根据端点类型选择 TCP 或串口
        modbusPoller_.setSendCallback([](int endpointKey, const std::string& data) -> bool {
            if (EdgeNodeSerialLinkManager::instance().isOpen(endpointKey)) {
                return EdgeNodeSerialLinkManager::instance().sendData(endpointKey, data);
            }
            return TcpLinkManager::instance().sendData(endpointKey, data);
        });

        loop_->runEvery(static_cast<double>(config_.heartbeatIntervalSec), [this]() {
            sendHeartbeat();
        });
        loop_->runEvery(1.0, [this]() {
            checkInterfaceChanges();
        });
        loop_->runEvery(10.0, [this]() {
            sendStatusSnapshot();
        });
        // Modbus 轮询器回调：解析结果存入 SQLite
        modbusPoller_.setDataCallback([this](std::vector<ParsedFrameResult>&& results) {
            dataStore_.storeBatch(results);
            if (connected_) {
                loop_->queueInLoop([this]() { flushPendingData(); });
            }
        });
        s7Poller_.setDataCallback([this](std::vector<ParsedFrameResult>&& results) {
            dataStore_.storeBatch(results);
            if (connected_) {
                loop_->queueInLoop([this]() { flushPendingData(); });
            }
        });

        // 续传定时器：每秒尝试上报缓存的待发送数据
        loop_->runEvery(1.0, [this]() {
            flushPendingData();
        });
        // Modbus 轮询定时器：每 200ms tick 一次
        loop_->runEvery(0.2, [this]() {
            modbusPoller_.tick();
        });
        // S7 轮询定时器：每 200ms tick 一次
        loop_->runEvery(0.2, [this]() {
            s7Poller_.tick();
        });
        // SQLite 缓存清理：每小时清理已确认的旧数据
        loop_->runEvery(3600.0, [this]() {
            dataStore_.cleanup(7);
        });

        // L2 二层配置桥接（免 IP 发现 + SSH 隧道）
        startL2Bridge();

        std::cout << "[EdgeNode] connecting to " << config_.platformHost << "/agent/ws ..." << std::endl;

        loop_->queueInLoop([this]() {
            connectPlatform();
        });
        loop_->loop();
    }

    void stop() {
        l2Bridge_.stop();
        shellManager_.close();
        loop_->queueInLoop([this]() {
            modbusPoller_.clear();
            s7Poller_.clear();
            if (wsClient_) {
                wsClient_->stop();
            }
            TcpLinkManager::instance().stopAll();
            EdgeNodeSerialLinkManager::instance().closeAll();
            dataStore_.close();
            loop_->quit();
        });
    }

private:
    /**
     * @brief 端点运行时状态
     *
     * 每个 DeviceEndpoint 被分配一个整数 key（用于 TcpLinkManager），
     * 并维护 endpointId、设备列表等元数据。
     */
    struct EndpointRuntime {
        int key = 0;                                    // TcpLinkManager 使用的整数 key
        agent::DeviceEndpoint endpoint;                 // 端点配置
        std::unordered_map<std::string, int> deviceCodeToId;   // deviceCode → deviceId（SL651）
        std::unordered_map<int, int> slaveIdToDeviceId;        // slaveId → deviceId（Modbus）
    };

    void connectPlatform() {
        wsClient_ = drogon::WebSocketClient::newWebSocketClient(
            config_.platformHost,
            loop_,
            false,
            config_.validateCert
        );

        wsClient_->setMessageHandler(
            [this](std::string&& message, const drogon::WebSocketClientPtr&, const drogon::WebSocketMessageType& type) {
                onWsMessage(std::move(message), type);
            }
        );
        wsClient_->setConnectionClosedHandler([this](const drogon::WebSocketClientPtr&) {
            std::cout << "[WARN] " << "[EdgeNode] WebSocket connection closed" << std::endl;
            connected_ = false;
            agentId_ = 0;
            scheduleReconnect();
        });

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/agent/ws");
        req->addHeader("X-Agent-SN", config_.sn);
        req->addHeader("X-Agent-Model", config_.model);

        wsClient_->connectToServer(
            req,
            [this](drogon::ReqResult result, const drogon::HttpResponsePtr& resp, const drogon::WebSocketClientPtr&) {
                if (result != drogon::ReqResult::Ok) {
                    std::cout << "[EdgeNode] connect failed (code=" << static_cast<int>(result)
                              << "), retry in 5s..." << std::endl;
                    connected_ = false;
                    scheduleReconnect();
                    return;
                }

                // 检查 HTTP 升级响应状态码
                if (resp) {
                    auto statusCode = resp->getStatusCode();
                    if (statusCode != drogon::k101SwitchingProtocols) {
                        std::cout << "[Agent] upgrade rejected, HTTP " << static_cast<int>(statusCode)
                                  << ", retry in 5s..." << std::endl;
                        connected_ = false;
                        scheduleReconnect();
                        return;
                    }
                }

                connected_ = true;
                reconnectScheduled_ = false;
                std::cout << "[Agent] connected to platform" << std::endl;
                sendHello();
            }
        );
    }

    void scheduleReconnect() {
        if (reconnectScheduled_.exchange(true)) {
            return;
        }

        // 认证失败使用较长间隔（30s），避免频繁无效重连
        double delay = authFailed_ ? 30.0 : 5.0;
        loop_->runAfter(delay, [this]() {
            reconnectScheduled_ = false;
            connectPlatform();
        });
    }

    void onWsMessage(std::string&& message, const drogon::WebSocketMessageType& type) {
        if (type == drogon::WebSocketMessageType::Binary) {
            if (message.empty()) return;
            uint8_t ch = static_cast<uint8_t>(message[0]);
            if (!agent::isShellFrame(ch)) {
                // 二进制控制消息帧（0x10/0x11）
                auto payloadOpt = agent::parseBinaryControlFrame(message);
                if (!payloadOpt) {
                    std::cout << "[WARN] [Agent] binary control frame parse failed" << std::endl;
                    return;
                }
                handlePayload(*payloadOpt);
            }
            return;
        }

        if (type != drogon::WebSocketMessageType::Text) {
            return;
        }

        // 兼容文本帧（向后兼容旧版 Server）
        auto payloadOpt = agent::parseMessage(message);
        if (!payloadOpt) {
            std::cout << "[WARN] [Agent] invalid ws payload" << std::endl;
            return;
        }
        handlePayload(*payloadOpt);
    }

    void handlePayload(const Json::Value& payload) {
        const auto typeName = payload.get("type", "").asString();
        const auto& data = payload["data"];

        std::cout << "[Agent] recv: " << typeName << std::endl;

        if (typeName == agent::MESSAGE_ERROR) {
            const auto code = data.get("code", "").asString();
            const auto msg = data.get("message", "").asString();
            std::cout << "[ERROR] " << "[Agent] server error: " << code << " - " << msg << std::endl;
            // 认证失败使用较长的重连间隔
            if (code == "auth_failed") {
                authFailed_ = true;
            }
            return;
        }

        if (typeName == agent::MESSAGE_HELLO_ACK) {
            agentId_ = data.get("agentId", 0).asInt();
            authFailed_ = false;
            std::cout << "[Agent] registered, agentId=" << agentId_ << std::endl;
            sendStatusSnapshot();
            return;
        }

        if (typeName == agent::MESSAGE_CONFIG_SYNC) {
            const auto configVersion = agent::parseConfigVersion(data);
            if (configVersion <= 0) {
                sendConfigApplyFailed(0, "平台下发配置缺少 configVersion");
                return;
            }

            const auto& endpoints = data.get("endpoints", Json::Value(Json::arrayValue));
            std::cout << "[Agent] config:sync received, version=" << configVersion
                      << ", endpoints=" << endpoints.size() << std::endl;

            const auto error = applyConfigSync(endpoints);
            if (!error.empty()) {
                std::cout << "[ERROR] " << "[Agent] config apply failed: " << error << std::endl;
                sendConfigApplyFailed(configVersion, error);
                return;
            }

            lastAppliedConfigVersion_ = configVersion;
            std::cout << "[Agent] config applied successfully, version=" << configVersion << std::endl;
            sendConfigApplied(configVersion);
            sendStatusSnapshot();
            return;
        }

        if (typeName == agent::MESSAGE_DEVICE_SEND) {
            handleSendCommand(data);
            return;
        }

        if (typeName == agent::MESSAGE_DEVICE_COMMAND) {
            handleDeviceCommand(data);
            return;
        }

        if (typeName == agent::MESSAGE_DEVICE_PARSED_ACK) {
            handleParsedDataAck(data);
            return;
        }

        if (typeName == agent::MESSAGE_NETWORK_CONFIG) {
            handleNetworkConfig(data);
            return;
        }

        // ==================== Shell ====================
        if (typeName == agent::MESSAGE_SHELL_OPEN) {
            handleShellOpen(data);
            return;
        }
        if (typeName == agent::MESSAGE_SHELL_DATA) {
            shellManager_.write(data.get("data", "").asString());
            return;
        }
        if (typeName == agent::MESSAGE_SHELL_RESIZE) {
            shellManager_.resize(data.get("cols", 80).asInt(), data.get("rows", 24).asInt());
            return;
        }
        if (typeName == agent::MESSAGE_SHELL_CLOSE) {
            shellManager_.close();
            return;
        }

        if (typeName == agent::MESSAGE_ENDPOINT_DISCONNECT) {
            const auto endpointId = data.get("endpointId", "").asString();
            auto it = endpointsByEpId_.find(endpointId);
            if (it != endpointsByEpId_.end()) {
                TcpLinkManager::instance().disconnectServerClients(it->second->key);
            }
            return;
        }
    }

    void sendHello() {
        Json::Value data(Json::objectValue);
        data["sn"] = config_.sn;
        data["model"] = config_.model;
        data["version"] = IOT_AGENT_VERSION;
        data["capabilities"] = AgentCapabilitiesCollector::collectCapabilities();
        data["runtime"] = buildRuntimePayload();
        sendMessage(agent::MESSAGE_HELLO, data);
        lastInterfacesFingerprint_ = computeInterfacesFingerprint();
    }

    void sendHeartbeat(bool includeCapabilities = false) {
        if (!connected_) {
            return;
        }

        Json::Value data(Json::objectValue);
        data["version"] = IOT_AGENT_VERSION;
        if (includeCapabilities) {
            data["capabilities"] = AgentCapabilitiesCollector::collectCapabilities();
        }
        data["runtime"] = buildRuntimePayload();
        sendMessage(agent::MESSAGE_HEARTBEAT, data);
    }

    void sendStatusSnapshot() {
        if (!connected_ || agentId_ <= 0) {
            return;
        }

        Json::Value items(Json::arrayValue);
        for (const auto& [epId, runtime] : endpointsByEpId_) {
            (void)epId;
            auto status = TcpLinkManager::instance().getStatus(runtime->key);
            status["id"] = runtime->endpoint.id;
            status["endpointId"] = runtime->endpoint.id;
            items.append(status);
        }

        Json::Value data(Json::objectValue);
        data["agentId"] = agentId_;
        data["items"] = items;
        sendMessage(agent::MESSAGE_ENDPOINT_STATUS, data);
    }

    // ==================== 配置同步 ====================

    std::string applyConfigSync(const Json::Value& endpoints) {
        if (!endpoints.isArray()) {
            return "配置格式错误";
        }

        // 解析新的端点列表
        std::vector<agent::DeviceEndpoint> nextEndpoints;
        for (const auto& item : endpoints) {
            auto epOpt = agent::DeviceEndpoint::fromJson(item);
            if (!epOpt) {
                return "配置中包含无效端点";
            }
            nextEndpoints.push_back(std::move(*epOpt));
        }

        // 详细打印每个端点配置
        for (const auto& ep : nextEndpoints) {
            std::cout << "[Agent] endpoint: id=" << ep.id
                      << ", protocol=" << ep.protocol
                      << ", transport=" << ep.transport;
            if (ep.transport == "serial") {
                std::cout << ", channel=" << ep.channel
                          << ", baudRate=" << ep.baudRate;
            } else {
                std::cout << ", mode=" << ep.mode
                          << ", addr=" << ep.ip << ":" << ep.port;
            }
            std::cout << ", devices=" << ep.devices.size() << std::endl;
            for (const auto& dev : ep.devices) {
                std::cout << "[Agent]   device: id=" << dev.id
                          << ", name=" << dev.name
                          << ", code=" << dev.deviceCode
                          << ", slaveId=" << dev.slaveId
                          << ", hasProtocolConfig=" << (dev.protocolConfig.isObject() ? "yes" : "no") << std::endl;
            }
        }

        const auto capabilities = AgentCapabilitiesCollector::collectCapabilities();
        const auto interfaces = capabilities.get("interfaces", Json::Value(Json::arrayValue));
        if (const auto error = validateEndpoints(nextEndpoints, interfaces); !error.empty()) {
            return error;
        }

        // 保存旧状态以便回滚
        const auto previousEndpoints = endpointsByEpId_;
        currentEndpoints_ = nextEndpoints;  // 保存完整端点配置（含 protocolConfig）

        try {
            // 停止所有旧端点
            for (const auto& [epId, runtime] : previousEndpoints) {
                (void)epId;
                deactivateEndpoint(*runtime);
            }

            // 构建新的端点运行时
            std::unordered_map<std::string, std::shared_ptr<EndpointRuntime>> nextRuntimes;
            keyByEpId_.clear();
            endpointsByKey_.clear();

            for (auto& ep : nextEndpoints) {
                auto runtime = std::make_shared<EndpointRuntime>();
                runtime->key = nextEndpointKey_++;
                runtime->endpoint = std::move(ep);

                // 构建设备查找索引
                for (const auto& dev : runtime->endpoint.devices) {
                    if (!dev.deviceCode.empty()) {
                        runtime->deviceCodeToId[dev.deviceCode] = dev.id;
                    }
                    if (dev.slaveId > 0) {
                        runtime->slaveIdToDeviceId[dev.slaveId] = dev.id;
                    }
                }

                keyByEpId_[runtime->endpoint.id] = runtime->key;
                endpointsByKey_[runtime->key] = runtime;
                nextRuntimes[runtime->endpoint.id] = runtime;

                applyEndpoint(*runtime);
            }

            endpointsByEpId_ = std::move(nextRuntimes);

            // 加载协议配置到本地引擎（用于本地解析）
            protocolEngine_.loadConfig(currentEndpoints_);

            // 加载 Modbus 轮询配置
            modbusPoller_.loadConfig(protocolEngine_.getModbusEndpoints(), keyByEpId_);
            // 加载 S7 轮询配置
            s7Poller_.loadConfig(protocolEngine_.getS7Endpoints());
        } catch (const std::exception& e) {
            return rollbackEndpoints(previousEndpoints, e.what());
        }

        return {};
    }

    void applyEndpoint(const EndpointRuntime& runtime) {
        const auto& ep = runtime.endpoint;

        if (ep.transport == "serial") {
            EdgeNodeSerialLinkManager::instance().openPort(
                runtime.key,
                "ep:" + ep.id,
                ep.channel,
                ep.baudRate
            );
            return;
        }

        if (ep.mode == Constants::LINK_MODE_TCP_SERVER) {
            TcpLinkManager::instance().startServer(
                runtime.key,
                "ep:" + ep.id,
                ep.ip,
                static_cast<uint16_t>(ep.port)
            );
            return;
        }

        if (ep.mode == Constants::LINK_MODE_TCP_CLIENT) {
            TcpLinkManager::instance().startClient(
                runtime.key,
                "ep:" + ep.id,
                ep.ip,
                static_cast<uint16_t>(ep.port)
            );
            return;
        }

        throw std::runtime_error("暂不支持的端点模式: " + ep.mode);
    }

    void deactivateEndpoint(const EndpointRuntime& runtime) {
        if (runtime.endpoint.transport == "serial") {
            EdgeNodeSerialLinkManager::instance().closePort(runtime.key);
        } else {
            TcpLinkManager::instance().stop(runtime.key);
        }
    }

    // ==================== 数据处理 ====================

    // ==================== 网络配置 ====================

    void handleNetworkConfig(const Json::Value& data) {
        const auto& interfaces = data["interfaces"];
        if (!interfaces.isArray()) {
            std::cout << "[WARN] " << "[Agent] network:config missing interfaces array" << std::endl;
            return;
        }

        const auto backend = AgentNetworkConfigurator::detectBackend();
        std::cout << "[Agent] network:config received, " << interfaces.size()
                  << " interface(s), backend=" << AgentNetworkConfigurator::backendName(backend) << std::endl;

        // 先清理旧的 iot 网络配置文件，避免历史残留与当前配置冲突
        if (backend != AgentNetworkConfigurator::Backend::NONE) {
            std::vector<std::string> targetInterfaces;
            targetInterfaces.reserve(static_cast<size_t>(interfaces.size()) * 3);
            for (const auto& iface : interfaces) {
                const auto name = iface.get("name", "").asString();
                if (!name.empty()) targetInterfaces.push_back(name);
                if (iface.isMember("bridge_ports") && iface["bridge_ports"].isArray()) {
                    for (const auto& p : iface["bridge_ports"]) {
                        const auto port = p.asString();
                        if (!port.empty()) targetInterfaces.push_back(port);
                    }
                }
            }
            auto cleanupError = AgentNetworkConfigurator::cleanupManagedConfigs(targetInterfaces);
            if (!cleanupError.empty()) {
                std::cout << "[ERROR] " << "[Agent] cleanup old network config failed: " << cleanupError << std::endl;
                sendNetworkConfigFailed("cleanup", cleanupError);
                return;
            }
        }

        for (const auto& iface : interfaces) {
            const auto name = iface.get("name", "").asString();
            const auto type = iface.get("type", "ethernet").asString();
            const auto mode = iface.get("mode", "dhcp").asString();

            // ========== 桥接配置 ==========
            if (type == "bridge") {
                std::vector<std::string> ports;
                if (iface.isMember("bridge_ports") && iface["bridge_ports"].isArray()) {
                    for (const auto& p : iface["bridge_ports"]) {
                        ports.push_back(p.asString());
                    }
                }

                if (iface.get("action", "").asString() == "delete") {
                    std::cout << "[Agent] deleting bridge: " << name << std::endl;
                    auto error = AgentNetworkConfigurator::deleteBridge(name);
                    if (!error.empty()) {
                        std::cout << "[ERROR] " << "[Agent] delete bridge " << name << ": " << error << std::endl;
                        sendNetworkConfigFailed(name, error);
                        return;
                    }
                    std::cout << "[Agent] bridge " << name << " deleted" << std::endl;
                    continue;
                }

                // 检查桥接口是否已存在
                bool bridgeExists = AgentCapabilitiesCollector::isBridgeInterface(name);
                if (!bridgeExists) {
                    std::cout << "[Agent] creating bridge: " << name
                              << ", ports=" << ports.size() << std::endl;
                    auto error = AgentNetworkConfigurator::createBridge(name, ports);
                    if (!error.empty()) {
                        std::cout << "[ERROR] " << "[Agent] create bridge " << name << ": " << error << std::endl;
                        sendNetworkConfigFailed(name, error);
                        return;
                    }
                } else if (!ports.empty()) {
                    std::cout << "[Agent] syncing bridge ports: " << name
                              << ", desired=" << ports.size() << std::endl;
                    auto error = AgentNetworkConfigurator::setBridgePorts(name, ports);
                    if (!error.empty()) {
                        std::cout << "[ERROR] " << "[Agent] set bridge ports " << name << ": " << error << std::endl;
                        sendNetworkConfigFailed(name, error);
                        return;
                    }
                }

                // 桥创建后如果没指定 IP 模式，跳过地址配置
                if (mode == "none" || mode == "disabled") {
                    std::cout << "[Agent] bridge " << name << " configured (no address)" << std::endl;
                    continue;
                }

                // 继续下面的 IP 配置流程
            }

            // ========== 地址配置 ==========
            if (mode == "dhcp") {
                std::cout << "[Agent] configuring interface: " << name << ", mode=DHCP" << std::endl;
                auto error = AgentNetworkConfigurator::applyDhcp(name);
                if (!error.empty()) {
                    std::cout << "[ERROR] " << "[Agent] DHCP failed on " << name << ": " << error << std::endl;
                    sendNetworkConfigFailed(name, error);
                    return;
                }
            } else if (mode == "static") {
                const auto ip = iface.get("ip", "").asString();
                const int prefixLength = iface.get("prefix_length", 24).asInt();
                const auto gateway = iface.get("gateway", "").asString();

                std::cout << "[Agent] configuring interface: " << name
                          << ", mode=static, ip=" << ip << "/" << prefixLength
                          << ", gateway=" << (gateway.empty() ? "(none)" : gateway) << std::endl;

                auto error = AgentNetworkConfigurator::applyInterfaceAddress(name, ip, prefixLength);
                if (!error.empty()) {
                    std::cout << "[ERROR] " << "[Agent] failed to configure " << name << ": " << error << std::endl;
                    sendNetworkConfigFailed(name, error);
                    return;
                }

                if (!gateway.empty()) {
                    error = AgentNetworkConfigurator::applyDefaultGateway(name, gateway);
                    if (!error.empty()) {
                        std::cout << "[ERROR] " << "[Agent] failed to set gateway on " << name << ": " << error << std::endl;
                        sendNetworkConfigFailed(name, error);
                        return;
                    }
                }
            }

            std::cout << "[Agent] interface " << name << " configured successfully" << std::endl;
        }

        // 配置成功，刷新 capabilities 并回报
        Json::Value ackData(Json::objectValue);
        ackData["capabilities"] = AgentCapabilitiesCollector::collectCapabilities();
        sendMessage(agent::MESSAGE_NETWORK_CONFIG_APPLIED, ackData);

        std::cout << "[Agent] network config applied successfully" << std::endl;
    }

    void sendNetworkConfigFailed(const std::string& interfaceName, const std::string& error) {
        Json::Value data(Json::objectValue);
        data["interfaceName"] = interfaceName;
        data["error"] = error;
        sendMessage(agent::MESSAGE_NETWORK_CONFIG_FAILED, data);
    }

    // ==================== Shell ====================

    void handleShellOpen(const Json::Value& data) {
        int cols = data.get("cols", 80).asInt();
        int rows = data.get("rows", 24).asInt();

        auto error = shellManager_.open(cols, rows,
            // onData: PTY 输出 → server（二进制帧 + 可选 zlib 压缩）
            [this](const std::string& output) {
                sendShellOutput(output);
            },
            // onClose: PTY 退出 → server
            [this](int exitCode) {
                Json::Value d(Json::objectValue);
                d["exitCode"] = exitCode;
                sendMessage(agent::MESSAGE_SHELL_CLOSED, d);
                std::cout << "[AgentShell] session ended, exitCode=" << exitCode << std::endl;
            }
        );

        Json::Value reply(Json::objectValue);
        if (error.empty()) {
            reply["success"] = true;
            sendMessage(agent::MESSAGE_SHELL_OPENED, reply);
        } else {
            reply["success"] = false;
            reply["error"] = error;
            sendMessage(agent::MESSAGE_SHELL_OPENED, reply);
            std::cout << "[AgentShell] open failed: " << error << std::endl;
        }
    }

    // ==================== 设备命令 ====================

    /**
     * @brief 处理 Server 下发的设备写命令（device:command）
     *
     * 通过 Modbus 轮询器执行写入并回读确认，结果回报给 Server。
     */
    void handleDeviceCommand(const Json::Value& data) {
        const auto commandKey = data.get("commandKey", "").asString();
        const int deviceId = data.get("deviceId", 0).asInt();
        const auto& elements = data["elements"];
        const int readbackCount = data.get("readbackCount", 3).asInt();

        if (commandKey.empty() || deviceId <= 0 || !elements.isArray() || elements.empty()) {
            std::cout << "[Agent] device:command invalid params" << std::endl;
            return;
        }

        std::cout << "[Agent] device:command received: key=" << commandKey
                  << ", deviceId=" << deviceId
                  << ", elements=" << elements.size()
                  << ", readback=" << readbackCount << std::endl;

        modbusPoller_.executeWriteCommand(deviceId, elements, readbackCount,
            [this, commandKey](bool success, const std::string& message) {
                Json::Value result(Json::objectValue);
                result["commandKey"] = commandKey;
                result["success"] = success;
                result["message"] = message;
                sendMessage(agent::MESSAGE_DEVICE_COMMAND_RESULT, result);

                std::cout << "[Agent] device:command:result key=" << commandKey
                          << ", success=" << (success ? "true" : "false")
                          << ", message=" << message << std::endl;
            });
    }

    void handleSendCommand(const Json::Value& data) {
        const int deviceId = data.get("deviceId", 0).asInt();
        const auto payloadBase64 = data.get("payload", "").asString();
        if (deviceId <= 0 || payloadBase64.empty()) {
            return;
        }

        const auto payload = drogon::utils::base64Decode(payloadBase64);
        const auto clientAddr = data.get("clientAddr", "").asString();

        std::cout << "[Agent] device:send deviceId=" << deviceId
                  << ", bytes=" << payload.size()
                  << ", clientAddr=" << (clientAddr.empty() ? "(broadcast)" : clientAddr) << std::endl;

        // 查找该设备所在的端点
        for (const auto& [epId, runtime] : endpointsByEpId_) {
            (void)epId;
            for (const auto& dev : runtime->endpoint.devices) {
                if (dev.id == deviceId) {
                    if (!clientAddr.empty()) {
                        (void)TcpLinkManager::instance().sendToClient(runtime->key, clientAddr, payload);
                    } else {
                        (void)TcpLinkManager::instance().sendData(runtime->key, payload);
                    }
                    return;
                }
            }
        }

        std::cout << "[WARN] " << "[Agent] device:send target device not found, deviceId=" << deviceId << std::endl;
    }

    /**
     * @brief TCP 端点收到数据
     *
     * 优先本地解析（device:parsed），解析失败或无配置时 fallback 到原始透传（device:data）。
     * 解析结果先存入 SQLite 缓存，再尝试即时上报。
     */
    void onEndpointData(int epKey, const std::string& clientAddr, const std::string& data) {
        auto it = endpointsByKey_.find(epKey);
        if (it == endpointsByKey_.end() || !it->second) return;

        const auto& runtime = *it->second;
        const auto& ep = runtime.endpoint;
        if (ep.devices.empty()) return;

        const auto& endpointId = ep.id;

        std::cout << "[Agent] data received: endpoint=" << endpointId
                  << ", client=" << clientAddr
                  << ", bytes=" << data.size() << std::endl;

        const auto protocol = protocolEngine_.getProtocol(endpointId);

        if (protocol == Constants::PROTOCOL_MODBUS) {
            // Modbus 响应交给轮询器解析
            modbusPoller_.onData(epKey, clientAddr, data);
            return;
        }
        if (protocol == Constants::PROTOCOL_S7) {
            // S7 由轮询器主动采集，忽略被动上行帧
            return;
        }

        // SL651 或其他协议：尝试本地解析
        auto results = protocolEngine_.parseData(endpointId, clientAddr, data);

        if (!results.empty()) {
            std::cout << "[Agent] parsed " << results.size() << " frame(s) from endpoint=" << endpointId
                      << ", protocol=" << protocol << ", client=" << clientAddr << std::endl;
            for (const auto& r : results) {
                std::cout << "[Agent]   deviceId=" << r.deviceId
                          << ", funcCode=" << r.funcCode
                          << ", reportTime=" << r.reportTime << std::endl;
            }

            // 解析成功 → 存入 SQLite 缓存
            dataStore_.storeBatch(results);

            // 在线时立即尝试上报
            if (connected_) {
                loop_->queueInLoop([this]() {
                    flushPendingData();
                });
            }
        } else if (protocolEngine_.hasConfig(endpointId)) {
            // 有配置但解析失败（可能数据不完整，等待更多数据）
            std::cout << "[Agent] incomplete frame buffered: endpoint=" << endpointId
                      << ", bytes=" << data.size() << std::endl;
        } else {
            // 无协议配置 → fallback 到原始透传
            std::cout << "[Agent] no protocol config, fallback raw: endpoint=" << endpointId << std::endl;
            sendRawDeviceData(epKey, clientAddr, data);
        }
    }

    /**
     * @brief 原始数据透传（fallback 模式）
     */
    void sendRawDeviceData(int epKey, const std::string& clientAddr, const std::string& data) {
        auto it = endpointsByKey_.find(epKey);
        if (it == endpointsByKey_.end() || !it->second) return;

        const auto& ep = it->second->endpoint;
        if (ep.devices.empty()) return;

        int deviceId = ep.devices.front().id;

        Json::Value payload(Json::objectValue);
        payload["deviceId"] = deviceId;
        payload["endpointId"] = ep.id;
        payload["clientAddr"] = clientAddr;
        payload["payload"] = drogon::utils::base64Encode(data);
        sendMessage(agent::MESSAGE_DEVICE_DATA, payload);
    }

    void onEndpointConnection(int epKey, const std::string& clientAddr, bool connected) {
        auto it = endpointsByKey_.find(epKey);
        if (it == endpointsByKey_.end() || !it->second) return;

        const auto& ep = it->second->endpoint;

        std::cout << "[Agent] endpoint " << (connected ? "connected" : "disconnected")
                  << ": id=" << ep.id << ", client=" << clientAddr << std::endl;

        Json::Value payload(Json::objectValue);
        payload["endpointId"] = ep.id;
        payload["clientAddr"] = clientAddr;
        payload["connected"] = connected;
        sendMessage(agent::MESSAGE_ENDPOINT_CONNECTION, payload);

        loop_->queueInLoop([this]() {
            sendStatusSnapshot();
        });
    }

    // ==================== 缓存续传 ====================

    /**
     * @brief 上报缓存中的待发送数据
     *
     * 从 SQLite 取出 pending 记录，组装 device:parsed 消息批量上报。
     * Server ACK 后通过 handleParsedDataAck() 标记为已发送。
     */
    void flushPendingData() {
        if (!connected_) return;

        // ACK 超时保护：如果 10 秒未收到 ACK，重置 flushInProgress 允许重发
        if (flushInProgress_) {
            auto elapsed = std::chrono::steady_clock::now() - lastFlushAt_;
            if (elapsed < std::chrono::seconds(10)) return;
            std::cout << "[WARN] " << "[Agent] ACK timeout after 10s, resetting flush state for retry" << std::endl;
            flushInProgress_ = false;
        }

        auto pending = dataStore_.fetchPending(100);
        if (pending.empty()) return;

        flushInProgress_ = true;
        lastFlushAt_ = std::chrono::steady_clock::now();

        Json::Value batch(Json::arrayValue);
        Json::Value cacheIds(Json::arrayValue);

        for (const auto& record : pending) {
            Json::Value item(Json::objectValue);
            item["deviceId"] = record.deviceId;
            item["protocol"] = record.protocol;
            item["funcCode"] = record.funcCode;
            item["data"] = record.data;
            item["reportTime"] = record.reportTime;
            batch.append(std::move(item));
            cacheIds.append(static_cast<Json::Int64>(record.id));
        }

        std::cout << "[Agent] flushing " << pending.size() << " record(s) to platform"
                  << ", cacheIds=[" << pending.front().id << ".." << pending.back().id << "]" << std::endl;

        Json::Value data(Json::objectValue);
        data["batch"] = std::move(batch);
        data["cacheIds"] = std::move(cacheIds);
        sendMessage(agent::MESSAGE_DEVICE_PARSED, data);

        // 缓存当前批次的 cacheIds，等 ACK 后标记
        pendingAckIds_.clear();
        for (const auto& record : pending) {
            pendingAckIds_.push_back(record.id);
        }
    }

    /**
     * @brief 处理 device:parsed:ack，标记 SQLite 记录为已发送
     */
    void handleParsedDataAck(const Json::Value& data) {
        std::vector<int64_t> ids;

        if (data.isMember("cacheIds") && data["cacheIds"].isArray()) {
            for (const auto& id : data["cacheIds"]) {
                ids.push_back(id.asInt64());
            }
        }

        if (ids.empty() && !pendingAckIds_.empty()) {
            ids = std::move(pendingAckIds_);
        }

        if (!ids.empty()) {
            std::cout << "[Agent] ACK received, marking " << ids.size() << " record(s) as sent" << std::endl;
            dataStore_.markSent(ids);
        }

        flushInProgress_ = false;
        pendingAckIds_.clear();

        // 如果还有待发送数据，继续下一批
        auto remaining = dataStore_.pendingCount();
        if (remaining > 0 && connected_) {
            std::cout << "[Agent] " << remaining << " record(s) remaining, scheduling next flush" << std::endl;
            loop_->queueInLoop([this]() {
                flushPendingData();
            });
        }
    }

    Json::Value buildRuntimePayload() const {
        auto runtime = AgentCapabilitiesCollector::collectRuntime(static_cast<int>(endpointsByEpId_.size()));
        runtime["lastAppliedConfigVersion"] = static_cast<Json::Int64>(lastAppliedConfigVersion_);
        return runtime;
    }

    void checkInterfaceChanges() {
        const auto currentFingerprint = computeInterfacesFingerprint();
        if (currentFingerprint.empty() || currentFingerprint == lastInterfacesFingerprint_) {
            return;
        }

        lastInterfacesFingerprint_ = currentFingerprint;
        std::cout << "[Agent] network interfaces changed, reporting snapshot" << std::endl;
        sendHeartbeat(true);
    }

    std::string computeInterfacesFingerprint() const {
        const auto capabilities = AgentCapabilitiesCollector::collectCapabilities();
        const auto& interfaces = capabilities["interfaces"];
        if (!interfaces.isArray()) {
            return {};
        }

        std::string fingerprint;
        for (const auto& iface : interfaces) {
            fingerprint += iface.get("name", "").asString();
            fingerprint += '|';
            fingerprint += iface.get("up", false).asBool() ? "1" : "0";
            fingerprint += '|';
            fingerprint += iface.get("ip", "").asString();
            fingerprint += '|';
            fingerprint += std::to_string(iface.get("prefix_length", 0).asInt());
            fingerprint += '|';
            fingerprint += iface.get("method", "").asString();
            fingerprint += '|';
            fingerprint += iface.get("gateway", "").asString();
            fingerprint += '|';

            const auto& ports = iface["bridge_ports"];
            if (ports.isArray()) {
                for (const auto& port : ports) {
                    fingerprint += port.asString();
                    fingerprint += ',';
                }
            }
            fingerprint += ';';
        }
        return fingerprint;
    }

    // ==================== 验证 ====================

    std::string validateEndpoints(const std::vector<agent::DeviceEndpoint>& endpoints,
                                  const Json::Value& /*interfaces*/) const {
        for (const auto& ep : endpoints) {
            if (const auto error = validateEndpoint(ep); !error.empty()) {
                return error;
            }
        }
        return {};
    }

    std::string validateEndpoint(const agent::DeviceEndpoint& ep) const {
        if (ep.transport == "serial") {
            if (ep.channel.empty()) {
                return "端点[" + ep.id + "] 串口端点缺少通道";
            }
            return {};
        }

        // ethernet
        if (ep.mode != Constants::LINK_MODE_TCP_SERVER &&
            ep.mode != Constants::LINK_MODE_TCP_CLIENT) {
            return "端点[" + ep.id + "] 模式不受支持: " + ep.mode;
        }
        if (ep.ip.empty() || !AgentNetworkConfigurator::isValidIpv4(ep.ip)) {
            return "端点[" + ep.id + "] IP格式错误";
        }
        if (ep.port <= 0 || ep.port > 65535) {
            return "端点[" + ep.id + "] 端口不合法";
        }
        return {};
    }

    // ==================== 回滚 ====================

    std::string rollbackEndpoints(
        const std::unordered_map<std::string, std::shared_ptr<EndpointRuntime>>& previousEndpoints,
        const std::string& reason) {

        try {
            // 停止所有当前端点
            for (const auto& [epId, runtime] : endpointsByEpId_) {
                (void)epId;
                TcpLinkManager::instance().stop(runtime->key);
            }

            // 恢复旧端点
            endpointsByEpId_.clear();
            keyByEpId_.clear();
            endpointsByKey_.clear();

            for (const auto& [epId, runtime] : previousEndpoints) {
                (void)epId;
                keyByEpId_[runtime->endpoint.id] = runtime->key;
                endpointsByKey_[runtime->key] = runtime;
                applyEndpoint(*runtime);
            }
            endpointsByEpId_ = previousEndpoints;
        } catch (const std::exception& rollbackError) {
            return reason + std::string("；回滚失败: ") + rollbackError.what();
        }

        return reason + std::string("；已回滚到上一版配置");
    }

    // ==================== L2 二层配置 ====================

    void startL2Bridge() {
        l2Bridge_.setDiscoverHandler([this](const l2config::MacAddr& localMac) -> std::string {
            Json::Value info(Json::objectValue);
            info["mac"] = l2config::macToString(localMac);
            info["sn"] = config_.sn;
            info["model"] = config_.model;
            info["version"] = IOT_AGENT_VERSION;
            info["interfaces"] = AgentCapabilitiesCollector::collectCapabilities()["interfaces"];
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            return Json::writeString(writer, info);
        });

        auto error = l2Bridge_.start();
        if (!error.empty()) {
            std::cout << "[Agent] L2ConfigBridge: " << error << std::endl;
        }
    }

    // ==================== 网络配置 ====================
    // 网络配置已移至 Agent 配置层面，端点只负责 TCP 监听/连接

    // ==================== 工具方法 ====================

    void sendConfigApplied(agent::ConfigVersion configVersion) {
        Json::Value data(Json::objectValue);
        data["configVersion"] = static_cast<Json::Int64>(configVersion);
        data["runtime"] = buildRuntimePayload();
        sendMessage(agent::MESSAGE_CONFIG_APPLIED, data);
    }

    void sendConfigApplyFailed(agent::ConfigVersion configVersion, const std::string& error) {
        Json::Value data(Json::objectValue);
        data["configVersion"] = static_cast<Json::Int64>(configVersion);
        data["error"] = error;
        data["runtime"] = buildRuntimePayload();
        sendMessage(agent::MESSAGE_CONFIG_APPLY_FAILED, data);
    }

    /**
     * @brief 发送 shell 输出（二进制帧，可选 zlib 压缩）
     *
     * 帧格式: [1字节 flags] [payload]
     *   flags bit 0: 1=zlib 压缩, 0=原始数据
     *
     * 相比 JSON 封装：消除序列化开销，服务端零拷贝转发，
     * 压缩后在 4G 网络下带宽占用降低 60-80%。
     */
    void sendShellOutput(const std::string& output) {
        constexpr size_t kCompressThreshold = 256;

        std::string frame;
        if (output.size() >= kCompressThreshold) {
            auto compressed = agent::zlibDeflate(output);
            if (!compressed.empty() && compressed.size() < output.size()) {
                frame.reserve(1 + compressed.size());
                frame.push_back(static_cast<char>(agent::FRAME_SHELL_ZLIB));
                frame.append(compressed);
            }
        }
        if (frame.empty()) {
            frame.reserve(1 + output.size());
            frame.push_back(static_cast<char>(agent::FRAME_SHELL_RAW));
            frame.append(output);
        }
        sendBinary(std::move(frame));
    }

    void sendBinary(std::string data) {
        loop_->queueInLoop([this, data = std::move(data)]() {
            if (!wsClient_) return;
            auto conn = wsClient_->getConnection();
            if (!conn || !conn->connected()) return;
            conn->send(data, drogon::WebSocketMessageType::Binary);
        });
    }

    void sendMessage(const std::string& type, const Json::Value& data) {
        auto frame = agent::buildBinaryMessage(type, data);
        loop_->queueInLoop([this, frame = std::move(frame)]() {
            if (!wsClient_) return;
            auto conn = wsClient_->getConnection();
            if (!conn || !conn->connected()) return;
            conn->send(frame, drogon::WebSocketMessageType::Binary);
        });
    }

private:
    EdgeNodeConfig config_;
    std::unique_ptr<trantor::EventLoop> loopOwned_;   // 拥有的 EventLoop（如果是自己创建的）
    trantor::EventLoop* loop_ = nullptr;              // 当前使用的 EventLoop 指针
    drogon::WebSocketClientPtr wsClient_;

    // 端点运行时状态（按 endpointId 和整数 key 双重索引）
    std::unordered_map<std::string, std::shared_ptr<EndpointRuntime>> endpointsByEpId_;
    std::unordered_map<int, std::shared_ptr<EndpointRuntime>> endpointsByKey_;
    std::unordered_map<std::string, int> keyByEpId_;  // endpointId → key
    int nextEndpointKey_ = 1;

    std::atomic<bool> connected_{false};
    std::atomic<bool> reconnectScheduled_{false};
    bool authFailed_ = false;
    int agentId_ = 0;
    agent::ConfigVersion lastAppliedConfigVersion_ = 0;
    std::string lastInterfacesFingerprint_;

    // 协议引擎 + 数据缓存
    EdgeNodeProtocolEngine protocolEngine_;
    EdgeNodeModbusPoller modbusPoller_;
    EdgeNodeS7Poller s7Poller_;
    EdgeNodeDataStore dataStore_;
    std::vector<agent::DeviceEndpoint> currentEndpoints_;  // 当前端点配置（含 protocolConfig）

    // Shell
    EdgeNodeShellManager shellManager_;

    // L2 二层配置
    EdgeNodeL2ConfigBridge l2Bridge_;

    // 续传状态
    bool flushInProgress_ = false;
    std::chrono::steady_clock::time_point lastFlushAt_;
    std::vector<int64_t> pendingAckIds_;

};

using AgentRuntime = EdgeNodeRuntime;

}  // namespace agent_app
