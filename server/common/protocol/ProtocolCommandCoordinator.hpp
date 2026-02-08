#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "common/utils/DrogonLoopSelector.hpp"
#include "modules/device/domain/CommandRepository.hpp"

/**
 * @brief 协议命令协调器
 *
 * 负责：
 * - 待应答指令登记与并发互斥
 * - 指令等待超时
 * - 命令完成事件到达后的协程唤醒
 * - 下行记录与响应记录关联
 */
class ProtocolCommandCoordinator {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    using ResponseMatcher = std::function<bool(
        const std::string& requestCode,
        const std::string& responseCode)>;

    bool tryReserve(const std::string& commandKey, const std::string& requestCode,
                    int userId, int timeoutMs, int64_t downCommandId = 0,
                    ResponseMatcher responseMatcher = {}) {
        auto waitState = std::make_shared<CommandWaitState>();
        waitState->loop = trantor::EventLoop::getEventLoopOfCurrentThread();

        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto existIt = pendingCommands_.find(commandKey);
        if (existIt != pendingCommands_.end()) {
            if (std::chrono::steady_clock::now() < existIt->second.expireTime) {
                return false;
            }
            pendingCommands_.erase(existIt);
        }

        PendingCommand cmd;
        cmd.commandKey = commandKey;
        cmd.requestCode = requestCode;
        cmd.waitState = std::move(waitState);
        cmd.expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        cmd.downCommandId = downCommandId;
        cmd.userId = userId;
        cmd.responseMatcher = std::move(responseMatcher);
        pendingCommands_.emplace(commandKey, std::move(cmd));
        return true;
    }

    void attachDownCommandId(const std::string& commandKey, int64_t downCommandId) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto it = pendingCommands_.find(commandKey);
        if (it != pendingCommands_.end()) {
            it->second.downCommandId = downCommandId;
        }
    }

    void release(const std::string& commandKey) {
        if (commandKey.empty()) return;
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCommands_.erase(commandKey);
    }

    Task<bool> wait(const std::string& commandKey, int timeoutMs) {
        std::shared_ptr<CommandWaitState> waitState;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingCommands_.find(commandKey);
            if (it == pendingCommands_.end()) {
                co_return false;
            }
            waitState = it->second.waitState;
        }

        co_return co_await CommandAwaiter{waitState, timeoutMs};
    }

    void notifyCompletion(const std::string& commandKey, const std::string& responseCode,
                          bool success, int64_t responseRecordId) {
        int64_t downCommandId = 0;
        std::shared_ptr<CommandWaitState> waitState;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pendingCommands_.find(commandKey);
            if (it != pendingCommands_.end() && isMatchingResponse(it->second, responseCode)) {
                downCommandId = it->second.downCommandId;
                waitState = it->second.waitState;
                LOG_DEBUG << "[ProtocolCommandCoordinator] Command completion handled: key=" << commandKey
                          << ", requestCode=" << it->second.requestCode
                          << ", responseCode=" << responseCode << ", success=" << success;
            }
        }

        if (waitState) {
            resolveWaitState(waitState, success);
        }

        if (downCommandId > 0 && responseRecordId > 0) {
            auto* ioLoop = DrogonLoopSelector::getNext();
            ioLoop->queueInLoop([downCommandId, responseRecordId]() {
                drogon::async_run([downCommandId, responseRecordId]() -> Task<> {
                    try {
                        co_await CommandRepository::linkResponse(downCommandId, responseRecordId);
                    } catch (const std::exception& e) {
                        LOG_WARN << "[ProtocolCommandCoordinator] updateDownCommandResponse failed: "
                                 << "downCommandId=" << downCommandId
                                 << ", responseId=" << responseRecordId
                                 << ", error=" << e.what();
                    }
                });
            });
        }
    }

    size_t pendingCount() const {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        return pendingCommands_.size();
    }

private:
    struct CommandWaitState {
        trantor::EventLoop* loop = nullptr;
        std::coroutine_handle<> handle;
        trantor::TimerId timerId{0};
        std::atomic<bool> resolved{false};
        std::atomic<bool> suspended{false};
        std::atomic_flag resumedOnce = ATOMIC_FLAG_INIT;  // 保证只 resume 一次
        bool result = false;
    };

    /**
     * @brief 安全地解决等待状态并唤醒协程
     *
     * 使用 resolved CAS + resumedOnce 双重保护：
     * - resolved 保证只有一方（定时器或应答）设置结果
     * - resumedOnce 保证 handle.resume() 最多调用一次
     */
    static void resolveWaitState(const std::shared_ptr<CommandWaitState>& state, bool success) {
        if (!state->resolved.exchange(true, std::memory_order_acq_rel)) {
            state->result = success;
            if (state->suspended.load(std::memory_order_acquire)) {
                // 协程已挂起，需要通过 queueInLoop 在正确的线程上恢复
                state->loop->queueInLoop([state]() {
                    state->loop->invalidateTimer(state->timerId);
                    if (state->resumedOnce.test_and_set(std::memory_order_acq_rel) == false) {
                        state->handle.resume();
                    }
                });
            }
            // 如果 suspended 为 false，await_suspend 中的双重检查会处理
        }
    }

    struct CommandAwaiter {
        std::shared_ptr<CommandWaitState> state;
        int timeoutMs;

        bool await_ready() const noexcept {
            return state->resolved.load(std::memory_order_acquire);
        }

        bool await_suspend(std::coroutine_handle<> h) {
            state->handle = h;

            std::weak_ptr<CommandWaitState> weak = state;
            state->timerId = state->loop->runAfter(
                static_cast<double>(timeoutMs) / 1000.0,
                [weak]() {
                    if (auto s = weak.lock()) {
                        resolveWaitState(s, false);
                    }
                }
            );

            // 在设置 suspended 之前检查是否已解决（notifyCompletion 可能在
            // runAfter 和这里之间已经设置了 resolved）
            if (state->resolved.load(std::memory_order_acquire)) {
                state->loop->invalidateTimer(state->timerId);
                return false;
            }

            state->suspended.store(true, std::memory_order_release);

            // 双重检查：suspended 设置后 resolved 可能已改变
            if (state->resolved.load(std::memory_order_acquire)) {
                // 已解决但可能没人 resume（因为 suspended 刚设置），我们自己处理
                if (state->resumedOnce.test_and_set(std::memory_order_acq_rel) == false) {
                    state->loop->invalidateTimer(state->timerId);
                    // 返回 false 表示不挂起，直接返回结果
                    return false;
                }
            }

            return true;
        }

        bool await_resume() const noexcept {
            return state->result;
        }
    };

    struct PendingCommand {
        std::string commandKey;
        std::string requestCode;
        std::shared_ptr<CommandWaitState> waitState;
        std::chrono::steady_clock::time_point expireTime;
        int64_t downCommandId = 0;
        int userId = 0;
        ResponseMatcher responseMatcher;
    };

    static bool isMatchingResponse(const PendingCommand& command, const std::string& responseFunc) {
        if (command.responseMatcher) {
            return command.responseMatcher(command.requestCode, responseFunc);
        }
        return command.requestCode == responseFunc;
    }

    std::map<std::string, PendingCommand> pendingCommands_;
    mutable std::mutex pendingMutex_;
};
