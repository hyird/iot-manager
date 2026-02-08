#pragma once

#include <vector>

/**
 * @brief 设备连接信息
 */
struct DeviceConnection {
    int linkId;                                         // 链路 ID
    std::string clientAddr;                             // 客户端地址 (IP:Port)
    uint8_t slaveId = 0;                                // Modbus 从机地址（SL651 为 0）
    std::chrono::steady_clock::time_point lastSeen;     // 最后活跃时间
};

/**
 * @brief 设备连接缓存（单例）
 *
 * 正向索引: deviceKey -> DeviceConnection
 * 反向索引: "linkId:clientAddr:slaveId" -> set<deviceKey>
 */
class DeviceConnectionCache {
public:
    static DeviceConnectionCache& instance() {
        static DeviceConnectionCache inst;
        return inst;
    }

    /**
     * @brief 注册/更新设备连接
     * @param deviceKey 设备标识（Modbus: "modbus_<id>", SL651: deviceCode）
     * @param linkId 链路 ID
     * @param clientAddr 客户端地址
     * @param slaveId Modbus 从机地址（SL651 传 0）
     */
    void registerConnection(const std::string& deviceKey, int linkId,
                            const std::string& clientAddr, uint8_t slaveId = 0) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查是否已有旧连接
        auto it = connections_.find(deviceKey);
        if (it != connections_.end()) {
            if (it->second.clientAddr != clientAddr) {
                LOG_INFO << "[DeviceConnectionCache] " << deviceKey
                         << " re-registered: " << it->second.clientAddr << " -> " << clientAddr;
            }
            // 移除旧的反向索引
            std::string oldKey = makeReverseKey(it->second.linkId, it->second.clientAddr, it->second.slaveId);
            auto& oldDevices = clientDevices_[oldKey];
            oldDevices.erase(deviceKey);
            if (oldDevices.empty()) clientDevices_.erase(oldKey);
        }

        // 注册新连接
        connections_[deviceKey] = {linkId, clientAddr, slaveId, std::chrono::steady_clock::now()};

        // 更新反向索引
        clientDevices_[makeReverseKey(linkId, clientAddr, slaveId)].insert(deviceKey);

