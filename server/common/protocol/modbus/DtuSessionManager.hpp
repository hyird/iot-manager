#pragma once

#include "Modbus.SessionTypes.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace modbus {

/**
 * @brief DTU 会话管理器
 *
 * 管理运行态 session：
 * - 唯一键：linkId + clientAddr
 * - 绑定后建立 clientAddr + slaveId -> deviceId 的在线路由
 */
class DtuSessionManager {
public:
    using SessionMutator = std::function<void(DtuSession&)>;
    using OldSessionDisplacedCallback = std::function<void(int linkId, const std::string& clientAddr)>;

    void setOldSessionDisplacedCallback(OldSessionDisplacedCallback cb) {
        oldSessionDisplacedCallback_ = std::move(cb);
    }

    /** 连接建立时创建或刷新会话 */
    void onConnected(int linkId, const std::string& clientAddr);

    /** 断连时销毁会话和在线路由 */
    void onDisconnected(int linkId, const std::string& clientAddr);

    /** 刷新活跃时间 */
    void touch(int linkId, const std::string& clientAddr);

    /** 获取单个会话快照 */
    std::optional<DtuSession> getSession(int linkId, const std::string& clientAddr) const;

    /** 按 dtuKey 获取已绑定会话 */
    std::optional<DtuSession> getBoundSessionByDtuKey(const std::string& dtuKey) const;

    /** 获取链路下所有未知会话 */
    std::vector<DtuSession> listUnknownSessions(int linkId) const;

    /** 获取所有会话快照 */
    std::vector<DtuSession> listSessions() const;

    /** 绑定会话到逻辑 DTU，并生成 slave 设备路由 */
    bool bindSession(int linkId, const std::string& clientAddr, const DtuDefinition& dtu);

    /** 原地修改会话运行态（供引擎更新队列、buffer、in-flight） */
    bool mutateSession(int linkId, const std::string& clientAddr, const SessionMutator& mutator);

    /** 获取在线路由：clientAddr + slaveId -> deviceId */
    std::optional<OnlineRoute> getOnlineRoute(
        int linkId,
        const std::string& clientAddr,
        uint8_t slaveId) const;

    /** 通过设备 ID 获取在线路由 */
    std::optional<OnlineRoute> getOnlineRouteByDevice(int deviceId) const;

    /** 清除所有 session 的 inflight 请求和 PollRead 队列（配置热重载时调用） */
    void clearInflightAndPollQueues();

private:
    std::map<std::string, DtuSession> sessions_;
    std::map<std::string, std::string> dtuToSessionKey_;
    std::map<std::string, OnlineRoute> routeBySessionAndSlave_;
    std::map<int, OnlineRoute> routeByDeviceId_;
    OldSessionDisplacedCallback oldSessionDisplacedCallback_;
    mutable std::mutex mutex_;
};

namespace detail {

inline std::string makeRouteKey(const std::string& sessionKey, uint8_t slaveId) {
    return sessionKey + ":" + std::to_string(slaveId);
}

}  // namespace detail

inline void DtuSessionManager::onConnected(int linkId, const std::string& clientAddr) {
    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    auto& session = sessions_[sessionKey];
    session.linkId = linkId;
    session.clientAddr = clientAddr;
    session.sessionKey = sessionKey;
    session.lastSeen = now;
    if (session.bindState == SessionBindState::Bound && session.deviceIdsBySlave.empty()) {
        session.bindState = SessionBindState::Unknown;
        session.dtuKey.clear();
    }
    if (session.bindState != SessionBindState::Bound) {
        session.bindState = SessionBindState::Unknown;
        session.discoveryRequested = false;
        session.discoveryCursor = 0;
        session.nextDiscoveryTime = std::chrono::steady_clock::time_point{};
        session.deviceIdsBySlave.clear();
    }
}

