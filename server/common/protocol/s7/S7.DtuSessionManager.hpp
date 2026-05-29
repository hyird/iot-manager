#pragma once

#include "S7.SessionTypes.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace s7 {

class DtuSessionManager {
public:
    using SessionMutator = std::function<void(S7DtuSession&)>;
    using OldSessionDisplacedCallback = std::function<void(int linkId, const std::string& clientAddr)>;

    void setOldSessionDisplacedCallback(OldSessionDisplacedCallback cb) {
        oldSessionDisplacedCallback_ = std::move(cb);
    }

    void onConnected(int linkId, const std::string& clientAddr);
    void onDisconnected(int linkId, const std::string& clientAddr);
    void touch(int linkId, const std::string& clientAddr);

    std::optional<S7DtuSession> getSession(int linkId, const std::string& clientAddr) const;
    std::optional<S7DtuSession> getBoundSessionByDtuKey(const std::string& dtuKey) const;
    std::optional<S7DtuSession> getProbingSessionByDevice(int deviceId) const;
    std::vector<S7DtuSession> listSessions() const;

    bool bindSession(int linkId, const std::string& clientAddr, const S7DtuDefinition& dtu);
    std::vector<S7DtuSession> acquireLinkProbeSessions(int linkId, int deviceId);
    bool releaseProbingSession(int deviceId, bool advanceCursor);
    std::vector<S7DtuSession> reconcileDefinitions(const std::vector<S7DtuDefinition>& definitions);
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

inline std::string joinClientAddrs(const std::vector<S7DtuSession>& sessions) {
    std::ostringstream oss;
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        if (index > 0) {
            oss << ",";
        }
        oss << sessions[index].clientAddr;
    }
    return oss.str();
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
            session.probingDeviceId = 0;
            session.discoveryCursor = 0;
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

inline std::optional<S7DtuSession> DtuSessionManager::getProbingSessionByDevice(int deviceId) const {
    if (deviceId <= 0) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        if (session.bindState == SessionBindState::Probing
            && session.probingDeviceId == deviceId) {
            return session;
        }
    }
    return std::nullopt;
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
                oldSessionIt->second.probingDeviceId = 0;
                oldSessionIt->second.discoveryCursor = 0;
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
        session.probingDeviceId = 0;
        session.discoveryCursor = 0;
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

    LOG_INFO << "[S7][DtuSessionManager] Bound session: "
             << (dtu.name.empty() ? "<unnamed>" : dtu.name)
             << " (dtuKey=" << dtu.dtuKey
             << ", deviceId=" << dtu.deviceId
             << ", linkId=" << linkId
             << ", client=" << clientAddr
             << ")";

    if (displacedLinkId > 0 && !displacedClientAddr.empty() && oldSessionDisplacedCallback_) {
        LOG_INFO << "[S7][DtuSessionManager] Rebound session: "
                 << (dtu.name.empty() ? "<unnamed>" : dtu.name)
                 << " (dtuKey=" << dtu.dtuKey
                 << ", deviceId=" << dtu.deviceId
                 << ", from=" << displacedClientAddr
                 << ", to=" << clientAddr
                 << ")";
        oldSessionDisplacedCallback_(displacedLinkId, displacedClientAddr);
    }

    return true;
}

