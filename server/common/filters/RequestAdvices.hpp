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
    static bool isSensitiveField(const std::string& key) {
        std::string lower;
        lower.reserve(key.size());
        for (char c : key) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        return lower.find("token") != std::string::npos ||
               lower.find("password") != std::string::npos ||
               lower.find("passwd") != std::string::npos ||
               lower.find("secret") != std::string::npos ||
               lower.find("authorization") != std::string::npos;
    }

    static std::string truncateForLog(const std::string& value) {
        if (value.length() <= Constants::REQUEST_LOG_MAX_LENGTH) {
            return value;
        }
        return value.substr(0, Constants::REQUEST_LOG_MAX_LENGTH) + "...(truncated)";
    }

    static void redactJsonValue(Json::Value& value) {
        if (value.isObject()) {
            for (const auto& name : value.getMemberNames()) {
                if (isSensitiveField(name)) {
                    value[name] = "[REDACTED]";
                } else {
                    redactJsonValue(value[name]);
                }
            }
            return;
        }

        if (value.isArray()) {
            for (auto& item : value) {
                redactJsonValue(item);
            }
        }
    }

    static std::string sanitizeBodyForLog(const std::string& body) {
        if (body.empty()) return "";

        Json::CharReaderBuilder builder;
        Json::Value parsed;
        std::string errs;
        std::istringstream stream(body);

        if (Json::parseFromStream(builder, stream, &parsed, &errs)) {
            redactJsonValue(parsed);
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            return truncateForLog(Json::writeString(writer, parsed));
        }

        return "[non-json-body length=" + std::to_string(body.size()) + "]";
    }

    static std::string sanitizeQueryForLog(const std::string& query) {
        if (query.empty()) return "";

        std::stringstream ss(query);
        std::ostringstream out;
        std::string pair;
        bool first = true;

        while (std::getline(ss, pair, '&')) {
            auto eqPos = pair.find('=');
            std::string key = eqPos == std::string::npos ? pair : pair.substr(0, eqPos);
            std::string value = eqPos == std::string::npos ? "" : pair.substr(eqPos + 1);

            if (isSensitiveField(key)) {
                value = "[REDACTED]";
            } else {
                value = truncateForLog(value);
            }

            if (!first) out << "&";
            first = false;
            out << key;
            if (eqPos != std::string::npos) {
                out << "=" << value;
            }
        }

        return out.str();
    }

    /** 请求前拦截：记录请求开始时间和请求体 */
    static void setupPreHandling() {
        drogon::app().registerPreHandlingAdvice([](const HttpRequestPtr &req) {
            req->attributes()->insert("startTime", std::chrono::steady_clock::now());

            if (req->method() != Get && !req->body().empty()) {
                req->attributes()->insert("requestBody", sanitizeBodyForLog(std::string(req->body())));
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

            std::string query = sanitizeQueryForLog(req->query());

            LOG_DEBUG << "[" << username << "] "
                      << req->methodString() << " " << req->path()
                      << (query.empty() ? "" : "?" + query)
                      << " -> " << static_cast<int>(resp->statusCode())
                      << " (" << duration << ")"
                      << (body.empty() ? "" : " " + body);
        });
    }
};
