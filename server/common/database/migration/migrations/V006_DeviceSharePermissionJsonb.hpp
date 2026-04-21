#pragma once

#include "../MigrationTypes.hpp"

class V006_DeviceSharePermissionJsonb : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 6,
            .name = "DeviceSharePermissionJsonb",
            .description = "Use JSONB permission and remove legacy user_id from device_share",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP CONSTRAINT IF EXISTS chk_device_share_permission");

        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS permission_json JSONB");
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET permission_json = CASE
                WHEN permission = 'control' THEN '{"view":true,"control":true}'::jsonb
                ELSE '{"view":true,"control":false}'::jsonb
            END
            WHERE permission_json IS NULL
        )");
        co_await txn->execSqlCoro(R"(
            ALTER TABLE device_share
            ALTER COLUMN permission_json SET DEFAULT '{"view":true,"control":false}'::jsonb
        )");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN permission_json SET NOT NULL");

        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS permission");
        co_await txn->execSqlCoro("ALTER TABLE device_share RENAME COLUMN permission_json TO permission");
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS user_id");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_share_user_id");

        co_await txn->execSqlCoro(R"(
            DO $$
            BEGIN
                IF NOT EXISTS (
                    SELECT 1
                    FROM pg_constraint
                    WHERE conname = 'chk_device_share_permission_json'
                ) THEN
                    ALTER TABLE device_share
                    ADD CONSTRAINT chk_device_share_permission_json
                    CHECK (
                        jsonb_typeof(permission) = 'object'
                        AND jsonb_typeof(permission->'view') = 'boolean'
                        AND jsonb_typeof(permission->'control') = 'boolean'
                    );
                END IF;
            END $$;
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS user_id INT");
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET user_id = target_id
            WHERE target_type = 'user'
        )");

        co_await txn->execSqlCoro("ALTER TABLE device_share ADD COLUMN IF NOT EXISTS permission_text VARCHAR(20)");
        co_await txn->execSqlCoro(R"(
            UPDATE device_share
            SET permission_text = CASE
                WHEN COALESCE((permission->>'control')::boolean, false) THEN 'control'
                ELSE 'view'
            END
            WHERE permission_text IS NULL
        )");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN permission_text SET DEFAULT 'view'");
        co_await txn->execSqlCoro("ALTER TABLE device_share ALTER COLUMN permission_text SET NOT NULL");

        co_await txn->execSqlCoro("ALTER TABLE device_share DROP CONSTRAINT IF EXISTS chk_device_share_permission_json");
        co_await txn->execSqlCoro("ALTER TABLE device_share DROP COLUMN IF EXISTS permission");
        co_await txn->execSqlCoro("ALTER TABLE device_share RENAME COLUMN permission_text TO permission");

        co_await txn->execSqlCoro(R"(
            DO $$
            BEGIN
                IF NOT EXISTS (
                    SELECT 1
                    FROM pg_constraint
                    WHERE conname = 'chk_device_share_permission'
                ) THEN
                    ALTER TABLE device_share
                    ADD CONSTRAINT chk_device_share_permission
                    CHECK (permission IN ('view', 'control'));
                END IF;
            END $$;
        )");
    }
};
