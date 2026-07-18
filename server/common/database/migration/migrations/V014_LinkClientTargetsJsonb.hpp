#pragma once

#include "../MigrationTypes.hpp"

class V014_LinkClientTargetsJsonb : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 14,
            .name = "LinkClientTargetsJsonb",
            .description = "Store TCP Client targets in link.targets JSONB and route devices by target_id",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            ALTER TABLE link
            ADD COLUMN IF NOT EXISTS targets JSONB NOT NULL DEFAULT '[]'::jsonb
        )");

        // 每条旧 TCP Client 链路迁移为一个稳定的默认目标。
        co_await txn->execSqlCoro(R"(
            UPDATE link
            SET targets = jsonb_build_array(jsonb_build_object(
                'id', 'legacy-' || id::text,
                'name', COALESCE(NULLIF(name, ''), '默认目标'),
                'ip', ip,
                'port', port,
                'status', 'enabled'
            ))
            WHERE mode = 'TCP Client'
              AND deleted_at IS NULL
              AND jsonb_array_length(targets) = 0
        )");

        // 设备继续使用现有 protocol_params JSONB，不新增列。
        co_await txn->execSqlCoro(R"(
            UPDATE device d
            SET protocol_params = COALESCE(d.protocol_params, '{}'::jsonb)
                || jsonb_build_object('target_id', 'legacy-' || d.link_id::text)
            FROM link l
            WHERE d.link_id = l.id
              AND l.mode = 'TCP Client'
              AND d.deleted_at IS NULL
              AND l.deleted_at IS NULL
              AND COALESCE(d.protocol_params->>'target_id', '') = ''
        )");

        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_link_endpoint_route");
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_link_server_endpoint_route
            ON link (mode, ip, port, COALESCE(agent_id, 0), COALESCE(agent_bind_ip, ''))
            WHERE deleted_at IS NULL AND mode = 'TCP Server'
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_link_target
            ON device (link_id, (protocol_params->>'target_id'))
            WHERE deleted_at IS NULL
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_link_target");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_link_server_endpoint_route");
        co_await txn->execSqlCoro(R"(
            UPDATE device
            SET protocol_params = protocol_params - 'target_id'
            WHERE protocol_params ? 'target_id'
        )");
        co_await txn->execSqlCoro("ALTER TABLE link DROP COLUMN IF EXISTS targets");
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_link_endpoint_route
            ON link (mode, ip, port, COALESCE(agent_id, 0), COALESCE(agent_bind_ip, ''))
            WHERE deleted_at IS NULL
        )");
    }
};
