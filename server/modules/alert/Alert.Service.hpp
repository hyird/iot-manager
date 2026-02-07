#pragma once

#include "domain/AlertRule.hpp"
#include "domain/AlertRecord.hpp"
#include "domain/AlertEngine.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 告警业务服务层
 *
 * 协调 AlertRule 聚合根操作，CRUD 后自动刷新 AlertEngine 内存规则。
 */
class AlertService {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    // ==================== 规则 CRUD ====================

    Task<void> createRule(const Json::Value& data) {
        co_await AlertRule::create(data)
            .require(AlertRule::nameUnique)
            .require(AlertRule::deviceExists)
            .save();

        co_await AlertEngine::instance().reloadRules();
    }

    Task<void> updateRule(int id, const Json::Value& data) {
        auto rule = co_await AlertRule::of(id);

        if (data.isMember("name")) {
            rule.require(AlertRule::nameUnique);
        }
        if (data.isMember("device_id")) {
            rule.require(AlertRule::deviceExists);
        }

        rule.update(data);
        co_await rule.save();

        co_await AlertEngine::instance().reloadRules();
    }

    Task<void> deleteRule(int id) {
        auto rule = co_await AlertRule::of(id);
        rule.remove();
        co_await rule.save();

        co_await AlertEngine::instance().reloadRules();
    }

    Task<Json::Value> getRuleDetail(int id) {
        auto rule = co_await AlertRule::of(id);
        co_return rule.toJson();
    }

    Task<PagedResult<AlertRule>> listRules(const Pagination& page, int deviceId = 0, const std::string& severity = "") {
        co_return co_await AlertRule::list(page, deviceId, severity);
    }

    // ==================== 告警记录 ====================

    Task<std::pair<Json::Value, int>> listRecords(int page, int pageSize,
        int deviceId = 0, int ruleId = 0,
        const std::string& status = "", const std::string& severity = "") {
        co_return co_await AlertRecord::list(page, pageSize, deviceId, ruleId, status, severity);
    }

    Task<void> acknowledgeRecord(int64_t id, int userId) {
        co_await AlertRecord::acknowledge(id, userId);
    }

    Task<void> batchAcknowledge(const std::vector<int64_t>& ids, int userId) {
        co_await AlertRecord::batchAcknowledge(ids, userId);
    }

    // ==================== 统计 ====================

    Task<Json::Value> activeAlertStats() {
        co_return co_await AlertRecord::activeStats();
    }
};
