#pragma once

#include "Link.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

namespace {
    // 允许的链路模式列表
    const std::vector<std::string> ALLOWED_LINK_MODES = {
        Constants::LINK_MODE_TCP_SERVER,
        Constants::LINK_MODE_TCP_CLIENT
    };

    // 允许的链路协议类型列表
    const std::vector<std::string> ALLOWED_LINK_PROTOCOLS = {
        Constants::PROTOCOL_SL651,
        Constants::PROTOCOL_MODBUS,
        Constants::PROTOCOL_MODBUS_TCP,
        Constants::PROTOCOL_MODBUS_RTU
    };
}

/**
 * @brief 链路管理控制器
 */
class LinkController : public drogon::HttpController<LinkController> {
private:
    LinkService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LinkController::list, "/api/link", Get, "AuthFilter");
    ADD_METHOD_TO(LinkController::detail, "/api/link/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(LinkController::create, "/api/link", Post, "AuthFilter");
    ADD_METHOD_TO(LinkController::update, "/api/link/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(LinkController::remove, "/api/link/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(LinkController::options, "/api/link/options", Get, "AuthFilter");
    ADD_METHOD_TO(LinkController::enums, "/api/link/enums", Get, "AuthFilter");
    ADD_METHOD_TO(LinkController::publicIp, "/api/link/public-ip", Get, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取链路列表（支持参数化 ETag 缓存）
     */
    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});

        auto page = Pagination::fromRequest(req);
        std::string mode = req->getParameter("mode");

        // 参数化 ETag 检查
        std::string params = mode + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "link", params)) {
            co_return notModified;
        }

        auto result = co_await service_.list(page, mode);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "link", params);
        co_return resp;
    }

    /**
     * @brief 获取链路详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
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
        ValidatorHelper::requireNonEmptyString(*json, "protocol", "协议").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "ip", "IP地址").throwIfInvalid();
        ValidatorHelper::requirePositiveInt(*json, "port", "端口").throwIfInvalid();
        ValidatorHelper::requireInList(*json, "mode", ALLOWED_LINK_MODES,
            "模式", "TCP Server 或 TCP Client").throwIfInvalid();
        ValidatorHelper::requireInList(*json, "protocol", ALLOWED_LINK_PROTOCOLS,
            "协议", "SL651、Modbus、Modbus TCP 或 Modbus RTU").throwIfInvalid();

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    /**
     * @brief 更新链路
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireInListIfPresent(*json, "mode", ALLOWED_LINK_MODES,
            "模式", "TCP Server 或 TCP Client").throwIfInvalid();
        ValidatorHelper::requireInListIfPresent(*json, "protocol", ALLOWED_LINK_PROTOCOLS,
            "协议", "SL651、Modbus、Modbus TCP 或 Modbus RTU").throwIfInvalid();

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除链路
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
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

    /**
     * @brief 获取链路枚举值（模式和协议列表，支持 ETag 缓存）
     */
    Task<HttpResponsePtr> enums(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});

        // 静态数据，使用固定版本号的 ETag
        if (auto notModified = ETagUtils::checkETag(req, "link_enums")) {
            co_return notModified;
        }

        Json::Value result;

        // 模式列表
        Json::Value modes(Json::arrayValue);
        for (const auto& mode : ALLOWED_LINK_MODES) {
            modes.append(mode);
        }
        result["modes"] = modes;

        // 协议列表
        Json::Value protocols(Json::arrayValue);
        for (const auto& protocol : ALLOWED_LINK_PROTOCOLS) {
            protocols.append(protocol);
        }
        result["protocols"] = protocols;

        auto resp = Response::ok(result);
        ETagUtils::addETag(resp, "link_enums");
        co_return resp;
    }

    /**
     * @brief 获取服务器公网 IP（缓存 5 分钟）
     */
    Task<HttpResponsePtr> publicIp(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:link:query"});

        static std::mutex ipMutex;
        static std::string cachedIp;
        static std::chrono::steady_clock::time_point cacheTime;
        constexpr auto CACHE_TTL = std::chrono::minutes(5);

        {
            std::lock_guard lock(ipMutex);
            auto now = std::chrono::steady_clock::now();
            if (!cachedIp.empty() && (now - cacheTime) < CACHE_TTL) {
                Json::Value data;
                data["ip"] = cachedIp;
                co_return Response::ok(data);
            }
        }

        try {
            auto client = drogon::HttpClient::newHttpClient("http://ip.sb",
                drogon::app().getLoop());
            client->setUserAgent("curl/8.0");
            auto httpReq = drogon::HttpRequest::newHttpRequest();
            httpReq->setPath("/");

            auto httpResp = co_await client->sendRequestCoro(httpReq, 5.0);

            if (httpResp && httpResp->getStatusCode() == drogon::k200OK) {
                std::string ip = std::string(httpResp->getBody());
                ip.erase(0, ip.find_first_not_of(" \t\r\n"));
                ip.erase(ip.find_last_not_of(" \t\r\n") + 1);

                {
                    std::lock_guard lock(ipMutex);
                    cachedIp = ip;
                    cacheTime = std::chrono::steady_clock::now();
                }

                Json::Value data;
                data["ip"] = ip;
                co_return Response::ok(data);
            }

            co_return Response::internalError("获取公网 IP 失败");
        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to fetch public IP: " << e.what();
            co_return Response::internalError("获取公网 IP 失败");
        }
    }
};
