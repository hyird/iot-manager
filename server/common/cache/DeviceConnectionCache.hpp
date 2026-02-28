#pragma once

/**
 * @brief 设备连接信息
 */
struct DeviceConnection {
    int linkId;                                         // 链路 ID
    std::string clientAddr;                             // 客户端地址 (IP:Port)
    std::chrono::steady_clock::time_point lastSeen;     // 最后活跃时间
};

/**
 * @brief 设备连接缓存（单例）
 *
 * 维护 deviceCode -> (linkId, clientAddr) 的映射关系
 * 用于 TCP Server 模式下定向发送指令到特定设备
 */
class DeviceConnectionCache {
public:
    static DeviceConnectionCache& instance() {
        static DeviceConnectionCache inst;
        return inst;
    }

    /**
     * @brief 注册/更新设备连接
     * @param deviceCode 设备编码
     * @param linkId 链路 ID
     * @param clientAddr 客户端地址
     */
    void registerConnection(const std::string& deviceCode, int linkId, const std::string& clientAddr) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查是否已有旧连接
        auto it = connections_.find(deviceCode);
        if (it != connections_.end()) {
            const std::string& oldAddr = it->second.clientAddr;
            if (oldAddr != clientAddr) {
                LOG_INFO << "[DeviceConnectionCache] " << deviceCode
                         << " re-registered: " << oldAddr << " -> " << clientAddr;
            }
            // 移除旧的反向索引
            std::string oldKey = std::to_string(it->second.linkId) + ":" + oldAddr;
            auto& oldDevices = clientDevices_[oldKey];
            oldDevices.erase(deviceCode);
            if (oldDevices.empty()) {
                clientDevices_.erase(oldKey);
            }
        }

        // 注册新连接
        DeviceConnection conn;
        conn.linkId = linkId;
        conn.clientAddr = clientAddr;
        conn.lastSeen = std::chrono::steady_clock::now();
        connections_[deviceCode] = conn;

        // 更新反向索引
        std::string key = std::to_string(linkId) + ":" + clientAddr;
        clientDevices_[key].insert(deviceCode);

        LOG_TRACE << "[DeviceConnectionCache] Registered: " << deviceCode
                  << " -> " << linkId << ":" << clientAddr;
    }

    /**
     * @brief 获取设备连接信息
     * @param deviceCode 设备编码
     * @return 连接信息，未找到返回 nullopt
     */
    std::optional<DeviceConnection> getConnection(const std::string& deviceCode) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(deviceCode);
        if (it != connections_.end()) {
            return it->second;
        }
        return std::nullopt;
    }


    /**
     * @brief 检查客户端是否已注册
     */
    bool isClientRegistered(int linkId, const std::string& clientAddr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = std::to_string(linkId) + ":" + clientAddr;
        return clientDevices_.count(key) > 0;
    }

    /**
     * @brief 移除设备连接
     * @param deviceCode 设备编码
     */
    void removeConnection(const std::string& deviceCode) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(deviceCode);
        if (it == connections_.end()) return;

        // 移除反向索引
        std::string key = std::to_string(it->second.linkId) + ":" + it->second.clientAddr;
        auto& devices = clientDevices_[key];
        devices.erase(deviceCode);
        if (devices.empty()) {
            clientDevices_.erase(key);
        }

        connections_.erase(it);
        LOG_DEBUG << "[DeviceConnectionCache] Removed: " << deviceCode;
    }

    /**
     * @brief 移除链路上的所有设备连接
     * @param linkId 链路 ID
     */
    void removeByLinkId(int linkId) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> toRemove;
        for (const auto& [deviceCode, conn] : connections_) {
            if (conn.linkId == linkId) {
                toRemove.push_back(deviceCode);
            }
        }

        for (const auto& deviceCode : toRemove) {
            auto it = connections_.find(deviceCode);
            if (it != connections_.end()) {
                std::string key = std::to_string(it->second.linkId) + ":" + it->second.clientAddr;
                clientDevices_.erase(key);
                connections_.erase(it);
            }
        }

        if (!toRemove.empty()) {
            LOG_DEBUG << "[DeviceConnectionCache] Removed " << toRemove.size()
                      << " devices for linkId=" << linkId;
        }
    }

    /**
     * @brief 移除指定客户端的所有设备连接
     * @param linkId 链路 ID
     * @param clientAddr 客户端地址
     */
    void removeByClient(int linkId, const std::string& clientAddr) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string key = std::to_string(linkId) + ":" + clientAddr;
        auto it = clientDevices_.find(key);
        if (it == clientDevices_.end()) return;

        // 移除该客户端关联的所有设备
        int count = static_cast<int>(it->second.size());
        for (const auto& deviceCode : it->second) {
            connections_.erase(deviceCode);
        }

        clientDevices_.erase(it);

        LOG_DEBUG << "[DeviceConnectionCache] Removed " << count
                  << " devices for client " << clientAddr;
    }

    /**
     * @brief 清理过期连接
     * @param timeoutSeconds 超时时间（秒）
     */
    void cleanExpired(int timeoutSeconds = 300) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> expired;

        for (const auto& [deviceCode, conn] : connections_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - conn.lastSeen).count();
            if (elapsed > timeoutSeconds) {
                expired.push_back(deviceCode);
            }
        }

        for (const auto& deviceCode : expired) {
            auto it = connections_.find(deviceCode);
            if (it != connections_.end()) {
                std::string key = std::to_string(it->second.linkId) + ":" + it->second.clientAddr;
                auto& devices = clientDevices_[key];
                devices.erase(deviceCode);
                if (devices.empty()) {
                    clientDevices_.erase(key);
                }
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
        for (const auto& [deviceCode, conn] : connections_) {
            Json::Value item;
            item["deviceCode"] = deviceCode;
            item["linkId"] = conn.linkId;
            item["clientAddr"] = conn.clientAddr;
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

    mutable std::mutex mutex_;
    std::map<std::string, DeviceConnection> connections_;           // deviceCode -> DeviceConnection
    std::map<std::string, std::set<std::string>> clientDevices_;    // "linkId:clientAddr" -> set<deviceCode>
};
