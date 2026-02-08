#pragma once

#include "AppException.hpp"
#include "ErrorCodes.hpp"

/**
 * @brief 全局异常处理器
 *
 * 将 AppException 转换为对应 HTTP 状态码的 JSON 响应，
 * 其他异常统一返回 500 错误
 */
class AppExceptionHandler {
public:
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    using HttpResponse = drogon::HttpResponse;
    using HttpStatusCode = drogon::HttpStatusCode;
    using enum drogon::HttpStatusCode;

    static void setup() {
        drogon::app().setExceptionHandler([](const std::exception& e,
                                       const HttpRequestPtr& /*req*/,
                                       std::function<void (const HttpResponsePtr &)> &&callback) {
            Json::Value json;
            HttpStatusCode status = k500InternalServerError;

            if (const auto* appEx = dynamic_cast<const AppException*>(&e)) {
                json["code"] = appEx->getCode();
                json["message"] = appEx->getMessage();
                status = appEx->getStatus();
            } else {
                LOG_ERROR << "Unhandled exception: " << e.what();
                json["code"] = ErrorCodes::INTERNAL_ERROR;
                json["message"] = "服务器内部错误";
            }

            json["status"] = static_cast<int>(status);
            auto resp = HttpResponse::newHttpJsonResponse(json);
            resp->setStatusCode(status);
            callback(resp);
        });
    }
};
