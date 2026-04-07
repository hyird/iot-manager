#pragma once

#include "FrameResult.hpp"
#include "../utils/AppException.hpp"
#include "ProtocolCommandCoordinator.hpp"
#include "ProtocolCommandStore.hpp"
#include "common/cache/DeviceCache.hpp"

#include <drogon/drogon.h>
#include <json/json.h>

#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using ParsedResultSubmitter = std::function<void(std::vector<ParsedFrameResult>&&)>;
using CommandCompletionNotifier = std::function<void(
    const std::string& commandKey,
    const std::string& responseCode,
    bool success,
    int64_t responseRecordId)>;

struct ProtocolRuntimeContext {
    ParsedResultSubmitter submitParsedResults;
    ProtocolCommandCoordinator& commandCoordinator;
    CommandCompletionNotifier notifyCommandCompletion;
    const ProtocolCommandStore& commandStore;
};

struct CommandRequest {
    int linkId = 0;
    int deviceId = 0;
    std::string deviceCode;
    std::string funcCode;
    Json::Value elements;
    int userId = 0;
    int timeoutMs = 10000;
};

enum class CommandStatus {
    Success,
    DeviceOffline,
    DeviceBusy,
    SendFailed,
    Timeout,
    Error
};

struct CommandResult {
    CommandStatus status = CommandStatus::Error;
    std::string message;

    bool ok() const { return status == CommandStatus::Success; }

    AppException toException() const {
        using enum drogon::HttpStatusCode;

        switch (status) {
        case CommandStatus::Success:
            return AppException(ErrorCodes::SUCCESS, "Success", k200OK);
        case CommandStatus::DeviceOffline:
            return AppException(
                ErrorCodes::EXTERNAL_SERVICE_ERROR,
                message.empty() ? "设备离线" : message,
                k503ServiceUnavailable
            );
        case CommandStatus::DeviceBusy:
            return ConflictException(
                message.empty() ? "设备有未完成的指令" : message
            );
        case CommandStatus::SendFailed:
            return AppException(
                ErrorCodes::EXTERNAL_SERVICE_ERROR,
                message.empty() ? "发送失败" : message,
                k502BadGateway
            );
        case CommandStatus::Timeout:
            return AppException(
                ErrorCodes::EXTERNAL_SERVICE_ERROR,
                message.empty() ? "设备应答超时" : message,
                k504GatewayTimeout
            );
        case CommandStatus::Error:
        default:
            return AppException(
                ErrorCodes::INTERNAL_ERROR,
                message.empty() ? "未知错误" : message,
                k500InternalServerError
            );
        }
    }

    static CommandResult success() {
        return {CommandStatus::Success, "指令下发成功，设备已应答"};
    }
    static CommandResult offline(std::string msg = "设备离线") {
        return {CommandStatus::DeviceOffline, std::move(msg)};
    }
    static CommandResult busy(std::string msg = "设备有未完成的指令") {
        return {CommandStatus::DeviceBusy, std::move(msg)};
    }
    static CommandResult sendFailed(std::string msg = "发送失败") {
        return {CommandStatus::SendFailed, std::move(msg)};
    }
    static CommandResult timeout(std::string msg = "设备应答超时") {
        return {CommandStatus::Timeout, std::move(msg)};
    }
    static CommandResult error(std::string msg = "未知错误") {
        return {CommandStatus::Error, std::move(msg)};
    }
};

struct ProtocolAdapterMetrics {
    bool available = false;
    Json::Value stats = Json::Value(Json::objectValue);
};

enum class DeviceLifecycleAction {
    Created,
    Updated,
    Deleted
};

struct DeviceLifecycleEvent {
    DeviceLifecycleAction action = DeviceLifecycleAction::Updated;
    int deviceId = 0;
    int linkId = 0;
    std::string protocol;
    std::string deviceCode;
    bool registrationChanged = false;
};

enum class ProtocolConfigLifecycleAction {
    Created,
    Updated,
    Deleted
};

struct ProtocolConfigLifecycleEvent {
    ProtocolConfigLifecycleAction action = ProtocolConfigLifecycleAction::Updated;
    int configId = 0;
    std::string protocol;
    std::string name;
};

enum class ProtocolLifecycleImpact {
    None,
    Reload
};

class ProtocolAdapter {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    explicit ProtocolAdapter(ProtocolRuntimeContext runtimeContext)
        : runtimeContext_(std::move(runtimeContext)) {}

    virtual ~ProtocolAdapter() = default;

    virtual std::string_view protocol() const = 0;

    /**
     * @brief 协议运行时初始化
     *
     * 在进程启动或协议适配器首次装载时调用，通常用于构建初始配置视图、
     * 清理缓存和预热调度器。
     */
    virtual Task<> initializeAsync() = 0;

    /**
     * @brief 协议运行时重载
     *
     * 在设备配置、协议配置或拓扑关系变化后调用，用于刷新配置快照，
     * 并按协议语义恢复当前的运行态。
     */
    virtual Task<> reloadAsync() = 0;

    /**
     * @brief 物理连接状态变化
     */
    virtual void onConnectionChanged(int linkId, const std::string& clientAddr, bool connected) = 0;

    /**
     * @brief 收到链路原始数据
     */
    virtual void onDataReceived(int linkId, const std::string& clientAddr, std::vector<uint8_t> bytes) = 0;

