#pragma once

#include "PermissionFilter.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/utils/AppException.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/SqlHelper.hpp"

#include <unordered_map>
#include <vector>

/**
 * @brief 资源级权限检查器
 *
 * 规则：
 * - 超级管理员可访问所有资源
 * - 链路/协议配置/设备：仅创建者可编辑、删除
 * - 设备：可通过分享授予查看(view)/控制(control)权限
 */
class ResourcePermission {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    static Task<void> ensureLinkOwnerOrSuperAdmin(int linkId, int userId) {
        if (co_await PermissionChecker::isSuperAdmin(userId)) {
            co_return;
        }

        auto creator = co_await getResourceCreator(
            "SELECT created_by FROM link WHERE id = ? AND deleted_at IS NULL",
            linkId,
            "链路"
        );
        if (creator != userId) {
            throw ForbiddenException("仅创建者可编辑或删除链路");
        }
    }

    static Task<void> ensureProtocolOwnerOrSuperAdmin(int protocolConfigId, int userId) {
        if (co_await PermissionChecker::isSuperAdmin(userId)) {
            co_return;
        }

        auto creator = co_await getResourceCreator(
            "SELECT created_by FROM protocol_config WHERE id = ? AND deleted_at IS NULL",
            protocolConfigId,
            "设备类型"
        );
        if (creator != userId) {
            throw ForbiddenException("仅创建者可编辑或删除设备类型");
        }
    }

    static Task<void> ensureDeviceOwnerOrSuperAdmin(int deviceId, int userId) {
        if (co_await PermissionChecker::isSuperAdmin(userId)) {
            co_return;
        }

        auto creator = co_await getResourceCreator(
            "SELECT created_by FROM device WHERE id = ? AND deleted_at IS NULL",
            deviceId,
            "设备"
        );
        if (creator != userId) {
            throw ForbiddenException("仅创建者可编辑或删除设备");
        }
    }

    static Task<void> ensureDeviceViewPermission(int deviceId, int userId) {
        auto info = co_await getDeviceAccessInfo(deviceId, userId, "设备");

        if (info.isSuperAdmin || info.creatorId == userId) {
            co_return;
        }
        if (!info.sharePermission.empty()) {
            co_return;
        }

        throw ForbiddenException("未获得设备查看权限");
    }

    static Task<void> ensureDeviceControlPermission(int deviceId, int userId) {
        auto info = co_await getDeviceAccessInfo(deviceId, userId, "设备");

        if (info.isSuperAdmin || info.creatorId == userId) {
            co_return;
        }
        if (info.sharePermission == "control") {
            co_return;
        }

        throw ForbiddenException("未获得设备控制权限");
    }

    static Task<std::unordered_map<int, std::string>> loadDeviceSharePermissions(
        int userId,
        const std::vector<int>& deviceIds
    ) {
        std::unordered_map<int, std::string> permissions;
        if (deviceIds.empty()) {
            co_return permissions;
        }

        auto [placeholders, idParams] = SqlHelper::buildParameterizedIn(deviceIds);
        std::vector<std::string> params;
        params.reserve(1 + idParams.size());
        params.push_back(std::to_string(userId));
        params.insert(params.end(), idParams.begin(), idParams.end());

        DatabaseService db;
        auto rows = co_await db.execSqlCoro(
            "SELECT device_id, permission FROM device_share WHERE user_id = ? AND device_id IN (" + placeholders + ")",
            params
        );

        for (const auto& row : rows) {
            permissions[FieldHelper::getInt(row["device_id"])] = FieldHelper::getString(row["permission"], "view");
        }
        co_return permissions;
    }

    static Task<std::string> getSingleDeviceSharePermission(int deviceId, int userId) {
        DatabaseService db;
        auto rows = co_await db.execSqlCoro(
            "SELECT permission FROM device_share WHERE device_id = ? AND user_id = ? LIMIT 1",
            {std::to_string(deviceId), std::to_string(userId)}
        );
        if (rows.empty()) {
            co_return "";
        }
        co_return FieldHelper::getString(rows[0]["permission"], "");
    }

private:
    struct DeviceAccessInfo {
        int creatorId = 0;
        bool isSuperAdmin = false;
        std::string sharePermission;
    };

    static Task<int> getResourceCreator(
        const std::string& sql,
        int resourceId,
        const std::string& resourceName
    ) {
        DatabaseService db;
        auto rows = co_await db.execSqlCoro(sql, {std::to_string(resourceId)});
        if (rows.empty()) {
            throw NotFoundException(resourceName + "不存在");
        }
        if (rows[0]["created_by"].isNull()) {
            co_return 0;
        }
        co_return FieldHelper::getInt(rows[0]["created_by"]);
    }

    static Task<DeviceAccessInfo> getDeviceAccessInfo(
        int deviceId,
        int userId,
        const std::string& resourceName
    ) {
        DeviceAccessInfo info;

        DatabaseService db;
        auto rows = co_await db.execSqlCoro(
            "SELECT created_by FROM device WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(deviceId)}
        );
        if (rows.empty()) {
            throw NotFoundException(resourceName + "不存在");
        }
        if (!rows[0]["created_by"].isNull()) {
            info.creatorId = FieldHelper::getInt(rows[0]["created_by"]);
        }

        info.isSuperAdmin = co_await PermissionChecker::isSuperAdmin(userId);
        if (!info.isSuperAdmin && info.creatorId != userId) {
            info.sharePermission = co_await getSingleDeviceSharePermission(deviceId, userId);
        }

        co_return info;
    }
};
