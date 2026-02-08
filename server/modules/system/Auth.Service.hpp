#pragma once

#include "common/utils/JwtUtils.hpp"
#include "common/utils/PasswordUtils.hpp"
#include "common/utils/AppException.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/cache/AuthCache.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/Constants.hpp"

/**
 * @brief 认证服务
 *
 * 封装登录、登出、刷新令牌、用户信息查询等业务逻辑。
 * Controller 只负责 HTTP 参数校验和响应构建。
 */
class AuthService {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    AuthService() {
        auto config = drogon::app().getCustomConfig();

        std::string secret = config["jwt"]["secret"].asString();
        int accessExpiresIn = config["jwt"]["access_token_expires_in"].asInt();
        jwtUtils_ = std::make_shared<JwtUtils>(secret, accessExpiresIn);

        std::string refreshSecret = config["jwt"]["refresh_token_secret"].asString();
        int refreshExpiresIn = config["jwt"]["refresh_token_expires_in"].asInt();
        refreshJwtUtils_ = std::make_shared<JwtUtils>(refreshSecret, refreshExpiresIn);
    }

    /**
     * @brief 用户登录
     * @return {token, refreshToken, user}
     */
    Task<Json::Value> login(const std::string& username, const std::string& password) {
        // 检查登录失败次数
        auto failureCount = co_await authCache_.getLoginFailureCount(username);
        if (failureCount >= Constants::LOGIN_MAX_FAILURE_COUNT) {
            throw AppException(429, "登录失败次数过多，请15分钟后再试", drogon::k429TooManyRequests);
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

        co_return data;
    }

    /**
     * @brief 刷新令牌
     * @return {token, refreshToken}
     */
    Task<Json::Value> refresh(const std::string& refreshToken) {
        Json::Value payload = refreshJwtUtils_->verify(refreshToken);
        int userId = payload["userId"].asInt();
        std::string username = payload["username"].asString();

        // 检查刷新令牌是否在黑名单中（用户登出后阻止刷新）
        if (co_await authCache_.isTokenBlacklisted(refreshToken)) {
            throw AuthException::TokenInvalid();
        }

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

        // Refresh Token 轮转：旧 refresh token 进入黑名单，防止重放
        int64_t exp = payload["exp"].asInt64();
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        int64_t remainingTtl = exp - now;
        if (remainingTtl > 0 && remainingTtl <= INT_MAX) {
            co_await authCache_.blacklistToken(refreshToken, static_cast<int>(remainingTtl));
        }

        Json::Value data;
        data["token"] = newAccessToken;
        data["refreshToken"] = newRefreshToken;

        co_return data;
    }

    /**
     * @brief 用户登出
     */
    Task<void> logout(int userId, const std::string& accessToken, const std::string& refreshToken = "") {
        auto blacklistIfValid = [this, userId](
            const std::string& token,
            const std::shared_ptr<JwtUtils>& jwt,
            const std::string& tokenType
        ) -> Task<void> {
            if (token.empty()) {
                co_return;
            }

            try {
                Json::Value payload = jwt->verify(token);
                int tokenUserId = payload["userId"].asInt();
                if (tokenUserId != userId) {
                    LOG_WARN << "Skip blacklisting " << tokenType << " token: user mismatch";
                    co_return;
                }

                int64_t exp = payload["exp"].asInt64();
                int64_t now = static_cast<int64_t>(std::time(nullptr));
                int64_t remainingTtl = exp - now;

                if (remainingTtl > 0 && remainingTtl <= INT_MAX) {
                    co_await authCache_.blacklistToken(token, static_cast<int>(remainingTtl));
                    LOG_INFO << tokenType << " token blacklisted for userId: " << userId
                             << ", TTL: " << remainingTtl;
                }
            } catch (const std::exception&) {
                // Token 已失效或不合法，忽略
            }
        };

        co_await blacklistIfValid(accessToken, jwtUtils_, "access");
        co_await blacklistIfValid(refreshToken, refreshJwtUtils_, "refresh");

        // 清除用户会话缓存
        co_await authCache_.deleteUserSession(userId);
    }

    /**
     * @brief 获取用户信息（带缓存）
     */
    Task<Json::Value> getUserInfo(int userId, const std::string& username) {
        // 先尝试从缓存获取
        auto cached = co_await authCache_.getUserSession(userId);
        if (cached) {
            LOG_DEBUG << "User session cache hit for userId: " << userId;
            co_return *cached;
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

        co_return userInfo;
    }

private:
    std::shared_ptr<JwtUtils> jwtUtils_;
    std::shared_ptr<JwtUtils> refreshJwtUtils_;
    DatabaseService dbService_;
    AuthCache authCache_;

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

        co_await authCache_.cacheUserRoles(userId, roles);
        co_return roles;
    }

    Task<Json::Value> getAllMenus() {
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

        co_await authCache_.cacheAllMenus(menus);
        co_return menus;
    }

    Task<Json::Value> getUserMenus(int userId) {
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

        co_await authCache_.cacheUserMenus(userId, menus);
        co_return menus;
    }
};
