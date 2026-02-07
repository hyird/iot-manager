#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/domain/EventBus.hpp"
#include "modules/alert/domain/AlertRecord.hpp"
#include "modules/alert/domain/Events.hpp"

#include <shared_mutex>

/**
 * @brief 告警引擎（单例）
 *
 * 职责：
 * - 启动时从数据库加载所有启用的告警规则到内存
 * - 设备数据上报时实时检查规则（ProtocolDispatcher 调用）
 * - 定时检测设备离线状态
 * - 告警冷却期管理（防止重复告警）
 */
class AlertEngine {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    static AlertEngine& instance() {
        static AlertEngine engine;
        return engine;
    }

    // ==================== 生命周期 ====================

    /**
     * @brief 从数据库加载所有启用的规则到内存
     */
    Task<void> initialize() {
        co_await loadRulesFromDb();
        LOG_INFO << "[AlertEngine] Initialized with " << ruleCount() << " rules";
    }

    /**
     * @brief 规则 CRUD 后重新加载
     */
    Task<void> reloadRules() {
        co_await loadRulesFromDb();
        LOG_INFO << "[AlertEngine] Rules reloaded, count=" << ruleCount();
    }

    /**
     * @brief 启动离线检测定时器（60 秒周期）
     */
    void startOfflineChecker(trantor::EventLoop* loop) {
        if (!loop) return;
        offlineLoop_ = loop;

        // 每 60 秒检查一次
        offlineTimerId_ = loop->runEvery(60.0, [this]() {
            checkOfflineDevices();
        });

        LOG_INFO << "[AlertEngine] Offline checker started (60s interval)";
    }

    // ==================== 数据判定入口 ====================

    /**
     * @brief 检查设备上报数据是否触发告警
     *
     * 在 ProtocolDispatcher::saveBatchResults() 中调用。
     */
    Task<void> checkData(int deviceId, const Json::Value& data) {
        std::vector<CachedRule> matchedRules;

        // 1. shared_lock 读取该设备的规则
        {
            std::shared_lock lock(rulesMutex_);
            auto it = rulesByDevice_.find(deviceId);
            if (it == rulesByDevice_.end()) co_return;
            matchedRules = it->second;  // 拷贝，减少锁持有时间
        }

        // 2. 遍历规则
        for (const auto& rule : matchedRules) {
            // 检查冷却期
            if (isInCooldown(rule.id, rule.silenceDuration)) continue;

            // 评估条件组合
            bool triggered = evaluateRule(rule, data, deviceId);

            if (triggered) {
                auto detail = buildDetail(rule, data);
                auto message = buildMessage(rule, data);
                co_await triggerAlert(rule, message, detail);
            }
        }

        // 3. 更新 previousValues（用于变化率计算）
        updatePreviousValues(deviceId, data);
    }

private:
    AlertEngine() = default;

    // ==================== 内存结构 ====================

    struct CachedCondition {
        std::string type;           // threshold / offline / rate_of_change
        std::string elementKey;     // D0_004E
        std::string op;             // > >= < <= == !=
        double value = 0;           // 阈值
        int duration = 0;           // 离线超时秒数
        double changeRate = 0;      // 变化率百分比
        std::string changeDirection; // rise / fall / any
    };

    struct CachedRule {
        int id = 0;
        std::string name;
        int deviceId = 0;
        std::string deviceName;
        std::string severity;
        std::vector<CachedCondition> conditions;
        std::string logic;          // and / or
        int silenceDuration = 300;
    };

    // deviceId → 该设备的规则列表
    std::map<int, std::vector<CachedRule>> rulesByDevice_;
    mutable std::shared_mutex rulesMutex_;

    // 变化率检测："deviceId:elementKey" → 上一次的值
    std::map<std::string, double> previousValues_;
    std::mutex prevValuesMutex_;

    // 冷却期缓存：ruleId → 上次触发时间
    std::map<int, std::chrono::steady_clock::time_point> cooldownMap_;
    std::mutex cooldownMutex_;

    // 离线检测
    trantor::EventLoop* offlineLoop_ = nullptr;
    trantor::TimerId offlineTimerId_{0};

    // ==================== 规则加载 ====================