inline void DtuSessionManager::onDisconnected(int linkId, const std::string& clientAddr) {
    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto sessionIt = sessions_.find(sessionKey);
    if (sessionIt == sessions_.end()) return;

    if (!sessionIt->second.dtuKey.empty()) {
        auto boundIt = dtuToSessionKey_.find(sessionIt->second.dtuKey);
        if (boundIt != dtuToSessionKey_.end() && boundIt->second == sessionKey) {
            dtuToSessionKey_.erase(boundIt);
        }
    }

    for (auto routeIt = routeBySessionAndSlave_.begin(); routeIt != routeBySessionAndSlave_.end();) {
        if (routeIt->second.sessionKey == sessionKey) {
            routeByDeviceId_.erase(routeIt->second.deviceId);
            routeIt = routeBySessionAndSlave_.erase(routeIt);
        } else {
            ++routeIt;
        }
    }

    sessions_.erase(sessionIt);
}

inline void DtuSessionManager::touch(int linkId, const std::string& clientAddr) {
    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionKey);
    if (it == sessions_.end()) return;
    it->second.lastSeen = std::chrono::steady_clock::now();
}

inline std::optional<DtuSession> DtuSessionManager::getSession(
    int linkId, const std::string& clientAddr) const {
    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionKey);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<DtuSession> DtuSessionManager::getBoundSessionByDtuKey(
    const std::string& dtuKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keyIt = dtuToSessionKey_.find(dtuKey);
    if (keyIt == dtuToSessionKey_.end()) return std::nullopt;

    auto sessionIt = sessions_.find(keyIt->second);
    if (sessionIt == sessions_.end()) return std::nullopt;
    return sessionIt->second;
}

inline std::vector<DtuSession> DtuSessionManager::listUnknownSessions(int linkId) const {
    std::vector<DtuSession> result;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        if (session.linkId != linkId) continue;
        if (session.bindState == SessionBindState::Unknown) {
            result.push_back(session);
        }
    }
    return result;
}

inline std::vector<DtuSession> DtuSessionManager::listSessions() const {
    std::vector<DtuSession> result;

    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(sessions_.size());
    for (const auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        result.push_back(session);
    }
    return result;
}

inline bool DtuSessionManager::bindSession(
    int linkId, const std::string& clientAddr, const DtuDefinition& dtu) {
    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);

    // 记录被替换的旧 session 信息（需要在锁外关闭旧连接）
    int displacedLinkId = 0;
    std::string displacedClientAddr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sessionIt = sessions_.find(sessionKey);
        if (sessionIt == sessions_.end()) return false;

        // 同一个 dtuKey 只允许一个活跃 session；新绑定覆盖旧绑定。
        auto existingBoundIt = dtuToSessionKey_.find(dtu.dtuKey);
        if (existingBoundIt != dtuToSessionKey_.end() && existingBoundIt->second != sessionKey) {
            auto oldSessionIt = sessions_.find(existingBoundIt->second);
            if (oldSessionIt != sessions_.end()) {
                // 记录旧 session 信息，稍后关闭其 TCP 连接
                displacedLinkId = oldSessionIt->second.linkId;
                displacedClientAddr = oldSessionIt->second.clientAddr;

                oldSessionIt->second.bindState = SessionBindState::Unknown;
                oldSessionIt->second.dtuKey.clear();
                oldSessionIt->second.deviceIdsBySlave.clear();
                // 清除旧 session 的队列和 inflight，防止命令发到死连接
                oldSessionIt->second.inflight.reset();
                oldSessionIt->second.highQueue.clear();
                oldSessionIt->second.normalQueue.clear();
                oldSessionIt->second.rxBuffer.clear();
            }
            for (auto routeIt = routeBySessionAndSlave_.begin(); routeIt != routeBySessionAndSlave_.end();) {
                if (routeIt->second.dtuKey == dtu.dtuKey) {
                    routeByDeviceId_.erase(routeIt->second.deviceId);
                    routeIt = routeBySessionAndSlave_.erase(routeIt);
                } else {
                    ++routeIt;
                }
            }
        }

        auto& session = sessionIt->second;
        if (!session.dtuKey.empty() && session.dtuKey != dtu.dtuKey) {
            auto oldBoundIt = dtuToSessionKey_.find(session.dtuKey);
            if (oldBoundIt != dtuToSessionKey_.end() && oldBoundIt->second == sessionKey) {
                dtuToSessionKey_.erase(oldBoundIt);
            }
        }

        for (auto routeIt = routeBySessionAndSlave_.begin(); routeIt != routeBySessionAndSlave_.end();) {
            if (routeIt->second.sessionKey == sessionKey) {
                routeByDeviceId_.erase(routeIt->second.deviceId);
                routeIt = routeBySessionAndSlave_.erase(routeIt);
            } else {
                ++routeIt;
            }
        }

        session.bindState = SessionBindState::Bound;
        session.dtuKey = dtu.dtuKey;
        session.deviceIdsBySlave.clear();
        session.lastSeen = std::chrono::steady_clock::now();
        session.discoveryRequested = false;
        session.discoveryCursor = 0;
        session.nextDiscoveryTime = std::chrono::steady_clock::time_point{};

        for (const auto& [slaveId, device] : dtu.devicesBySlave) {
            session.deviceIdsBySlave[slaveId] = device.deviceId;

            OnlineRoute route;
            route.sessionKey = sessionKey;
            route.dtuKey = dtu.dtuKey;
            route.linkId = linkId;
            route.clientAddr = clientAddr;
            route.slaveId = slaveId;
            route.deviceId = device.deviceId;

            routeBySessionAndSlave_[detail::makeRouteKey(sessionKey, slaveId)] = route;
            routeByDeviceId_[device.deviceId] = route;
        }

        dtuToSessionKey_[dtu.dtuKey] = sessionKey;
    }

    // 在锁外关闭旧连接，避免死锁
    if (displacedLinkId > 0 && !displacedClientAddr.empty() && oldSessionDisplacedCallback_) {
        LOG_INFO << "[Modbus][DtuSessionManager] Rebound session: "
                 << (dtu.name.empty() ? "<unnamed>" : dtu.name)
                 << " (dtuKey=" << dtu.dtuKey
                 << ", from=" << displacedClientAddr
                 << ", to=" << clientAddr
                 << ")";
        oldSessionDisplacedCallback_(displacedLinkId, displacedClientAddr);
    }

    return true;
}

