#pragma once

#include <json/json.h>

#include <sstream>
#include <stdexcept>

/**
 * @brief JSON 序列化/反序列化辅助工具
 *
 * 统一 JSON 操作，避免重复的 StreamWriterBuilder 配置代码
 */
namespace JsonHelper {

/**
 * @brief 将 Json::Value 序列化为紧凑字符串（无换行、无缩进）
 */
inline std::string serialize(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    return Json::writeString(writer, value);
}

/**
 * @brief 将字符串反序列化为 Json::Value
 * @throws std::runtime_error 解析失败时抛出
 */
inline Json::Value parse(const std::string& jsonStr) {
    Json::CharReaderBuilder reader;
    Json::Value result;
    std::string errs;
    std::istringstream iss(jsonStr);
    if (!Json::parseFromStream(reader, iss, &result, &errs)) {
        throw std::runtime_error("JSON parse error: " + errs);
    }
    return result;
}

}  // namespace JsonHelper
