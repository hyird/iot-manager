#pragma once

#include "../MigrationTypes.hpp"

class V007_DeviceShareTargetInPermission : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 7,
            .name = "DeviceShareTargetInPermission",
            .description = "Store target_type and target_id into device_share.permission JSONB",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET permission = jsonb_set(
                jsonb_set(
                    COALESCE(permission, '{}'::jsonb),
                    '{target_type}',
                    to_jsonb(target_type::text),
                    true
                ),
                '{target_id}',
                to_jsonb(target_id),
                true
            )
            WHERE target_type IS NOT NULL
              AND target_id IS NOT NULL
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET permission = COALESCE(permission, '{}'::jsonb) - 'target_type' - 'target_id'
        )");
    }
};

