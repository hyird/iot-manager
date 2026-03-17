#pragma once

#include "../MigrationTypes.hpp"

/**
 * @brief V001 基线迁移 — 完整 schema 定义
 *
 * 从原 DatabaseInitializer.hpp 提取的全部 schema：
 * - 枚举类型（status_enum, menu_type_enum）
 * - 20+ 张核心表及索引
 * - 数据迁移（agent_endpoint 回填、device.protocol_params 回填）
 * - 触发器（13 个 updated_at 自动更新触发器）
 *
 * 对于已有数据库，MigrationRunner 会自动将此迁移标记为基线，跳过执行。
 * 对于全新数据库，此迁移会创建完整的表结构。
 *
 * 注意：TimescaleDB 和物化视图不包含在此迁移中（见 V002_TimescaleDB）。
 * 物化视图依赖运行时数据（protocol_config 表内容），保留在 DatabaseInitializer 中。
 */
class V001_Baseline : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 1,
            .name = "Baseline",
            .description = "Initial schema: enums, tables, indexes, triggers",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        // ==================== 枚举类型 ====================
        co_await createEnumTypes(txn);

        // ==================== 系统管理表 ====================
        co_await createSystemTables(txn);

        // ==================== Agent 采集表 ====================
        co_await createAgentTables(txn);

        // ==================== 链路与协议表 ====================
        co_await createLinkAndProtocolTables(txn);

        // ==================== 设备表 ====================
        co_await createDeviceTables(txn);

        // ==================== 设备数据表 ====================
        co_await createDeviceDataTable(txn);

        // ==================== 开放访问表 ====================
        co_await createOpenAccessTables(txn);

        // ==================== 告警表 ====================
        co_await createAlertTables(txn);

        // ==================== 归档表 ====================
        co_await createArchiveTable(txn);

        // ==================== 触发器 ====================
        co_await createTriggers(txn);

        // ==================== 数据迁移 ====================
        co_await migrateData(txn);
    }

    Task<> down(const TransactionPtr& txn) override {
        // 回滚：按依赖反序删除（仅限开发环境）
        // 先删触发器函数（CASCADE 会删除所有依赖触发器）
        co_await txn->execSqlCoro("DROP FUNCTION IF EXISTS update_updated_at_column() CASCADE");
        co_await txn->execSqlCoro("DROP FUNCTION IF EXISTS archive_old_device_data(INT) CASCADE");
        co_await txn->execSqlCoro("DROP FUNCTION IF EXISTS refresh_device_data_views() CASCADE");
        co_await txn->execSqlCoro("DROP VIEW IF EXISTS device_data_all CASCADE");

        // 按依赖反序删除表
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS alert_rule_template CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS alert_record CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS alert_rule CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS open_access_log CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS open_webhook CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS open_access_key_device CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS open_access_key CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS device_data_archive CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS device_data CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS device CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS device_group CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS protocol_config CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS link CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS agent_endpoint CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS agent_event CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS agent_node CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS sys_role_menu CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS sys_user_role CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS sys_user CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS sys_role CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS sys_department CASCADE");
        co_await txn->execSqlCoro("DROP TABLE IF EXISTS sys_menu CASCADE");

        // 删除枚举类型
        co_await txn->execSqlCoro("DROP TYPE IF EXISTS menu_type_enum CASCADE");
        co_await txn->execSqlCoro("DROP TYPE IF EXISTS status_enum CASCADE");
    }

