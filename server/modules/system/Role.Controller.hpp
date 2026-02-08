#pragma once

#include "Role.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 角色管理控制器
 */
class RoleController : public drogon::HttpController<RoleController> {
private:
    RoleService service_;

    /**
     * @brief 根据更新内容确定所需权限
     * @param json 请求体
     * @return 所需的权限列表
     */
    static std::vector<std::string> determineUpdatePermissions(const Json::Value& json) {
        bool hasBasicFields = json.isMember("name") || json.isMember("code") ||
                              json.isMember("description") || json.isMember("status");
        bool hasMenuIds = json.isMember("menu_ids");

        std::vector<std::string> permissions;
        if (hasBasicFields) {
            permissions.push_back("system:role:edit");
        }
        if (hasMenuIds) {
            permissions.push_back("system:role:perm");
        }
        return permissions;
    }

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RoleController::list, "/api/roles", Get, "AuthFilter");
    ADD_METHOD_TO(RoleController::all, "/api/roles/all", Get, "AuthFilter");
    ADD_METHOD_TO(RoleController::detail, "/api/roles/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(RoleController::create, "/api/roles", Post, "AuthFilter");
    ADD_METHOD_TO(RoleController::update, "/api/roles/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(RoleController::remove, "/api/roles/{id}", Delete, "AuthFilter");
    METHOD_LIST_END

    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:role:query"});

        auto page = Pagination::fromRequest(req);
        std::string status = req->getParameter("status");

        // 参数化 ETag 检查
        std::string params = status + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "role", params)) {
            co_return notModified;
        }

        auto result = co_await service_.list(page, status);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "role", params);
        co_return resp;
    }

    Task<HttpResponsePtr> all(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:role:query"});

        auto page = Pagination::fromRequest(req);

        std::string params = std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "role", params)) {
            co_return notModified;
        }

        auto items = co_await service_.all();
        auto [pagedItems, total] = Pagination::paginate(items, page);
        auto resp = Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "role", params);
        co_return resp;
    }

    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:role:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:role:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "角色名称").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "code", "角色编码").throwIfInvalid();

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        // 根据更新内容动态检查权限（至少需要基础编辑权限）
        auto permissions = determineUpdatePermissions(*json);
        if (permissions.empty()) {
            permissions.push_back("system:role:edit");
        }
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), permissions);

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:role:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }
};
