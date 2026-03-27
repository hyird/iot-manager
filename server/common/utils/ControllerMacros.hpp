#pragma once

#include "AppException.hpp"

/**
 * @brief Controller 工具函数
 *
 * 提供 Controller 常用的辅助函数
 */
namespace ControllerUtils {

/**
 * @brief 从请求中获取当前用户 ID
 * @param req HTTP 请求
 * @return 用户 ID
 */
inline int getUserId(const drogon::HttpRequestPtr& req) {
    return req->attributes()->get<int>("userId");
}

/**
 * @brief 从请求中获取 JSON 请求体
 * @param req HTTP 请求
 * @return JSON 对象指针，无效时返回 nullptr
 */
inline std::shared_ptr<Json::Value> getJson(const drogon::HttpRequestPtr& req) {
    return req->getJsonObject();
}

/**
 * @brief 校验资源 ID 必须为正数
 */
inline void requirePositiveId(int id, const std::string& message = "无效的资源ID") {
    if (id <= 0) {
        throw ValidationException(message);
    }
}

/**
 * @brief 校验数值不能小于 0
 */
inline void requireNonNegativeValue(int value, const std::string& message) {
    if (value < 0) {
        throw ValidationException(message);
    }
}

/**
 * @brief 校验数值范围
 */
inline void requireInRange(int value, int minValue, int maxValue, const std::string& message) {
    if (value < minValue || value > maxValue) {
        throw ValidationException(message);
    }
}

/**
 * @brief 校验字符串不能为空
 */
inline void requireNonEmptyString(const std::string& value, const std::string& message) {
    if (value.empty()) {
        throw ValidationException(message);
    }
}

/**
 * @brief 校验设备编码 / 设备ID 至少提供一个
 */
inline void requireDeviceSelector(const std::string& deviceCode,
                                  int deviceId,
                                  const std::string& message = "设备编码或设备ID不能为空") {
    if (deviceCode.empty() && deviceId <= 0) {
        throw ValidationException(message);
    }
}

/**
 * @brief 校验必须提供时间范围
 */
inline void requireTimeRange(const std::string& startTime,
                             const std::string& endTime,
                             const std::string& message = "必须指定时间范围") {
    if (startTime.empty() || endTime.empty()) {
        throw ValidationException(message);
    }
}

/**
 * @brief 从请求中提取 JSON 请求体，缺失时抛出统一验证异常
 */
inline std::shared_ptr<Json::Value> requireJson(const drogon::HttpRequestPtr& req,
                                                const std::string& message = "请求体格式错误") {
    auto json = req->getJsonObject();
    if (!json) {
        throw ValidationException(message);
    }
    return json;
}

/**
 * @brief 校验 Authorization Bearer Token 格式并提取 Token
 */
inline std::string requireBearerToken(const drogon::HttpRequestPtr& req,
                                      const std::string& headerName = "Authorization",
                                      const std::string& message = "Token 格式错误") {
    std::string authorization = req->getHeader(headerName);
    if (authorization.size() <= 7 || authorization.find("Bearer ") != 0) {
        throw AuthException::TokenInvalid(message);
    }
    return authorization.substr(7);
}

/**
 * @brief 校验当前请求已通过认证过滤器注入用户信息
 */
inline void requireAuthAttributes(const drogon::HttpRequestPtr& req,
                                  const std::string& message = "未授权访问") {
    auto attrs = req->attributes();
    if (!attrs->find("userId") || !attrs->find("username")) {
        throw AuthException::TokenInvalid(message);
    }
}

/**
 * @brief 校验 JSON 字段是数组
 */
inline const Json::Value& requireArrayField(const Json::Value& json,
                                            const std::string& field,
                                            const std::string& message) {
    if (!json.isMember(field) || !json[field].isArray()) {
        throw ValidationException(message);
    }
    return json[field];
}

/**
 * @brief 校验 JSON 字段是非空数组
 */
inline const Json::Value& requireNonEmptyArrayField(const Json::Value& json,
                                                    const std::string& field,
                                                    const std::string& message) {
    const auto& array = requireArrayField(json, field, message);
    if (array.empty()) {
        throw ValidationException(message);
    }
    return array;
}

/**
 * @brief 校验设备指令要素数组结构
 */
inline const Json::Value& requireCommandElements(const Json::Value& json,
                                                 const std::string& field = "elements",
                                                 const std::string& message = "要素列表不能为空") {
    const auto& elements = requireNonEmptyArrayField(json, field, message);
    for (const auto& elem : elements) {
        if (!elem.isObject()) {
            throw ValidationException("要素格式错误");
        }
        if (elem.get("elementId", "").asString().empty()) {
            throw ValidationException("要素 elementId 不能为空");
        }
        if (elem.get("value", "").asString().empty()) {
            throw ValidationException("要素值不能为空");
        }
    }
    return elements;
}

/**
 * @brief 将 JSON 数组转换为 int 列表
 */
inline std::vector<int> toIntArray(const Json::Value& array) {
    std::vector<int> values;
    values.reserve(array.size());
    for (const auto& value : array) {
        values.push_back(value.asInt());
    }
    return values;
}

/**
 * @brief 将 JSON 数组转换为 int64 列表
 */
inline std::vector<int64_t> toInt64Array(const Json::Value& array) {
    std::vector<int64_t> values;
    values.reserve(array.size());
    for (const auto& value : array) {
        values.push_back(value.asInt64());
    }
    return values;
}

}  // namespace ControllerUtils