inline std::vector<S7DtuSession> DtuSessionManager::acquireLinkProbeSessions(int linkId, int deviceId) {
    std::vector<S7DtuSession> result;
    if (linkId <= 0 || deviceId <= 0) {
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        if (session.linkId != linkId) continue;
        if (session.bindState == SessionBindState::Probing
            && session.probingDeviceId == deviceId) {
            result.push_back(session);
        }
    }
    if (!result.empty()) {
        LOG_INFO << "[S7][DtuSessionManager] Reuse probing session(s): linkId=" << linkId
                 << ", deviceId=" << deviceId
                 << ", count=" << result.size()
                 << ", clients=" << detail::joinClientAddrs(result);
        return result;
    }

    for (const auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        if (session.linkId != linkId) continue;
        if (session.bindState == SessionBindState::Probing
            && session.probingDeviceId > 0
            && session.probingDeviceId != deviceId) {
            LOG_INFO << "[S7][DtuSessionManager] Probe acquisition blocked: linkId=" << linkId
                     << ", requestedDeviceId=" << deviceId
                     << ", probingDeviceId=" << session.probingDeviceId
                     << ", probingClient=" << session.clientAddr;
            return result;
        }
    }

    for (auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        if (session.linkId != linkId) continue;
        if (session.clientAddr.empty()) continue;
        if (session.bindState != SessionBindState::Unknown) continue;

        session.bindState = SessionBindState::Probing;
        session.probingDeviceId = deviceId;
        session.lastSeen = std::chrono::steady_clock::now();
        result.push_back(session);
    }

    if (result.empty()) {
        LOG_INFO << "[S7][DtuSessionManager] No unknown session available for probe: linkId="
                 << linkId << ", deviceId=" << deviceId;
    } else {
        LOG_INFO << "[S7][DtuSessionManager] Assigned unknown probe session(s): linkId=" << linkId
                 << ", deviceId=" << deviceId
                 << ", count=" << result.size()
                 << ", clients=" << detail::joinClientAddrs(result);
    }

    return result;
}

inline bool DtuSessionManager::releaseProbingSession(int deviceId, bool advanceCursor) {
    if (deviceId <= 0) return false;

    std::vector<S7DtuSession> releasedSessions;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [sessionKey, session] : sessions_) {
        (void)sessionKey;
        if (session.bindState != SessionBindState::Probing
            || session.probingDeviceId != deviceId) {
            continue;
        }

        session.probingDeviceId = 0;
        if (advanceCursor) {
            ++session.discoveryCursor;
        }
        if (session.bindState != SessionBindState::Bound) {
            session.bindState = SessionBindState::Unknown;
        }
        releasedSessions.push_back(session);
    }

    if (!releasedSessions.empty()) {
        LOG_INFO << "[S7][DtuSessionManager] Released probing session(s): deviceId=" << deviceId
                 << ", count=" << releasedSessions.size()
                 << ", advanceCursor=" << (advanceCursor ? "yes" : "no")
                 << ", clients=" << detail::joinClientAddrs(releasedSessions);
    }

    return !releasedSessions.empty();
}

inline std::vector<S7DtuSession> DtuSessionManager::reconcileDefinitions(
    const std::vector<S7DtuDefinition>& definitions) {
    std::map<std::string, S7DtuDefinition> validByDtuKey;
    std::set<int> validLinks;
    for (const auto& definition : definitions) {
        if (definition.dtuKey.empty()) continue;
        validByDtuKey[definition.dtuKey] = definition;
        if (definition.linkId > 0) {
            validLinks.insert(definition.linkId);
        }
    }

    std::vector<S7DtuSession> staleSessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto sessionIt = sessions_.begin(); sessionIt != sessions_.end();) {
            auto& session = sessionIt->second;
            bool stale = validLinks.find(session.linkId) == validLinks.end();
            if (!stale && session.bindState == SessionBindState::Bound) {
                auto defIt = validByDtuKey.find(session.dtuKey);
                stale = defIt == validByDtuKey.end()
                    || defIt->second.deviceId != session.deviceId
                    || defIt->second.linkId != session.linkId;
            }

            if (!stale) {
                ++sessionIt;
                continue;
            }

            staleSessions.push_back(session);
            if (!session.dtuKey.empty()) {
                auto boundIt = dtuToSessionKey_.find(session.dtuKey);
                if (boundIt != dtuToSessionKey_.end() && boundIt->second == session.sessionKey) {
                    dtuToSessionKey_.erase(boundIt);
                }
            }
            routeByDeviceId_.erase(session.deviceId);
            routeBySessionKey_.erase(detail::makeRouteKey(session.sessionKey));
            sessionIt = sessions_.erase(sessionIt);
        }
    }
    return staleSessions;
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
        LOG_INFO << "[S7][DtuSessionManager] Cleared sessions: sessionCount=" << sessionCount
                 << ", routeCount=" << routeCount;
    }
}

}  // namespace s7
