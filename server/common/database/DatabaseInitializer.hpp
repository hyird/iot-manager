#pragma once

#include "common/utils/Constants.hpp"
#include "common/utils/PasswordUtils.hpp"
#include "DatabaseService.hpp"
#include <cstdlib>

class DatabaseInitializer {
public:
    using DbClientPtr = drogon::orm::DbClientPtr;
    template<typename T = void> using Task = drogon::Task<T>;

private:
    static DbClientPtr getDbClient() {
        return AppDbConfig::useFast()
            ? drogon::app().getFastDbClient("default")
            : drogon::app().getDbClient("default");
    }

public:
    static Task<> initialize() {
        auto db = getDbClient();

        LOG_INFO << "Checking database initialization...";

        // 抑制 IF NOT EXISTS 产生的 NOTICE（"relation already exists, skipping"）
        co_await db->execSqlCoro("SET client_min_messages = WARNING");

        // 数据库级别固定 UTC 时区（所有连接生效，无需每个连接 SET timezone）
        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                EXECUTE format('ALTER DATABASE %I SET timezone = ''UTC''', current_database());
            END $$
        )");

        // 创建枚举类型
        co_await createEnumTypes(db);

        // 检查表是否存在
        co_await createTables(db);

        // 创建触发器
        co_await createTriggers(db);

        // 检查是否有管理员用户
        co_await initializeAdminUser(db);

        LOG_INFO << "Database initialization completed";
    }

