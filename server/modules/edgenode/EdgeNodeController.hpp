#pragma once

#include "EdgeNodeService.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/ValidatorHelper.hpp"

class EdgeNodeController : public drogon::HttpController<EdgeNodeController> {
private:
    EdgeNodeService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(EdgeNodeController::list, "/api/agent", Get, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::create, "/api/agent", Post, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::options, "/api/agent/options", Get, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::events, "/api/agent/{id}/events", Get, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::update, "/api/agent/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::resync, "/api/agent/{id}/resync", Post, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::updateNetworkConfig, "/api/agent/{id}/network-config", Put, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::remove, "/api/agent/{id}", Delete, "AuthFilter");
    // Agent Endpoint routes
    ADD_METHOD_TO(EdgeNodeController::listEndpoints, "/api/agent/{id}/endpoints", Get, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::createEndpoint, "/api/agent/{id}/endpoints", Post, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::updateEndpoint, "/api/agent/endpoints/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(EdgeNodeController::removeEndpoint, "/api/agent/endpoints/{id}", Delete, "AuthFilter");
    METHOD_LIST_END

    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});
        co_return Response::ok(co_await service_.list());
    }

    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        auto json = req->getJsonObject();
        if (!json) {
            co_return Response::badRequest("无效的请求体");
        }

        ValidatorHelper::requireNonEmptyString(*json, "code", "Agent编码").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "name", "名称").throwIfInvalid();

        co_return Response::ok(co_await service_.create(*json), "创建成功");
    }

    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});
        co_return Response::ok(co_await service_.options());
    }

    Task<HttpResponsePtr> events(HttpRequestPtr req, int id) {
        if (id <= 0) {
            co_return Response::badRequest("无效的资源ID");
        }

        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});

        int hours = ValidatorHelper::getIntParam(req, "hours", 24);
        int limit = ValidatorHelper::getIntParam(req, "limit", 100);
        if (hours <= 0 || hours > 168) {
            co_return Response::badRequest("hours 参数范围必须在 1-168 之间");
        }
        if (limit <= 0 || limit > 500) {
            co_return Response::badRequest("limit 参数范围必须在 1-500 之间");
        }

        const auto result = co_await service_.recentEvents(id, hours, limit);
        if (!result.found) {
            co_return Response::notFound("采集 Agent 不存在");
        }

        co_return Response::ok(result.items);
    }

    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) {
            co_return Response::badRequest("无效的资源ID");
        }

        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        auto json = req->getJsonObject();
        if (!json) {
            co_return Response::badRequest("无效的请求体");
        }

        co_await service_.update(id, *json);
        co_return Response::ok(Json::Value::null, "更新成功");
    }

    Task<HttpResponsePtr> resync(HttpRequestPtr req, int id) {
        if (id <= 0) {
            co_return Response::badRequest("无效的资源ID");
        }

        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});
        const auto result = co_await service_.requestConfigSync(id);
        if (!result.found) {
            co_return Response::notFound("采集 Agent 不存在");
        }
        if (!result.dispatched) {
            co_return Response::conflict("采集 Agent 当前离线，无法立即重同步");
        }

        co_return Response::ok(Json::Value::null, "已发送配置重同步请求");
    }

    Task<HttpResponsePtr> updateNetworkConfig(HttpRequestPtr req, int id) {
        if (id <= 0) {
            co_return Response::badRequest("无效的资源ID");
        }

        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});
        auto json = req->getJsonObject();
        if (!json) {
            co_return Response::badRequest("无效的请求体");
        }

        const auto result = co_await service_.updateNetworkConfig(id, *json);
        if (!result.found) {
            co_return Response::notFound("采集 Agent 不存在");
        }

        co_return Response::ok(Json::Value::null,
            result.dispatched ? "网络配置已保存并下发" : "网络配置已保存，Agent 离线时将在上线后自动下发");
    }

    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) {
            co_return Response::badRequest("无效的资源ID");
        }

        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});
        const auto result = co_await service_.remove(id);
        if (!result.found) {
            co_return Response::notFound("采集 Agent 不存在");
        }
        if (result.rejected) {
            co_return Response::conflict(result.rejectReason);
        }

        co_return Response::ok(Json::Value::null, "采集 Agent 已删除");
    }

    // ========== Agent Endpoint ==========

    Task<HttpResponsePtr> listEndpoints(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});
        co_return Response::ok(co_await service_.listEndpoints(id));
    }

    Task<HttpResponsePtr> createEndpoint(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("无效的请求体");

        ValidatorHelper::requireNonEmptyString(*json, "name", "端点名称").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "protocol", "协议类型").throwIfInvalid();

        co_return Response::ok(co_await service_.createEndpoint(id, *json), "创建成功");
    }

    Task<HttpResponsePtr> updateEndpoint(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("无效的请求体");

        co_await service_.updateEndpoint(id, *json);
        co_return Response::ok(Json::Value::null, "更新成功");
    }

    Task<HttpResponsePtr> removeEndpoint(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        const auto result = co_await service_.removeEndpoint(id);
        if (!result.found) co_return Response::notFound("端点不存在");
        if (result.rejected) co_return Response::conflict(result.rejectReason);

        co_return Response::ok(Json::Value::null, "端点已删除");
    }
};

using AgentController = EdgeNodeController;
