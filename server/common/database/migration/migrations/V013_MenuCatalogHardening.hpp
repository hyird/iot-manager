#pragma once

#include "../MigrationTypes.hpp"

class V013_MenuCatalogHardening : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 13,
            .name = "MenuCatalogHardening",
            .description = "Add EdgeNode menu and enforce active menu uniqueness",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        // 先归并历史重复菜单。保留最早记录，并把角色授权、非冲突子项迁移过去。
        co_await txn->execSqlCoro(R"(
            DO $$
            DECLARE
                duplicate RECORD;
                child RECORD;
                target_child_id INT;
            BEGIN
                FOR duplicate IN
                    SELECT id, keep_id
                    FROM (
                        SELECT id, MIN(id) OVER (PARTITION BY component) AS keep_id,
                               ROW_NUMBER() OVER (PARTITION BY component ORDER BY id) AS rn
                        FROM sys_menu
                        WHERE deleted_at IS NULL AND type = 'page' AND component IS NOT NULL
                    ) ranked
                    WHERE rn > 1
                LOOP
                    INSERT INTO sys_role_menu (role_id, menu_id)
                    SELECT role_id, duplicate.keep_id FROM sys_role_menu WHERE menu_id = duplicate.id
                    ON CONFLICT (role_id, menu_id) DO NOTHING;

                    FOR child IN SELECT * FROM sys_menu WHERE parent_id = duplicate.id AND deleted_at IS NULL
                    LOOP
                        target_child_id := NULL;
                        IF child.permission_code IS NOT NULL THEN
                            SELECT id INTO target_child_id
                            FROM sys_menu
                            WHERE parent_id = duplicate.keep_id
                              AND permission_code = child.permission_code
                              AND deleted_at IS NULL
                            ORDER BY id LIMIT 1;
                        END IF;

                        IF target_child_id IS NULL THEN
                            UPDATE sys_menu SET parent_id = duplicate.keep_id,
                                updated_at = CURRENT_TIMESTAMP WHERE id = child.id;
                        ELSE
                            INSERT INTO sys_role_menu (role_id, menu_id)
                            SELECT role_id, target_child_id FROM sys_role_menu WHERE menu_id = child.id
                            ON CONFLICT (role_id, menu_id) DO NOTHING;
                            DELETE FROM sys_role_menu WHERE menu_id = child.id;
                            UPDATE sys_menu SET deleted_at = CURRENT_TIMESTAMP,
                                updated_at = CURRENT_TIMESTAMP WHERE id = child.id;
                        END IF;
                    END LOOP;

                    DELETE FROM sys_role_menu WHERE menu_id = duplicate.id;
                    UPDATE sys_menu SET deleted_at = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP WHERE id = duplicate.id;
                END LOOP;

                FOR duplicate IN
                    SELECT id, keep_id
                    FROM (
                        SELECT id, MIN(id) OVER (PARTITION BY path) AS keep_id,
                               ROW_NUMBER() OVER (PARTITION BY path ORDER BY id) AS rn
                        FROM sys_menu
                        WHERE deleted_at IS NULL AND type = 'page' AND path IS NOT NULL
                    ) ranked
                    WHERE rn > 1
                LOOP
                    INSERT INTO sys_role_menu (role_id, menu_id)
                    SELECT role_id, duplicate.keep_id FROM sys_role_menu WHERE menu_id = duplicate.id
                    ON CONFLICT (role_id, menu_id) DO NOTHING;
                    UPDATE sys_menu SET parent_id = duplicate.keep_id, updated_at = CURRENT_TIMESTAMP
                    WHERE parent_id = duplicate.id AND deleted_at IS NULL;
                    DELETE FROM sys_role_menu WHERE menu_id = duplicate.id;
                    UPDATE sys_menu SET deleted_at = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP WHERE id = duplicate.id;
                END LOOP;

                FOR duplicate IN
                    SELECT id, keep_id
                    FROM (
                        SELECT id, MIN(id) OVER (PARTITION BY path) AS keep_id,
                               ROW_NUMBER() OVER (PARTITION BY path ORDER BY id) AS rn
                        FROM sys_menu
                        WHERE deleted_at IS NULL AND type = 'menu' AND path IS NOT NULL
                    ) ranked
                    WHERE rn > 1
                LOOP
                    INSERT INTO sys_role_menu (role_id, menu_id)
                    SELECT role_id, duplicate.keep_id FROM sys_role_menu WHERE menu_id = duplicate.id
                    ON CONFLICT (role_id, menu_id) DO NOTHING;
                    UPDATE sys_menu SET parent_id = duplicate.keep_id, updated_at = CURRENT_TIMESTAMP
                    WHERE parent_id = duplicate.id AND deleted_at IS NULL;
                    DELETE FROM sys_role_menu WHERE menu_id = duplicate.id;
                    UPDATE sys_menu SET deleted_at = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP WHERE id = duplicate.id;
                END LOOP;

                FOR duplicate IN
                    SELECT id, keep_id
                    FROM (
                        SELECT id, MIN(id) OVER (PARTITION BY parent_id, permission_code) AS keep_id,
                               ROW_NUMBER() OVER (PARTITION BY parent_id, permission_code ORDER BY id) AS rn
                        FROM sys_menu
                        WHERE deleted_at IS NULL AND type = 'button' AND permission_code IS NOT NULL
                    ) ranked
                    WHERE rn > 1
                LOOP
                    INSERT INTO sys_role_menu (role_id, menu_id)
                    SELECT role_id, duplicate.keep_id FROM sys_role_menu WHERE menu_id = duplicate.id
                    ON CONFLICT (role_id, menu_id) DO NOTHING;
                    DELETE FROM sys_role_menu WHERE menu_id = duplicate.id;
                    UPDATE sys_menu SET deleted_at = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP WHERE id = duplicate.id;
                END LOOP;
            END $$
        )");

        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS uq_sys_menu_page_component_active
            ON sys_menu (component)
            WHERE deleted_at IS NULL AND type = 'page' AND component IS NOT NULL
        )");
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS uq_sys_menu_page_path_active
            ON sys_menu (path)
            WHERE deleted_at IS NULL AND type = 'page' AND path IS NOT NULL
        )");
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS uq_sys_menu_group_path_active
            ON sys_menu (path)
            WHERE deleted_at IS NULL AND type = 'menu' AND path IS NOT NULL
        )");
        co_await txn->execSqlCoro(R"(
            CREATE UNIQUE INDEX IF NOT EXISTS uq_sys_menu_button_permission_active
            ON sys_menu (parent_id, permission_code)
            WHERE deleted_at IS NULL AND type = 'button' AND permission_code IS NOT NULL
        )");

        co_await txn->execSqlCoro(R"(
            DO $$
            DECLARE edge_page_id INT;
            BEGIN
                SELECT id INTO edge_page_id
                FROM sys_menu
                WHERE deleted_at IS NULL AND component = 'EdgeNode'
                ORDER BY id LIMIT 1;

                IF edge_page_id IS NULL THEN
                    INSERT INTO sys_menu (
                        name, parent_id, type, path, component, icon, status, sort_order
                    ) VALUES (
                        '边缘节点', 0, 'page', '/edge-node', 'EdgeNode',
                        'CloudServerOutlined', 'enabled', 2
                    ) RETURNING id INTO edge_page_id;
                ELSE
                    UPDATE sys_menu
                    SET name = '边缘节点', parent_id = 0, type = 'page',
                        path = '/edge-node', component = 'EdgeNode',
                        permission_code = NULL, icon = 'CloudServerOutlined',
                        status = 'enabled', sort_order = 2,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE id = edge_page_id;
                END IF;

                INSERT INTO sys_menu (name, parent_id, type, permission_code, status, sort_order)
                VALUES ('查询边缘节点', edge_page_id, 'button', 'iot:link:query', 'enabled', 1)
                ON CONFLICT (parent_id, permission_code)
                    WHERE deleted_at IS NULL AND type = 'button' AND permission_code IS NOT NULL
                DO UPDATE SET name = EXCLUDED.name, status = 'enabled', sort_order = EXCLUDED.sort_order,
                              updated_at = CURRENT_TIMESTAMP;

                INSERT INTO sys_menu (name, parent_id, type, permission_code, status, sort_order)
                VALUES ('管理边缘节点', edge_page_id, 'button', 'iot:link:edit', 'enabled', 2)
                ON CONFLICT (parent_id, permission_code)
                    WHERE deleted_at IS NULL AND type = 'button' AND permission_code IS NOT NULL
                DO UPDATE SET name = EXCLUDED.name, status = 'enabled', sort_order = EXCLUDED.sort_order,
                              updated_at = CURRENT_TIMESTAMP;

                INSERT INTO sys_role_menu (role_id, menu_id)
                SELECT role.id, menu.id
                FROM sys_role role
                CROSS JOIN sys_menu menu
                WHERE role.code = 'superadmin' AND role.deleted_at IS NULL
                  AND menu.deleted_at IS NULL
                  AND (menu.id = edge_page_id OR menu.parent_id = edge_page_id)
                ON CONFLICT (role_id, menu_id) DO NOTHING;
            END $$
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS uq_sys_menu_button_permission_active");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS uq_sys_menu_group_path_active");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS uq_sys_menu_page_path_active");
        co_await txn->execSqlCoro("DROP INDEX IF EXISTS uq_sys_menu_page_component_active");
    }
};
