#pragma once

#include "common/utils/Constants.hpp"
#include "common/utils/PasswordUtils.hpp"
#include "DatabaseService.hpp"
#include "migration/MigrationRunner.hpp"
#include "migration/MigrationRegistry.hpp"
#include "migration/migrations/V001_Baseline.hpp"
#include "migration/migrations/V002_TimescaleDB.hpp"
#include "migration/migrations/V003_AgentSelfRegistration.hpp"
#include "migration/migrations/V004_ResourceOwnershipAndDeviceShare.hpp"
#include "migration/migrations/V005_DeviceShareTargetScope.hpp"
#include "migration/migrations/V006_DeviceSharePermissionJsonb.hpp"
#include "migration/migrations/V007_DeviceShareTargetInPermission.hpp"
#include "migration/migrations/V008_DeviceShareDropTargetColumns.hpp"
#include "migration/migrations/V009_Gb28181Menu.hpp"
#include "migration/migrations/V010_Gb28181PtzMenu.hpp"
#include "migration/migrations/V011_VideoMonitorMenu.hpp"
#include "migration/migrations/V012_MenuSeedRepair.hpp"
#include "migration/migrations/V013_MenuCatalogHardening.hpp"
#include "migration/migrations/V014_LinkClientTargetsJsonb.hpp"
#include "migration/migrations/V015_DeviceDataStoragePolicy.hpp"
#include <cstdlib>

/**
 * @brief 数据库初始化器
 *
 * 职责：
 * 1. 执行版本化迁移（schema 创建/变更）
 * 2. 初始化种子数据（管理员用户、默认菜单）
 * 3. 创建动态物化视图（依赖运行时数据）
 *
 * schema 定义已迁移到 migration/migrations/ 目录下的版本化迁移文件中。
 * 新增 schema 变更请创建新的迁移文件（参考 V003_Example.hpp）。
 */
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

    /// 注册所有迁移（新增迁移在此处添加）
    static MigrationRegistry registerMigrations() {
        MigrationRegistry registry;
        registry.add<V001_Baseline>();
        registry.add<V002_TimescaleDB>();
        registry.add<V003_AgentSelfRegistration>();
        registry.add<V004_ResourceOwnershipAndDeviceShare>();
        registry.add<V005_DeviceShareTargetScope>();
        registry.add<V006_DeviceSharePermissionJsonb>();
        registry.add<V007_DeviceShareTargetInPermission>();
        registry.add<V008_DeviceShareDropTargetColumns>();
        registry.add<V009_Gb28181Menu>();
        registry.add<V010_Gb28181PtzMenu>();
        registry.add<V011_VideoMonitorMenu>();
        registry.add<V012_MenuSeedRepair>();
        registry.add<V013_MenuCatalogHardening>();
        registry.add<V014_LinkClientTargetsJsonb>();
        registry.add<V015_DeviceDataStoragePolicy>();
        // 新增迁移在此处注册，例如：
        // registry.add<V003_AddDeviceTags>();
        return registry;
    }

public:
    static Task<> initialize() {
        auto db = getDbClient();

        LOG_INFO << "Starting database initialization...";

        // 抑制 IF NOT EXISTS 产生的 NOTICE
        co_await db->execSqlCoro("SET client_min_messages = WARNING");

        // 数据库级别固定 UTC 时区
        co_await db->execSqlCoro(R"(
            DO $$ BEGIN
                EXECUTE format('ALTER DATABASE %I SET timezone = ''UTC''', current_database());
            END $$
        )");

        // ─── 1. 执行版本化迁移 ──────────────────────────────
        auto registry = registerMigrations();
        auto result = co_await MigrationRunner::migrate(db, registry);

        if (!result.ok()) {
            std::string errorMsg = "Database migration failed:";
            for (const auto& err : result.errors) {
                errorMsg += "\n  - " + err;
                LOG_ERROR << "[Migration] " << err;
            }
            throw std::runtime_error(errorMsg);
        }

        LOG_INFO << "[Migration] Complete: "
                 << result.applied << " applied, "
                 << result.skipped << " skipped, "
                 << "current version: V"
                 << fmtVersion(result.currentVersion);

        // ─── 2. 种子数据（管理员用户） ──────────────────────
        int superadminRoleId = co_await initializeAdminUser(db);
        co_await initializeMenus(db, superadminRoleId);

        // ─── 3. 动态物化视图（依赖运行时 protocol_config 数据） ──
        co_await createMaterializedViews(db);

        LOG_INFO << "Database initialization completed";
    }

