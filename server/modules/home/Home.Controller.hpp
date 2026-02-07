#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/AppException.hpp"
#include "common/cache/AuthCache.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/network/WebSocketManager.hpp"
#include "common/database/RedisService.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/protocol/ProtocolDispatcher.hpp"

#ifdef _WIN32
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <fstream>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

/**
 * @brief 首页控制器
 */
class HomeController : public drogon::HttpController<HomeController> {
private:
    DatabaseService db_;
    AuthCache authCache_;
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HomeController::stats, "/api/home/stats", Get, "AuthFilter");
    ADD_METHOD_TO(HomeController::systemInfo, "/api/home/system", Get, "AuthFilter");
    ADD_METHOD_TO(HomeController::clearCache, "/api/home/cache/clear", Post, "AuthFilter");
    ADD_METHOD_TO(HomeController::monitor, "/api/home/monitor", Get, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取统计数据
     * 单条 SQL 合并所有 COUNT，减少 DB 往返
     */
    Task<HttpResponsePtr> stats(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        // 合并所有统计查询为单条 SQL（4 次往返 → 1 次）
        auto result = co_await db_.execSqlCoro(R"(
            SELECT
                (SELECT COUNT(*) FROM sys_user WHERE deleted_at IS NULL) AS user_count,
                (SELECT COUNT(*) FROM sys_role WHERE deleted_at IS NULL) AS role_count,
                (SELECT COUNT(*) FROM sys_menu WHERE deleted_at IS NULL AND type = 'page') AS menu_count,
                (SELECT COUNT(*) FROM sys_department WHERE deleted_at IS NULL) AS dept_count,
                (SELECT COUNT(*) FROM device WHERE deleted_at IS NULL) AS device_count,
                (SELECT COUNT(*) FROM link WHERE deleted_at IS NULL) AS link_count,
                (SELECT COUNT(*) FROM alert_record WHERE status = 'active') AS active_alert_count,
                (SELECT COUNT(*) FROM alert_record WHERE status = 'active' AND severity = 'critical') AS critical_alert_count
        )");

        Json::Value data;
        data["userCount"] = result[0]["user_count"].as<int>();
        data["roleCount"] = result[0]["role_count"].as<int>();
        data["menuCount"] = result[0]["menu_count"].as<int>();
        data["departmentCount"] = result[0]["dept_count"].as<int>();
        data["deviceCount"] = result[0]["device_count"].as<int>();
        data["linkCount"] = result[0]["link_count"].as<int>();
        data["activeAlertCount"] = result[0]["active_alert_count"].as<int>();
        data["criticalAlertCount"] = result[0]["critical_alert_count"].as<int>();

        // 今日数据量：优先从连续聚合读取，回退到直接 COUNT
        // 注意：MSVC 不支持 catch 块中使用 co_await，用标志位重构
        bool useFallbackCount = false;
        try {
            auto todayResult = co_await db_.execSqlCoro(R"(
                SELECT COALESCE(SUM(record_count), 0) AS today_count
                FROM device_data_hourly
                WHERE bucket >= date_trunc('day', now())
            )");
            data["todayDataCount"] = static_cast<Json::Int64>(todayResult[0]["today_count"].as<int64_t>());
        } catch (...) {
            useFallbackCount = true;
        }
        if (useFallbackCount) {
            // 连续聚合不可用，回退到直接查询
            auto todayResult = co_await db_.execSqlCoro(R"(
                SELECT COUNT(*) AS today_count
                FROM device_data
                WHERE report_time >= date_trunc('day', now())
            )");
            data["todayDataCount"] = static_cast<Json::Int64>(todayResult[0]["today_count"].as<int64_t>());
        }

        co_return Response::ok(data);
    }

    /**
     * @brief 获取系统信息
     */
    Task<HttpResponsePtr> systemInfo(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();

        // 获取当前时间（UTC）
        auto currentTime = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(currentTime);
        std::ostringstream oss;
        std::tm tmBuf{};
#ifdef _WIN32
        gmtime_s(&tmBuf, &timeT);
#else
        gmtime_r(&timeT, &tmBuf);
#endif
        oss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%SZ");

        // 查询数据库时区配置
        auto tzResult = co_await db_.execSqlCoro("SHOW timezone");
        std::string dbTimezone = tzResult[0][0].as<std::string>();

        Json::Value data;
        data["version"] = "1.0.0";
        data["serverTime"] = oss.str();
        data["timezone"] = dbTimezone;
        data["uptime"] = static_cast<Json::Int64>(uptime);
#ifdef _WIN32
        data["platform"] = "Windows";
#elif __APPLE__
        data["platform"] = "macOS";
#else
        data["platform"] = "Linux";
#endif

        co_return Response::ok(data);
    }

    /**
     * @brief 清理所有缓存
     */
    Task<HttpResponsePtr> clearCache(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"system:cache:clear"}
        );

        // 清理认证相关缓存
        co_await authCache_.clearAllUserSessionsCache();
        co_await authCache_.clearAllUserRolesCache();
        co_await authCache_.clearAllUserMenusCache();

        // 清理设备缓存
        DeviceCache::instance().markStale();

        // 清理设备实时数据缓存
        RealtimeDataCache::instance().invalidateAll();

        // 重置所有资源版本
        ResourceVersion::instance().resetAll();

        LOG_INFO << "All caches cleared by userId: " << req->attributes()->get<int>("userId");

        co_return Response::ok("缓存清理成功");
    }

    /**
     * @brief 获取系统监控数据
     */
    Task<HttpResponsePtr> monitor(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            req->attributes()->get<int>("userId"),
            {"home:dashboard:query"}
        );

        Json::Value data;

        // 1. TCP 链路状态
        auto allStatus = TcpLinkManager::instance().getAllStatus();
        int totalLinks = allStatus.size();
        int activeLinks = 0;
        int totalConnections = 0;
        for (const auto& link : allStatus) {
            auto connStatus = link["conn_status"].asString();
            if (connStatus == "listening" || connStatus == "connected") {
                activeLinks++;
            }
            totalConnections += link["client_count"].asInt();
        }
        auto tcpStats = TcpLinkManager::instance().getTcpStats();
        Json::Value tcp;
        tcp["totalLinks"] = totalLinks;
        tcp["activeLinks"] = activeLinks;
        tcp["totalConnections"] = totalConnections;
        tcp["bytesRx"] = static_cast<Json::Int64>(tcpStats.bytesRx);
        tcp["bytesTx"] = static_cast<Json::Int64>(tcpStats.bytesTx);
        tcp["packetsRx"] = static_cast<Json::Int64>(tcpStats.packetsRx);
        tcp["packetsTx"] = static_cast<Json::Int64>(tcpStats.packetsTx);
        data["tcp"] = tcp;

        // 2. WebSocket 状态
        Json::Value ws;
        ws["connections"] = static_cast<int>(WebSocketManager::instance().connectionCount());
        ws["onlineUsers"] = static_cast<int>(WebSocketManager::instance().onlineUserCount());
        data["websocket"] = ws;

        // 3. Redis 状态（INFO memory/stats/server/clients + DBSIZE）
        Json::Value redis;
        bool redisOk = true;
        try {
            RedisService redisService;
            auto client = redisService.getClient();

            // INFO memory
            auto memResult = co_await client->execCommandCoro("INFO memory");
            std::string memInfo = memResult.asString();
            redis["usedMemory"] = parseRedisField(memInfo, "used_memory_human:");

            // DBSIZE
            auto dbsizeResult = co_await client->execCommandCoro("DBSIZE");
            redis["keyCount"] = static_cast<int>(dbsizeResult.asInteger());

            // INFO stats: ops/sec, hit/miss
            auto statsResult = co_await client->execCommandCoro("INFO stats");
            std::string statsInfo = statsResult.asString();
            redis["opsPerSec"] = parseRedisInt(statsInfo, "instantaneous_ops_per_sec:");
            auto hits = parseRedisInt64(statsInfo, "keyspace_hits:");
            auto misses = parseRedisInt64(statsInfo, "keyspace_misses:");
            if (hits + misses > 0) {
                redis["hitRate"] = static_cast<double>(hits) / static_cast<double>(hits + misses) * 100.0;
            } else {
                redis["hitRate"] = 0.0;
            }

            // INFO server: uptime
            auto serverResult = co_await client->execCommandCoro("INFO server");
            std::string serverInfo = serverResult.asString();
            redis["uptimeSeconds"] = parseRedisInt(serverInfo, "uptime_in_seconds:");

            // INFO clients: connected_clients
            auto clientsResult = co_await client->execCommandCoro("INFO clients");
            std::string clientsInfo = clientsResult.asString();
            redis["connectedClients"] = parseRedisInt(clientsInfo, "connected_clients:");
        } catch (...) {
            redisOk = false;
        }
        redis["status"] = redisOk ? "ok" : "error";
        if (!redisOk) {
            redis["usedMemory"] = "N/A";
            redis["keyCount"] = 0;
        }
        data["redis"] = redis;

        // 4. PostgreSQL 连接 + 性能状态
        Json::Value pg;
        bool pgOk = true;
        try {
            auto connResult = co_await db_.execSqlCoro(R"(
                SELECT
                    (SELECT count(*) FROM pg_stat_activity
                     WHERE state = 'active') AS active_connections,
                    (SELECT count(*) FROM pg_stat_activity
                     WHERE state = 'idle') AS idle_connections,
                    (SELECT setting::int FROM pg_settings
                     WHERE name = 'max_connections') AS max_connections,
                    (SELECT ROUND(
                        sum(heap_blks_hit)::numeric / NULLIF(sum(heap_blks_hit) + sum(heap_blks_read), 0) * 100, 2
                    ) FROM pg_statio_user_tables) AS cache_hit_ratio,
                    (SELECT xact_commit FROM pg_stat_database
                     WHERE datname = current_database()) AS xact_commit,
                    (SELECT xact_rollback FROM pg_stat_database
                     WHERE datname = current_database()) AS xact_rollback,
                    (SELECT pg_size_pretty(pg_database_size(current_database()))) AS db_size
            )");
            pg["activeConnections"] = connResult[0]["active_connections"].as<int>();
            pg["idleConnections"] = connResult[0]["idle_connections"].as<int>();
            pg["maxConnections"] = connResult[0]["max_connections"].as<int>();
            if (!connResult[0]["cache_hit_ratio"].isNull()) {
                pg["cacheHitRatio"] = connResult[0]["cache_hit_ratio"].as<double>();
            } else {
                pg["cacheHitRatio"] = 0.0;
            }
            pg["xactCommit"] = static_cast<Json::Int64>(connResult[0]["xact_commit"].as<int64_t>());
            pg["xactRollback"] = static_cast<Json::Int64>(connResult[0]["xact_rollback"].as<int64_t>());
            pg["databaseSize"] = connResult[0]["db_size"].as<std::string>();
        } catch (...) {
            pgOk = false;
        }
        pg["status"] = pgOk ? "ok" : "error";
        if (!pgOk) {
            pg["activeConnections"] = 0;
            pg["idleConnections"] = 0;
            pg["maxConnections"] = 0;
        }
        data["postgres"] = pg;

        // 5. 设备连接注册
        auto connStats = DeviceConnectionCache::instance().getStats();
        Json::Value device;
        device["registeredDevices"] = connStats["deviceCount"];
        device["registeredClients"] = connStats["clientCount"];
        data["device"] = device;

        // 6. 协议处理统计
        auto protoStats = ProtocolDispatcher::instance().getProtocolStats();
        Json::Value protocol;
        protocol["pendingCommands"] = static_cast<int>(protoStats.pendingCommands);
        protocol["framesProcessed"] = static_cast<Json::Int64>(protoStats.framesProcessed);
        protocol["batchFlushes"] = static_cast<Json::Int64>(protoStats.batchFlushes);
        protocol["batchFallbacks"] = static_cast<Json::Int64>(protoStats.batchFallbacks);
        data["protocol"] = protocol;

        // 6b. Modbus 性能统计
        auto& dispatcher = ProtocolDispatcher::instance();
        if (dispatcher.hasModbusHandler()) {
            auto modbusStats = dispatcher.getModbusStats();
            Json::Value modbus;
            modbus["totalResponses"] = static_cast<Json::Int64>(modbusStats.totalResponses);
            modbus["avgLatencyMs"] = modbusStats.avgLatencyMs;
            modbus["timeouts"] = static_cast<Json::Int64>(modbusStats.timeouts);
            modbus["crcErrors"] = static_cast<Json::Int64>(modbusStats.crcErrors);
            modbus["exceptions"] = static_cast<Json::Int64>(modbusStats.exceptions);
            data["modbus"] = modbus;
        }

        // 6c. SL651 性能统计
        if (dispatcher.hasSl651Parser()) {
            auto sl651Stats = dispatcher.getSl651Stats();
            Json::Value sl651;
            sl651["framesParsed"] = static_cast<Json::Int64>(sl651Stats.framesParsed);
            sl651["crcErrors"] = static_cast<Json::Int64>(sl651Stats.crcErrors);
            sl651["multiPacketCompleted"] = static_cast<Json::Int64>(sl651Stats.multiPacketCompleted);
            sl651["multiPacketExpired"] = static_cast<Json::Int64>(sl651Stats.multiPacketExpired);
            sl651["parseErrors"] = static_cast<Json::Int64>(sl651Stats.parseErrors);
            data["sl651"] = sl651;
        }

        // 7. 服务器系统状态
        Json::Value server;
        server["processMemory"] = getProcessMemoryMB();
        server["hostname"] = getHostname();
        server["os"] = getOsInfo();
        server["cpuCores"] = getCpuCoreCount();

        auto [memTotal, memUsed] = getSystemMemory();
        server["memoryTotal"] = memTotal;
        server["memoryUsed"] = memUsed;

        auto [diskTotal, diskUsed] = getDiskUsage();
        server["diskTotal"] = diskTotal;
        server["diskUsed"] = diskUsed;

