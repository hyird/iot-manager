#pragma once

#include "../MigrationTypes.hpp"

class V004_ResourceOwnershipAndDeviceShare : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 4,
            .name = "ResourceOwnershipAndDeviceShare",
            .description = "Add created_by ownership fields and device share permission table",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        // 资源创建者字段（用于创建者编辑/删除权限）
        co_await txn->execSqlCoro("ALTER TABLE link ADD COLUMN IF NOT EXISTS created_by INT");
        co_await txn->execSqlCoro("ALTER TABLE protocol_config ADD COLUMN IF NOT EXISTS created_by INT");
        co_await txn->execSqlCoro("ALTER TABLE device ADD COLUMN IF NOT EXISTS created_by INT");

        // 历史数据兜底：默认归属到 admin(1)
        co_await txn->execSqlCoro("UPDATE link SET created_by = COALESCE(created_by, 1)");
        co_await txn->execSqlCoro("UPDATE protocol_config SET created_by = COALESCE(created_by, 1)");
        co_await txn->execSqlCoro("UPDATE device SET created_by = COALESCE(created_by, 1)");

        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_link_created_by
            ON link (created_by)
            WHERE deleted_at IS NULL
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_protocol_config_created_by
            ON protocol_config (created_by)
            WHERE deleted_at IS NULL
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_created_by
            ON device (created_by)
            WHERE deleted_at IS NULL
        )");

        // 设备分享权限（仅设备支持分享）
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS device_share (
                id SERIAL PRIMARY KEY,
                device_id INT NOT NULL,
                user_id INT NOT NULL,
                permission VARCHAR(20) NOT NULL DEFAULT 'view',
                created_by INT NULL,
                created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                UNIQUE (device_id, user_id),
                CONSTRAINT chk_device_share_permission CHECK (permission IN ('view', 'control'))
            )
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_share_device_id
            ON device_share (device_id)
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_share_user_id
            ON device_share (user_id)
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_user_id");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_device_id");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS device_share");

        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_created_by");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_protocol_config_created_by");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_link_created_by");

        co_await txn->execSqlCoro("ALTER TABLE device DROP COLUMN IF EXISTS created_by");
        co_await txn->execSqlCoro("ALTER TABLE protocol_config DROP COLUMN IF EXISTS created_by");
        co_await txn->execSqlCoro("ALTER TABLE link DROP COLUMN IF EXISTS created_by");
    }
};

