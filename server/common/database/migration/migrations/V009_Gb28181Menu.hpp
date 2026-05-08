#pragma once

#include "../MigrationTypes.hpp"

class V009_Gb28181Menu : public MigrationBase {
public:
    MigrationInfo info() const override {
        return {
            .version = 9,
            .name = "Gb28181Menu",
            .description = "Add GB28181 single-page console menu and permissions",
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
                    status = 'enabled',
                    deleted_at = NULL,
                    updated_at = CURRENT_TIMESTAMP
                FROM (
                    VALUES
                        ('查询国标', 'iot:gb28181:query', 1),
                        ('国标控制', 'iot:gb28181:control', 2),
                        ('录像回放', 'iot:gb28181:record', 3)
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
                        ('录像回放', 'iot:gb28181:record', 3)
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
            WITH page AS (
                SELECT id
                FROM sys_menu
                WHERE component = 'GB28181'
                  AND deleted_at IS NULL
                LIMIT 1
            ),
            menu_ids AS (
                SELECT id
                FROM sys_menu
                WHERE deleted_at IS NULL
                  AND (
                      component = 'GB28181'
                      OR (
                          parent_id = (SELECT id FROM page)
                          AND permission_code IN (
                              'iot:gb28181:query',
                              'iot:gb28181:control',
                              'iot:gb28181:record'
                          )
                      )
                  )
            )
            INSERT INTO sys_role_menu (role_id, menu_id)
            SELECT role.id, menu_ids.id
            FROM sys_role role
            CROSS JOIN menu_ids
            WHERE role.code = 'superadmin'
              AND role.deleted_at IS NULL
            ON CONFLICT (role_id, menu_id) DO NOTHING
        )");
    }

    Task<> down(const TransactionPtr& txn) override {
        co_await txn->execSqlCoro(R"(
            WITH page AS (
                SELECT id
                FROM sys_menu
                WHERE component = 'GB28181' OR path = '/iot/gb28181'
            ),
            menu_ids AS (
                SELECT id
                FROM sys_menu
                WHERE id IN (SELECT id FROM page)
                   OR parent_id IN (SELECT id FROM page)
            )
            DELETE FROM sys_role_menu
            WHERE menu_id IN (SELECT id FROM menu_ids)
        )");

        co_await txn->execSqlCoro(R"(
            WITH page AS (
                SELECT id
                FROM sys_menu
                WHERE component = 'GB28181' OR path = '/iot/gb28181'
            )
            UPDATE sys_menu
            SET deleted_at = CURRENT_TIMESTAMP,
                updated_at = CURRENT_TIMESTAMP
            WHERE id IN (SELECT id FROM page)
               OR parent_id IN (SELECT id FROM page)
        )");
    }
};
