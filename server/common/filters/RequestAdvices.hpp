#pragma once

#include "../utils/Constants.hpp"

/**
 * @brief 请求/响应拦截器
 *
 * 注册全局的 pre-handling 和 post-handling advices，
 * 用于记录请求耗时和请求体日志
 */
class RequestAdvices {
public:
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    using enum drogon::HttpMethod;
    static void setup() {
        setupPreHandling();
        setupPostHandling();
    }

private:
    /** 请求前拦截：记录请求开始时间和请求体 */
    static void setupPreHandling() {
        drogon::app().registerPreHandlingAdvice([](const HttpRequestPtr &req) {
            req->attributes()->insert("startTime", std::chrono::steady_clock::now());

            if (req->method() != Get && !req->body().empty()) {
                std::string body = std::string(req->body());
                if (body.length() > Constants::REQUEST_LOG_MAX_LENGTH) {
                    body = body.substr(0, Constants::REQUEST_LOG_MAX_LENGTH) + "...(truncated)";
                }
                req->attributes()->insert("requestBody", body);
            }
        });
    }

    /** 请求后拦截：记录请求日志 + API 防缓存 */
    static void setupPostHandling() {
        drogon::app().registerPostHandlingAdvice([](const HttpRequestPtr &req, const HttpResponsePtr &resp) {
            // API 响应禁止 CDN/代理缓存（ETag 需浏览器每次回源验证）
            if (req->path().starts_with("/api/") && resp->getHeader("Cache-Control").empty()) {
                resp->addHeader("Cache-Control", "no-cache");
            }
            std::string username = "-";
            try {
                username = req->attributes()->get<std::string>("username");
            } catch (...) {}

            std::string duration = "-";
            try {
                auto startTime = req->attributes()->get<std::chrono::steady_clock::time_point>("startTime");
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                duration = std::to_string(elapsed) + "ms";
            } catch (...) {}

            std::string body;
            try {
                body = req->attributes()->get<std::string>("requestBody");
            } catch (...) {}

            LOG_DEBUG << "[" << username << "] "
                      << req->methodString() << " " << req->path()
                      << (req->query().empty() ? "" : "?" + req->query())
                      << " -> " << static_cast<int>(resp->statusCode())
                      << " (" << duration << ")"
                      << (body.empty() ? "" : " " + body);
        });
    }
};
