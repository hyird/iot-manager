#pragma once

#include "DeviceGroup.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 设备分组管理控制器
 */
class DeviceGroupController : public drogon::HttpController<DeviceGroupController> {
private:
    DeviceGroupService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DeviceGroupController::list, "/api/device-groups", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceGroupController::tree, "/api/device-groups/tree", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceGroupController::treeWithCount, "/api/device-groups/tree-count", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceGroupController::detail, "/api/device-groups/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceGroupController::create, "/api/device-groups", Post, "AuthFilter");
    ADD_METHOD_TO(DeviceGroupController::update, "/api/device-groups/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(DeviceGroupController::remove, "/api/device-groups/{id}", Delete, "AuthFilter");
    METHOD_LIST_END

    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        if (auto notModified = ETagUtils::checkETag(req, "deviceGroup")) {
            co_return notModified;
        }

        auto items = co_await service_.list(req->getParameter("keyword"), req->getParameter("status"));
        auto resp = Response::ok(items);
        ETagUtils::addETag(resp, "deviceGroup");
        co_return resp;
    }

    Task<HttpResponsePtr> tree(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        if (auto notModified = ETagUtils::checkETag(req, "deviceGroup")) {
            co_return notModified;
        }

        auto tree = co_await service_.tree(req->getParameter("status"));
        auto resp = Response::ok(tree);
        ETagUtils::addETag(resp, "deviceGroup");
        co_return resp;
    }

    Task<HttpResponsePtr> treeWithCount(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        // 使用 device + deviceGroup 双版本组合 ETag
        if (auto notModified = ETagUtils::checkETag(req, {"device", "deviceGroup"})) {
            co_return notModified;
        }

        auto tree = co_await service_.treeWithCount();
        auto resp = Response::ok(tree);
        ETagUtils::addETag(resp, {"device", "deviceGroup"});
        co_return resp;
    }

    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device-group:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "分组名称").throwIfInvalid();

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device-group:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device-group:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }
};