        LOG_TRACE << "[DeviceConnectionCache] Registered: " << deviceKey
                  << " -> " << linkId << ":" << clientAddr << " slave=" << static_cast<int>(slaveId);
    }

    /**
     * @brief 获取设备连接信息
     */
    std::optional<DeviceConnection> getConnection(const std::string& deviceKey) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(deviceKey);
        if (it != connections_.end()) return it->second;
        return std::nullopt;
    }

    /**
     * @brief 检查客户端是否已注册（任意 slaveId）
     */
    bool isClientRegistered(int linkId, const std::string& clientAddr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string prefix = makeClientPrefix(linkId, clientAddr);
        auto it = clientDevices_.lower_bound(prefix);
        return it != clientDevices_.end() && it->first.compare(0, prefix.size(), prefix) == 0;
    }

    /**
     * @brief 刷新客户端下所有已注册设备的 lastSeen（心跳保活用）
     * @return 刷新的设备数量
     */
    int refreshClient(int linkId, const std::string& clientAddr) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string prefix = makeClientPrefix(linkId, clientAddr);
        auto now = std::chrono::steady_clock::now();
        int count = 0;
        for (auto it = clientDevices_.lower_bound(prefix);
             it != clientDevices_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
            for (const auto& deviceKey : it->second) {
                auto connIt = connections_.find(deviceKey);
                if (connIt != connections_.end()) {
                    connIt->second.lastSeen = now;
                    ++count;
                }
            }
        }
        return count;
    }

    /**
     * @brief 移除单个设备连接
     */
    void removeConnection(const std::string& deviceKey) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(deviceKey);
        if (it == connections_.end()) return;

        // 移除反向索引
        std::string rKey = makeReverseKey(it->second.linkId, it->second.clientAddr, it->second.slaveId);
        auto& devices = clientDevices_[rKey];
        devices.erase(deviceKey);
        if (devices.empty()) clientDevices_.erase(rKey);

        connections_.erase(it);
        LOG_DEBUG << "[DeviceConnectionCache] Removed: " << deviceKey;
    }

    /**
     * @brief 移除链路上的所有设备连接
     */
    void removeByLinkId(int linkId) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> toRemove;
        for (const auto& [deviceKey, conn] : connections_) {
            if (conn.linkId == linkId) toRemove.push_back(deviceKey);
        }

        for (const auto& deviceKey : toRemove) {
            auto it = connections_.find(deviceKey);
            if (it != connections_.end()) {
                std::string rKey = makeReverseKey(it->second.linkId, it->second.clientAddr, it->second.slaveId);
                auto& devices = clientDevices_[rKey];
                devices.erase(deviceKey);
                if (devices.empty()) clientDevices_.erase(rKey);
                connections_.erase(it);
            }
        }

        if (!toRemove.empty()) {
            LOG_DEBUG << "[DeviceConnectionCache] Removed " << toRemove.size()
                      << " devices for linkId=" << linkId;
        }
    }

    /**
     * @brief 移除指定客户端的所有设备连接（所有 slaveId）
     */
    void removeByClient(int linkId, const std::string& clientAddr) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string prefix = makeClientPrefix(linkId, clientAddr);
        int count = 0;
        for (auto it = clientDevices_.lower_bound(prefix);
             it != clientDevices_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ) {
            count += static_cast<int>(it->second.size());
            for (const auto& deviceKey : it->second) {
                connections_.erase(deviceKey);
            }
            it = clientDevices_.erase(it);
        }

        if (count > 0) {
            LOG_DEBUG << "[DeviceConnectionCache] Removed " << count
                      << " devices for client " << clientAddr;
        }
    }

    /**
     * @brief 按 slaveId 清理映射（用于清理历史 Modbus 映射）
     * @param slaveId 最小 slaveId（默认 1，等价于清理所有 slaveId != 0 的映射）
     * @return 清理的设备键数量
     */
    int removeByMinSlaveId(uint8_t slaveId = 1) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> toRemove;
        toRemove.reserve(connections_.size());
        for (const auto& [deviceKey, conn] : connections_) {
            if (conn.slaveId >= slaveId) {
                toRemove.push_back(deviceKey);
            }
        }

        for (const auto& deviceKey : toRemove) {
            auto it = connections_.find(deviceKey);
            if (it == connections_.end()) continue;

            std::string rKey = makeReverseKey(it->second.linkId, it->second.clientAddr, it->second.slaveId);
            auto reverseIt = clientDevices_.find(rKey);
            if (reverseIt != clientDevices_.end()) {
                reverseIt->second.erase(deviceKey);
                if (reverseIt->second.empty()) {
                    clientDevices_.erase(reverseIt);
                }
            }
            connections_.erase(it);
        }

        if (!toRemove.empty()) {
            LOG_INFO << "[DeviceConnectionCache] Removed " << toRemove.size()
                     << " mapping(s) with slaveId >= " << static_cast<int>(slaveId);
        }
        return static_cast<int>(toRemove.size());
    }

    /**
     * @brief 清理过期连接
     */
    void cleanExpired(int timeoutSeconds = 300) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> expired;

        for (const auto& [deviceKey, conn] : connections_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - conn.lastSeen).count();
            if (elapsed > timeoutSeconds) expired.push_back(deviceKey);
        }

        for (const auto& deviceKey : expired) {
            auto it = connections_.find(deviceKey);
            if (it != connections_.end()) {
                std::string rKey = makeReverseKey(it->second.linkId, it->second.clientAddr, it->second.slaveId);
                auto& devices = clientDevices_[rKey];
                devices.erase(deviceKey);
                if (devices.empty()) clientDevices_.erase(rKey);
                connections_.erase(it);
            }
        }

        if (!expired.empty()) {
            LOG_DEBUG << "[DeviceConnectionCache] Cleaned " << expired.size() << " expired connections";
        }
    }

    /**
     * @brief 获取缓存统计信息
     */
    Json::Value getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);

        Json::Value stats;
        stats["deviceCount"] = static_cast<int>(connections_.size());
        stats["clientCount"] = static_cast<int>(clientDevices_.size());

        Json::Value devices(Json::arrayValue);
        for (const auto& [deviceKey, conn] : connections_) {
            Json::Value item;
            item["deviceKey"] = deviceKey;
            item["linkId"] = conn.linkId;
            item["clientAddr"] = conn.clientAddr;
            item["slaveId"] = conn.slaveId;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - conn.lastSeen).count();
            item["lastSeenSeconds"] = static_cast<int>(elapsed);
            devices.append(item);
        }
        stats["devices"] = devices;

        return stats;
    }

private:
    DeviceConnectionCache() = default;
    DeviceConnectionCache(const DeviceConnectionCache&) = delete;
    DeviceConnectionCache& operator=(const DeviceConnectionCache&) = delete;

    static std::string makeReverseKey(int linkId, const std::string& clientAddr, uint8_t slaveId) {
        return std::to_string(linkId) + ":" + clientAddr + ":" + std::to_string(slaveId);
    }

    static std::string makeClientPrefix(int linkId, const std::string& clientAddr) {
        return std::to_string(linkId) + ":" + clientAddr + ":";
    }

    mutable std::mutex mutex_;
    std::map<std::string, DeviceConnection> connections_;           // deviceKey -> DeviceConnection
    std::map<std::string, std::set<std::string>> clientDevices_;    // "linkId:clientAddr:slaveId" -> set<deviceKey>
};
