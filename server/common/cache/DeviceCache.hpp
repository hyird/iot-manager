#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"

/**
 * @brief 设备缓存服务
 *
 * 缓存设备基本信息和协议配置，减少数据库查询
 * 实时数据每次从数据库获取，静态数据从缓存读取
 */
class DeviceCache {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    // 缓存的设备信息
    struct CachedDevice {
        int id;
        std::string name;
        std::string deviceCode;
        int linkId;
        int protocolConfigId;
        std::string status;
        int onlineTimeout;  // 在线超时时间（秒），默认 300
        bool remoteControl;  // 是否允许远控，默认 true
        std::string timezone;  // 设备时区
        uint8_t slaveId = 1;     // Modbus 从站地址，默认 1
        std::string modbusMode;  // Modbus 模式: "TCP" / "RTU"
        std::string remark;
        std::string createdAt;
        std::string linkName;
        std::string linkMode;
        std::string protocolName;
        std::string protocolType;
        Json::Value protocolConfig;  // 解析后的协议配置
    };

    static DeviceCache& instance() {
        static DeviceCache instance;
        return instance;
    }

    /**
     * @brief 获取缓存的设备列表
     */
    Task<std::vector<CachedDevice>> getDevices() {
        auto now = std::chrono::steady_clock::now();

        {
            std::unique_lock lock(mutex_);
            // 缓存有效，直接返回
            if (!devices_.empty() &&
                std::chrono::duration_cast<std::chrono::seconds>(now - lastRefresh_).count() < CACHE_TTL_SECONDS) {
                co_return devices_;
            }

            // 如果已有协程正在刷新，等待其完成后返回缓存
            if (refreshing_) {
                // 释放锁后等待一段时间再重试
            } else {
                refreshing_ = true;
            }
        }

        // 等待其他协程刷新完成
        while (true) {
            {
                std::unique_lock lock(mutex_);
                if (!refreshing_) {
                    // 其他协程已完成刷新，返回缓存
                    co_return devices_;
                }
                // 检查是否是本协程负责刷新
                if (!devices_.empty() &&
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - lastRefresh_).count() < CACHE_TTL_SECONDS) {
                    refreshing_ = false;
                    co_return devices_;
                }
            }
            // 本协程负责刷新
            break;
        }

        // 执行刷新
        try {
            co_await refreshCache();
        } catch (...) {
            std::unique_lock lock(mutex_);
            refreshing_ = false;
            throw;
        }

