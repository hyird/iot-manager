#pragma once

#include <drogon/HttpController.h>
#include <functional>
#include "Link.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

using namespace drogon;

namespace {
    // 允许的链路模式列表
    const std::vector<std::string> ALLOWED_LINK_MODES = {
        Constants::LINK_MODE_TCP_SERVER,
        Constants::LINK_MODE_TCP_CLIENT
    };
}

/**
 * @brief 链路管理控制器
 */
class LinkController : public HttpController<LinkController> {
private:
    LinkService service_;

public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LinkController::list, "/api/link", Get, "AuthFilter");
    ADD_METHOD_TO(LinkController::detail, "/api/link/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(LinkController::create, "/api/link", Post, "AuthFilter");
    ADD_METHOD_TO(LinkController::update, "/api/link/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(LinkController::remove, "/api/link/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(LinkController::options, "/api/link/options", Get, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取链路列表（支持参数化 ETag 缓存）
     */
    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});

        auto page = Pagination::fromRequest(req);
        std::string mode = req->getParameter("mode");

        // 参数化 ETag：参数哈希 + 资源版本号
        std::string paramStr = mode + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        size_t paramHash = std::hash<std::string>{}(paramStr);
        std::string version = ResourceVersion::instance().getVersion("link");
        std::string etag = "\"" + std::to_string(paramHash) + "-" + version + "\"";

        std::string ifNoneMatch = req->getHeader("If-None-Match");
        if (!ifNoneMatch.empty() && ifNoneMatch == etag) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k304NotModified);
            co_return resp;
        }

        auto result = co_await service_.list(page, mode);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        resp->addHeader("ETag", etag);
        co_return resp;
    }

    /**
     * @brief 获取链路详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    /**
     * @brief 创建链路
     */
    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "链路名称").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "mode", "模式").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "ip", "IP地址").throwIfInvalid();
        ValidatorHelper::requirePositiveInt(*json, "port", "端口").throwIfInvalid();
        ValidatorHelper::requireInList(*json, "mode", ALLOWED_LINK_MODES,
            "模式", "TCP Server 或 TCP Client").throwIfInvalid();

        auto result = co_await service_.create(*json);
        co_return Response::ok(result, "创建成功");
    }

    /**
     * @brief 更新链路
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireInListIfPresent(*json, "mode", ALLOWED_LINK_MODES,
            "模式", "TCP Server 或 TCP Client").throwIfInvalid();

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除链路
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }

    /**
     * @brief 获取链路选项（下拉列表，支持 ETag 缓存）
     */
    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});

        if (auto notModified = ETagUtils::checkETag(req, "link")) {
            co_return notModified;
        }

        auto resp = Response::ok(co_await service_.options());
        ETagUtils::addETag(resp, "link");
        co_return resp;
    }
};