#ifndef _WIN32
        server["loadAvg"] = getLoadAverage();
#endif

        data["server"] = server;

        co_return Response::ok(data);
    }

private:
    // ==================== Redis INFO 解析助手 ====================

    /** 从 Redis INFO 输出中提取字段值（字符串） */
    static std::string parseRedisField(const std::string& info, const std::string& key) {
        auto pos = info.find(key);
        if (pos == std::string::npos) return "N/A";
        auto end = info.find("\r\n", pos);
        return info.substr(pos + key.size(), end - pos - key.size());
    }

    /** 从 Redis INFO 输出中提取整数字段 */
    static int parseRedisInt(const std::string& info, const std::string& key) {
        auto val = parseRedisField(info, key);
        try { return std::stoi(val); } catch (...) { return 0; }
    }

    /** 从 Redis INFO 输出中提取 int64 字段 */
    static int64_t parseRedisInt64(const std::string& info, const std::string& key) {
        auto val = parseRedisField(info, key);
        try { return std::stoll(val); } catch (...) { return 0; }
    }

    // ==================== 系统信息助手 ====================

    /** 获取进程内存使用（MB） */
    static double getProcessMemoryMB() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        }
        return 0.0;
#else
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line.substr(6));
                double kb = 0;
                iss >> kb;
                return kb / 1024.0;
            }
        }
        return 0.0;
