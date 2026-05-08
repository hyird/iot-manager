#pragma once

#include <boost/json.hpp>
#include <drogon/HttpResponse.h>

inline drogon::HttpResponsePtr jsonResponse(const boost::json::value& value) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(boost::json::serialize(value));
    return response;
}
