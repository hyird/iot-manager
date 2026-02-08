#pragma once

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

}  // namespace ControllerUtils
