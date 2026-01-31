#pragma once

#include <drogon/HttpController.h>
#include <functional>
#include "ProtocolConfig.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

using namespace drogon;

namespace {
    // 允许的协议配置类型列表
    const std::vector<std::string> ALLOWED_CONFIG_PROTOCOLS = {
        Constants::PROTOCOL_SL651,
        Constants::PROTOCOL_MODBUS,
        Constants::PROTOCOL_MODBUS_TCP,
        Constants::PROTOCOL_MODBUS_RTU
    };
}

/**
 * @brief 协议配置控制器
 * 提供协议配置的 CRUD 接口
 */
class ProtocolConfigController : public HttpController<ProtocolConfigController> {
private:
    ProtocolConfigService service_;

public:
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

        // 参数化 ETag：参数哈希 + 资源版本号
        std::string paramStr = protocol + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        size_t paramHash = std::hash<std::string>{}(paramStr);
        std::string version = ResourceVersion::instance().getVersion("protocol");
        std::string etag = "\"" + std::to_string(paramHash) + "-" + version + "\"";

        std::string ifNoneMatch = req->getHeader("If-None-Match");
        if (!ifNoneMatch.empty() && ifNoneMatch == etag) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k304NotModified);
            co_return resp;
        }

        auto result = co_await service_.list(page, protocol);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        resp->addHeader("ETag", etag);
        co_return resp;
    }

    /**
     * @brief 获取配置详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    /**
     * @brief 创建配置
     */
    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "protocol", "协议类型").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "name", "配置名称").throwIfInvalid();
        ValidatorHelper::requireInList(*json, "protocol", ALLOWED_CONFIG_PROTOCOLS,
            "协议类型", "SL651、MODBUS、Modbus TCP 或 Modbus RTU").throwIfInvalid();

        auto result = co_await service_.create(*json);
        co_return Response::ok(result, "创建成功");
    }

    /**
     * @brief 更新配置
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireInListIfPresent(*json, "protocol", ALLOWED_CONFIG_PROTOCOLS,
            "协议类型", "SL651、MODBUS、Modbus TCP 或 Modbus RTU").throwIfInvalid();

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除配置
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }

    /**
     * @brief 获取指定协议的配置选项（用于下拉选择，支持 ETag 缓存）
     */
    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});

        std::string protocol = req->getParameter("protocol");
        if (protocol.empty()) {
            co_return Response::badRequest("协议类型不能为空");
        }

        // 使用 "protocol" 作为 ETag key，任何协议配置变更都会失效
        if (auto notModified = ETagUtils::checkETag(req, "protocol")) {
            co_return notModified;
        }

        auto data = co_await service_.options(protocol);
        Json::Value result;
        result["list"] = data;

        auto resp = Response::ok(result);
        ETagUtils::addETag(resp, "protocol");
        co_return resp;
    }
};