private:
    static Task<> createEnumTypes(const DbClientPtr& db) {
        // 创建状态枚举类型
        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TYPE status_enum AS ENUM ('enabled', 'disabled');
            EXCEPTION
                WHEN duplicate_object THEN null;
            END $$
        )");

        // 创建菜单类型枚举
        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TYPE menu_type_enum AS ENUM ('menu', 'page', 'button');
            EXCEPTION
                WHEN duplicate_object THEN null;
            END $$
        )");

        LOG_INFO << "Enum types created/verified";
    }

    static Task<> createTables(const DbClientPtr& db) {
        // 创建菜单表
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_parent ON sys_menu (parent_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_deleted ON sys_menu (deleted_at))");
        // 菜单状态+删除+排序复合索引（用于缓存预热和权限检查）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_status_deleted ON sys_menu (status, deleted_at, sort_order))");
        // 权限码索引（用于快速权限查找）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_permission ON sys_menu (permission_code) WHERE deleted_at IS NULL AND permission_code IS NOT NULL)");
        // 父菜单+软删除部分索引（用于子菜单计数查询）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_menu_parent_active ON sys_menu (parent_id) WHERE deleted_at IS NULL)");

        // 创建部门表
        co_await db->execSqlCoro(R"(
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
        // 确保 code 字段存在（兼容旧表结构）
        co_await db->execSqlCoro(R"(ALTER TABLE sys_department ADD COLUMN IF NOT EXISTS code VARCHAR(50))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_department_parent ON sys_department (parent_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_department_deleted ON sys_department (deleted_at))");
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_department_code ON sys_department (code) WHERE code IS NOT NULL AND deleted_at IS NULL)");

        // 创建角色表
        co_await db->execSqlCoro(R"(
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
        // 部分唯一索引（兼容软删除，已删除的角色不占用 code 唯一性）
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_role_code_active ON sys_role (code) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_deleted ON sys_role (deleted_at))");
        // 角色状态+删除复合索引（用于权限检查）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_status_deleted ON sys_role (status, deleted_at))");

        // 创建用户表
        co_await db->execSqlCoro(R"(
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
        // 部分唯一索引（兼容软删除，已删除的用户不占用 username 唯一性）
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_user_username_active ON sys_user (username) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_deleted ON sys_user (deleted_at))");
        // 用户按部门筛选索引
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_department ON sys_user (department_id) WHERE deleted_at IS NULL)");

        // 创建用户角色关联表
        co_await db->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_user_role (
                id SERIAL PRIMARY KEY,
                user_id INT NOT NULL,
                role_id INT NOT NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                UNIQUE (user_id, role_id)
            )
        )");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_role_user ON sys_user_role (user_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_user_role_role ON sys_user_role (role_id))");

        // 创建角色菜单关联表
        co_await db->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS sys_role_menu (
                id SERIAL PRIMARY KEY,
                role_id INT NOT NULL,
                menu_id INT NOT NULL,
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                UNIQUE (role_id, menu_id)
            )
        )");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_menu_role ON sys_role_menu (role_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_sys_role_menu_menu ON sys_role_menu (menu_id))");

        // 创建链路表
        co_await db->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS link (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                mode VARCHAR(20) NOT NULL,
                ip VARCHAR(50) NOT NULL,
                port INT NOT NULL,
                status status_enum DEFAULT 'enabled',
                created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                deleted_at TIMESTAMPTZ NULL
            )
        )");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_mode ON link (mode))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_deleted ON link (deleted_at))");
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_link_name ON link (name) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_link_endpoint ON link (mode, ip, port) WHERE deleted_at IS NULL)");
        // 启用链路查询索引（启动时查询所有启用链路）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_status_active ON link (status) WHERE deleted_at IS NULL)");
        // 迁移：添加协议类型字段
        co_await db->execSqlCoro(R"(ALTER TABLE link ADD COLUMN IF NOT EXISTS protocol VARCHAR(20) NOT NULL DEFAULT 'SL651')");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_link_protocol ON link (protocol))");

        // 创建协议配置表（统一存储所有协议的设备类型配置）
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_protocol_config_protocol ON protocol_config (protocol))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_protocol_config_deleted ON protocol_config (deleted_at))");
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_protocol_config_name ON protocol_config (protocol, name) WHERE deleted_at IS NULL)");

        // 创建设备分组表
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_group_parent ON device_group (parent_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_group_deleted ON device_group (deleted_at))");
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_device_group_name ON device_group (name) WHERE deleted_at IS NULL)");

        // 创建设备表
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(ALTER TABLE device ADD COLUMN IF NOT EXISTS group_id INT NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_link ON device (link_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_protocol ON device (protocol_config_id))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_group ON device (group_id) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_deleted ON device (deleted_at))");
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_device_name ON device (name) WHERE deleted_at IS NULL)");

        // JSONB 函数索引：device_code 唯一性
        co_await db->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_device_protocol_params_code
            ON device ((protocol_params->>'device_code'))
            WHERE deleted_at IS NULL
              AND protocol_params->>'device_code' IS NOT NULL
              AND protocol_params->>'device_code' != ''
        )");

        // 创建统一的设备数据表（存储所有协议的原始报文和解析数据）
        // 协议特有字段（如 SL651 的 funcCode）存储在 JSONB 的 data 字段中
        // 使用 TimescaleDB 推荐的数据类型：TEXT 替代 VARCHAR，TIMESTAMPTZ 替代 TIMESTAMP
        // 注意：TimescaleDB 超表要求唯一约束/主键必须包含分区列 report_time
        co_await db->execSqlCoro(R"(
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

        // 精简索引（仅保留 3 个核心索引，减少写入开销）
        // 1. 设备+时间复合索引：设备历史数据查询
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_data_device_time ON device_data (device_id, report_time DESC))");
        // 2. 链路+时间复合索引：链路历史数据查询
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_data_link_time ON device_data (link_id, report_time DESC))");
        // 3. 设备+功能码+时间复合索引：DISTINCT ON 查询优化
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_device_data_history ON device_data (device_id, (data->>'funcCode'), report_time DESC))");

        // ==================== 告警规则表 ====================
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_alert_rule_name ON alert_rule (name) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_rule_device ON alert_rule (device_id) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_rule_status ON alert_rule (status) WHERE deleted_at IS NULL)");

        // ==================== 告警记录表 ====================
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_device_time ON alert_record (device_id, triggered_at DESC))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_status ON alert_record (status, triggered_at DESC))");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_cooldown ON alert_record (rule_id, device_id, triggered_at DESC))");
        // 状态+严重级别+时间复合索引（用于告警列表多条件过滤，避免全表扫描）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_status_severity ON alert_record (status, severity, triggered_at DESC))");
        // 严重级别+时间索引（用于仪表盘按 severity 聚合统计）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_severity_time ON alert_record (severity, triggered_at DESC))");
        // 创建时间索引（用于 "今日新增告警" 统计查询）
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_record_created_at ON alert_record (created_at DESC))");

        // ==================== 告警规则模板表 ====================
        co_await db->execSqlCoro(R"(
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
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_template_category ON alert_rule_template (category) WHERE deleted_at IS NULL)");
        co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_alert_template_creator ON alert_rule_template (created_by) WHERE deleted_at IS NULL)");

        // 初始化 TimescaleDB 超表（如果扩展已安装）
        co_await initializeTimescaleDB(db);

        // 创建物化视图（每协议一个）
        co_await createMaterializedViews(db);

        LOG_INFO << "Database tables created/verified";
    }

    // 校验标识符（仅允许字母数字和下划线，防止 SQL 注入）
    static bool isValidIdentifier(const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
    }

    static Task<> createMaterializedViews(const DbClientPtr& db) {
        // 获取所有协议类型
        auto protocols = co_await db->execSqlCoro(
            "SELECT DISTINCT protocol FROM protocol_config WHERE deleted_at IS NULL"
        );

        for (const auto& row : protocols) {
            std::string protocol = row["protocol"].as<std::string>();

            // 校验协议名称，防止 SQL 注入
            if (!isValidIdentifier(protocol)) {
                LOG_WARN << "Skipping invalid protocol name for materialized view: " << protocol;
                continue;
            }

            std::string viewName = "mv_device_data_latest_" + protocol;
            std::string indexName = "idx_mv_" + protocol + "_device_func";

            // isValidIdentifier 已确保 protocol 仅含字母数字下划线，拼接安全
            std::string createViewSql = R"(
                CREATE MATERIALIZED VIEW IF NOT EXISTS )" + viewName + R"( AS
                SELECT DISTINCT ON (device_id, data->>'funcCode')
                    device_id,
                    data->>'funcCode' as func_code,
                    data,
                    report_time,
                    created_at
                FROM device_data
                WHERE protocol = ')" + protocol + R"('
                ORDER BY device_id, data->>'funcCode', report_time DESC NULLS LAST
            )";

            try {
                co_await db->execSqlCoro(createViewSql);

                // 创建唯一索引（支持并发刷新）
                std::string createIndexSql = "CREATE UNIQUE INDEX IF NOT EXISTS " + indexName +
                    " ON " + viewName + " (device_id, func_code)";
                co_await db->execSqlCoro(createIndexSql);

                LOG_INFO << "Materialized view created: " << viewName;
            } catch (const std::exception& e) {
                LOG_WARN << "Failed to create materialized view " << viewName << ": " << e.what();
            }
        }

        // 创建刷新物化视图的函数（使用 quote_ident 防止注入）
        co_await db->execSqlCoro(R"(
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

        LOG_INFO << "Materialized views created/verified";
    }

    static Task<> initializeTimescaleDB(const DbClientPtr& db) {
        // 检查 TimescaleDB 扩展是否可用
        try {
            auto extResult = co_await db->execSqlCoro(R"(
                SELECT EXISTS (
                    SELECT 1 FROM pg_extension WHERE extname = 'timescaledb'
                ) as installed
            )");

            bool isInstalled = extResult[0]["installed"].as<bool>();

            if (!isInstalled) {
                // 尝试创建扩展
                try {
                    co_await db->execSqlCoro("CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE");
                    LOG_INFO << "TimescaleDB extension created";
                } catch (const std::exception&) {
                    LOG_WARN << "TimescaleDB extension not available, using standard PostgreSQL table";
                    co_return;
                }
            }

            // 检查 device_data 是否已经是超表
            auto hyperResult = co_await db->execSqlCoro(R"(
                SELECT EXISTS (
                    SELECT 1 FROM timescaledb_information.hypertables
                    WHERE hypertable_name = 'device_data'
                ) as is_hypertable
            )");

            bool isHypertable = hyperResult[0]["is_hypertable"].as<bool>();

            if (!isHypertable) {
                // 转换为超表（每周一个分区）
                co_await db->execSqlCoro(R"(
                    SELECT create_hypertable(
                        'device_data',
                        'report_time',
                        chunk_time_interval => INTERVAL '7 days',
                        if_not_exists => TRUE,
                        migrate_data => TRUE
                    )
                )");
                LOG_INFO << "device_data converted to TimescaleDB hypertable";

                // 启用压缩（按 device_id 分段，按 report_time + id 排序）
                co_await db->execSqlCoro(R"(
                    ALTER TABLE device_data SET (
                        timescaledb.compress,
                        timescaledb.compress_segmentby = 'device_id',
                        timescaledb.compress_orderby = 'report_time DESC, id DESC'
                    )
                )");

                // 添加压缩策略（7天后自动压缩）
                co_await db->execSqlCoro(R"(
                    SELECT add_compression_policy('device_data', INTERVAL '7 days', if_not_exists => TRUE)
                )");
                LOG_INFO << "TimescaleDB compression policy configured";
            } else {
                LOG_INFO << "device_data is already a TimescaleDB hypertable";
            }

            // 创建连续聚合（每小时数据量统计，用于首页仪表盘）
            co_await createContinuousAggregates(db);

            // 创建归档表和归档机制
            co_await createArchiveTable(db);
        } catch (const std::exception& e) {
            LOG_WARN << "TimescaleDB initialization skipped: " << e.what();
        }
    }

    /**
     * @brief 创建 TimescaleDB 连续聚合
     * 预计算每小时的数据量统计，加速首页仪表盘查询
     */
    static Task<> createContinuousAggregates(const DbClientPtr& db) {
        try {
            // 检查连续聚合是否已存在
            auto viewExists = co_await db->execSqlCoro(R"(
                SELECT EXISTS (
                    SELECT 1 FROM timescaledb_information.continuous_aggregates
                    WHERE view_name = 'device_data_hourly'
                ) as exists
            )");

            if (!viewExists[0]["exists"].as<bool>()) {
                // 每小时聚合：每设备的记录数
                co_await db->execSqlCoro(R"(
                    CREATE MATERIALIZED VIEW device_data_hourly
                    WITH (timescaledb.continuous) AS
                    SELECT
                        time_bucket('1 hour', report_time) AS bucket,
                        device_id,
                        COUNT(*) AS record_count
                    FROM device_data
                    GROUP BY bucket, device_id
                    WITH NO DATA
                )");

                // 刷新策略：每小时刷新，覆盖最近 3 小时（确保延迟数据被统计）
                co_await db->execSqlCoro(R"(
                    SELECT add_continuous_aggregate_policy('device_data_hourly',
                        start_offset => INTERVAL '3 hours',
                        end_offset => INTERVAL '1 hour',
                        schedule_interval => INTERVAL '1 hour',
                        if_not_exists => TRUE
                    )
                )");

                LOG_INFO << "Continuous aggregate device_data_hourly created";
            } else {
                LOG_INFO << "Continuous aggregate device_data_hourly already exists";
            }
        } catch (const std::exception& e) {
            LOG_WARN << "Failed to create continuous aggregates: " << e.what();
        }
    }

    /**
     * @brief 创建归档表和归档机制
     * 将超过指定天数的数据自动归档到 device_data_archive 表
     */
    static Task<> createArchiveTable(const DbClientPtr& db) {
        try {
            // 1. 创建归档表（使用 TimescaleDB 推荐的数据类型）
            // 注意：TimescaleDB 超表要求主键必须包含分区列 report_time
            co_await db->execSqlCoro(R"(
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

            // 2. 为归档表创建索引（用于历史查询）
            co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_archive_device_time ON device_data_archive (device_id, report_time DESC))");
            co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_archive_report_time ON device_data_archive (report_time DESC))");
            co_await db->execSqlCoro(R"(CREATE INDEX IF NOT EXISTS idx_archive_device_func ON device_data_archive (device_id, (data->>'funcCode'), report_time DESC))");

            // 3. 尝试将归档表也转换为 TimescaleDB 超表（按 report_time 分区）
            try {
                co_await db->execSqlCoro(R"(
                    SELECT create_hypertable(
                        'device_data_archive',
                        'report_time',
                        chunk_time_interval => INTERVAL '30 days',
                        if_not_exists => TRUE
                    )
                )");

                // 为归档表启用压缩（归档数据可以更积极地压缩）
                co_await db->execSqlCoro(R"(
                    ALTER TABLE device_data_archive SET (
                        timescaledb.compress,
                        timescaledb.compress_segmentby = 'device_id',
                        timescaledb.compress_orderby = 'report_time DESC, id DESC'
                    )
                )");

                // 归档表 1 天后自动压缩
                co_await db->execSqlCoro(R"(
                    SELECT add_compression_policy('device_data_archive', INTERVAL '1 day', if_not_exists => TRUE)
                )");
            } catch (const std::exception&) {
                // TimescaleDB 不可用，使用普通表
            }

            // 4. 创建归档函数（不使用 ON CONFLICT，因为超表不支持没有分区键的唯一约束）
            co_await db->execSqlCoro(R"(
                CREATE OR REPLACE FUNCTION archive_old_device_data(archive_days INT DEFAULT 365)
                RETURNS TABLE(archived_count BIGINT, deleted_count BIGINT) AS $$
                DECLARE
                    v_archived BIGINT := 0;
                    v_deleted BIGINT := 0;
                    v_cutoff_time TIMESTAMPTZ;
                BEGIN
                    v_cutoff_time := NOW() - (archive_days || ' days')::INTERVAL;

                    -- 将旧数据插入归档表（使用 NOT EXISTS 避免重复）
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

                    -- 从主表删除已归档的数据
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

            // 5. 创建合并视图（透明访问主表+归档表）
            // 注意：归档需要手动执行 SELECT * FROM archive_old_device_data(365);
            co_await db->execSqlCoro(R"(
                CREATE OR REPLACE VIEW device_data_all AS
                SELECT id, device_id, link_id, protocol, data, report_time, created_at, FALSE as is_archived
                FROM device_data
                UNION ALL
                SELECT id, device_id, link_id, protocol, data, report_time, created_at, TRUE as is_archived
                FROM device_data_archive
            )");

            LOG_INFO << "Archive table, function and view created";
        } catch (const std::exception& e) {
            LOG_WARN << "Archive table creation failed: " << e.what();
        }
    }

    static Task<> createTriggers(const DbClientPtr& db) {
        // 创建自动更新 updated_at 的触发器函数
        co_await db->execSqlCoro(R"(
            CREATE OR REPLACE FUNCTION update_updated_at_column()
            RETURNS TRIGGER AS $$
            BEGIN
                NEW.updated_at = CURRENT_TIMESTAMP;
                RETURN NEW;
            END;
            $$ language 'plpgsql'
        )");

        // 为各表创建触发器
        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_sys_user_updated_at BEFORE UPDATE ON sys_user
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_sys_role_updated_at BEFORE UPDATE ON sys_role
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_sys_menu_updated_at BEFORE UPDATE ON sys_menu
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_sys_department_updated_at BEFORE UPDATE ON sys_department
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_link_updated_at BEFORE UPDATE ON link
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_protocol_config_updated_at BEFORE UPDATE ON protocol_config
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_device_group_updated_at BEFORE UPDATE ON device_group
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_device_updated_at BEFORE UPDATE ON device
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                CREATE TRIGGER update_alert_rule_updated_at BEFORE UPDATE ON alert_rule
                    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
            EXCEPTION WHEN duplicate_object THEN null; END $$
        )");

        LOG_INFO << "Database triggers created/verified";
    }

    static Task<> initializeAdminUser(const DbClientPtr& db) {
        // 检查是否已有用户
        auto userCount = co_await db->execSqlCoro(
            R"(SELECT COUNT(*) as count FROM sys_user WHERE deleted_at IS NULL)"
        );

        if (!userCount.empty() && userCount[0]["count"].as<int64_t>() > 0) {
            LOG_INFO << "Users already exist, skipping initialization";
            co_return;
        }

        LOG_INFO << "No users found, creating default admin...";

        // 创建超级管理员角色
        co_await db->execSqlCoro(
            R"(INSERT INTO sys_role (name, code, description, sort_order)
               VALUES ('超级管理员', 'superadmin', '拥有系统所有权限', 1)
               ON CONFLICT (code) WHERE deleted_at IS NULL DO NOTHING)"
        );

        // 获取角色ID
        auto roleResult = co_await db->execSqlCoro(
            "SELECT id FROM sys_role WHERE code = 'superadmin'"
        );
        int roleId = roleResult[0]["id"].as<int>();

        // 创建管理员用户（优先读取环境变量 IOT_ADMIN_INIT_PASSWORD，否则使用随机密码）
        std::string initPassword;
        if (const char* envPwd = std::getenv("IOT_ADMIN_INIT_PASSWORD")) {
            initPassword = envPwd;
        }
        if (initPassword.empty()) {
            initPassword = PasswordUtils::generateRandomPassword(16);
            LOG_WARN << "IOT_ADMIN_INIT_PASSWORD not set. Generated random initial admin password: "
                     << initPassword
                     << " (please change immediately after first login)";
        }

        std::string passwordHash = PasswordUtils::hashPassword(initPassword);
        co_await db->execSqlCoro(
            R"(INSERT INTO sys_user (username, password_hash, nickname, status)
               VALUES ('admin', $1, '超级管理员', 'enabled')
               ON CONFLICT (username) WHERE deleted_at IS NULL DO NOTHING)",
            passwordHash
        );

        // 获取用户ID
        auto userResult = co_await db->execSqlCoro(
            "SELECT id FROM sys_user WHERE username = 'admin'"
        );
        int userId = userResult[0]["id"].as<int>();

        // 分配角色
        co_await db->execSqlCoro(
            R"(INSERT INTO sys_user_role (user_id, role_id)
               VALUES ($1, $2)
               ON CONFLICT (user_id, role_id) DO NOTHING)",
            userId, roleId
        );

        // 创建基础菜单
        co_await initializeMenus(db, roleId);

        LOG_INFO << "Default admin user created: admin";
    }

    // 插入菜单并返回 ID
    static Task<int> insertMenu(const DbClientPtr& db, const std::string& name,
                                int parentId, const std::string& type,
                                const std::string& path, const std::string& component,
                                const std::string& permissionCode, const std::string& icon,
                                bool isDefault, int sortOrder) {
        auto result = co_await db->execSqlCoro(
            R"(INSERT INTO sys_menu (name, parent_id, type, path, component, permission_code, icon, is_default, sort_order)
               VALUES ($1, $2, $3, NULLIF($4, ''), NULLIF($5, ''), NULLIF($6, ''), NULLIF($7, ''), $8, $9)
               RETURNING id)",
            name, parentId, type, path, component, permissionCode, icon, isDefault, sortOrder
        );
        co_return result[0]["id"].as<int>();
    }

    // 插入按钮菜单（简化版）
    static Task<int> insertButton(const DbClientPtr& db, const std::string& name,
                                  int parentId, const std::string& permissionCode, int sortOrder) {
        co_return co_await insertMenu(db, name, parentId, Constants::MENU_TYPE_BUTTON, "", "", permissionCode, "", false, sortOrder);
    }

    static Task<> initializeMenus(const DbClientPtr& db, int roleId) {
        // ==================== 首页 ====================
        int homeId = co_await insertMenu(db, "首页", 0, Constants::MENU_TYPE_PAGE, "/home", "Home", "", "HomeOutlined", true, 0);
        co_await insertButton(db, "查看统计", homeId, "home:dashboard:query", 1);
        co_await insertButton(db, "清理缓存", homeId, "system:cache:clear", 2);

        // ==================== 链路管理 ====================
        int linkId = co_await insertMenu(db, "链路管理", 0, Constants::MENU_TYPE_PAGE, "/link", "Link", "", "LinkOutlined", false, 1);
        co_await insertButton(db, "查询链路", linkId, "iot:link:query", 1);
        co_await insertButton(db, "新增链路", linkId, "iot:link:add", 2);
        co_await insertButton(db, "编辑链路", linkId, "iot:link:edit", 3);
        co_await insertButton(db, "删除链路", linkId, "iot:link:delete", 4);

        // ==================== 协议配置 ====================
        int protocolId = co_await insertMenu(db, "协议配置", 0, Constants::MENU_TYPE_MENU, "/iot", "", "", "ApiOutlined", false, 2);

        // SL651配置
        int sl651Id = co_await insertMenu(db, "SL651配置", protocolId, Constants::MENU_TYPE_PAGE, "/iot/sl651", "SL651Config", "", "SettingOutlined", false, 0);
        co_await insertButton(db, "查询配置", sl651Id, "iot:protocol:query", 1);
        co_await insertButton(db, "新增配置", sl651Id, "iot:protocol:add", 2);
        co_await insertButton(db, "编辑配置", sl651Id, "iot:protocol:edit", 3);
        co_await insertButton(db, "删除配置", sl651Id, "iot:protocol:delete", 4);
        co_await insertButton(db, "导入配置", sl651Id, "iot:protocol:import", 5);
        co_await insertButton(db, "导出配置", sl651Id, "iot:protocol:export", 6);

        // Modbus配置
        int modbusId = co_await insertMenu(db, "Modbus配置", protocolId, Constants::MENU_TYPE_PAGE, "/iot/modbus", "ModbusConfig", "", "SettingOutlined", false, 1);
        co_await insertButton(db, "查询配置", modbusId, "iot:protocol:query", 1);
        co_await insertButton(db, "新增配置", modbusId, "iot:protocol:add", 2);
        co_await insertButton(db, "编辑配置", modbusId, "iot:protocol:edit", 3);
        co_await insertButton(db, "删除配置", modbusId, "iot:protocol:delete", 4);
        co_await insertButton(db, "导入配置", modbusId, "iot:protocol:import", 5);
        co_await insertButton(db, "导出配置", modbusId, "iot:protocol:export", 6);

        // ==================== 设备管理 ====================
        int deviceId = co_await insertMenu(db, "设备管理", 0, Constants::MENU_TYPE_PAGE, "/device", "Device", "", "HddOutlined", false, 3);
        co_await insertButton(db, "查询设备", deviceId, "iot:device:query", 1);
        co_await insertButton(db, "新增设备", deviceId, "iot:device:add", 2);
        co_await insertButton(db, "编辑设备", deviceId, "iot:device:edit", 3);
        co_await insertButton(db, "删除设备", deviceId, "iot:device:delete", 4);
        co_await insertButton(db, "查询分组", deviceId, "iot:device-group:query", 5);
        co_await insertButton(db, "新增分组", deviceId, "iot:device-group:add", 6);
        co_await insertButton(db, "编辑分组", deviceId, "iot:device-group:edit", 7);
        co_await insertButton(db, "删除分组", deviceId, "iot:device-group:delete", 8);

        // ==================== 告警管理 ====================
        int alertId = co_await insertMenu(db, "告警管理", 0, Constants::MENU_TYPE_MENU, "/alert", "", "", "AlertOutlined", false, 4);

        int alertPageId = co_await insertMenu(db, "告警管理", alertId, Constants::MENU_TYPE_PAGE, "/alert", "Alert", "", "AlertOutlined", false, 1);
        co_await insertButton(db, "查询", alertPageId, "iot:alert:query", 1);
        co_await insertButton(db, "新增规则", alertPageId, "iot:alert:add", 2);
        co_await insertButton(db, "编辑规则", alertPageId, "iot:alert:edit", 3);
        co_await insertButton(db, "删除规则", alertPageId, "iot:alert:delete", 4);
        co_await insertButton(db, "确认告警", alertPageId, "iot:alert:ack", 5);

        // ==================== 系统管理 ====================
        int systemId = co_await insertMenu(db, "系统管理", 0, Constants::MENU_TYPE_MENU, "/system", "", "", "SettingOutlined", false, 999);

        // 菜单管理
        int menuId = co_await insertMenu(db, "菜单管理", systemId, Constants::MENU_TYPE_PAGE, "/system/menu", "Menu", "", "MenuOutlined", false, 1);
        co_await insertButton(db, "查询菜单", menuId, "system:menu:query", 1);
        co_await insertButton(db, "新增菜单", menuId, "system:menu:add", 2);
        co_await insertButton(db, "编辑菜单", menuId, "system:menu:edit", 3);
        co_await insertButton(db, "删除菜单", menuId, "system:menu:delete", 4);

        // 部门管理
        int deptId = co_await insertMenu(db, "部门管理", systemId, Constants::MENU_TYPE_PAGE, "/system/department", "Dept", "", "ApartmentOutlined", false, 2);
        co_await insertButton(db, "查询部门", deptId, "system:dept:query", 1);
        co_await insertButton(db, "新增部门", deptId, "system:dept:add", 2);
        co_await insertButton(db, "编辑部门", deptId, "system:dept:edit", 3);
        co_await insertButton(db, "删除部门", deptId, "system:dept:delete", 4);

        // 角色管理
        int roleMenuId = co_await insertMenu(db, "角色管理", systemId, Constants::MENU_TYPE_PAGE, "/system/role", "Role", "", "TeamOutlined", false, 3);
        co_await insertButton(db, "查询角色", roleMenuId, "system:role:query", 1);
        co_await insertButton(db, "新增角色", roleMenuId, "system:role:add", 2);
        co_await insertButton(db, "编辑角色", roleMenuId, "system:role:edit", 3);
        co_await insertButton(db, "删除角色", roleMenuId, "system:role:delete", 4);
        co_await insertButton(db, "分配权限", roleMenuId, "system:role:perm", 5);

        // 用户管理
        int userId = co_await insertMenu(db, "用户管理", systemId, Constants::MENU_TYPE_PAGE, "/system/user", "User", "", "UserOutlined", false, 4);
        co_await insertButton(db, "查询用户", userId, "system:user:query", 1);
        co_await insertButton(db, "新增用户", userId, "system:user:add", 2);
        co_await insertButton(db, "编辑用户", userId, "system:user:edit", 3);
        co_await insertButton(db, "删除用户", userId, "system:user:delete", 4);

        // 为超级管理员角色分配所有菜单权限
        co_await db->execSqlCoro(
            R"(INSERT INTO sys_role_menu (role_id, menu_id)
               SELECT $1, id FROM sys_menu WHERE deleted_at IS NULL
               ON CONFLICT (role_id, menu_id) DO NOTHING)",
            roleId
        );

        LOG_INFO << "Default menus created";
    }
};