    Task<void> loadRulesFromDb() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(R"(
            SELECT r.*, d.name AS device_name
            FROM alert_rule r
            LEFT JOIN device d ON r.device_id = d.id
            WHERE r.status = 'enabled' AND r.deleted_at IS NULL
        )");

        std::map<int, std::vector<CachedRule>> newRules;

        for (const auto& row : result) {
            CachedRule rule;
            rule.id = row["id"].as<int>();
            rule.name = row["name"].as<std::string>();
            rule.deviceId = row["device_id"].as<int>();
            rule.deviceName = row["device_name"].isNull() ? "" : row["device_name"].as<std::string>();
            rule.severity = row["severity"].as<std::string>();
            rule.logic = row["logic"].as<std::string>();
            rule.silenceDuration = row["silence_duration"].as<int>();

            // 解析 conditions JSONB
            Json::CharReaderBuilder builder;
            Json::Value condJson;
            std::string condStr = row["conditions"].as<std::string>();
            std::istringstream stream(condStr);
            Json::parseFromStream(builder, stream, &condJson, nullptr);

            for (const auto& cond : condJson) {
                CachedCondition cc;
                cc.type = cond.get("type", "").asString();
                cc.elementKey = cond.get("elementKey", "").asString();
                cc.op = cond.get("operator", ">").asString();

                if (cond.isMember("value")) {
                    try { cc.value = std::stod(cond["value"].asString()); } catch (...) {}
                }
                if (cond.isMember("duration")) {
                    cc.duration = cond["duration"].asInt();
                }
                if (cond.isMember("changeRate")) {
                    try { cc.changeRate = std::stod(cond["changeRate"].asString()); } catch (...) {}
                }
                cc.changeDirection = cond.get("changeDirection", "any").asString();

                rule.conditions.push_back(std::move(cc));
            }

            newRules[rule.deviceId].push_back(std::move(rule));
        }

