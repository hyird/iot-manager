#pragma once

#include "ProtocolConfig.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/filters/ResourcePermission.hpp"

#include <string>

/**
 * @brief 协议配置控制器
 * 提供协议配置的 CRUD 接口
 */
class ProtocolConfigController : public drogon::HttpController<ProtocolConfigController> {
private:
    ProtocolConfigService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ProtocolConfigController::list, "/api/protocol/configs", Get, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::detail, "/api/protocol/configs/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::create, "/api/protocol/configs", Post, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::update, "/api/protocol/configs/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::remove, "/api/protocol/configs/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::options, "/api/protocol/configs/options", Get, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取配置列表（支持参数化 ETag 缓存）
     * @param protocol 协议类型筛选（可选）
     */
    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});

        auto page = Pagination::fromRequest(req);
        std::string protocol = req->getParameter("protocol");

        // 参数化 ETag 检查
        std::string params = protocol + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "protocol", params)) {
            co_return notModified;
        }

        auto result = co_await service_.list(page, protocol);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "protocol", params);
        co_return resp;
    }

    /**
     * @brief 获取配置详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    /**
     * @brief 创建配置
     */
    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:protocol:add"});

        auto json = ControllerUtils::requireJson(req);

        co_await service_.create(*json, userId);
        co_return Response::created("创建成功");
    }

    /**
     * @brief 更新配置
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:protocol:edit"});
        co_await ResourcePermission::ensureProtocolOwnerOrSuperAdmin(id, userId);

        auto json = ControllerUtils::requireJson(req);
        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除配置
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:protocol:delete"});
        co_await ResourcePermission::ensureProtocolOwnerOrSuperAdmin(id, userId);
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }

    /**
     * @brief 获取指定协议的配置选项（用于下拉选择，支持 ETag 缓存 + 可选分页）
     */
    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});

        std::string protocol = req->getParameter("protocol");
        auto page = Pagination::fromRequest(req);

        std::string params = protocol + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "protocol", params)) {
            co_return notModified;
        }

        auto items = co_await service_.options(protocol);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        auto resp = Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "protocol", params);
        co_return resp;
    }
};
