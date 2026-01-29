#pragma once

#include <drogon/HttpController.h>
#include "common/utils/Response.hpp"
#include "common/utils/JwtUtils.hpp"
#include "common/utils/PasswordUtils.hpp"
#include "common/utils/AppException.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/cache/AuthCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/Constants.hpp"

using namespace drogon;

/**
 * @brief 认证控制器
 */
class AuthController : public HttpController<AuthController> {
private:
    std::shared_ptr<JwtUtils> jwtUtils_;
    std::shared_ptr<JwtUtils> refreshJwtUtils_;
    DatabaseService dbService_;
    AuthCache authCache_;

public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::login, "/api/auth/login", Post);
    ADD_METHOD_TO(AuthController::refresh, "/api/auth/refresh", Post);
    ADD_METHOD_TO(AuthController::logout, "/api/auth/logout", Post, "AuthFilter");
    ADD_METHOD_TO(AuthController::getCurrentUser, "/api/auth/me", Get, "AuthFilter");
    METHOD_LIST_END

    AuthController() {
        auto config = app().getCustomConfig();

        std::string secret = config["jwt"]["secret"].asString();
        int accessExpiresIn = config["jwt"]["access_token_expires_in"].asInt();
        jwtUtils_ = std::make_shared<JwtUtils>(secret, accessExpiresIn);

        std::string refreshSecret = config["jwt"]["refresh_token_secret"].asString();
        int refreshExpiresIn = config["jwt"]["refresh_token_expires_in"].asInt();
        refreshJwtUtils_ = std::make_shared<JwtUtils>(refreshSecret, refreshExpiresIn);
    }

    Task<HttpResponsePtr> login(HttpRequestPtr req) {
        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        std::string username = (*json)["username"].asString();
        std::string password = (*json)["password"].asString();

        if (username.empty() || password.empty()) {
            co_return Response::badRequest("用户名和密码不能为空");
        }

        // 检查登录失败次数
        auto failureCount = co_await authCache_.getLoginFailureCount(username);
        if (failureCount >= Constants::LOGIN_MAX_FAILURE_COUNT) {
            co_return Response::error(429, "登录失败次数过多，请15分钟后再试", k429TooManyRequests);
        }

        std::string sql = R"(
            SELECT id, username, password_hash, nickname, status
            FROM sys_user
            WHERE username = ? AND deleted_at IS NULL
        )";

        auto result = co_await dbService_.execSqlCoro(sql, {username});

        if (result.empty()) {
            co_await authCache_.recordLoginFailure(username);
            throw AuthException::PasswordIncorrect();
        }

        auto row = result[0];
        int userId = FieldHelper::getInt(row["id"]);
        std::string passwordHash = FieldHelper::getString(row["password_hash"]);
        std::string nickname = FieldHelper::getString(row["nickname"], "");
        std::string status = FieldHelper::getString(row["status"]);

        if (!PasswordUtils::verifyPassword(password, passwordHash)) {
            co_await authCache_.recordLoginFailure(username);
            throw AuthException::PasswordIncorrect();
        }

        if (status == Constants::USER_STATUS_DISABLED) {
            throw AuthException::UserDisabled();
        }

        // 登录成功，清除失败记录
        co_await authCache_.clearLoginFailure(username);

        Json::Value tokenPayload;
        tokenPayload["userId"] = userId;
        tokenPayload["username"] = username;

        std::string accessToken = jwtUtils_->sign(tokenPayload);
        std::string refreshToken = refreshJwtUtils_->sign(tokenPayload);

        auto userInfo = co_await buildUserInfo(userId, username, nickname, status);

        // 缓存用户会话信息
        co_await authCache_.cacheUserSession(userId, userInfo);

        Json::Value data;
        data["token"] = accessToken;
        data["refreshToken"] = refreshToken;
        data["user"] = userInfo;

        co_return Response::ok(data, "登录成功");
    }

    Task<HttpResponsePtr> refresh(HttpRequestPtr req) {
        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        std::string refreshToken = (*json)["refreshToken"].asString();
        if (refreshToken.empty()) {
            co_return Response::badRequest("刷新令牌不能为空");
        }

        Json::Value payload = refreshJwtUtils_->verify(refreshToken);
        int userId = payload["userId"].asInt();
        std::string username = payload["username"].asString();

        std::string sql = R"(
            SELECT id, status
            FROM sys_user
            WHERE id = ? AND deleted_at IS NULL
        )";

        auto result = co_await dbService_.execSqlCoro(sql, {std::to_string(userId)});

        if (result.empty()) {
            throw AuthException::UserNotFound();
        }

        std::string status = FieldHelper::getString(result[0]["status"]);
        if (status == Constants::USER_STATUS_DISABLED) {
            throw AuthException::UserDisabled();
        }

        Json::Value tokenPayload;
        tokenPayload["userId"] = userId;
        tokenPayload["username"] = username;

        std::string newAccessToken = jwtUtils_->sign(tokenPayload);
        std::string newRefreshToken = refreshJwtUtils_->sign(tokenPayload);

        Json::Value data;
        data["token"] = newAccessToken;
        data["refreshToken"] = newRefreshToken;

        co_return Response::ok(data, "刷新成功");
    }

    Task<HttpResponsePtr> logout(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);

        // 从 Header 中提取 Token
        std::string authorization = req->getHeader("Authorization");
        std::string token;
        if (authorization.find("Bearer ") == 0) {
            token = authorization.substr(7);
        } else {
            co_return Response::badRequest("Token 格式错误");
        }

        // 验证 Token 并获取剩余有效期（使用 int64_t 避免溢出）
        try {
            Json::Value payload = jwtUtils_->verify(token);
            int64_t exp = payload["exp"].asInt64();
            int64_t now = static_cast<int64_t>(std::time(nullptr));
            int64_t remainingTtl = exp - now;

            if (remainingTtl > 0 && remainingTtl <= INT_MAX) {
                // 将 Token 加入黑名单（TTL 为剩余有效期）
                co_await authCache_.blacklistToken(token, static_cast<int>(remainingTtl));
                LOG_INFO << "Token blacklisted for userId: " << userId << ", TTL: " << remainingTtl;
            }
        } catch (const std::exception&) {
            // Token 已失效，忽略
        }

        // 清除用户会话缓存
        co_await authCache_.deleteUserSession(userId);

        co_return Response::ok("登出成功");
    }

    Task<HttpResponsePtr> getCurrentUser(HttpRequestPtr req) {
        auto attrs = req->attributes();
        if (!attrs->find("userId") || !attrs->find("username")) {
            co_return Response::unauthorized("未授权访问");
        }

        int userId = attrs->get<int>("userId");
        std::string username = attrs->get<std::string>("username");

        // 组合 ETag：用户特定版本 + 角色版本 + 菜单版本
        // 任何一个变化都会导致 ETag 失效
        auto& rv = ResourceVersion::instance();
        std::string userVersion = rv.getVersion("auth:user:" + std::to_string(userId));
        std::string roleVersion = rv.getVersion("role");
        std::string menuVersion = rv.getVersion("menu");
        std::string etag = "\"" + userVersion + "-" + roleVersion + "-" + menuVersion + "\"";

        std::string ifNoneMatch = req->getHeader("If-None-Match");
        if (!ifNoneMatch.empty() && ifNoneMatch == etag) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k304NotModified);
            co_return resp;
        }

        // 先尝试从缓存获取
        auto cached = co_await authCache_.getUserSession(userId);
        if (cached) {
            LOG_DEBUG << "User session cache hit for userId: " << userId;
            auto resp = Response::ok(*cached, "获取成功");
            resp->addHeader("ETag", etag);
            co_return resp;
        }

        LOG_DEBUG << "User session cache miss for userId: " << userId;

        std::string sql = R"(
            SELECT id, username, nickname, status
            FROM sys_user
            WHERE id = ? AND deleted_at IS NULL
        )";

        auto result = co_await dbService_.execSqlCoro(sql, {std::to_string(userId)});

        if (result.empty()) {
            throw AuthException::UserNotFound();
        }

        auto row = result[0];
        std::string nickname = FieldHelper::getString(row["nickname"], "");
        std::string status = FieldHelper::getString(row["status"]);

        if (status == Constants::USER_STATUS_DISABLED) {
            throw AuthException::UserDisabled();
        }

        auto userInfo = co_await buildUserInfo(userId, username, nickname, status);

        // 缓存结果
        co_await authCache_.cacheUserSession(userId, userInfo);

        auto resp = Response::ok(userInfo, "获取成功");
        resp->addHeader("ETag", etag);
        co_return resp;
    }

