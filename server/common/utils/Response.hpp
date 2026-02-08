#pragma once

#include "ErrorCodes.hpp"

/**
 * @brief 统一响应格式工具类
 */
class Response {
public:
    using HttpResponsePtr = drogon::HttpResponsePtr;
    using HttpResponse = drogon::HttpResponse;
    using HttpStatusCode = drogon::HttpStatusCode;
    using enum drogon::HttpStatusCode;

    /** 直接返回预构建的 JSON 字符串（零 Json::Value 开销） */
    static HttpResponsePtr rawJson(std::string body) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(std::move(body));
        return resp;
    }

    static HttpResponsePtr ok(const Json::Value &data = Json::Value::null,
                               const std::string &message = "Success") {
        Json::Value json;
        json["code"] = 0;
        json["message"] = message;
        if (!data.isNull()) {
            json["data"] = data;
        }

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(k200OK);
        return resp;
    }

    static HttpResponsePtr page(const Json::Value &items,
                                  int total,
                                  int page,
                                  int pageSize) {
        if (pageSize < 1) pageSize = 1;
        Json::Value data;
        data["list"] = items;
        data["total"] = total;
        data["page"] = page;
        data["pageSize"] = pageSize;
        data["totalPages"] = (total + pageSize - 1) / pageSize;

        Json::Value json;
        json["code"] = 0;
        json["message"] = "Success";
        json["data"] = data;

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(k200OK);
        return resp;
    }

    static HttpResponsePtr created(const std::string &message = "创建成功") {
        Json::Value json;
        json["code"] = 0;
        json["message"] = message;

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(k201Created);
        return resp;
    }

    static HttpResponsePtr updated(const std::string &message = "更新成功") {
        Json::Value json;
        json["code"] = 0;
        json["message"] = message;

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(k200OK);
        return resp;
    }

    static HttpResponsePtr deleted(const std::string &message = "删除成功") {
        Json::Value json;
        json["code"] = 0;
        json["message"] = message;

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(k200OK);
        return resp;
    }

    static HttpResponsePtr error(int code,
                                   const std::string &message,
                                   HttpStatusCode status = k400BadRequest) {
        Json::Value json;
        json["code"] = code;
        json["message"] = message;

        auto resp = HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(status);
        return resp;
    }

    static HttpResponsePtr unauthorized(const std::string &message = "未授权访问") {
        return error(ErrorCodes::UNAUTHORIZED, message, k401Unauthorized);
    }

    static HttpResponsePtr forbidden(const std::string &message = "无权限访问") {
        return error(ErrorCodes::FORBIDDEN, message, k403Forbidden);
    }

    static HttpResponsePtr notFound(const std::string &message = "资源不存在") {
        return error(ErrorCodes::NOT_FOUND, message, k404NotFound);
    }

    static HttpResponsePtr badRequest(const std::string &message = "请求参数错误") {
        return error(ErrorCodes::BAD_REQUEST, message, k400BadRequest);
    }

    static HttpResponsePtr internalError(const std::string &message = "服务器内部错误") {
        return error(ErrorCodes::INTERNAL_ERROR, message, k500InternalServerError);
    }

    static HttpResponsePtr conflict(const std::string &message = "数据冲突") {
        return error(ErrorCodes::CONFLICT, message, k409Conflict);
    }
};
