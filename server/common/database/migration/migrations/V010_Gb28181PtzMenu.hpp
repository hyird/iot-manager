#pragma once

#include "../MigrationTypes.hpp"

class V010_Gb28181PtzMenu : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 10,
            .name = "Gb28181PtzMenu",
            .description = "Add GB28181 PTZ permission and repair its menu seed",
            .transactional = true
        };
    }

    Task<> up(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            DO $$
            DECLARE
                page_id INT;
            BEGIN
                UPDATE sys_menu
                SET name = 'GB28181',
                    parent_id = 0,
                    type = 'page',
                    path = '/iot/gb28181',
                    component = 'GB28181',
                    permission_code = NULL,
                    icon = 'VideoCameraOutlined',
                    is_default = false,
                    status = 'enabled',
                    sort_order = 4,
                    deleted_at = NULL,
                    updated_at = CURRENT_TIMESTAMP
                WHERE id = (
                    SELECT id
                    FROM sys_menu
                    WHERE component = 'GB28181' OR path = '/iot/gb28181'
                    ORDER BY deleted_at NULLS FIRST, id
                    LIMIT 1
                )
                RETURNING id INTO page_id;

                IF page_id IS NULL THEN
                    INSERT INTO sys_menu (
                        name, parent_id, type, path, component, permission_code,
                        icon, is_default, status, sort_order, created_at
                    )
                    VALUES (
                        'GB28181', 0, 'page', '/iot/gb28181', 'GB28181', NULL,
                        'VideoCameraOutlined', false, 'enabled', 4, CURRENT_TIMESTAMP
                    )
                    RETURNING id INTO page_id;
                END IF;

                UPDATE sys_menu AS menu
                SET name = item.name,
                    type = 'button',
                    path = NULL,
                    component = NULL,
                    icon = NULL,
                    is_default = false,
                    status = 'enabled',
                    sort_order = item.sort_order,
                    deleted_at = NULL,
                    updated_at = CURRENT_TIMESTAMP
                FROM (
                    VALUES
                        ('查询国标', 'iot:gb28181:query', 1),
                        ('国标控制', 'iot:gb28181:control', 2),
                        ('云台控制', 'iot:gb28181:ptz', 3),
                        ('录像回放', 'iot:gb28181:record', 4)
                ) AS item(name, permission_code, sort_order)
                WHERE menu.parent_id = page_id
                  AND menu.permission_code = item.permission_code;

                INSERT INTO sys_menu (
                    name, parent_id, type, path, component, permission_code,
                    icon, is_default, status, sort_order, created_at
                )
                SELECT
                    item.name, page_id, 'button', NULL, NULL, item.permission_code,
                    NULL, false, 'enabled', item.sort_order, CURRENT_TIMESTAMP
                FROM (
                    VALUES
                        ('查询国标', 'iot:gb28181:query', 1),
                        ('国标控制', 'iot:gb28181:control', 2),
                        ('云台控制', 'iot:gb28181:ptz', 3),
                        ('录像回放', 'iot:gb28181:record', 4)
                ) AS item(name, permission_code, sort_order)
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM sys_menu
                    WHERE parent_id = page_id
                      AND permission_code = item.permission_code
                      AND deleted_at IS NULL
                );
            END $$;
        )");

        co_await txn->execSqlCoro(R"(
            WITH ptz_menu AS (
                SELECT id
                FROM sys_menu
                WHERE permission_code = 'iot:gb28181:ptz'
                  AND deleted_at IS NULL
                LIMIT 1
            ),
            inherited_roles AS (
                SELECT DISTINCT role_menu.role_id
                FROM sys_role_menu role_menu
                INNER JOIN sys_menu menu ON menu.id = role_menu.menu_id
                WHERE menu.permission_code = 'iot:gb28181:control'
                  AND menu.deleted_at IS NULL
                UNION
                SELECT id
                FROM sys_role
                WHERE code = 'superadmin'
                  AND deleted_at IS NULL
            )
            INSERT INTO sys_role_menu (role_id, menu_id)
            SELECT inherited_roles.role_id, ptz_menu.id
            FROM inherited_roles
            CROSS JOIN ptz_menu
            ON CONFLICT (role_id, menu_id) DO NOTHING
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            DELETE FROM sys_role_menu
            WHERE menu_id IN (
                SELECT id
                FROM sys_menu
                WHERE permission_code = 'iot:gb28181:ptz'
            )
        )");

        co_await txn->execSqlCoro(R"(
            UPDATE sys_menu
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE permission_code = 'iot:gb28181:ptz'
              AND deleted_at IS NULL
        )");
    }
};
