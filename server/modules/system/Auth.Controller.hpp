#pragma once

#include "Auth.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/cache/ResourceVersion.hpp"

/**
 * @brief 认证控制器
 *
 * 只负责 HTTP 参数校验和响应构建，业务逻辑委托给 AuthService。
 */
class AuthController : public drogon::HttpController<AuthController> {
private:
    AuthService authService_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::login, "/api/auth/login", Post);
    ADD_METHOD_TO(AuthController::refresh, "/api/auth/refresh", Post);
    ADD_METHOD_TO(AuthController::logout, "/api/auth/logout", Post, "AuthFilter");
    ADD_METHOD_TO(AuthController::getCurrentUser, "/api/auth/me", Get, "AuthFilter");
    METHOD_LIST_END

    Task<HttpResponsePtr> login(HttpRequestPtr req) {
        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        std::string username = (*json)["username"].asString();
        std::string password = (*json)["password"].asString();

        if (username.empty() || password.empty()) {
            co_return Response::badRequest("用户名和密码不能为空");
        }

        auto data = co_await authService_.login(username, password);
        co_return Response::ok(data, "登录成功");
    }

    Task<HttpResponsePtr> refresh(HttpRequestPtr req) {
        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        std::string refreshToken = (*json)["refreshToken"].asString();
        if (refreshToken.empty()) {
            co_return Response::badRequest("刷新令牌不能为空");
        }

        auto data = co_await authService_.refresh(refreshToken);
        co_return Response::ok(data, "刷新成功");
    }

    Task<HttpResponsePtr> logout(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);

        // 从 Header 中提取 Token
        std::string authorization = req->getHeader("Authorization");
        if (authorization.size() <= 7 || authorization.find("Bearer ") != 0) {
            co_return Response::badRequest("Token 格式错误");
        }
        std::string token = authorization.substr(7);

        std::string refreshToken;
        if (auto json = req->getJsonObject(); json && (*json).isMember("refreshToken")) {
            refreshToken = (*json)["refreshToken"].asString();
        }

        co_await authService_.logout(userId, token, refreshToken);
        co_return Response::ok("登出成功");
    }

    Task<HttpResponsePtr> getCurrentUser(HttpRequestPtr req) {
        auto attrs = req->attributes();
        if (!attrs->find("userId") || !attrs->find("username")) {
            co_return Response::unauthorized("未授权访问");
        }

        int userId = attrs->get<int>("userId");
        std::string username = attrs->get<std::string>("username");

        // ETag 检查（HTTP 缓存层，属于 Controller 职责）
        auto& rv = ResourceVersion::instance();
        std::string userVersion = rv.getVersion("auth:user:" + std::to_string(userId));
        std::string roleVersion = rv.getVersion("role");
        std::string menuVersion = rv.getVersion("menu");
        std::string etag = "\"" + userVersion + "-" + roleVersion + "-" + menuVersion + "\"";

        std::string ifNoneMatch = req->getHeader("If-None-Match");
        if (!ifNoneMatch.empty() && ifNoneMatch == etag) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k304NotModified);
            co_return resp;
        }

        auto userInfo = co_await authService_.getUserInfo(userId, username);

        auto resp = Response::ok(userInfo, "获取成功");
        resp->addHeader("ETag", etag);
        co_return resp;
    }
};
