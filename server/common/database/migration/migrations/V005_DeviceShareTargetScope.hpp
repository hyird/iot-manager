#pragma once

#include "../MigrationTypes.hpp"

class V005_DeviceShareTargetScope : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 5,
            .name = "DeviceShareTargetScope",
            .description = "Support device share targets by user or department scope",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        // 兼容旧表结构：允许部门级分享（无需展开为用户）
        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS target_type VARCHAR(20)");
        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS target_id INT");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN user_id DROP NOT NULL");

        // 历史数据回填：旧记录视为用户级分享
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET target_type = COALESCE(NULLIF(target_type, ''), 'user')
            WHERE target_type IS NULL OR target_type = ''
        )");
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET target_id = user_id
            WHERE target_id IS NULL AND user_id IS NOT NULL
        )");

        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN target_type SET NOT NULL");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN target_id SET NOT NULL");

        co_await txn->execSqlCoro("ALTER TABLE device_share DROP CONSTRAINT IF EXISTS device_share_device_id_user_id_key");

        co_await txn->execSqlCoro(R"(
            DO $$
            BEGIN
                IF NOT EXISTS (
                    SELECT 1
                    FROM pg_constraint
                    WHERE conname = 'chk_device_share_target_type'
                ) THEN
                    ALTER TABLE device_share
                    ADD CONSTRAINT chk_device_share_target_type
                    CHECK (target_type IN ('user', 'department'));
                END IF;
            END $$;
        )");

        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_device_share_target_unique
            ON device_share (device_id, target_type, target_id)
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_share_target_scope
            ON device_share (target_type, target_id)
        )");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_user_id");
    }

    Task<> down(const TransactionPtr& txn) override {
        // 回滚前先清理部门级记录（旧结构仅支持 user_id）
        co_await txn->execSqlCoro("DELETE FROM device_share WHERE target_type = 'department'");
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET user_id = target_id
            WHERE target_type = 'user' AND user_id IS NULL
        )");

        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_target_scope");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_target_unique");
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP CONSTRAINT IF EXISTS chk_device_share_target_type");

        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS target_type");
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS target_id");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN user_id SET NOT NULL");

        co_await txn->execSqlCoro(R"(
            DO $$
            BEGIN
                IF NOT EXISTS (
                    SELECT 1
                    FROM pg_constraint
                    WHERE conname = 'device_share_device_id_user_id_key'
                ) THEN
                    ALTER TABLE device_share
                    ADD CONSTRAINT device_share_device_id_user_id_key
                    UNIQUE (device_id, user_id);
                END IF;
            END $$;
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_share_user_id
            ON device_share (user_id)
        )");
    }
};