    /**
     * @brief 周期性维护入口
     */
    virtual void onMaintenanceTick() = 0;
    virtual ProtocolAdapterMetrics getMetrics() const { return {}; }
    virtual ProtocolLifecycleImpact onDeviceLifecycleEvent(const DeviceLifecycleEvent&) {
        return ProtocolLifecycleImpact::None;
    }
    virtual ProtocolLifecycleImpact onProtocolConfigLifecycleEvent(const ProtocolConfigLifecycleEvent&) {
        return ProtocolLifecycleImpact::None;
    }

    virtual Task<CommandResult> sendCommand(const CommandRequest& req) = 0;

    /**
     * @brief 检查设备是否在线（基于实际连接状态）
     */
    virtual bool isDeviceConnected(int /*deviceId*/) const { return false; }

    /**
     * @brief 获取设备的统一标识符（对外暴露）
     *
     * SL651: 直接返回 deviceCode（协议原生）
     * Modbus/S7: 返回数据库中配置的 deviceCode，若为空则返回 "modbus:<id>" / "s7:<id>"
     *
     * 用于日志、WebSocket 推送、Webhook 等对外部暴露的场景，
     * 确保各协议设备标识格式统一。
     */
    virtual std::string getDeviceCode(int deviceId) const {
        auto device = DeviceCache::instance().findByIdSync(deviceId);
        if (!device) return std::to_string(deviceId);
        if (!device->deviceCode.empty()) return device->deviceCode;
        return std::string(protocol()) + ":" + std::to_string(deviceId);
    }

protected:
    /**
     * @brief 遍历会话快照并对满足条件的会话执行回调
     *
     * 只负责“重放运行态会话”，不关心具体协议里的 Bound / Activated 语义，
     * 由协议自己决定 predicate 和 action。
     */
    template<typename SessionList, typename Predicate, typename Action>
    void replaySessions(const SessionList& sessions, Predicate predicate, Action action) {
        for (const auto& session : sessions) {
            if (predicate(session)) {
                action(session);
            }
        }
    }

    /**
     * @brief 遍历已绑定会话，并为每个会话解析对应的运行对象后执行回调
     *
     * 适用于“配置重载后，恢复当前运行态”的场景。
     */
    template<typename SessionManagerT, typename Predicate, typename ResolveDtuFn, typename Action>
    void replayBoundSessions(
        SessionManagerT* sessionManager,
        Predicate predicate,
        ResolveDtuFn resolveDtu,
        Action action
    ) {
        if (!sessionManager) {
            return;
        }

        replaySessions(
            sessionManager->listSessions(),
            std::move(predicate),
            [&](const auto& session) {
                auto dtuOpt = resolveDtu(session);
                if (dtuOpt) {
                    action(*dtuOpt, session);
                }
            }
        );
    }

    /**
     * @brief RAII 守卫：确保命令协调器槽位在离开作用域时被释放
     */
    struct CommandGuard {
        ProtocolCommandCoordinator& coordinator;
        std::string key;

        CommandGuard(ProtocolCommandCoordinator& coord, std::string k)
            : coordinator(coord), key(std::move(k)) {}
        ~CommandGuard() {
            if (!key.empty()) {
                coordinator.release(key);
            }
        }
        void release() { key.clear(); }

        CommandGuard(const CommandGuard&) = delete;
        CommandGuard& operator=(const CommandGuard&) = delete;
        CommandGuard(CommandGuard&& other) noexcept
            : coordinator(other.coordinator), key(std::move(other.key)) {
            other.key.clear();
        }
        CommandGuard& operator=(CommandGuard&&) = delete;
    };

    CommandGuard makeCommandGuard() {
        return CommandGuard{runtimeContext_.commandCoordinator, {}};
    }

    /**
     * @brief 等待命令应答并更新最终状态（SUCCESS / TIMEOUT）
     */
    Task<CommandResult> awaitCommandResponse(const std::string& commandKey, int timeoutMs,
                                              int64_t downCommandId) {
        bool success = co_await runtimeContext_.commandCoordinator.wait(commandKey, timeoutMs);
        if (success) {
            co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SUCCESS", "");
            co_return CommandResult::success();
        } else {
            co_await runtimeContext_.commandStore.updateCommandStatus(
                downCommandId, "TIMEOUT", "设备应答超时");
            co_return CommandResult::timeout();
        }
    }

    /**
     * @brief 保存 SEND_FAILED 记录（快捷方法）
     */
    Task<int64_t> saveFailedCommand(int deviceId, int linkId, const std::string& protocolName,
                                     const std::string& funcCode, const std::string& funcName,
                                     const std::string& rawHex, int userId,
                                     const std::string& failReason,
                                     Json::Value elementsData = Json::Value(Json::objectValue)) {
        auto record = ProtocolCommandStore::CommandRecord::create(
            deviceId, linkId, protocolName, funcCode, funcName,
            rawHex, userId, "SEND_FAILED", failReason, std::move(elementsData));
        co_return co_await runtimeContext_.commandStore.saveCommand(std::move(record));
    }

    /**
     * @brief 保存 PENDING 记录（快捷方法）
     */
    Task<int64_t> savePendingCommand(int deviceId, int linkId, const std::string& protocolName,
                                      const std::string& funcCode, const std::string& funcName,
                                      const std::string& rawHex, int userId,
                                      Json::Value elementsData = Json::Value(Json::objectValue)) {
        auto record = ProtocolCommandStore::CommandRecord::create(
            deviceId, linkId, protocolName, funcCode, funcName,
            rawHex, userId, "PENDING", "", std::move(elementsData));
        co_return co_await runtimeContext_.commandStore.saveCommand(std::move(record));
    }

    ProtocolRuntimeContext runtimeContext_;
};
