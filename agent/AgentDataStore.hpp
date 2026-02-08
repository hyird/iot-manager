#pragma once

#include "common/protocol/FrameResult.hpp"
#include "common/utils/JsonHelper.hpp"

#include <sqlite3.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/**
 * @brief Agent 本地 SQLite 数据缓存
 *
 * 用于：
 * - 解析结果持久化（断网时不丢数据）
 * - 断网续传（重连后批量上报）
 * - ACK 确认后标记已发送
 */
class AgentDataStore {
public:
    struct CachedRecord {
        int64_t id = 0;
        int deviceId = 0;
        std::string protocol;
        std::string funcCode;
        Json::Value data;
        std::string reportTime;
    };

    ~AgentDataStore() { close(); }

    bool initialize(const std::string& dbPath = "./data/agent_cache.db") {
        std::lock_guard lock(mutex_);
        if (db_) return true;

        // 确保目录存在
        auto slashPos = dbPath.find_last_of("/\\");
        if (slashPos != std::string::npos) {
            auto dir = dbPath.substr(0, slashPos);
#ifdef _WIN32
            _mkdir(dir.c_str());
#else
            mkdir(dir.c_str(), 0755);
#endif
        }

        int rc = sqlite3_open(dbPath.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::cout << "[ERROR] " << "[AgentDataStore] Failed to open SQLite: " << sqlite3_errmsg(db_) << std::endl;
            db_ = nullptr;
            return false;
        }

        // WAL 模式 + 性能优化
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=NORMAL");
        exec("PRAGMA cache_size=-8000");  // 8MB cache

        // 建表
        exec(R"(
            CREATE TABLE IF NOT EXISTS cached_data (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id INTEGER NOT NULL,
                protocol TEXT NOT NULL,
                func_code TEXT,
                data TEXT NOT NULL,
                report_time TEXT NOT NULL,
                status INTEGER DEFAULT 0,
                created_at TEXT DEFAULT (datetime('now', 'localtime')),
                sent_at TEXT
            )
        )");
        exec("CREATE INDEX IF NOT EXISTS idx_cached_status ON cached_data(status, id)");
        exec("CREATE INDEX IF NOT EXISTS idx_cached_device ON cached_data(device_id, report_time)");

        std::cout << "[AgentDataStore] Initialized: " << dbPath << std::endl;
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    /**
     * @brief 批量存储解析结果
     */
    void storeBatch(const std::vector<ParsedFrameResult>& results) {
        if (results.empty()) return;
        std::lock_guard lock(mutex_);
        if (!db_) return;

        exec("BEGIN TRANSACTION");
        for (const auto& r : results) {
            store(r);
        }
        exec("COMMIT");

        std::cout << "[AgentDataStore] stored " << results.size() << " record(s)" << std::endl;
    }

    /**
     * @brief 获取待上报数据（status=0，按 id 升序 FIFO）
     */
    std::vector<CachedRecord> fetchPending(int limit = 100) {
        std::lock_guard lock(mutex_);
        std::vector<CachedRecord> records;
        if (!db_) return records;

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id, device_id, protocol, func_code, data, report_time "
                          "FROM cached_data WHERE status = 0 ORDER BY id ASC LIMIT ?";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cout << "[ERROR] " << "[AgentDataStore] fetchPending prepare failed: " << sqlite3_errmsg(db_) << std::endl;
            return records;
        }

        sqlite3_bind_int(stmt, 1, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            CachedRecord rec;
            rec.id = sqlite3_column_int64(stmt, 0);
            rec.deviceId = sqlite3_column_int(stmt, 1);
            rec.protocol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            auto funcCodeText = sqlite3_column_text(stmt, 3);
            rec.funcCode = funcCodeText ? reinterpret_cast<const char*>(funcCodeText) : "";
            auto dataText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (dataText) {
                try {
                    rec.data = JsonHelper::parse(dataText);
                } catch (...) {}
            }
            rec.reportTime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            records.push_back(std::move(rec));
        }

        sqlite3_finalize(stmt);
        return records;
    }

    /**
     * @brief 标记记录为已发送（ACK 确认后调用）
     */
    void markSent(const std::vector<int64_t>& ids) {
        if (ids.empty()) return;
        std::lock_guard lock(mutex_);
        if (!db_) return;

        exec("BEGIN TRANSACTION");
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "UPDATE cached_data SET status = 1, sent_at = datetime('now', 'localtime') WHERE id = ?";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            exec("ROLLBACK");
            return;
        }

        for (int64_t id : ids) {
            sqlite3_bind_int64(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
        exec("COMMIT");
    }

    /**
     * @brief 清理已发送的旧数据
     */
    void cleanup(int retainDays = 7) {
        std::lock_guard lock(mutex_);
        if (!db_) return;

        std::string sql = "DELETE FROM cached_data WHERE status = 1 AND sent_at < datetime('now', '-"
                          + std::to_string(retainDays) + " days', 'localtime')";
        exec(sql);

        int deleted = sqlite3_changes(db_);
        if (deleted > 0) {
            std::cout << "[AgentDataStore] cleanup: removed " << deleted << " old record(s)" << std::endl;
        }

        exec("PRAGMA optimize");
    }

    /**
     * @brief 待上报记录数
     */
    int64_t pendingCount() {
        std::lock_guard lock(mutex_);
        if (!db_) return 0;

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT COUNT(*) FROM cached_data WHERE status = 0";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

        int64_t count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

private:
    void store(const ParsedFrameResult& r) {
        if (!db_) return;

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO cached_data (device_id, protocol, func_code, data, report_time) "
                          "VALUES (?, ?, ?, ?, ?)";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cout << "[ERROR] " << "[AgentDataStore] store prepare failed: " << sqlite3_errmsg(db_) << std::endl;
            return;
        }

        std::string dataStr = JsonHelper::serialize(r.data);

        sqlite3_bind_int(stmt, 1, r.deviceId);
        sqlite3_bind_text(stmt, 2, r.protocol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, r.funcCode.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, dataStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, r.reportTime.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cout << "[ERROR] " << "[AgentDataStore] store failed: " << sqlite3_errmsg(db_) << std::endl;
        }
        sqlite3_finalize(stmt);
    }

    void exec(const std::string& sql) {
        if (!db_) return;
        char* errMsg = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cout << "[ERROR] " << "[AgentDataStore] exec failed: " << (errMsg ? errMsg : "unknown") << std::endl;
            if (errMsg) sqlite3_free(errMsg);
        }
    }

    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};
