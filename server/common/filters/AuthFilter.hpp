#pragma once

#include "common/utils/JwtUtils.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/StringUtils.hpp"
#include "common/cache/AuthCache.hpp"

/**
 * @brief JWT 认证过滤器
 */
class AuthFilter : public drogon::HttpFilter<AuthFilter> {
private:
    std::shared_ptr<JwtUtils> jwtUtils_;
    AuthCache authCache_;

public:
    template<typename T = void> using Task = drogon::Task<T>;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    using FilterCallback = drogon::FilterCallback;
    using FilterChainCallback = drogon::FilterChainCallback;

    AuthFilter() {
        auto config = drogon::app().getCustomConfig();
        std::string secret = config["jwt"]["secret"].asString();
        int expiresIn = config["jwt"]["access_token_expires_in"].asInt();
        jwtUtils_ = std::make_shared<JwtUtils>(secret, expiresIn);
    }

    void doFilter(const HttpRequestPtr& req,
                   FilterCallback&& fcb,
                   FilterChainCallback&& fccb) override {
        auto authHeader = req->getHeader("Authorization");

        if (authHeader.empty()) {
            fcb(Response::unauthorized("缺少认证令牌"));
            return;
        }

        if (!StringUtils::startsWith(authHeader, "Bearer ")) {
            fcb(Response::unauthorized("令牌格式错误"));
            return;
        }

        std::string token = authHeader.substr(7);

        // 使用协程检查 Token 黑名单
        drogon::async_run([this, token, req, fcb = std::move(fcb), fccb = std::move(fccb)]() mutable -> Task<void> {
            try {
                // 检查 Token 是否在黑名单中
                bool isBlacklisted = co_await authCache_.isTokenBlacklisted(token);
                if (isBlacklisted) {
                    LOG_INFO << "Token is blacklisted: " << token.substr(0, 20) << "...";
                    fcb(Response::unauthorized("令牌已失效，请重新登录"));
                    co_return;
                }

                // 验证 Token
                Json::Value payload = jwtUtils_->verify(token);

                req->attributes()->insert("userId", payload["userId"].asInt());
                req->attributes()->insert("username", payload["username"].asString());

                fccb();

            } catch (const AppException& e) {
                fcb(Response::error(e.getCode(), e.getMessage(), e.getStatus()));
            } catch (const std::exception&) {
                fcb(Response::unauthorized("令牌验证失败"));
            }
        });
    }
};
