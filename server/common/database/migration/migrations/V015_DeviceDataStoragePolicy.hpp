#pragma once

#include "../MigrationTypes.hpp"

/**
 * @brief Keep the device_data hot rowstore window small.
 *
 * Daily chunks allow completed data to enter TimescaleDB columnstore promptly.
 * This preserves all history while avoiding a second full week of rowstore
 * indexes waiting for the previous seven-day chunk to become eligible.
 */
class V015_DeviceDataStoragePolicy : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 15,
            .name = "DeviceDataStoragePolicy",
            .description = "Use daily device_data chunks and compress completed history after one day",
            .transactional = false
        };
    }

    Task<> up(const TransactionPtr&) override {
        throw std::runtime_error("V015_DeviceDataStoragePolicy is non-transactional, use upNoTxn()");
        co_return;
    }

    Task<> down(const TransactionPtr&) override {
        throw std::runtime_error("V015_DeviceDataStoragePolicy is non-transactional, use downNoTxn()");
        co_return;
    }

    Task<> upNoTxn(const DbClientPtr& db) override {
        if (!co_await hasDeviceDataHypertable(db)) {
            LOG_WARN << "[Migration] device_data hypertable unavailable, skipping V015";
            co_return;
        }

        co_await db->execSqlCoro(
            "SELECT remove_compression_policy('device_data', if_exists => TRUE)");
        co_await db->execSqlCoro(
            "SELECT set_chunk_time_interval('device_data', INTERVAL '1 day')");
        co_await db->execSqlCoro(R"(
            SELECT add_compression_policy(
                'device_data',
                compress_after => INTERVAL '1 day',
                if_not_exists => TRUE
            )
        )");
        LOG_INFO << "[Migration] device_data storage policy set to daily chunks and one-day compression";
    }

    Task<> downNoTxn(const DbClientPtr& db) override {
        if (!co_await hasDeviceDataHypertable(db)) {
            co_return;
        }

        co_await db->execSqlCoro(
            "SELECT remove_compression_policy('device_data', if_exists => TRUE)");
        co_await db->execSqlCoro(
            "SELECT set_chunk_time_interval('device_data', INTERVAL '7 days')");
        co_await db->execSqlCoro(R"(
            SELECT add_compression_policy(
                'device_data',
                compress_after => INTERVAL '7 days',
                if_not_exists => TRUE
            )
        )");
    }

private:
    Task<bool> hasDeviceDataHypertable(const DbClientPtr& db) {
        auto result = co_await db->execSqlCoro(R"(
            SELECT EXISTS (
                SELECT 1
                FROM timescaledb_information.hypertables
                WHERE hypertable_schema = 'public'
                  AND hypertable_name = 'device_data'
            ) AS available
        )");
        co_return !result.empty() && result[0]["available"].as<bool>();
    }
};
