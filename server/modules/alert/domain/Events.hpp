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

    AlertResolved(int64_t recId, int devId, int rlId)
        : DomainEvent("AlertResolved", static_cast<int>(recId), "AlertRecord")
        , deviceId(devId)
        , ruleId(rlId) {}
};