inline bool DtuSessionManager::mutateSession(
    int linkId, const std::string& clientAddr, const SessionMutator& mutator) {
    if (!mutator) return false;

    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionKey);
    if (it == sessions_.end()) return false;

    mutator(it->second);
    return true;
}

inline std::optional<OnlineRoute> DtuSessionManager::getOnlineRoute(
    int linkId, const std::string& clientAddr, uint8_t slaveId) const {
    const std::string sessionKey = makeDtuSessionKey(linkId, clientAddr);
    const std::string routeKey = detail::makeRouteKey(sessionKey, slaveId);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routeBySessionAndSlave_.find(routeKey);
    if (it == routeBySessionAndSlave_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<OnlineRoute> DtuSessionManager::getOnlineRouteByDevice(int deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routeByDeviceId_.find(deviceId);
    if (it == routeByDeviceId_.end()) return std::nullopt;
    return it->second;
}

inline void DtuSessionManager::clearInflightAndPollQueues() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;

        // 清除 inflight（旧的读组索引在 reload 后可能无效）
        session.inflight.reset();

        // 清除 normalQueue 和 highQueue 中的 PollRead 任务，保留 Write 任务
        auto filterQueue = [](std::deque<ModbusJob>& queue) {
            std::deque<ModbusJob> kept;
            for (auto& job : queue) {
                if (job.kind != ModbusJobKind::PollRead && job.kind != ModbusJobKind::DiscoveryRead) {
                    kept.push_back(std::move(job));
                }
            }
            queue = std::move(kept);
        };
        filterQueue(session.highQueue);
        filterQueue(session.normalQueue);

        // reload 后 discovery 任务已被清空，必须重置 discovery 状态，
        // 否则会话可能永远停留在 Probing/discoveryRequested=true，导致后续不再探测。
        session.discoveryRequested = false;
        session.nextDiscoveryTime = std::chrono::steady_clock::time_point{};
        if (session.bindState != SessionBindState::Bound) {
            session.bindState = SessionBindState::Unknown;
        }

        // 清除残留的接收缓冲区，防止旧数据影响新 poll
        session.rxBuffer.clear();
    }
}

}  // namespace modbus
