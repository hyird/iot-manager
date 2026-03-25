#pragma once

#include "S7.SessionTypes.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace s7 {

class DtuSessionManager {
public:
    using OldSessionDisplacedCallback = std::function<void(int linkId, const std::string& clientAddr)>;

    void setOldSessionDisplacedCallback(OldSessionDisplacedCallback cb) {
        oldSessionDisplacedCallback_ = std::move(cb);
    }

    void onConnected(int linkId, const std::string& clientAddr);
    void onDisconnected(int linkId, const std::string& clientAddr);
    void touch(int linkId, const std::string& clientAddr);

    std::optional<S7DtuSession> getSession(int linkId, const std::string& clientAddr) const;
    std::optional<S7DtuSession> getBoundSessionByDtuKey(const std::string& dtuKey) const;
    std::vector<S7DtuSession> listSessions() const;

    bool bindSession(int linkId, const std::string& clientAddr, const S7DtuDefinition& dtu);
    std::optional<S7OnlineRoute> getOnlineRoute(int linkId, const std::string& clientAddr) const;
    std::optional<S7OnlineRoute> getOnlineRouteByDevice(int deviceId) const;
    void clearAllSessions();

private:
    std::map<std::string, S7DtuSession> sessions_;
    std::map<std::string, std::string> dtuToSessionKey_;
    std::map<std::string, S7OnlineRoute> routeBySessionKey_;
    std::map<int, S7OnlineRoute> routeByDeviceId_;
    OldSessionDisplacedCallback oldSessionDisplacedCallback_;
    mutable std::mutex mutex_;
};

namespace detail {

inline std::string makeRouteKey(const std::string& sessionKey) {
    return sessionKey;
}

}  // namespace detail

inline void DtuSessionManager::onConnected(int linkId, const std::string& clientAddr) {
    const std::string sessionKey = makeS7DtuSessionKey(linkId, clientAddr);
    const auto now = std::chrono::steady_clock::now();
    bool isNewSession = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        isNewSession = sessions_.find(sessionKey) == sessions_.end();
        auto& session = sessions_[sessionKey];
        session.linkId = linkId;
        session.clientAddr = clientAddr;
        session.sessionKey = sessionKey;
        session.lastSeen = now;
        if (session.bindState != SessionBindState::Bound) {
            session.bindState = SessionBindState::Unknown;
            session.dtuKey.clear();
            session.deviceId = 0;
        }
    }

    if (isNewSession) {
        LOG_DEBUG << "[S7][DtuSessionManager] Session opened: linkId=" << linkId
                  << ", client=" << clientAddr;
    }
}

inline void DtuSessionManager::onDisconnected(int linkId, const std::string& clientAddr) {
    const std::string sessionKey = makeS7DtuSessionKey(linkId, clientAddr);
    std::string dtuKey;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sessionIt = sessions_.find(sessionKey);
        if (sessionIt == sessions_.end()) return;

        found = true;
        dtuKey = sessionIt->second.dtuKey;

        if (!sessionIt->second.dtuKey.empty()) {
            auto boundIt = dtuToSessionKey_.find(sessionIt->second.dtuKey);
            if (boundIt != dtuToSessionKey_.end() && boundIt->second == sessionKey) {
                dtuToSessionKey_.erase(boundIt);
            }
        }

        for (auto routeIt = routeBySessionKey_.begin(); routeIt != routeBySessionKey_.end();) {
            if (routeIt->second.sessionKey == sessionKey) {
                routeByDeviceId_.erase(routeIt->second.deviceId);
                routeIt = routeBySessionKey_.erase(routeIt);
            } else {
                ++routeIt;
            }
        }

        sessions_.erase(sessionIt);
    }

    if (found) {
        LOG_INFO << "[S7][DtuSessionManager] Session closed: linkId=" << linkId
                 << ", client=" << clientAddr
                 << (dtuKey.empty() ? "" : (", dtuKey=" + dtuKey));
    }
}

inline void DtuSessionManager::touch(int linkId, const std::string& clientAddr) {
    const std::string sessionKey = makeS7DtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionKey);
    if (it == sessions_.end()) return;
    it->second.lastSeen = std::chrono::steady_clock::now();
}

inline std::optional<S7DtuSession> DtuSessionManager::getSession(
    int linkId, const std::string& clientAddr) const {
    const std::string sessionKey = makeS7DtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionKey);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<S7DtuSession> DtuSessionManager::getBoundSessionByDtuKey(
    const std::string& dtuKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keyIt = dtuToSessionKey_.find(dtuKey);
    if (keyIt == dtuToSessionKey_.end()) return std::nullopt;

    auto sessionIt = sessions_.find(keyIt->second);
    if (sessionIt == sessions_.end()) return std::nullopt;
    return sessionIt->second;
}