#endif
    }

    /** 获取主机名 */
    static std::string getHostname() {
        char buf[256]{};
#ifdef _WIN32
        DWORD size = sizeof(buf);
        GetComputerNameA(buf, &size);
#else
        gethostname(buf, sizeof(buf));
#endif
        return buf;
    }

    /** 获取操作系统信息 */
    static std::string getOsInfo() {
#ifdef _WIN32
        return "Windows";
#else
        struct utsname info{};
        if (uname(&info) == 0) {
            return std::string(info.sysname) + " " + info.release;
        }
        return "Linux";
#endif
    }

    /** 获取 CPU 核心数 */
    static int getCpuCoreCount() {
        int cores = static_cast<int>(std::thread::hardware_concurrency());
        return cores > 0 ? cores : 1;
    }

    /** 获取系统内存（MB）：{total, used} */
    static std::pair<double, double> getSystemMemory() {
#ifdef _WIN32
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            double total = static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0);
            double avail = static_cast<double>(mem.ullAvailPhys) / (1024.0 * 1024.0);
            return {total, total - avail};
        }
        return {0.0, 0.0};
#else
        std::ifstream f("/proc/meminfo");
        std::string line;
        double total = 0, available = 0;
        while (std::getline(f, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream iss(line.substr(9));
                iss >> total;
                total /= 1024.0; // kB → MB
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::istringstream iss(line.substr(13));
                iss >> available;
                available /= 1024.0;
            }
        }
        return {total, total - available};
#endif
    }

    /** 获取磁盘使用（GB）：{total, used} */
    static std::pair<double, double> getDiskUsage() {
#ifdef _WIN32
        ULARGE_INTEGER freeBytesAvailable, totalBytes, totalFreeBytes;
        if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvailable, &totalBytes, &totalFreeBytes)) {
            double total = static_cast<double>(totalBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
            double free = static_cast<double>(totalFreeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
            return {total, total - free};
        }
        return {0.0, 0.0};
#else
        struct statvfs stat{};
        if (statvfs("/", &stat) == 0) {
            double total = static_cast<double>(stat.f_blocks) * stat.f_frsize / (1024.0 * 1024.0 * 1024.0);
            double free = static_cast<double>(stat.f_bavail) * stat.f_frsize / (1024.0 * 1024.0 * 1024.0);
            return {total, total - free};
        }
        return {0.0, 0.0};
#endif
    }

#ifndef _WIN32
    /** 获取系统负载（Linux only）：1min 平均负载 */
    static double getLoadAverage() {
        std::ifstream f("/proc/loadavg");
        double load1 = 0;
        f >> load1;
        return load1;
    }
#endif
};
