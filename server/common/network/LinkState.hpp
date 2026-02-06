#pragma once

#include "common/utils/Constants.hpp"

#include <cmath>
#include <random>
#include <string>

/**
 * @brief 链路连接状态枚举
 *
 * 替代原来散落在 TcpLinkManager 各处的魔术字符串。
 * Reconnecting 是内部状态，对外序列化为 "connecting"。
 */
enum class LinkState {
    Stopped,       // 未启动或已停止
    Listening,     // Server 模式：正在监听
    Connected,     // Client 模式：已连接
    Connecting,    // Client 模式：首次连接中
    Reconnecting,  // Client 模式：断线重连等待（对外显示为 connecting）
    Error          // 连接错误
};

/**
 * @brief 状态枚举 → JSON 字符串（与前端 ConnStatus 类型对齐）
 */
inline std::string linkStateToString(LinkState state) {
    switch (state) {
        case LinkState::Stopped:      return "stopped";
        case LinkState::Listening:    return "listening";
        case LinkState::Connected:    return "connected";
        case LinkState::Connecting:   return "connecting";
        case LinkState::Reconnecting: return "connecting";  // 对外同 Connecting
        case LinkState::Error:        return "error";
    }
    return "stopped";
}

/**
 * @brief 指数退避重连策略
 *
 * - 基础延迟 2 秒，指数增长
 * - 最大延迟 5 分钟（300 秒），无最大重试限制
 * - ±20% 随机抖动防止雷群效应
 */
class ReconnectPolicy {
public:
    /**
     * @brief 获取当前重试的延迟时间（秒）
     */
    double getDelay() const {
        double delay = Constants::RECONNECT_BASE_DELAY_SEC
            * std::pow(2.0, static_cast<double>(attempts_));
        delay = (std::min)(delay, Constants::RECONNECT_MAX_DELAY_SEC);

        // ±20% 随机抖动
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<double> dist(
            -Constants::RECONNECT_JITTER_RATIO, Constants::RECONNECT_JITTER_RATIO);
        delay *= (1.0 + dist(rng));

        return (std::max)(delay, Constants::RECONNECT_BASE_DELAY_SEC);
    }

    /**
     * @brief 记录一次重连尝试
     */
    void recordAttempt() { ++attempts_; }

    /**
     * @brief 重置（连接成功或手动停止时调用）
     */
    void reset() { attempts_ = 0; }

    int attempts() const { return attempts_; }

private:
    int attempts_ = 0;
};

/**
 * @brief 链路状态机
 *
 * 每个 LinkRuntime 持有一个实例，所有状态转换集中在此。
 *
 * 状态转换表：
 *   Server: Stopped →[startServer]→ Listening →[stop]→ Stopped
 *   Client: Stopped →[startClient]→ Connecting →[connected]→ Connected
 *           Connected →[disconnected]→ Reconnecting →[reconnectTimer]→ Connecting
 *           Connecting →[connError]→ Reconnecting
 *           Any →[stop]→ Stopped
 */
class LinkStateMachine {
public:
    LinkState state() const { return state_; }
    std::string stateString() const { return linkStateToString(state_); }

    // ==================== 状态事件 ====================

    void onStartServer() {
        transition(LinkState::Listening, "startServer");
    }

    void onStartClient() {
        transition(LinkState::Connecting, "startClient");
    }

    void onConnected() {
        transition(LinkState::Connected, "connected");
        reconnect_.reset();
    }

    void onDisconnected() {
        transition(LinkState::Reconnecting, "disconnected");
    }

    void onConnectionError(const std::string& reason = "") {
        errorMsg_ = reason;
        transition(LinkState::Reconnecting, "connError");
    }

    /**
     * @brief 重连定时器触发 → 进入 Connecting
     */
    void onReconnecting() {
        reconnect_.recordAttempt();
        transition(LinkState::Connecting, "reconnectTimer");
    }

    void onStop() {
        transition(LinkState::Stopped, "stop");
        reconnect_.reset();
        errorMsg_.clear();
    }

    // ==================== 重连策略 ====================

    double getReconnectDelay() const { return reconnect_.getDelay(); }
    int reconnectAttempts() const { return reconnect_.attempts(); }
    const std::string& errorMsg() const { return errorMsg_; }

private:
    LinkState state_ = LinkState::Stopped;
    ReconnectPolicy reconnect_;
    std::string errorMsg_;

    void transition(LinkState newState, const char* event) {
        if (state_ != newState) {
            LOG_DEBUG << "LinkFSM: " << linkStateToString(state_)
                      << " →[" << event << "]→ " << linkStateToString(newState);
            state_ = newState;
        }
    }
};
