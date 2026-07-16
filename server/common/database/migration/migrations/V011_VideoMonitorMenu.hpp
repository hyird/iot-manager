#pragma once

#include "../MigrationTypes.hpp"

class V011_VideoMonitorMenu : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 11,
            .name = "VideoMonitorMenu",
            .description = "Rename GB28181 page to video monitor and remove standalone PTZ permission",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            UPDATE sys_menu
            SET name = '视频监控',
                updated_at = CURRENT_TIMESTAMP
            WHERE (component = 'GB28181' OR path = '/iot/gb28181')
              AND deleted_at IS NULL
        )");

        co_await txn->execSqlCoro(R"(
            DELETE FROM sys_role_menu
            WHERE menu_id IN (
                SELECT id
                FROM sys_menu
                WHERE permission_code = 'iot:gb28181:ptz'
            )
        )");

        co_await txn->execSqlCoro(R"(
            UPDATE sys_menu
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE permission_code = 'iot:gb28181:ptz'
              AND deleted_at IS NULL
        )");

        co_await txn->execSqlCoro(R"(
            UPDATE sys_menu
            SET sort_order = 3,
                updated_at = CURRENT_TIMESTAMP
            WHERE permission_code = 'iot:gb28181:record'
              AND deleted_at IS NULL
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            UPDATE sys_menu
            SET name = 'GB28181',
                updated_at = CURRENT_TIMESTAMP
            WHERE (component = 'GB28181' OR path = '/iot/gb28181')
              AND deleted_at IS NULL
        )");

        co_await txn->execSqlCoro(R"(
            UPDATE sys_menu
            SET deleted_at = NULL,
                status = 'enabled',
                updated_at = CURRENT_TIMESTAMP
            WHERE permission_code = 'iot:gb28181:ptz'
        )");
    }
};
