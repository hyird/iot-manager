#pragma once

#include "../MigrationTypes.hpp"

/**
 * @brief V002 TimescaleDB 特性初始化
 *
 * 非事务型迁移：TimescaleDB 的 DDL 操作（create_hypertable、压缩策略等）
 * 不支持在事务中执行。
 *
 * 如果 TimescaleDB 不可用，此迁移会跳过并记录警告，不影响核心功能。
 */
class V002_TimescaleDB : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 2,
            .name = "TimescaleDB",
            .description = "TimescaleDB hypertable, compression, continuous aggregates",
            .transactional = false  // TimescaleDB DDL 不支持事务
        };
    }

    Task<> up(const TransactionPtr&) override {
        throw std::runtime_error("V002_TimescaleDB is non-transactional, use upNoTxn()");
        co_return;
    }

    Task<> down(const TransactionPtr&) override {
        throw std::runtime_error("V002_TimescaleDB rollback not supported");
        co_return;
    }

    Task<> upNoTxn(const DbClientPtr& db) override {
        // 检查 TimescaleDB 扩展是否可用
        bool available = co_await ensureExtension(db);
        if (!available) {
            LOG_WARN << "[Migration] TimescaleDB not available, skipping V002";
            co_return;
        }

        // 将 device_data 转换为超表
        co_await createHypertable(db);

        // 配置压缩策略
        co_await configureCompression(db);

        // 创建连续聚合
        co_await createContinuousAggregates(db);

        // 将归档表也转换为超表
        co_await createArchiveHypertable(db);
    }

    Task<> downNoTxn(const DbClientPtr& db) override {
        // TimescaleDB 回滚：只能删除策略和连续聚合，超表无法回退为普通表
        LOG_WARN << "[Migration] TimescaleDB rollback is partial - hypertables cannot be reverted";

        try {
            // 删除连续聚合
            co_await db->execSqlCoro("DROP MATERIALIZED VIEW IF EXISTS device_data_hourly CASCADE");
        } catch (...) {}

        try {
            // 移除压缩策略
            co_await db->execSqlCoro("SELECT remove_compression_policy('device_data', if_exists => TRUE)");
            co_await db->execSqlCoro("SELECT remove_compression_policy('device_data_archive', if_exists => TRUE)");
        } catch (...) {}
    }

private:
    Task<bool> ensureExtension(const DbClientPtr& db) {
        try {
            auto r = co_await db->execSqlCoro(R"(
                SELECT EXISTS (
                    SELECT 1 FROM pg_extension WHERE extname = 'timescaledb'
                ) as installed
            )");

            if (!r[0]["installed"].as<bool>()) {
                try {
                    co_await db->execSqlCoro("CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE");
                    LOG_INFO << "[Migration] TimescaleDB extension created";
                } catch (const std::exception&) {
                    co_return false;
                }
            }
            co_return true;
        } catch (...) {
            co_return false;
        }
    }

    Task<> createHypertable(const DbClientPtr& db) {
        auto r = co_await db->execSqlCoro(R"(
            SELECT EXISTS (
                SELECT 1 FROM timescaledb_information.hypertables
                WHERE hypertable_name = 'device_data'
            ) as is_hypertable
        )");

        if (!r[0]["is_hypertable"].as<bool>()) {
            co_await db->execSqlCoro(R"(
                SELECT create_hypertable(
                    'device_data', 'report_time',
                    chunk_time_interval => INTERVAL '7 days',
                    if_not_exists => TRUE,
                    migrate_data => TRUE
                )
            )");
            LOG_INFO << "[Migration] device_data converted to hypertable";
        }
    }

    Task<> configureCompression(const DbClientPtr& db) {
        try {
            co_await db->execSqlCoro(R"(
                ALTER TABLE device_data SET (
                    timescaledb.compress,
                    timescaledb.compress_segmentby = 'device_id',
                    timescaledb.compress_orderby = 'report_time DESC, id DESC'
                )
            )");
            co_await db->execSqlCoro(R"(
                SELECT add_compression_policy('device_data', INTERVAL '7 days', if_not_exists => TRUE)
            )");
            LOG_INFO << "[Migration] device_data compression policy configured";
        } catch (const std::exception& e) {
            LOG_WARN << "[Migration] Compression configuration skipped: " << e.what();
        }
    }

    Task<> createContinuousAggregates(const DbClientPtr& db) {
        try {
            auto r = co_await db->execSqlCoro(R"(
                SELECT EXISTS (
                    SELECT 1 FROM timescaledb_information.continuous_aggregates
                    WHERE view_name = 'device_data_hourly'
                ) as exists
            )");

            if (!r[0]["exists"].as<bool>()) {
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

                co_await db->execSqlCoro(R"(
                    SELECT add_continuous_aggregate_policy('device_data_hourly',
                        start_offset => INTERVAL '3 hours',
                        end_offset => INTERVAL '1 hour',
                        schedule_interval => INTERVAL '1 hour',
                        if_not_exists => TRUE
                    )
                )");
                LOG_INFO << "[Migration] Continuous aggregate device_data_hourly created";
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[Migration] Continuous aggregates skipped: " << e.what();
        }
    }

    Task<> createArchiveHypertable(const DbClientPtr& db) {
        try {
            auto r = co_await db->execSqlCoro(R"(
                SELECT EXISTS (
                    SELECT 1 FROM timescaledb_information.hypertables
                    WHERE hypertable_name = 'device_data_archive'
                ) as is_hypertable
            )");

            if (!r[0]["is_hypertable"].as<bool>()) {
                co_await db->execSqlCoro(R"(
                    SELECT create_hypertable(
                        'device_data_archive', 'report_time',
                        chunk_time_interval => INTERVAL '30 days',
                        if_not_exists => TRUE
                    )
                )");

                co_await db->execSqlCoro(R"(
                    ALTER TABLE device_data_archive SET (
                        timescaledb.compress,
                        timescaledb.compress_segmentby = 'device_id',
                        timescaledb.compress_orderby = 'report_time DESC, id DESC'
                    )
                )");

                co_await db->execSqlCoro(R"(
                    SELECT add_compression_policy('device_data_archive', INTERVAL '1 day', if_not_exists => TRUE)
                )");
                LOG_INFO << "[Migration] device_data_archive converted to hypertable";
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[Migration] Archive hypertable skipped: " << e.what();
        }
    }
};
