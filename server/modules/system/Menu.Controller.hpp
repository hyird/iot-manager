#pragma once

#include "Menu.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 菜单管理控制器
 */
class MenuController : public drogon::HttpController<MenuController> {
private:
    MenuService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MenuController::list, "/api/menus", Get, "AuthFilter");
    ADD_METHOD_TO(MenuController::tree, "/api/menus/tree", Get, "AuthFilter");
    ADD_METHOD_TO(MenuController::detail, "/api/menus/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(MenuController::create, "/api/menus", Post, "AuthFilter");
    ADD_METHOD_TO(MenuController::update, "/api/menus/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(MenuController::remove, "/api/menus/{id}", Delete, "AuthFilter");
    METHOD_LIST_END

    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:menu:query"});

        // ETag 检查（菜单数据变化不频繁）
        if (auto notModified = ETagUtils::checkETag(req, "menu")) {
            co_return notModified;
        }

        auto items = co_await service_.list(req->getParameter("keyword"), req->getParameter("status"));
        auto resp = Response::ok(items);
        ETagUtils::addETag(resp, "menu");
        co_return resp;
    }

    Task<HttpResponsePtr> tree(HttpRequestPtr req) {
        // 权限检查优先，防止未授权用户通过 ETag 探测资源变更
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:menu:query"});

        // ETag 检查
        if (auto notModified = ETagUtils::checkETag(req, "menu")) {
            co_return notModified;
        }

        auto tree = co_await service_.tree(req->getParameter("status"));
        auto resp = Response::ok(tree);
        ETagUtils::addETag(resp, "menu");
        co_return resp;
    }

    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:menu:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:menu:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "菜单名称").throwIfInvalid();

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:menu:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:menu:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }
};
