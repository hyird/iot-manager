#pragma once

#include "../MigrationTypes.hpp"

/**
 * @brief V003 迁移模板示例
 *
 * 创建新迁移的步骤：
 * 1. 复制此文件，重命名为 V{NNN}_{Name}.hpp（如 V004_AddDeviceTags.hpp）
 * 2. 修改类名和 info() 中的版本号/名称
 * 3. 在 up() 中编写正向迁移 SQL
 * 4. 在 down() 中编写回滚 SQL
 * 5. 在 DatabaseInitializer.hpp 的 registerMigrations() 中注册：
 *    registry.add<V004_AddDeviceTags>();
 *
 * 注意事项：
 * - 版本号必须连续递增，不允许跳号
 * - up() 中的 SQL 应尽量保持幂等（使用 IF NOT EXISTS / IF EXISTS）
 * - down() 是回滚，必须能撤销 up() 的所有变更
 * - 每个迁移在独立事务中执行，失败自动回滚
 * - 如果需要非事务迁移（如 TimescaleDB DDL），设置 transactional = false
 *   并覆盖 upNoTxn() / downNoTxn()
 */

// ─── 示例：为设备表添加标签字段 ─────────────────────────────
//
// class V004_AddDeviceTags : public MigrationBase {
// public:
//     MigrationInfo info() const override {
//         return {
//             .version = 4,
//             .name = "AddDeviceTags",
//             .description = "Add tags column to device table",
//             .transactional = true
//         };
//     }
//
//     Task<> up(const TransactionPtr& txn) override {
//         co_await txn->execSqlCoro(R"(
//             ALTER TABLE device
//             ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb
//         )");
//         co_await txn->execSqlCoro(R"(
//             CREATE INDEX IF NOT EXISTS idx_device_tags
//             ON device USING gin (tags)
//             WHERE deleted_at IS NULL
//         )");
//     }
//
//     Task<> down(const TransactionPtr& txn) override {
//         co_await txn->execSqlCoro("DROP INDEX IF EXISTS idx_device_tags");
//         co_await txn->execSqlCoro("ALTER TABLE device DROP COLUMN IF EXISTS tags");
//     }
// };
