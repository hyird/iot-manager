#pragma once

#include "../MigrationTypes.hpp"

class V012_MenuSeedRepair : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 12,
            .name = "MenuSeedRepair",
            .description = "Deduplicate video monitor menu and add missing S7 configuration page",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            DO $$
            DECLARE
                keeper_id INT;
            BEGIN
                SELECT id INTO keeper_id
                FROM sys_menu
                WHERE deleted_at IS NULL
                  AND (component = 'GB28181' OR path = '/iot/gb28181')
                ORDER BY id
                LIMIT 1;

                IF keeper_id IS NOT NULL THEN
                    UPDATE sys_menu
                    SET name = '视频监控', parent_id = 0, type = 'page',
                        path = '/iot/gb28181', component = 'GB28181',
                        permission_code = NULL, icon = 'VideoCameraOutlined',
                        status = 'enabled', sort_order = 4,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE id = keeper_id;

                    UPDATE sys_menu AS child
                    SET parent_id = keeper_id,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE child.deleted_at IS NULL
                      AND child.parent_id IN (
                          SELECT id FROM sys_menu
                          WHERE id <> keeper_id AND deleted_at IS NULL
                            AND (component = 'GB28181' OR path = '/iot/gb28181')
                      )
                      AND NOT EXISTS (
                          SELECT 1 FROM sys_menu AS existing
                          WHERE existing.parent_id = keeper_id
                            AND existing.permission_code = child.permission_code
                            AND existing.deleted_at IS NULL
                      );

                    DELETE FROM sys_role_menu
                    WHERE menu_id IN (
                        SELECT child.id
                        FROM sys_menu AS child
                        WHERE child.parent_id IN (
                            SELECT id FROM sys_menu
                            WHERE id <> keeper_id AND deleted_at IS NULL
                              AND (component = 'GB28181' OR path = '/iot/gb28181')
                        )
                    );

                    UPDATE sys_menu
                    SET deleted_at = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE parent_id IN (
                        SELECT id FROM sys_menu
                        WHERE id <> keeper_id AND deleted_at IS NULL
                          AND (component = 'GB28181' OR path = '/iot/gb28181')
                    );

                    DELETE FROM sys_role_menu
                    WHERE menu_id IN (
                        SELECT id FROM sys_menu
                        WHERE id <> keeper_id AND deleted_at IS NULL
                          AND (component = 'GB28181' OR path = '/iot/gb28181')
                    );

                    UPDATE sys_menu
                    SET deleted_at = CURRENT_TIMESTAMP,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE id <> keeper_id AND deleted_at IS NULL
                      AND (component = 'GB28181' OR path = '/iot/gb28181');
                END IF;
            END $$;
        )");

        co_await txn->execSqlCoro(R"(
            DO $$
            DECLARE
                protocol_id INT;
                page_id INT;
            BEGIN
                SELECT id INTO protocol_id
                FROM sys_menu
                WHERE name = '协议配置' AND parent_id = 0 AND deleted_at IS NULL
                ORDER BY id LIMIT 1;

                IF protocol_id IS NOT NULL THEN
                    SELECT id INTO page_id
                    FROM sys_menu
                    WHERE deleted_at IS NULL
                      AND (component = 'S7Config' OR path = '/iot/s7')
                    ORDER BY id LIMIT 1;

                    IF page_id IS NULL THEN
                        INSERT INTO sys_menu (
                            name, parent_id, type, path, component, icon,
                            is_default, status, sort_order
                        ) VALUES (
                            'S7配置', protocol_id, 'page', '/iot/s7', 'S7Config',
                            'SettingOutlined', false, 'enabled', 2
                        ) RETURNING id INTO page_id;
                    ELSE
                        UPDATE sys_menu
                        SET name = 'S7配置', parent_id = protocol_id, type = 'page',
                            path = '/iot/s7', component = 'S7Config',
                            permission_code = NULL, icon = 'SettingOutlined',
                            status = 'enabled', sort_order = 2,
                            updated_at = CURRENT_TIMESTAMP
                        WHERE id = page_id;
                    END IF;

                    UPDATE sys_menu AS menu
                    SET name = item.name, type = 'button', status = 'enabled',
                        sort_order = item.sort_order, deleted_at = NULL,
                        updated_at = CURRENT_TIMESTAMP
                    FROM (VALUES
                        ('查询配置', 'iot:protocol:query', 1),
                        ('新增配置', 'iot:protocol:add', 2),
                        ('编辑配置', 'iot:protocol:edit', 3),
                        ('删除配置', 'iot:protocol:delete', 4),
                        ('导入配置', 'iot:protocol:import', 5),
                        ('导出配置', 'iot:protocol:export', 6)
                    ) AS item(name, permission_code, sort_order)
                    WHERE menu.parent_id = page_id
                      AND menu.permission_code = item.permission_code;

                    INSERT INTO sys_menu (
                        name, parent_id, type, permission_code, status, sort_order
                    )
                    SELECT item.name, page_id, 'button', item.permission_code,
                           'enabled', item.sort_order
                    FROM (VALUES
                        ('查询配置', 'iot:protocol:query', 1),
                        ('新增配置', 'iot:protocol:add', 2),
                        ('编辑配置', 'iot:protocol:edit', 3),
                        ('删除配置', 'iot:protocol:delete', 4),
                        ('导入配置', 'iot:protocol:import', 5),
                        ('导出配置', 'iot:protocol:export', 6)
                    ) AS item(name, permission_code, sort_order)
                    WHERE NOT EXISTS (
                        SELECT 1 FROM sys_menu
                        WHERE parent_id = page_id
                          AND permission_code = item.permission_code
                          AND deleted_at IS NULL
                    );
                END IF;
            END $$;
        )");

        co_await txn->execSqlCoro(R"(
            INSERT INTO sys_role_menu (role_id, menu_id)
            SELECT role.id, menu.id
            FROM sys_role AS role
            CROSS JOIN sys_menu AS menu
            WHERE role.code = 'superadmin'
              AND role.deleted_at IS NULL
              AND menu.deleted_at IS NULL
              AND (
                  menu.component IN ('GB28181', 'S7Config')
                  OR menu.parent_id IN (
                      SELECT id FROM sys_menu
                      WHERE deleted_at IS NULL
                        AND component IN ('GB28181', 'S7Config')
                  )
              )
            ON CONFLICT (role_id, menu_id) DO NOTHING
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            WITH page AS (
                SELECT id FROM sys_menu
                WHERE component = 'S7Config' OR path = '/iot/s7'
            )
            DELETE FROM sys_role_menu
            WHERE menu_id IN (SELECT id FROM page)
               OR menu_id IN (SELECT id FROM sys_menu WHERE parent_id IN (SELECT id FROM page))
        )");

        co_await txn->execSqlCoro(R"(
            WITH page AS (
                SELECT id FROM sys_menu
                WHERE component = 'S7Config' OR path = '/iot/s7'
            )
            UPDATE sys_menu
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE id IN (SELECT id FROM page)
               OR parent_id IN (SELECT id FROM page)
        )");
    }
};
