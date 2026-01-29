#pragma once

#include <drogon/HttpFilter.h>
#include <chrono>

using namespace drogon;

/**
 * @brief 请求日志过滤器 - 记录HTTP请求的详细信息
 * @details 在请求前记录开始时间和请求体，在请求后记录耗时和响应
 */
class RequestLogFilter : public HttpFilter<RequestLogFilter> {
public:
    void doFilter(const HttpRequestPtr& req,
                  FilterCallback&& fcb,
                  FilterChainCallback&& fccb) override {
        // 记录请求开始时间
        req->attributes()->insert("startTime", std::chrono::steady_clock::now());

        // 保存请求体用于日志（非GET请求且有body）
        if (req->method() != Get && !req->body().empty()) {
            std::string body = std::string(req->body());
            if (body.length() > 1000) {
                body = body.substr(0, 1000) + "...(truncated)";
            }
            req->attributes()->insert("requestBody", body);
        }

        // 继续处理请求
        fccb();

        // 在请求处理完成后记录日志
        logRequest(req);
    }

private:
    /**
     * @brief 记录请求日志
     */
    void logRequest(const HttpRequestPtr& req) {
        // 获取用户名（已登录用户）
        std::string username = "-";
        try {
            username = req->attributes()->get<std::string>("username");
        } catch (...) {}

        // 计算耗时
        std::string duration = "-";
        try {
            auto startTime = req->attributes()->get<std::chrono::steady_clock::time_point>("startTime");
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            duration = std::to_string(elapsed) + "ms";
        } catch (...) {}

        // 获取请求体（非GET请求）
        std::string body;
        try {
            body = req->attributes()->get<std::string>("requestBody");
        } catch (...) {}

        // 日志格式: [用户] 方法 路径 (耗时) body
        LOG_DEBUG << "[" << username << "] "
                  << req->methodString() << " " << req->path()
                  << (req->query().empty() ? "" : "?" + req->query())
                  << " (" << duration << ")"
                  << (body.empty() ? "" : " " + body);
    }
};
