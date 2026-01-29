#pragma once

#include <drogon/HttpController.h>
#include <functional>
#include "User.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

using namespace drogon;

/**
 * @brief 用户管理控制器
 */
class UserController : public HttpController<UserController> {
private:
    UserService service_;

public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserController::list, "/api/users", Get, "AuthFilter");
    ADD_METHOD_TO(UserController::detail, "/api/users/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(UserController::create, "/api/users", Post, "AuthFilter");
    ADD_METHOD_TO(UserController::update, "/api/users/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(UserController::remove, "/api/users/{id}", Delete, "AuthFilter");
    METHOD_LIST_END

    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:user:query"});

        auto page = Pagination::fromRequest(req);
        std::string status = req->getParameter("status");
        int departmentId = ValidatorHelper::getIntParam(req, "departmentId", 0);

        // 参数化 ETag：参数哈希 + 资源版本号
        std::string paramStr = status + ":" + std::to_string(departmentId) + ":" +
                               std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        size_t paramHash = std::hash<std::string>{}(paramStr);
        std::string version = ResourceVersion::instance().getVersion("user");
        std::string etag = "\"" + std::to_string(paramHash) + "-" + version + "\"";

        std::string ifNoneMatch = req->getHeader("If-None-Match");
        if (!ifNoneMatch.empty() && ifNoneMatch == etag) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k304NotModified);
            co_return resp;
        }

        auto result = co_await service_.list(page, status, departmentId);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        resp->addHeader("ETag", etag);
        co_return resp;
    }

    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:user:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:user:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "username", "用户名").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "password", "密码").throwIfInvalid();
        ValidatorHelper::requireMinLength(*json, "password", Constants::PASSWORD_MIN_LENGTH, "密码").throwIfInvalid();

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:user:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireMinLength(*json, "password", Constants::PASSWORD_MIN_LENGTH, "密码").throwIfInvalid();

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"system:user:delete"});
        co_await service_.remove(id, ControllerUtils::getUserId(req));
        co_return Response::deleted("删除成功");
    }
};