private:
    /**
     * @brief 从数据库行构建菜单 JSON 对象
     */
    static void buildMenuJson(const drogon::orm::Row& row, Json::Value& menu) {
        menu["id"] = FieldHelper::getInt(row["id"]);
        menu["name"] = FieldHelper::getString(row["name"]);
        menu["parent_id"] = FieldHelper::getInt(row["parent_id"]);
        menu["type"] = FieldHelper::getString(row["type"]);
        menu["path"] = FieldHelper::getString(row["path"], "");
        menu["component"] = FieldHelper::getString(row["component"], "");
        menu["permission_code"] = FieldHelper::getString(row["permission_code"], "");
        menu["icon"] = FieldHelper::getString(row["icon"], "");
        menu["sort_order"] = FieldHelper::getInt(row["sort_order"]);
        menu["visible"] = FieldHelper::getBool(row["visible"]);
    }

    Task<Json::Value> buildUserInfo(int userId, const std::string& username,
                                      const std::string& nickname, const std::string& status) {
        Json::Value userInfo;
        userInfo["id"] = userId;
        userInfo["username"] = username;
        userInfo["nickname"] = nickname;
        userInfo["status"] = status;

        auto roles = co_await getUserRoles(userId);
        userInfo["roles"] = roles;

        bool isSuperadmin = false;
        for (const auto& role : roles) {
            if (role["code"].asString() == Constants::ROLE_SUPERADMIN) {
                isSuperadmin = true;
                break;
            }
        }

        Json::Value menus;
        if (isSuperadmin) {
            menus = co_await getAllMenus();
        } else {
            menus = co_await getUserMenus(userId);
        }
        userInfo["menus"] = menus;

        co_return userInfo;
    }

    Task<Json::Value> getUserRoles(int userId) {
        // 先尝试从缓存获取
        auto cached = co_await authCache_.getUserRoles(userId);
        if (cached) {
            LOG_DEBUG << "User roles cache hit for userId: " << userId;
            co_return *cached;
        }

        LOG_DEBUG << "User roles cache miss for userId: " << userId;

        std::string sql = R"(
            SELECT r.id, r.name, r.code
            FROM sys_role r
            INNER JOIN sys_user_role ur ON r.id = ur.role_id
            WHERE ur.user_id = ? AND r.deleted_at IS NULL
        )";

        auto result = co_await dbService_.execSqlCoro(sql, {std::to_string(userId)});

        Json::Value roles(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value role;
            role["id"] = FieldHelper::getInt(row["id"]);
            role["name"] = FieldHelper::getString(row["name"]);
            role["code"] = FieldHelper::getString(row["code"]);
            roles.append(role);
        }

        // 缓存结果
        co_await authCache_.cacheUserRoles(userId, roles);

        co_return roles;
    }

    Task<Json::Value> getAllMenus() {
        // 先尝试从缓存获取
        auto cached = co_await authCache_.getAllMenus();
        if (cached) {
            LOG_DEBUG << "All menus cache hit";
            co_return *cached;
        }

        LOG_DEBUG << "All menus cache miss";

        std::string sql = R"(
            SELECT id, name, parent_id, type, path, component, permission_code,
                   icon, status, sort_order, 1 as visible
            FROM sys_menu
            WHERE status = 'enabled' AND deleted_at IS NULL
            ORDER BY sort_order ASC, id ASC
        )";

        auto result = co_await dbService_.execSqlCoro(sql);

        Json::Value menus(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value menu;
            buildMenuJson(row, menu);
            menus.append(menu);
        }

        // 缓存结果
        co_await authCache_.cacheAllMenus(menus);

        co_return menus;
    }

    Task<Json::Value> getUserMenus(int userId) {
        // 先尝试从缓存获取
        auto cached = co_await authCache_.getUserMenus(userId);
        if (cached) {
            LOG_DEBUG << "User menus cache hit for userId: " << userId;
            co_return *cached;
        }

        LOG_DEBUG << "User menus cache miss for userId: " << userId;

        std::string sql = R"(
            SELECT DISTINCT m.id, m.name, m.parent_id, m.type, m.path, m.component,
                   m.permission_code, m.icon, m.status, m.sort_order, 1 as visible
            FROM sys_menu m
            INNER JOIN sys_role_menu rm ON m.id = rm.menu_id
            INNER JOIN sys_user_role ur ON rm.role_id = ur.role_id
            INNER JOIN sys_role r ON ur.role_id = r.id
            WHERE ur.user_id = ?
              AND r.status = 'enabled' AND r.deleted_at IS NULL
              AND m.status = 'enabled' AND m.deleted_at IS NULL
            ORDER BY m.sort_order ASC, m.id ASC
        )";

        auto result = co_await dbService_.execSqlCoro(sql, {std::to_string(userId)});

        Json::Value menus(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value menu;
            buildMenuJson(row, menu);
            menus.append(menu);
        }

        // 缓存结果
        co_await authCache_.cacheUserMenus(userId, menus);

        co_return menus;
    }
};