private:
    // ─── 枚举类型 ────────────────────────────────────────────

    Task<> createEnumTypes(const TransactionPtr& txn) {
        co_await txn->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TYPE status_enum AS ENUM ('enabled', 'disabled');
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await txn->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TYPE menu_type_enum AS ENUM ('menu', 'page', 'button');
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");
    }

    // ─── 系统管理表 ──────────────────────────────────────────

    Task<> createSystemTables(const TransactionPtr& txn) {
        // 菜单表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_menu (
                id SERIAL PRIMARY KEY,
                name VARCHAR(50) NOT NULL,
                parent_id INT DEFAULT 0,
                type menu_type_enum DEFAULT 'page',
                path VARCHAR(255),
                component VARCHAR(255),
                permission_code VARCHAR(100),
                icon VARCHAR(100),
                is_default BOOLEAN DEFAULT FALSE,
                status status_enum DEFAULT 'enabled',
                sort_order INT DEFAULT 0,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_parent ON sys_menu (parent_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_deleted ON sys_menu (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_status_deleted ON sys_menu (status, deleted_at, sort_order))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_permission ON sys_menu (permission_code) WHERE deleted_at IS NULL AND permission_code IS NOT NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_parent_active ON sys_menu (parent_id) WHERE deleted_at IS NULL)");

        // 部门表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_department (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                code VARCHAR(50),
                parent_id INT DEFAULT 0,
                leader_id INT,
                phone VARCHAR(20),
                email VARCHAR(100),
                status status_enum DEFAULT 'enabled',
                sort_order INT DEFAULT 0,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_department_parent ON sys_department (parent_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_department_deleted ON sys_department (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_department_code ON sys_department (code) WHERE code IS NOT NULL AND deleted_at IS NULL)");

        // 角色表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_role (
                id SERIAL PRIMARY KEY,
                name VARCHAR(50) NOT NULL,
                code VARCHAR(50) NOT NULL,
                description VARCHAR(255),
                status status_enum DEFAULT 'enabled',
                sort_order INT DEFAULT 0,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_role_code_active ON sys_role (code) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_deleted ON sys_role (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_status_deleted ON sys_role (status, deleted_at))");

        // 用户表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_user (
                id SERIAL PRIMARY KEY,
                username VARCHAR(50) NOT NULL,
                password_hash VARCHAR(255) NOT NULL,
                nickname VARCHAR(100),
                email VARCHAR(100),
                phone VARCHAR(20),
                avatar VARCHAR(255),
                status status_enum DEFAULT 'enabled',
                department_id INT,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_user_username_active ON sys_user (username) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_deleted ON sys_user (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_department ON sys_user (department_id) WHERE deleted_at IS NULL)");

        // 用户角色关联表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_user_role (
                id SERIAL PRIMARY KEY,
                user_id INT NOT NULL,
                role_id INT NOT NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                UNIQUE (user_id, role_id)
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_role_user ON sys_user_role (user_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_role_role ON sys_user_role (role_id))");

        // 角色菜单关联表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_role_menu (
                id SERIAL PRIMARY KEY,
                role_id INT NOT NULL,
                menu_id INT NOT NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                UNIQUE (role_id, menu_id)
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_menu_role ON sys_role_menu (role_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_menu_menu ON sys_role_menu (menu_id))");
    }

    // ─── Agent 采集表 ────────────────────────────────────────

    Task<> createAgentTables(const TransactionPtr& txn) {
        // Agent 节点表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS agent_node (
                id SERIAL PRIMARY KEY,
                code VARCHAR(64) NOT NULL,
                name VARCHAR(100) NOT NULL,
                version VARCHAR(64),
                capabilities JSONB NOT NULL DEFAULT '{}'::jsonb,
                runtime JSONB NOT NULL DEFAULT '{}'::jsonb,
                is_online BOOLEAN NOT NULL DEFAULT FALSE,
                last_seen TIMESTAMPTZ NULL,
                connected_at TIMESTAMPTZ NULL,
                expected_config_version BIGINT NOT NULL DEFAULT 0,
                applied_config_version BIGINT NOT NULL DEFAULT 0,
                config_status VARCHAR(20) NOT NULL DEFAULT 'idle',
                config_error TEXT,
                last_config_sync_at TIMESTAMPTZ NULL,
                last_config_applied_at TIMESTAMPTZ NULL,
                secret VARCHAR(128),
                network_config JSONB NOT NULL DEFAULT '[]'::jsonb,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_agent_node_code ON agent_node (code) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_agent_node_online ON agent_node (is_online))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_agent_node_deleted ON agent_node (deleted_at))");

        // Agent 事件表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS agent_event (
                id BIGSERIAL PRIMARY KEY,
                agent_id INT NOT NULL,
                event_type VARCHAR(50) NOT NULL,
                level VARCHAR(20) NOT NULL DEFAULT 'info',
                message TEXT NOT NULL,
                detail JSONB NOT NULL DEFAULT '{}'::jsonb,
                created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_agent_event_agent_time ON agent_event (agent_id, created_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_agent_event_created ON agent_event (created_at DESC))");

        // Agent 端点表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS agent_endpoint (
                id SERIAL PRIMARY KEY,
                agent_id INT NOT NULL,
                name VARCHAR(100) NOT NULL,
                transport VARCHAR(20) NOT NULL DEFAULT 'ethernet',
                mode VARCHAR(20),
                protocol VARCHAR(20) NOT NULL DEFAULT 'SL651',
                ip VARCHAR(45),
                port INT,
                channel VARCHAR(100),
                baud_rate INT,
                status VARCHAR(20) NOT NULL DEFAULT 'enabled',
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_agent_endpoint_agent ON agent_endpoint (agent_id) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_agent_endpoint_deleted ON agent_endpoint (deleted_at))");
    }

    // ─── 链路与协议表 ────────────────────────────────────────

    Task<> createLinkAndProtocolTables(const TransactionPtr& txn) {
        // 链路表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS link (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                mode VARCHAR(20) NOT NULL,
                protocol VARCHAR(20) NOT NULL DEFAULT 'SL651',
                ip VARCHAR(50) NOT NULL,
                port INT NOT NULL,
                usage VARCHAR(20) NOT NULL DEFAULT 'device',
                status status_enum DEFAULT 'enabled',
                agent_id INT NULL,
                agent_interface VARCHAR(100),
                agent_bind_ip VARCHAR(50),
                agent_prefix_length INT,
                agent_gateway VARCHAR(50),
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_mode ON link (mode))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_deleted ON link (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_link_name ON link (name) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_link_endpoint_route ON link (mode, ip, port, COALESCE(agent_id, 0), COALESCE(agent_bind_ip, '')) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_status_active ON link (status) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_protocol ON link (protocol))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_usage ON link (usage))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_agent_id ON link (agent_id) WHERE deleted_at IS NULL)");

        // 协议配置表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS protocol_config (
                id SERIAL PRIMARY KEY,
                protocol VARCHAR(20) NOT NULL,
                name VARCHAR(64) NOT NULL,
                enabled BOOLEAN DEFAULT TRUE,
                config JSONB NOT NULL DEFAULT '{}',
                remark TEXT,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_protocol_config_protocol ON protocol_config (protocol))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_protocol_config_deleted ON protocol_config (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_protocol_config_name ON protocol_config (name) WHERE deleted_at IS NULL)");
    }

    // ─── 设备表 ──────────────────────────────────────────────

    Task<> createDeviceTables(const TransactionPtr& txn) {
        // 设备分组表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS device_group (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                parent_id INT DEFAULT 0,
                status status_enum DEFAULT 'enabled',
                sort_order INT DEFAULT 0,
                remark TEXT,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_group_parent ON device_group (parent_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_group_deleted ON device_group (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_device_group_name ON device_group (name) WHERE deleted_at IS NULL)");

        // 设备表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS device (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                link_id INT NOT NULL,
                protocol_config_id INT NOT NULL,
                group_id INT NULL,
                status status_enum DEFAULT 'enabled',
                protocol_params JSONB DEFAULT '{}',
                remark TEXT,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_link ON device (link_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_protocol ON device (protocol_config_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_group ON device (group_id) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_deleted ON device (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_device_name ON device (name) WHERE deleted_at IS NULL)");

        // JSONB 函数索引
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_device_protocol_params_code
            ON device ((protocol_params->>'device_code'))
            WHERE deleted_at IS NULL
              AND protocol_params->>'device_code' IS NOT NULL
              AND protocol_params->>'device_code' != ''
        )");
        co_await txn->execSqlCoro(R"(
            CREATE INDEX IF NOT EXISTS idx_device_agent_id
            ON device (((protocol_params->>'agent_id')::INT))
            WHERE deleted_at IS NULL
              AND protocol_params->>'agent_id' IS NOT NULL
        )");
    }

    // ─── 设备数据表 ──────────────────────────────────────────

    Task<> createDeviceDataTable(const TransactionPtr& txn) {
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS device_data (
                id BIGSERIAL,
                device_id INT NOT NULL,
                link_id INT NOT NULL,
                protocol TEXT NOT NULL,
                data JSONB NOT NULL DEFAULT '{}',
                report_time TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                PRIMARY KEY (id, report_time)
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_data_device_time ON device_data (device_id, report_time DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_data_link_time ON device_data (link_id, report_time DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_data_history ON device_data (device_id, (data->>'funcCode'), report_time DESC))");
    }

    // ─── 开放访问表 ──────────────────────────────────────────

    Task<> createOpenAccessTables(const TransactionPtr& txn) {
        // AccessKey 表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS open_access_key (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                access_key_prefix VARCHAR(32) NOT NULL,
                access_key_hash VARCHAR(64) NOT NULL,
                status status_enum NOT NULL DEFAULT 'enabled',
                allow_realtime BOOLEAN NOT NULL DEFAULT TRUE,
                allow_history BOOLEAN NOT NULL DEFAULT TRUE,
                allow_command BOOLEAN NOT NULL DEFAULT FALSE,
                allow_alert BOOLEAN NOT NULL DEFAULT FALSE,
                expires_at TIMESTAMPTZ NULL,
                last_used_at TIMESTAMPTZ NULL,
                last_used_ip VARCHAR(64),
                created_by INT NULL,
                remark TEXT,
                created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_open_access_key_hash ON open_access_key (access_key_hash))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_key_status ON open_access_key (status) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_key_expires ON open_access_key (expires_at) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_key_deleted ON open_access_key (deleted_at))");

        // AccessKey 设备绑定表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS open_access_key_device (
                id SERIAL PRIMARY KEY,
                access_key_id INT NOT NULL,
                device_id INT NOT NULL,
                created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                UNIQUE (access_key_id, device_id)
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_key_device_key ON open_access_key_device (access_key_id))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_key_device_device ON open_access_key_device (device_id))");

        // Webhook 表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS open_webhook (
                id SERIAL PRIMARY KEY,
                access_key_id INT NOT NULL,
                name VARCHAR(100) NOT NULL,
                url TEXT NOT NULL,
                status status_enum NOT NULL DEFAULT 'enabled',
                secret VARCHAR(255),
                headers JSONB NOT NULL DEFAULT '{}'::jsonb,
                event_types JSONB NOT NULL DEFAULT '["device.data.reported"]'::jsonb,
                timeout_seconds INT NOT NULL DEFAULT 5,
                last_triggered_at TIMESTAMPTZ NULL,
                last_success_at TIMESTAMPTZ NULL,
                last_failure_at TIMESTAMPTZ NULL,
                last_http_status INT NULL,
                last_error TEXT,
                created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_webhook_access_key ON open_webhook (access_key_id) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_webhook_status ON open_webhook (status) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_webhook_deleted ON open_webhook (deleted_at))");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_open_webhook_key_name ON open_webhook (access_key_id, name) WHERE deleted_at IS NULL)");

        // 调用日志表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS open_access_log (
                id BIGSERIAL PRIMARY KEY,
                access_key_id INT NULL,
                webhook_id INT NULL,
                direction VARCHAR(20) NOT NULL,
                action VARCHAR(50) NOT NULL,
                event_type VARCHAR(100) NULL,
                status VARCHAR(20) NOT NULL DEFAULT 'success',
                http_method VARCHAR(10) NULL,
                target TEXT NULL,
                request_ip VARCHAR(64) NULL,
                http_status INT NULL,
                device_id INT NULL,
                device_code VARCHAR(100) NULL,
                message TEXT NULL,
                request_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
                response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
                created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_log_access_key ON open_access_log (access_key_id, created_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_log_webhook ON open_access_log (webhook_id, created_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_log_device ON open_access_log (device_id, created_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_open_access_log_action ON open_access_log (action, created_at DESC))");
    }

    // ─── 告警表 ──────────────────────────────────────────────

    Task<> createAlertTables(const TransactionPtr& txn) {
        // 告警规则表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS alert_rule (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                device_id INT NOT NULL,
                severity VARCHAR(20) NOT NULL DEFAULT 'warning',
                conditions JSONB NOT NULL DEFAULT '[]',
                logic VARCHAR(5) NOT NULL DEFAULT 'and',
                silence_duration INT NOT NULL DEFAULT 300,
                recovery_condition VARCHAR(50) DEFAULT 'reverse',
                recovery_wait_seconds INT DEFAULT 60,
                status status_enum DEFAULT 'enabled',
                remark TEXT,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_alert_rule_name ON alert_rule (name) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_rule_device ON alert_rule (device_id) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_rule_status ON alert_rule (status) WHERE deleted_at IS NULL)");

        // 告警记录表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS alert_record (
                id BIGSERIAL PRIMARY KEY,
                rule_id INT NOT NULL,
                device_id INT NOT NULL,
                severity VARCHAR(20) NOT NULL,
                status VARCHAR(20) NOT NULL DEFAULT 'active',
                message TEXT NOT NULL,
                detail JSONB NOT NULL DEFAULT '{}',
                triggered_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                acknowledged_at TIMESTAMPTZ NULL,
                acknowledged_by INT NULL,
                resolved_at TIMESTAMPTZ NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_device_time ON alert_record (device_id, triggered_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_status ON alert_record (status, triggered_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_cooldown ON alert_record (rule_id, device_id, triggered_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_status_severity ON alert_record (status, severity, triggered_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_severity_time ON alert_record (severity, triggered_at DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_created_at ON alert_record (created_at DESC))");

        // 告警规则模板表
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS alert_rule_template (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL UNIQUE,
                category VARCHAR(50),
                description TEXT,
                severity VARCHAR(20) NOT NULL DEFAULT 'warning',
                conditions JSONB NOT NULL DEFAULT '[]',
                logic VARCHAR(5) NOT NULL DEFAULT 'and',
                silence_duration INT NOT NULL DEFAULT 300,
                recovery_condition VARCHAR(50) DEFAULT 'reverse',
                recovery_wait_seconds INT DEFAULT 60,
                applicable_protocols JSONB DEFAULT '["SL651", "Modbus"]',
                protocol_config_id INT,
                created_by INT NOT NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_template_category ON alert_rule_template (category) WHERE deleted_at IS NULL)");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_template_creator ON alert_rule_template (created_by) WHERE deleted_at IS NULL)");
    }

    // ─── 归档表 ──────────────────────────────────────────────

    Task<> createArchiveTable(const TransactionPtr& txn) {
        co_await txn->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS device_data_archive (
                id BIGINT NOT NULL,
                device_id INT NOT NULL,
                link_id INT NOT NULL,
                protocol TEXT NOT NULL,
                data JSONB NOT NULL DEFAULT '{}',
                report_time TIMESTAMPTZ NOT NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                archived_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                PRIMARY KEY (id, report_time)
            )
        )");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_archive_device_time ON device_data_archive (device_id, report_time DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_archive_report_time ON device_data_archive (report_time DESC))");
        co_await txn->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_archive_device_func ON device_data_archive (device_id, (data->>'funcCode'), report_time DESC))");

        // 归档函数
        co_await txn->execSqlCoro(R"(
            CREATE OR REPLACE FUNCTION archive_old_device_data(archive_days INT DEFAULT 365)
            RETURNS TABLE(archived_count BIGINT, deleted_count BIGINT) AS $$
            DECLARE
                v_archived BIGINT := 0;
                v_deleted BIGINT := 0;
                v_cutoff_time TIMESTAMPTZ;
            BEGIN
                v_cutoff_time := NOW() - (archive_days || ' days')::INTERVAL;

                WITH moved_rows AS (
                    INSERT INTO device_data_archive (id, device_id, link_id, protocol, data, report_time, created_at, archived_at)
                    SELECT id, device_id, link_id, protocol, data, report_time, created_at, NOW()
                    FROM device_data d
                    WHERE d.report_time < v_cutoff_time
                      AND NOT EXISTS (
                          SELECT 1 FROM device_data_archive a
                          WHERE a.id = d.id AND a.report_time = d.report_time
                      )
                    RETURNING 1
                )
                SELECT COUNT(*) INTO v_archived FROM moved_rows;

                WITH deleted_rows AS (
                    DELETE FROM device_data
                    WHERE report_time < v_cutoff_time
                    RETURNING 1
                )
                SELECT COUNT(*) INTO v_deleted FROM deleted_rows;

                RETURN QUERY SELECT v_archived, v_deleted;
            END;
            $$ LANGUAGE plpgsql
        )");

        // 合并视图
        co_await txn->execSqlCoro(R"(
            CREATE OR REPLACE VIEW device_data_all AS
            SELECT id, device_id, link_id, protocol, data, report_time, created_at, FALSE as is_archived
            FROM device_data
            UNION ALL
            SELECT id, device_id, link_id, protocol, data, report_time, created_at, TRUE as is_archived
            FROM device_data_archive
        )");
    }

    // ─── 触发器 ──────────────────────────────────────────────

    Task<> createTriggers(const TransactionPtr& txn) {
        // 自动更新 updated_at 的触发器函数
        co_await txn->execSqlCoro(R"(
            CREATE OR REPLACE FUNCTION update_updated_at_column()
            RETURNS TRIGGER AS $$
            BEGIN
                NEW.updated_at = CURRENT_TIMESTAMP;
                RETURN NEW;
            END;
            $$ language 'plpgsql'
        )");

        // 批量创建触发器
        static constexpr const char* tables[] = {
            "sys_user", "sys_role", "sys_menu", "sys_department",
            "agent_node", "agent_endpoint", "link", "protocol_config",
            "device_group", "device", "alert_rule", "open_access_key", "open_webhook"
        };

        for (const auto& table : tables) {
            std::string sql =
                "DO $$ BEGIN "
                "CREATE TRIGGER update_" + std::string(table) + "_updated_at "
                "BEFORE UPDATE ON " + std::string(table) + " "
                "FOR EACH ROW EXECUTE FUNCTION update_updated_at_column(); "
                "EXCEPTION WHEN duplicate_object THEN null; END $$";
            co_await txn->execSqlCoro(sql);
        }
    }

    // ─── 数据迁移 ────────────────────────────────────────────

    Task<> migrateData(const TransactionPtr& txn) {
        // 从 device.protocol_params 中的网络字段迁移到 agent_endpoint 表
        co_await txn->execSqlCoro(R"(
            DO $$ BEGIN
                IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'device') THEN
                    INSERT INTO agent_endpoint (agent_id, name, transport, mode, protocol, ip, port, status)
                    SELECT DISTINCT
                        (protocol_params->>'agent_id')::int,
                        CONCAT(
                            COALESCE(protocol_params->>'link_protocol', 'SL651'), ' ',
                            COALESCE(protocol_params->>'link_mode', ''), ' :',
                            COALESCE(protocol_params->>'link_port', '0')
                        ),
                        'ethernet',
                        COALESCE(protocol_params->>'link_mode', ''),
                        COALESCE(protocol_params->>'link_protocol', 'SL651'),
                        COALESCE(protocol_params->>'link_ip', '0.0.0.0'),
                        COALESCE((protocol_params->>'link_port')::int, 0),
                        'enabled'
                    FROM device
                    WHERE link_id = 0
                      AND protocol_params->>'agent_id' IS NOT NULL
                      AND protocol_params->>'link_port' IS NOT NULL
                      AND protocol_params->>'agent_endpoint_id' IS NULL
                      AND deleted_at IS NULL
                    ON CONFLICT DO NOTHING;

                    UPDATE device d SET
                        protocol_params = d.protocol_params
                            - 'agent_interface' - 'agent_bind_ip' - 'agent_prefix_length'
                            - 'agent_gateway' - 'link_mode' - 'link_protocol' - 'link_ip' - 'link_port'
                            || jsonb_build_object('agent_endpoint_id', ep.id)
                    FROM agent_endpoint ep
                    WHERE d.link_id = 0
                      AND d.protocol_params->>'agent_id' IS NOT NULL
                      AND d.protocol_params->>'agent_endpoint_id' IS NULL
                      AND d.deleted_at IS NULL
                      AND ep.agent_id = (d.protocol_params->>'agent_id')::int
                      AND ep.protocol = COALESCE(d.protocol_params->>'link_protocol', 'SL651')
                      AND ep.port = COALESCE((d.protocol_params->>'link_port')::int, 0)
                      AND COALESCE(ep.mode, '') = COALESCE(d.protocol_params->>'link_mode', '')
                      AND ep.deleted_at IS NULL;
                END IF;
            END $$
        )");

        // 将 Agent 链路的连接信息回填到 device.protocol_params
        co_await txn->execSqlCoro(R"(
            UPDATE device d SET
                link_id = 0,
                protocol_params = d.protocol_params || json_build_object(
                    'agent_id', l.agent_id,
                    'agent_interface', COALESCE(l.agent_interface, ''),
                    'agent_bind_ip', COALESCE(l.agent_bind_ip, ''),
                    'agent_prefix_length', COALESCE(l.agent_prefix_length, 24),
                    'agent_gateway', COALESCE(l.agent_gateway, ''),
                    'link_mode', l.mode,
                    'link_protocol', l.protocol,
                    'link_ip', l.ip,
                    'link_port', l.port
                )::jsonb
            FROM link l
            WHERE d.link_id = l.id AND l.agent_id IS NOT NULL
              AND d.deleted_at IS NULL AND l.deleted_at IS NULL
              AND d.protocol_params->>'agent_id' IS NULL
        )");

        // 刷新物化视图函数
        co_await txn->execSqlCoro(R"(
            CREATE OR REPLACE FUNCTION refresh_device_data_views()
            RETURNS void AS $$
            DECLARE
                view_name TEXT;
            BEGIN
                FOR view_name IN
                    SELECT matviewname FROM pg_matviews
                    WHERE matviewname LIKE 'mv_device_data_latest_%'
                LOOP
                    EXECUTE 'REFRESH MATERIALIZED VIEW CONCURRENTLY ' || quote_ident(view_name);
                END LOOP;
            END;
            $$ LANGUAGE plpgsql
        )");
    }
};
