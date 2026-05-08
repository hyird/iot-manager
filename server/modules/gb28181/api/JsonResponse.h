#pragma once

#include "common/utils/ErrorCodes.hpp"

#include <boost/json.hpp>
#include <drogon/HttpResponse.h>

inline drogon::HttpResponsePtr jsonBody(const boost::json::value& value, drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(boost::json::serialize(value));
    return response;
}

inline drogon::HttpResponsePtr jsonResponse(const boost::json::value& data, const std::string& message = "Success") {
    boost::json::object envelope;
    envelope["code"] = ErrorCodes::SUCCESS;
    envelope["message"] = message;
    envelope["data"] = data;
    return jsonBody(envelope);
}

inline drogon::HttpResponsePtr jsonError(
    const std::string& message,
    drogon::HttpStatusCode status = drogon::k400BadRequest,
    int code = ErrorCodes::BAD_REQUEST
) {
    boost::json::object envelope;
    envelope["code"] = code;
    envelope["message"] = message;
    envelope["status"] = static_cast<int>(status);
    return jsonBody(envelope, status);
}

inline drogon::HttpResponsePtr jsonBadRequest(const std::string& message) {
    return jsonError(message, drogon::k400BadRequest, ErrorCodes::BAD_REQUEST);
}

inline drogon::HttpResponsePtr jsonNotFound(const std::string& message) {
    return jsonError(message, drogon::k404NotFound, ErrorCodes::NOT_FOUND);
}