        // 替换内存规则
        {
            std::unique_lock lock(rulesMutex_);
            rulesByDevice_ = std::move(newRules);
        }
    }

    int ruleCount() const {
        std::shared_lock lock(rulesMutex_);
        int count = 0;
        for (const auto& [_, rules] : rulesByDevice_) {
            count += static_cast<int>(rules.size());
        }
        return count;
    }

    // ==================== 条件评估 ====================

    bool evaluateRule(const CachedRule& rule, const Json::Value& data, int deviceId) {
        if (rule.conditions.empty()) return false;

        bool hasNonOffline = false;
        for (const auto& cond : rule.conditions) {
            if (cond.type != "offline") {
                hasNonOffline = true;
                break;
            }
        }
        // 如果所有条件都是 offline 类型，checkData 中不触发（由定时器处理）
        if (!hasNonOffline) return false;

        if (rule.logic == "or") {
            for (const auto& cond : rule.conditions) {
                if (cond.type == "offline") continue;  // 离线由定时器处理
                if (evaluateCondition(cond, data, deviceId)) return true;
            }
            return false;
        }

        // and 逻辑
        for (const auto& cond : rule.conditions) {
            if (cond.type == "offline") continue;  // 离线由定时器处理
            if (!evaluateCondition(cond, data, deviceId)) return false;
        }
        return true;
    }

    bool evaluateCondition(const CachedCondition& cond, const Json::Value& data, int deviceId) {
        if (cond.type == "threshold") {
            return evaluateThreshold(cond, data);
        }
        if (cond.type == "rate_of_change") {
            return evaluateRateOfChange(cond, data, deviceId);
        }
        return false;
    }

    /**
     * @brief 阈值比较
     * 从 data["data"][elementKey]["value"] 取值
     */
    bool evaluateThreshold(const CachedCondition& cond, const Json::Value& data) {
        if (cond.elementKey.empty()) return false;
        if (!data.isMember("data") || !data["data"].isMember(cond.elementKey)) return false;

        const auto& elem = data["data"][cond.elementKey];
        if (!elem.isMember("value")) return false;

        double actual = 0;
        try { actual = std::stod(elem["value"].asString()); } catch (...) { return false; }

        if (cond.op == ">") return actual > cond.value;
        if (cond.op == ">=") return actual >= cond.value;
        if (cond.op == "<") return actual < cond.value;
        if (cond.op == "<=") return actual <= cond.value;
        if (cond.op == "==") return std::abs(actual - cond.value) < 1e-9;
        if (cond.op == "!=") return std::abs(actual - cond.value) >= 1e-9;
        return false;
    }

    /**
     * @brief 变化率检查
     * 计算 |current - previous| / |previous| * 100 与阈值对比
     */
    bool evaluateRateOfChange(const CachedCondition& cond, const Json::Value& data, int deviceId) {
        if (cond.elementKey.empty()) return false;
        if (!data.isMember("data") || !data["data"].isMember(cond.elementKey)) return false;

        const auto& elem = data["data"][cond.elementKey];
        if (!elem.isMember("value")) return false;

        double current = 0;
        try { current = std::stod(elem["value"].asString()); } catch (...) { return false; }

        std::string key = std::to_string(deviceId) + ":" + cond.elementKey;

        std::lock_guard lock(prevValuesMutex_);
        auto it = previousValues_.find(key);
        if (it == previousValues_.end()) return false;  // 无历史值，跳过

        double previous = it->second;
        if (std::abs(previous) < 1e-9) return false;  // 避免除零

        double rate = std::abs(current - previous) / std::abs(previous) * 100.0;

        // 方向检查
        if (cond.changeDirection == "rise" && current <= previous) return false;
        if (cond.changeDirection == "fall" && current >= previous) return false;

        return rate >= cond.changeRate;
    }

    // ==================== 辅助方法 ====================

    void updatePreviousValues(int deviceId, const Json::Value& data) {
        if (!data.isMember("data") || !data["data"].isObject()) return;

        std::lock_guard lock(prevValuesMutex_);
        for (const auto& key : data["data"].getMemberNames()) {
            const auto& elem = data["data"][key];
            if (elem.isMember("value")) {
                try {
                    double val = std::stod(elem["value"].asString());
                    previousValues_[std::to_string(deviceId) + ":" + key] = val;
                } catch (...) {}
            }
        }
    }

    bool isInCooldown(int ruleId, int silenceDuration) {
        std::lock_guard lock(cooldownMutex_);
        auto it = cooldownMap_.find(ruleId);
        if (it == cooldownMap_.end()) return false;
        auto elapsed = std::chrono::steady_clock::now() - it->second;
        return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < silenceDuration;
    }

    void markCooldown(int ruleId) {
        std::lock_guard lock(cooldownMutex_);
        cooldownMap_[ruleId] = std::chrono::steady_clock::now();
    }

    std::string buildMessage(const CachedRule& rule, const Json::Value& data) {
        std::string msg = rule.name;
        // 尝试获取触发值
        for (const auto& cond : rule.conditions) {
            if (cond.type == "threshold" && !cond.elementKey.empty()) {
                if (data.isMember("data") && data["data"].isMember(cond.elementKey)) {
                    const auto& elem = data["data"][cond.elementKey];
                    std::string elemName = elem.get("name", cond.elementKey).asString();
                    std::string elemValue = elem.get("value", "N/A").asString();
                    std::string unit = elem.get("unit", "").asString();
                    msg += " [" + elemName + "=" + elemValue + unit + " " + cond.op + " " + std::to_string(cond.value) + "]";
                    break;
                }
            }
            if (cond.type == "offline") {
                msg += " [设备离线超过 " + std::to_string(cond.duration) + " 秒]";
                break;
            }
            if (cond.type == "rate_of_change" && !cond.elementKey.empty()) {
                msg += " [变化率超过 " + std::to_string(static_cast<int>(cond.changeRate)) + "%]";
                break;
            }
        }
        return msg;
    }

    Json::Value buildDetail(const CachedRule& rule, const Json::Value& data) {
        Json::Value detail;
        detail["rule_id"] = rule.id;
        detail["rule_name"] = rule.name;
        detail["device_id"] = rule.deviceId;

        // 记录所有条件的评估情况
        Json::Value condResults(Json::arrayValue);
        for (const auto& cond : rule.conditions) {
            Json::Value cr;
            cr["type"] = cond.type;
            cr["elementKey"] = cond.elementKey;

            if (cond.type == "threshold") {
                cr["operator"] = cond.op;
                cr["threshold"] = cond.value;
                if (data.isMember("data") && data["data"].isMember(cond.elementKey)) {
                    cr["actualValue"] = data["data"][cond.elementKey].get("value", "N/A").asString();
                }
            } else if (cond.type == "rate_of_change") {
                cr["changeRate"] = cond.changeRate;
                cr["changeDirection"] = cond.changeDirection;
            } else if (cond.type == "offline") {
                cr["duration"] = cond.duration;
            }

            condResults.append(cr);
        }
        detail["conditions"] = condResults;
        return detail;
    }

    Task<void> triggerAlert(const CachedRule& rule, const std::string& message, const Json::Value& detail) {
        // 写入数据库
        int64_t recordId = co_await AlertRecord::create(
            rule.id, rule.deviceId, rule.severity, message, detail
        );

        if (recordId > 0) {
            // 设置冷却
            markCooldown(rule.id);

            // 发布事件（WsEventHandlers 订阅后广播到前端）
            co_await EventBus::instance().publish(
                AlertTriggered(recordId, rule.deviceId, rule.id, rule.severity, message, rule.deviceName)
            );

            LOG_INFO << "[AlertEngine] Alert triggered: rule=" << rule.name
                     << " device=" << rule.deviceId
                     << " severity=" << rule.severity
                     << " recordId=" << recordId;
        }
    }

    // ==================== 离线检测 ====================

    void checkOfflineDevices() {
        // 收集含 offline 条件的规则
        std::vector<std::pair<CachedRule, CachedCondition>> offlineRules;
        {
            std::shared_lock lock(rulesMutex_);
            for (const auto& [_, rules] : rulesByDevice_) {
                for (const auto& rule : rules) {
                    for (const auto& cond : rule.conditions) {
                        if (cond.type == "offline" && cond.duration > 0) {
                            offlineRules.emplace_back(rule, cond);
                        }
                    }
                }
            }
        }

        if (offlineRules.empty()) return;

        // 在 Drogon IO 线程中异步执行数据库查询和告警触发
        drogon::async_run([this, offlineRules = std::move(offlineRules)]() -> Task<void> {
            // 收集需要检查的设备 ID
            std::vector<int> deviceIds;
            for (const auto& [rule, _] : offlineRules) {
                deviceIds.push_back(rule.deviceId);
            }

            // 批量获取最后上报时间
            auto latestTimes = co_await RealtimeDataCache::instance().getLatestReportTimes(deviceIds);

            auto now = std::chrono::system_clock::now();

            for (const auto& [rule, cond] : offlineRules) {
                if (isInCooldown(rule.id, rule.silenceDuration)) continue;

                auto it = latestTimes.find(rule.deviceId);
                if (it == latestTimes.end() || it->second.empty()) {
                    // 从未上报，视为离线
                    Json::Value detail;
                    detail["rule_id"] = rule.id;
                    detail["type"] = "offline";
                    detail["duration"] = cond.duration;
                    detail["lastReportTime"] = "never";

                    std::string message = rule.name + " [设备离线 - 从未上报数据]";
                    co_await triggerAlert(rule, message, detail);
                    continue;
                }

                // 解析最后上报时间并计算差值
                std::string lastTime = it->second;
                try {
                    // 简单解析 ISO 时间的秒数差
                    std::tm tm{};
                    std::istringstream ss(lastTime);
                    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                    if (ss.fail()) {
                        // 尝试带 T 的格式
                        std::istringstream ss2(lastTime);
                        ss2 >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                        if (ss2.fail()) continue;
                    }

                    auto lastTp = std::chrono::system_clock::from_time_t(
#ifdef _WIN32
                        _mkgmtime(&tm)
#else
                        timegm(&tm)
#endif
                    );
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTp).count();

                    if (elapsed > cond.duration) {
                        Json::Value detail;
                        detail["rule_id"] = rule.id;
                        detail["type"] = "offline";
                        detail["duration"] = cond.duration;
                        detail["lastReportTime"] = lastTime;
                        detail["elapsedSeconds"] = static_cast<Json::Int64>(elapsed);

                        std::string message = rule.name + " [设备离线超过 " +
                            std::to_string(cond.duration) + " 秒，已 " +
                            std::to_string(elapsed) + " 秒未上报]";
                        co_await triggerAlert(rule, message, detail);
                    }
                } catch (const std::exception& e) {
                    LOG_WARN << "[AlertEngine] Failed to parse report time: " << e.what();
                }
            }
        });
    }
};
