#pragma once

#include "../MigrationTypes.hpp"

class V008_DeviceShareDropTargetColumns : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 8,
            .name = "DeviceShareDropTargetColumns",
            .description = "Drop device_share target_type/target_id columns and use permission JSONB only",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        // 最后一轮回填，确保历史数据都带上 target 信息
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

        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_target_scope");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_target_unique");
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP CONSTRAINT IF EXISTS chk_device_share_target_type");

        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS target_type");
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS target_id");

        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_device_share_target_unique_json
            ON device_share (
                device_id,
                (permission->>'target_type'),
                (permission->>'target_id')
            )
        )");

        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_share_target_scope_json
            ON device_share (
                (permission->>'target_type'),
                (permission->>'target_id')
            )
        )");

        co_await txn->execSqlCoro(R"(
            DO $$
            BEGIN
                IF NOT EXISTS (
                    SELECT 1
                    FROM pg_constraint
                    WHERE conname = 'chk_device_share_permission_scope_json'
                ) THEN
                    ALTER TABLE device_share
                    ADD CONSTRAINT chk_device_share_permission_scope_json
                    CHECK (
                        jsonb_typeof(permission) = 'object'
                        AND jsonb_typeof(permission->'view') = 'boolean'
                        AND jsonb_typeof(permission->'control') = 'boolean'
                        AND jsonb_typeof(permission->'target_type') = 'string'
                        AND (permission->>'target_type') IN ('user', 'department')
                        AND jsonb_exists(permission, 'target_id')
                        AND (
                            (
                                jsonb_typeof(permission->'target_id') = 'number'
                                AND (permission->>'target_id') ~ '^[1-9][0-9]*$'
                            )
                            OR (
                                jsonb_typeof(permission->'target_id') = 'string'
                                AND (permission->>'target_id') ~ '^[1-9][0-9]*$'
                            )
                        )
                    );
                END IF;
            END $$;
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP CONSTRAINT IF EXISTS chk_device_share_permission_scope_json");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_target_scope_json");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_target_unique_json");

        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS target_type VARCHAR(20)");
        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS target_id INT");

        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET target_type = COALESCE(NULLIF(permission->>'target_type', ''), 'user'),
                target_id = CASE
                    WHEN jsonb_exists(permission, 'target_id')
                         AND jsonb_typeof(permission->'target_id') = 'number'
                        THEN (permission->>'target_id')::INT
                    WHEN jsonb_exists(permission, 'target_id')
                         AND jsonb_typeof(permission->'target_id') = 'string'
                         AND (permission->>'target_id') ~ '^[0-9]+$'
                        THEN (permission->>'target_id')::INT
                    ELSE 0
                END
        )");

        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN target_type SET NOT NULL");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN target_id SET NOT NULL");

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
    }
};

