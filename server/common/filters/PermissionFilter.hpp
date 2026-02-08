#pragma once

#include "common/utils/AppException.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/database/DatabaseService.hpp"

/**
 * @brief 权限检查工具类
 *
 * 使用单次查询同时检查超级管理员和具体权限，优化数据库查询性能
 */
class PermissionChecker {
public:
    template<typename T = void> using Task = drogon::Task<T>;
    /**
     * @brief 检查用户是否有指定权限
     *
     * 优化：使用单次查询同时检查：
     * 1. 用户是否是超级管理员
     * 2. 用户是否拥有所需的权限码
     */
    static Task<bool> hasPermission(int userId, const std::vector<std::string>& requiredPermissions) {
        if (requiredPermissions.empty()) {
            co_return true;
        }

        DatabaseService dbService;

        // 构建权限码的 IN 子句占位符
        std::string permissionsPlaceholder;
        for (size_t i = 0; i < requiredPermissions.size(); ++i) {
            if (i > 0) permissionsPlaceholder += ", ";
            permissionsPlaceholder += "?";
        }

        // 单次查询：使用 CASE WHEN 同时检查超级管理员和权限码
        // 返回两个计数：is_superadmin 和 has_permission
        std::string sql =
            "SELECT "
            "  COALESCE(MAX(CASE WHEN r.code = ? THEN 1 ELSE 0 END), 0) as is_superadmin, "
            "  COUNT(DISTINCT m.permission_code) as permission_count "
            "FROM sys_user_role ur "
            "INNER JOIN sys_role r ON ur.role_id = r.id "
            "LEFT JOIN sys_role_menu rm ON r.id = rm.role_id "
            "LEFT JOIN sys_menu m ON rm.menu_id = m.id "
            "  AND m.permission_code IN (" + permissionsPlaceholder + ") "
            "  AND m.deleted_at IS NULL "
            "WHERE ur.user_id = ? "
            "  AND r.status = 'enabled' "
            "  AND r.deleted_at IS NULL";

        // 构建参数：角色码 → 权限码 → 用户ID
        std::vector<std::string> params;
        params.reserve(requiredPermissions.size() + 2);
        params.emplace_back(Constants::ROLE_SUPERADMIN);
        for (const auto& perm : requiredPermissions) {
            params.push_back(perm);
        }
        params.push_back(std::to_string(userId));

        auto result = co_await dbService.execSqlCoro(sql, params);

        if (result.empty()) {
            co_return false;
        }

        // 检查是否是超级管理员
        if (FieldHelper::getInt(result[0]["is_superadmin"]) > 0) {
            co_return true;
        }

        // 检查是否有任意一个所需权限
        co_return FieldHelper::getInt(result[0]["permission_count"]) > 0;
    }

    /**
     * @brief 检查权限，无权限时抛出异常
     */
    static Task<void> checkPermission(int userId, const std::vector<std::string>& requiredPermissions) {
        bool hasPermit = co_await hasPermission(userId, requiredPermissions);
        if (!hasPermit) {
            throw ForbiddenException("无权限访问此资源");
        }
    }
};