inline std::vector<S7DtuSession> DtuSessionManager::listSessions() const {
    std::vector<S7DtuSession> result;

    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(sessions_.size());
    for (const auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        result.push_back(session);
    }
    return result;
}

inline bool DtuSessionManager::bindSession(
    int linkId, const std::string& clientAddr, const S7DtuDefinition& dtu) {
    const std::string sessionKey = makeS7DtuSessionKey(linkId, clientAddr);

    int displacedLinkId = 0;
    std::string displacedClientAddr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sessionIt = sessions_.find(sessionKey);
        if (sessionIt == sessions_.end()) return false;

        auto existingBoundIt = dtuToSessionKey_.find(dtu.dtuKey);
        if (existingBoundIt != dtuToSessionKey_.end() && existingBoundIt->second != sessionKey) {
            auto oldSessionIt = sessions_.find(existingBoundIt->second);
            if (oldSessionIt != sessions_.end()) {
                displacedLinkId = oldSessionIt->second.linkId;
                displacedClientAddr = oldSessionIt->second.clientAddr;

                oldSessionIt->second.bindState = SessionBindState::Unknown;
                oldSessionIt->second.dtuKey.clear();
                oldSessionIt->second.deviceId = 0;
            }

            for (auto routeIt = routeBySessionKey_.begin(); routeIt != routeBySessionKey_.end();) {
                if (routeIt->second.dtuKey == dtu.dtuKey) {
                    routeByDeviceId_.erase(routeIt->second.deviceId);
                    routeIt = routeBySessionKey_.erase(routeIt);
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

        for (auto routeIt = routeBySessionKey_.begin(); routeIt != routeBySessionKey_.end();) {
            if (routeIt->second.sessionKey == sessionKey) {
                routeByDeviceId_.erase(routeIt->second.deviceId);
                routeIt = routeBySessionKey_.erase(routeIt);
            } else {
                ++routeIt;
            }
        }

        session.bindState = SessionBindState::Bound;
        session.dtuKey = dtu.dtuKey;
        session.deviceId = dtu.deviceId;
        session.lastSeen = std::chrono::steady_clock::now();

        S7OnlineRoute route;
        route.sessionKey = sessionKey;
        route.dtuKey = dtu.dtuKey;
        route.linkId = linkId;
        route.clientAddr = clientAddr;
        route.deviceId = dtu.deviceId;

        routeBySessionKey_[detail::makeRouteKey(sessionKey)] = route;
        routeByDeviceId_[dtu.deviceId] = route;
        dtuToSessionKey_[dtu.dtuKey] = sessionKey;
    }

    LOG_INFO << "[S7][DtuSessionManager] Bound DTU " << dtu.dtuKey
             << " to linkId=" << linkId
             << ", client=" << clientAddr
             << ", deviceId=" << dtu.deviceId;

    if (displacedLinkId > 0 && !displacedClientAddr.empty() && oldSessionDisplacedCallback_) {
        LOG_INFO << "[S7][DtuSessionManager] DTU " << dtu.dtuKey
                 << " rebound from " << displacedClientAddr
                 << " to " << clientAddr << ", closing old connection";
        oldSessionDisplacedCallback_(displacedLinkId, displacedClientAddr);
    }

    return true;
}

inline std::optional<S7OnlineRoute> DtuSessionManager::getOnlineRoute(
    int linkId, const std::string& clientAddr) const {
    const std::string sessionKey = makeS7DtuSessionKey(linkId, clientAddr);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routeBySessionKey_.find(sessionKey);
    if (it == routeBySessionKey_.end()) return std::nullopt;
    return it->second;
}

inline std::optional<S7OnlineRoute> DtuSessionManager::getOnlineRouteByDevice(int deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routeByDeviceId_.find(deviceId);
    if (it == routeByDeviceId_.end()) return std::nullopt;
    return it->second;
}

inline void DtuSessionManager::clearAllSessions() {
    std::size_t sessionCount = 0;
    std::size_t routeCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessionCount = sessions_.size();
        routeCount = routeBySessionKey_.size();
        sessions_.clear();
        dtuToSessionKey_.clear();
        routeBySessionKey_.clear();
        routeByDeviceId_.clear();
    }

    if (sessionCount > 0 || routeCount > 0) {
        LOG_INFO << "[S7][DtuSessionManager] Cleared " << sessionCount
                 << " session(s) and " << routeCount << " route(s)";
    }
}

}  // namespace s7
