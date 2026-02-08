#pragma once

#include "common/domain/DomainEvent.hpp"

// ==================== 告警规则事件 ====================

struct AlertRuleCreated : DomainEvent {
    AlertRuleCreated(int ruleId)
        : DomainEvent("AlertRuleCreated", ruleId, "AlertRule") {}
};

struct AlertRuleUpdated : DomainEvent {
    AlertRuleUpdated(int ruleId)
        : DomainEvent("AlertRuleUpdated", ruleId, "AlertRule") {}
};

struct AlertRuleDeleted : DomainEvent {
    AlertRuleDeleted(int ruleId)
        : DomainEvent("AlertRuleDeleted", ruleId, "AlertRule") {}
};

// ==================== 告警触发事件 ====================

struct AlertTriggered : DomainEvent {
    int deviceId = 0;
    int ruleId = 0;
    std::string severity;
    std::string message;
    std::string deviceName;
    int64_t recordId = 0;

    AlertTriggered(int64_t recId, int devId, int rlId, std::string sev, std::string msg, std::string devName)
        : DomainEvent("AlertTriggered", static_cast<int>(recId), "AlertRecord")
        , deviceId(devId)
        , ruleId(rlId)
        , severity(std::move(sev))
        , message(std::move(msg))
        , deviceName(std::move(devName))
        , recordId(recId) {}
};

struct AlertResolved : DomainEvent {
    int deviceId = 0;
    int ruleId = 0;
    std::string reason;              // "条件恢复" | "手动恢复" | "自动恢复"
    Json::Value recoveryData;        // 恢复时的数据值

    AlertResolved(int64_t recId, int devId, int rlId,
                  const std::string& rsn = "手动恢复",
                  const Json::Value& data = Json::Value())
        : DomainEvent("AlertResolved", static_cast<int>(recId), "AlertRecord")
        , deviceId(devId)
        , ruleId(rlId)
        , reason(rsn)
        , recoveryData(data) {}
};