        std::unique_lock lock(mutex_);
        refreshing_ = false;
        co_return devices_;
    }

    /**
     * @brief 强制刷新缓存
     */
    Task<void> refreshCache() {
        DatabaseService dbService;

        std::string sql = R"(
            SELECT d.id, d.name, d.link_id, d.protocol_config_id,
                   d.status, d.protocol_params, d.remark, d.created_at,
                   p.name as protocol_name, p.protocol as protocol_type, p.config as protocol_config,
                   l.name as link_name, l.mode as link_mode
            FROM device d
            LEFT JOIN protocol_config p ON d.protocol_config_id = p.id
            LEFT JOIN link l ON d.link_id = l.id
            WHERE d.deleted_at IS NULL
            ORDER BY d.id ASC
        )";

        auto result = co_await dbService.execSqlCoro(sql);

        std::vector<CachedDevice> newDevices;
        Json::CharReaderBuilder readerBuilder;

        for (const auto& row : result) {
            CachedDevice device;
            device.id = FieldHelper::getInt(row["id"]);
            device.name = FieldHelper::getString(row["name"], "");
            device.linkId = row["link_id"].isNull() ? 0 : FieldHelper::getInt(row["link_id"]);
            device.protocolConfigId = row["protocol_config_id"].isNull() ? 0 : FieldHelper::getInt(row["protocol_config_id"]);
            device.status = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
            device.remark = FieldHelper::getString(row["remark"], "");
            device.createdAt = FieldHelper::getString(row["created_at"], "");
            device.linkName = FieldHelper::getString(row["link_name"], "");
            device.linkMode = FieldHelper::getString(row["link_mode"], "");
            device.protocolName = FieldHelper::getString(row["protocol_name"], "");
            device.protocolType = FieldHelper::getString(row["protocol_type"], "");

            // 解析 protocol_params JSONB
            std::string ppStr = FieldHelper::getString(row["protocol_params"], "");
            if (!ppStr.empty()) {
                Json::Value pp;
                std::string errs;
                std::istringstream ppIss(ppStr);
                if (Json::parseFromStream(readerBuilder, ppIss, &pp, &errs)) {
                    device.deviceCode = pp.get("device_code", "").asString();
                    device.onlineTimeout = pp.get("online_timeout", 300).asInt();
                    device.remoteControl = pp.get("remote_control", true).asBool();
                    device.timezone = pp.get("timezone", "+08:00").asString();
                    device.slaveId = static_cast<uint8_t>(pp.get("slave_id", 1).asInt());
                    device.modbusMode = pp.get("modbus_mode", "").asString();
                }
            } else {
                device.onlineTimeout = 300;
                device.remoteControl = true;
                device.timezone = "+08:00";
            }

            // 解析协议配置 JSON
            std::string configStr = FieldHelper::getString(row["protocol_config"], "");
            if (!configStr.empty()) {
                std::string errs;
                std::istringstream iss(configStr);
                Json::parseFromStream(readerBuilder, iss, &device.protocolConfig, &errs);
            }

            newDevices.push_back(std::move(device));
        }

        {
            std::unique_lock lock(mutex_);
            devices_ = std::move(newDevices);
            // 重建索引
            deviceIndex_.clear();
            for (size_t i = 0; i < devices_.size(); ++i) {
                deviceIndex_[devices_[i].id] = i;
            }
            lastRefresh_ = std::chrono::steady_clock::now();
        }

        LOG_DEBUG << "[DeviceCache] Refreshed cache with " << devices_.size() << " devices";
    }

    /**
     * @brief 清除全部缓存（协议配置/链路更新时调用）
     */
    void invalidate() {
        std::unique_lock lock(mutex_);
        devices_.clear();
        deviceIndex_.clear();
        LOG_DEBUG << "[DeviceCache] Cache fully invalidated";
    }

    /**
     * @brief 按 ID 清除单个设备缓存（设备更新/删除时调用）
     * 使用 swap-and-pop 技术实现 O(1) 删除
     */
    void invalidateById(int deviceId) {
        std::unique_lock lock(mutex_);
        auto indexIt = deviceIndex_.find(deviceId);
        if (indexIt == deviceIndex_.end()) {
            return;  // 设备不在缓存中
        }

        size_t idx = indexIt->second;
        size_t lastIdx = devices_.size() - 1;

        // 先从索引中删除目标设备
        deviceIndex_.erase(indexIt);

        if (idx != lastIdx) {
            // 将最后一个元素移到被删除的位置
            int lastId = devices_[lastIdx].id;
            devices_[idx] = std::move(devices_[lastIdx]);
            deviceIndex_[lastId] = idx;
        }

        devices_.pop_back();
        LOG_DEBUG << "[DeviceCache] Device " << deviceId << " invalidated";
    }

    /**
     * @brief 按 ID 批量清除设备缓存
     * 注意：批量删除时，由于索引会动态变化，需要逐个处理
     */
    void invalidateByIds(const std::vector<int>& deviceIds) {
        std::unique_lock lock(mutex_);
        int removedCount = 0;

        for (int deviceId : deviceIds) {
            auto indexIt = deviceIndex_.find(deviceId);
            if (indexIt == deviceIndex_.end()) {
                continue;
            }

            size_t idx = indexIt->second;
            size_t lastIdx = devices_.size() - 1;

            deviceIndex_.erase(indexIt);

            if (idx != lastIdx) {
                int lastId = devices_[lastIdx].id;
                devices_[idx] = std::move(devices_[lastIdx]);
                deviceIndex_[lastId] = idx;
            }

            devices_.pop_back();
            removedCount++;
        }

        LOG_DEBUG << "[DeviceCache] " << removedCount << " devices invalidated";
    }

    /**
     * @brief 标记需要刷新（下次访问时重新加载）
     */
    void markStale() {
        std::unique_lock lock(mutex_);
        lastRefresh_ = std::chrono::steady_clock::time_point{};
        LOG_DEBUG << "[DeviceCache] Cache marked as stale";
    }

private:
    DeviceCache() = default;

    static constexpr int CACHE_TTL_SECONDS = 60;  // 缓存有效期 60 秒

    std::vector<CachedDevice> devices_;
    std::map<int, size_t> deviceIndex_;  // deviceId -> index in devices_
    std::chrono::steady_clock::time_point lastRefresh_;
    mutable std::shared_mutex mutex_;
    bool refreshing_ = false;  // 防止多协程同时刷新缓存

public:
    // ==================== 同步访问接口（TcpIoPool 线程使用） ====================

    /**
     * @brief 通过 linkId 同步获取协议类型
     * 线程安全：shared_lock 读，不触发缓存刷新
     */
    std::string getProtocolByLinkIdSync(int linkId) const {
        std::shared_lock lock(mutex_);
        for (const auto& device : devices_) {
            if (device.linkId == linkId && !device.protocolType.empty()) {
                return device.protocolType;
            }
        }
        return "";
    }

    /**
     * @brief 通过 linkId + deviceCode 同步查找设备
     */
    std::optional<CachedDevice> findByLinkAndCodeSync(int linkId, const std::string& deviceCode) const {
        std::shared_lock lock(mutex_);
        for (const auto& device : devices_) {
            if (device.linkId == linkId && device.deviceCode == deviceCode) {
                return device;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 通过 deviceCode 同步查找设备
     */
    std::optional<CachedDevice> findByCodeSync(const std::string& deviceCode) const {
        std::shared_lock lock(mutex_);
        for (const auto& device : devices_) {
            if (device.deviceCode == deviceCode) {
                return device;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 检查缓存是否已加载
     */
    bool isLoaded() const {
        std::shared_lock lock(mutex_);
        return !devices_.empty();
    }
};
