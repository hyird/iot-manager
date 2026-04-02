#pragma once

#include "../MigrationTypes.hpp"

class V003_AgentSelfRegistration : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 3,
            .name = "AgentSelfRegistration",
            .description = "Support agent self-registration by SN/model with approval workflow",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            ALTER TABLE agent_node
            ADD COLUMN IF NOT EXISTS sn VARCHAR(64)
        )");
        co_await txn->execSqlCoro(R"(
            ALTER TABLE agent_node
            ADD COLUMN IF NOT EXISTS model VARCHAR(128)
        )");
        co_await txn->execSqlCoro(R"(
            ALTER TABLE agent_node
            ADD COLUMN IF NOT EXISTS auth_status VARCHAR(20) NOT NULL DEFAULT 'approved'
        )");
        co_await txn->execSqlCoro(R"(
            ALTER TABLE agent_node
            ADD COLUMN IF NOT EXISTS approved_at TIMESTAMPTZ NULL
        )");
        co_await txn->execSqlCoro(R"(
            ALTER TABLE agent_node
            ADD COLUMN IF NOT EXISTS approved_by INT NULL
        )");

        co_await txn->execSqlCoro(R"(
            UPDATE agent_node
            SET sn = UPPER(code)
            WHERE (sn IS NULL OR sn = '') AND deleted_at IS NULL
        )");
        co_await txn->execSqlCoro(R"(
            UPDATE agent_node
            SET auth_status = 'approved',
                approved_at = COALESCE(approved_at, CURRENT_TIMESTAMP)
            WHERE deleted_at IS NULL AND (auth_status IS NULL OR auth_status = '')
        )");
        co_await txn->execSqlCoro(R"(
            UPDATE agent_node
            SET auth_status = 'approved',
                approved_at = COALESCE(approved_at, CURRENT_TIMESTAMP)
            WHERE deleted_at IS NULL AND auth_status NOT IN ('pending', 'approved', 'rejected')
        )");

        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_agent_node_auth_status
            ON agent_node (auth_status)
            WHERE deleted_at IS NULL
        )");
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_agent_node_sn
            ON agent_node (sn)
            WHERE deleted_at IS NULL AND sn IS NOT NULL
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_agent_node_sn");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_agent_node_auth_status");
        co_await txn->execSqlCoro("ALTER TABLE agent_node DROP COLUMN IF EXISTS approved_by");
        co_await txn->execSqlCoro("ALTER TABLE agent_node DROP COLUMN IF EXISTS approved_at");
        co_await txn->execSqlCoro("ALTER TABLE agent_node DROP COLUMN IF EXISTS auth_status");
        co_await txn->execSqlCoro("ALTER TABLE agent_node DROP COLUMN IF EXISTS model");
        co_await txn->execSqlCoro("ALTER TABLE agent_node DROP COLUMN IF EXISTS sn");
    }
};