private:
    // ─── 种子数据：管理员用户初始化 ─────────────────────────

    static Task<int> initializeAdminUser(const DbClientPtr& db) {
        // 超级管理员角色和默认菜单需要在已有数据库中持续校准，不能被用户计数短路。
        co_await db->execSqlCoro(
            R"(INSERT INTO sys_role (name, code, description, sort_order)
               VALUES ('超级管理员', 'superadmin', '拥有系统所有权限', 1)
               ON CONFLICT (code) WHERE deleted_at IS NULL DO NOTHING)"
        );

        auto roleResult = co_await db->execSqlCoro(
            "SELECT id FROM sys_role WHERE code = 'superadmin'"
        );
        int roleId = roleResult[0]["id"].as<int>();

        auto userCount = co_await db->execSqlCoro(
            R"(SELECT COUNT(*) as count FROM sys_user WHERE deleted_at IS NULL)"
        );

        if (!userCount.empty() && userCount[0]["count"].as<int64_t>() > 0) {
            LOG_INFO << "Users already exist, skipping default admin creation";
            co_return roleId;
        }

        LOG_INFO << "No users found, creating default admin...";

        // 创建管理员用户
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

        auto userResult = co_await db->execSqlCoro(
            "SELECT id FROM sys_user WHERE username = 'admin'"
        );
        int userId = userResult[0]["id"].as<int>();

        co_await db->execSqlCoro(
            R"(INSERT INTO sys_user_role (user_id, role_id)
               VALUES ($1, $2)
               ON CONFLICT (user_id, role_id) DO NOTHING)",
            userId, roleId
        );

        LOG_INFO << "Default admin user created: admin";
        co_return roleId;
    }

    // ─── 种子数据：菜单初始化 ────────────────────────────────

    static Task<int> insertMenu(const DbClientPtr& db, const std::string& name,
                                int parentId, const std::string& type,
                                const std::string& path, const std::string& component,
                                const std::string& permissionCode, const std::string& icon,
                                bool isDefault, int sortOrder) {
        auto existing = co_await db->execSqlCoro(
            R"(SELECT id FROM sys_menu
               WHERE deleted_at IS NULL AND (
                   ($1 = 'button' AND type = 'button' AND parent_id = $2
                       AND permission_code = NULLIF($3, ''))
                   OR ($1 = 'page' AND type = 'page' AND NULLIF($4, '') IS NOT NULL
                       AND component = NULLIF($4, ''))
                   OR ($1 NOT IN ('button', 'page') AND type = $1::menu_type_enum
                       AND path = NULLIF($5, ''))
               )
               ORDER BY id LIMIT 1)",
            type, parentId, permissionCode, component, path
        );

        if (!existing.empty()) {
            int id = existing[0]["id"].as<int>();
            co_await db->execSqlCoro(
                R"(UPDATE sys_menu
                   SET name = $1, parent_id = $2, type = $3,
                       path = NULLIF($4, ''), component = NULLIF($5, ''),
                       permission_code = NULLIF($6, ''), icon = NULLIF($7, ''),
                       is_default = $8, status = 'enabled', sort_order = $9,
                       updated_at = CURRENT_TIMESTAMP
                   WHERE id = $10)",
                name, parentId, type, path, component, permissionCode,
                icon, isDefault, sortOrder, id
            );
            co_return id;
        }

        auto result = co_await db->execSqlCoro(
            R"(INSERT INTO sys_menu (name, parent_id, type, path, component, permission_code, icon, is_default, sort_order)
               VALUES ($1, $2, $3, NULLIF($4, ''), NULLIF($5, ''), NULLIF($6, ''), NULLIF($7, ''), $8, $9)
               RETURNING id)",
            name, parentId, type, path, component, permissionCode, icon, isDefault, sortOrder
        );
        co_return result[0]["id"].as<int>();
    }

    static Task<int> insertButton(const DbClientPtr& db, const std::string& name,
                                  int parentId, const std::string& permissionCode, int sortOrder) {
        co_return co_await insertMenu(db, name, parentId, Constants::MENU_TYPE_BUTTON, "", "", permissionCode, "", false, sortOrder);
    }

    static Task<> ensureButton(const DbClientPtr& db, const std::string& name,
                               int parentId, const std::string& permissionCode, int sortOrder) {
        auto existing = co_await db->execSqlCoro(
            R"(SELECT id FROM sys_menu
               WHERE parent_id = $1 AND permission_code = $2 AND deleted_at IS NULL
               ORDER BY id LIMIT 1)",
            parentId, permissionCode
        );
        if (existing.empty()) {
            co_await insertButton(db, name, parentId, permissionCode, sortOrder);
        }
    }

    static Task<> initializeMenus(const DbClientPtr& db, int roleId) {
        // 首页
        int homeId = co_await insertMenu(db, "首页", 0, Constants::MENU_TYPE_PAGE, "/home", "Home", "", "HomeOutlined", true, 0);
        co_await insertButton(db, "查看统计", homeId, "home:dashboard:query", 1);
        co_await insertButton(db, "清理缓存", homeId, "system:cache:clear", 2);

        // 链路管理
        int linkId = co_await insertMenu(db, "链路管理", 0, Constants::MENU_TYPE_PAGE, "/link", "Link", "", "LinkOutlined", false, 1);
        co_await insertButton(db, "查询链路", linkId, "iot:link:query", 1);
        co_await insertButton(db, "新增链路", linkId, "iot:link:add", 2);
        co_await insertButton(db, "编辑链路", linkId, "iot:link:edit", 3);
        co_await insertButton(db, "删除链路", linkId, "iot:link:delete", 4);

        // 边缘节点（复用链路查询/编辑权限码，但独立挂载按钮，便于角色单独授权）
        int edgeNodeId = co_await insertMenu(db, "边缘节点", 0, Constants::MENU_TYPE_PAGE, "/edge-node", "EdgeNode", "", "CloudServerOutlined", false, 2);
        co_await insertButton(db, "查询边缘节点", edgeNodeId, "iot:link:query", 1);
        co_await insertButton(db, "管理边缘节点", edgeNodeId, "iot:link:edit", 2);

        // 协议配置
        int protocolId = co_await insertMenu(db, "协议配置", 0, Constants::MENU_TYPE_MENU, "/iot", "", "", "ApiOutlined", false, 3);

        int sl651Id = co_await insertMenu(db, "SL651配置", protocolId, Constants::MENU_TYPE_PAGE, "/iot/sl651", "SL651Config", "", "SettingOutlined", false, 0);
        co_await insertButton(db, "查询配置", sl651Id, "iot:protocol:query", 1);
        co_await insertButton(db, "新增配置", sl651Id, "iot:protocol:add", 2);
        co_await insertButton(db, "编辑配置", sl651Id, "iot:protocol:edit", 3);
        co_await insertButton(db, "删除配置", sl651Id, "iot:protocol:delete", 4);
        co_await insertButton(db, "导入配置", sl651Id, "iot:protocol:import", 5);
        co_await insertButton(db, "导出配置", sl651Id, "iot:protocol:export", 6);

        int modbusId = co_await insertMenu(db, "Modbus配置", protocolId, Constants::MENU_TYPE_PAGE, "/iot/modbus", "ModbusConfig", "", "SettingOutlined", false, 1);
        co_await insertButton(db, "查询配置", modbusId, "iot:protocol:query", 1);
        co_await insertButton(db, "新增配置", modbusId, "iot:protocol:add", 2);
        co_await insertButton(db, "编辑配置", modbusId, "iot:protocol:edit", 3);
        co_await insertButton(db, "删除配置", modbusId, "iot:protocol:delete", 4);
        co_await insertButton(db, "导入配置", modbusId, "iot:protocol:import", 5);
        co_await insertButton(db, "导出配置", modbusId, "iot:protocol:export", 6);

        int s7Id = co_await insertMenu(db, "S7配置", protocolId, Constants::MENU_TYPE_PAGE, "/iot/s7", "S7Config", "", "SettingOutlined", false, 2);
        co_await insertButton(db, "查询配置", s7Id, "iot:protocol:query", 1);
        co_await insertButton(db, "新增配置", s7Id, "iot:protocol:add", 2);
        co_await insertButton(db, "编辑配置", s7Id, "iot:protocol:edit", 3);
        co_await insertButton(db, "删除配置", s7Id, "iot:protocol:delete", 4);
        co_await insertButton(db, "导入配置", s7Id, "iot:protocol:import", 5);
        co_await insertButton(db, "导出配置", s7Id, "iot:protocol:export", 6);

        // 设备管理
        int deviceId = co_await insertMenu(db, "设备管理", 0, Constants::MENU_TYPE_PAGE, "/device", "Device", "", "HddOutlined", false, 4);
        co_await insertButton(db, "查询设备", deviceId, "iot:device:query", 1);
        co_await insertButton(db, "新增设备", deviceId, "iot:device:add", 2);
        co_await insertButton(db, "编辑设备", deviceId, "iot:device:edit", 3);
        co_await insertButton(db, "删除设备", deviceId, "iot:device:delete", 4);
        co_await insertButton(db, "查询分组", deviceId, "iot:device-group:query", 5);
        co_await insertButton(db, "新增分组", deviceId, "iot:device-group:add", 6);
        co_await insertButton(db, "编辑分组", deviceId, "iot:device-group:edit", 7);
        co_await insertButton(db, "删除分组", deviceId, "iot:device-group:delete", 8);

        // 视频监控（GB28181）
        auto gb28181Menus = co_await db->execSqlCoro(R"(
            SELECT id FROM sys_menu
            WHERE deleted_at IS NULL
              AND (component = 'GB28181' OR path = '/iot/gb28181')
            ORDER BY id
            LIMIT 1
        )");
        int gb28181Id;
        if (gb28181Menus.empty()) {
            gb28181Id = co_await insertMenu(db, "视频监控", 0, Constants::MENU_TYPE_PAGE, "/iot/gb28181", "GB28181", "", "VideoCameraOutlined", false, 5);
        } else {
            gb28181Id = gb28181Menus[0]["id"].as<int>();
            co_await db->execSqlCoro(R"(
                UPDATE sys_menu
                SET name = '视频监控', parent_id = 0, type = 'page',
                    path = '/iot/gb28181', component = 'GB28181',
                    permission_code = NULL, icon = 'VideoCameraOutlined',
                    status = 'enabled', sort_order = 5, updated_at = CURRENT_TIMESTAMP
                WHERE id = $1
            )", gb28181Id);
        }
        co_await ensureButton(db, "查询国标", gb28181Id, "iot:gb28181:query", 1);
        co_await ensureButton(db, "国标控制", gb28181Id, "iot:gb28181:control", 2);
        co_await ensureButton(db, "录像回放", gb28181Id, "iot:gb28181:record", 3);

        // 开放接入
        int openAccessId = co_await insertMenu(db, "开放接入", 0, Constants::MENU_TYPE_PAGE, "/iot/open-access", "OpenAccess", "", "CloudServerOutlined", false, 6);
        co_await insertButton(db, "查询开放接入", openAccessId, "iot:open-access:query", 1);
        co_await insertButton(db, "新增开放接入", openAccessId, "iot:open-access:add", 2);
        co_await insertButton(db, "编辑开放接入", openAccessId, "iot:open-access:edit", 3);
        co_await insertButton(db, "删除开放接入", openAccessId, "iot:open-access:delete", 4);

        // 告警管理
        int alertId = co_await insertMenu(db, "告警管理", 0, Constants::MENU_TYPE_MENU, "/alert", "", "", "AlertOutlined", false, 7);

        int alertPageId = co_await insertMenu(db, "告警管理", alertId, Constants::MENU_TYPE_PAGE, "/alert", "Alert", "", "AlertOutlined", false, 1);
        co_await insertButton(db, "查询", alertPageId, "iot:alert:query", 1);
        co_await insertButton(db, "新增规则", alertPageId, "iot:alert:add", 2);
        co_await insertButton(db, "编辑规则", alertPageId, "iot:alert:edit", 3);
        co_await insertButton(db, "删除规则", alertPageId, "iot:alert:delete", 4);
        co_await insertButton(db, "确认告警", alertPageId, "iot:alert:ack", 5);

        // 系统管理
        int systemId = co_await insertMenu(db, "系统管理", 0, Constants::MENU_TYPE_MENU, "/system", "", "", "SettingOutlined", false, 999);

        int menuId = co_await insertMenu(db, "菜单管理", systemId, Constants::MENU_TYPE_PAGE, "/system/menu", "Menu", "", "MenuOutlined", false, 1);
        co_await insertButton(db, "查询菜单", menuId, "system:menu:query", 1);
        co_await insertButton(db, "新增菜单", menuId, "system:menu:add", 2);
        co_await insertButton(db, "编辑菜单", menuId, "system:menu:edit", 3);
        co_await insertButton(db, "删除菜单", menuId, "system:menu:delete", 4);

        int deptId = co_await insertMenu(db, "部门管理", systemId, Constants::MENU_TYPE_PAGE, "/system/department", "Dept", "", "ApartmentOutlined", false, 2);
        co_await insertButton(db, "查询部门", deptId, "system:dept:query", 1);
        co_await insertButton(db, "新增部门", deptId, "system:dept:add", 2);
        co_await insertButton(db, "编辑部门", deptId, "system:dept:edit", 3);
        co_await insertButton(db, "删除部门", deptId, "system:dept:delete", 4);

        int roleMenuId = co_await insertMenu(db, "角色管理", systemId, Constants::MENU_TYPE_PAGE, "/system/role", "Role", "", "TeamOutlined", false, 3);
        co_await insertButton(db, "查询角色", roleMenuId, "system:role:query", 1);
        co_await insertButton(db, "新增角色", roleMenuId, "system:role:add", 2);
        co_await insertButton(db, "编辑角色", roleMenuId, "system:role:edit", 3);
        co_await insertButton(db, "删除角色", roleMenuId, "system:role:delete", 4);
        co_await insertButton(db, "分配权限", roleMenuId, "system:role:perm", 5);

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

        LOG_INFO << "Default menus synchronized";
    }

    // ─── 动态物化视图（依赖运行时数据） ─────────────────────

    static bool isValidIdentifier(const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
    }

    static Task<> createMaterializedViews(const DbClientPtr& db) {
        auto protocols = co_await db->execSqlCoro(
            "SELECT DISTINCT protocol FROM protocol_config WHERE deleted_at IS NULL"
        );

        for (const auto& row : protocols) {
            std::string protocol = row["protocol"].as<std::string>();

            // 校验协议名称：仅允许字母数字下划线，同时防止标识符和字符串字面量注入
            if (!isValidIdentifier(protocol)) {
                LOG_WARN << "Skipping invalid protocol name for materialized view: " << protocol;
                continue;
            }

            try {
                const std::string viewName = "mv_device_data_latest_" + protocol;
                const std::string indexName = "idx_mv_" + protocol + "_device_func";

                // 这里不能把参数交给 DO 块里的 format()，否则 libpq 会把它当成“无参数语句”并报错。
                // 协议名已通过 isValidIdentifier 校验，只包含字母、数字和下划线。
                std::ostringstream viewSql;
                viewSql << "CREATE MATERIALIZED VIEW IF NOT EXISTS " << viewName << R"(
AS
SELECT DISTINCT ON (device_id, data->>'funcCode')
    device_id,
    data->>'funcCode' as func_code,
    data,
    report_time,
    created_at
FROM device_data
WHERE protocol = ')" << protocol << R"('
ORDER BY device_id, data->>'funcCode', report_time DESC NULLS LAST)";
                co_await db->execSqlCoro(viewSql.str());

                std::ostringstream indexSql;
                indexSql << "CREATE UNIQUE INDEX IF NOT EXISTS " << indexName
                         << " ON " << viewName << " (device_id, func_code)";
                co_await db->execSqlCoro(indexSql.str());

                LOG_INFO << "Materialized view created: mv_device_data_latest_" << protocol;
            } catch (const std::exception& e) {
                LOG_WARN << "Failed to create materialized view mv_device_data_latest_"
                         << protocol << ": " << e.what();
            }
        }

        LOG_INFO << "Materialized views created/verified";
    }
};
