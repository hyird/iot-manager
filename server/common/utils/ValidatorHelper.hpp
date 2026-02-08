#pragma once

#include "AppException.hpp"

/**
 * @brief 参数校验和解析工具类
 *
 * 提供安全的参数解析和统一的 JSON 校验功能
 */
class ValidatorHelper {
public:
    using HttpRequestPtr = drogon::HttpRequestPtr;

    // ==================== 安全参数解析 ====================

    /**
     * @brief 安全解析整数参数
     * @param value 字符串值
     * @param defaultValue 解析失败时的默认值
     * @return 解析后的整数
     */
    static int parseInt(const std::string& value, int defaultValue = 0) {
        if (value.empty()) return defaultValue;
        try {
            return std::stoi(value);
        } catch (...) {
            return defaultValue;
        }
    }

    /**
     * @brief 安全解析整数参数，失败时返回空
     * @param value 字符串值
     * @return 解析后的整数，失败返回 std::nullopt
     */
    static std::optional<int> tryParseInt(const std::string& value) {
        if (value.empty()) return std::nullopt;
        try {
            return std::stoi(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    /**
     * @brief 安全解析 int64 参数
     * @param value 字符串值
     * @param defaultValue 解析失败时的默认值
     * @return 解析后的 int64
     */
    static int64_t parseInt64(const std::string& value, int64_t defaultValue = 0) {
        if (value.empty()) return defaultValue;
        try {
            return std::stoll(value);
        } catch (...) {
            return defaultValue;
        }
    }

    /**
     * @brief 安全解析浮点数参数
     * @param value 字符串值
     * @param defaultValue 解析失败时的默认值
     * @return 解析后的浮点数
     */
    static double parseDouble(const std::string& value, double defaultValue = 0.0) {
        if (value.empty()) return defaultValue;
        try {
            return std::stod(value);
        } catch (...) {
            return defaultValue;
        }
    }

    /**
     * @brief 从请求中安全获取整数参数
     * @param req HTTP 请求
     * @param name 参数名
     * @param defaultValue 默认值
     * @return 解析后的整数
     */
    static int getIntParam(const HttpRequestPtr& req, const std::string& name, int defaultValue = 0) {
        return parseInt(req->getParameter(name), defaultValue);
    }

    /**
     * @brief 从请求中安全获取 int64 参数
     */
    static int64_t getInt64Param(const HttpRequestPtr& req, const std::string& name, int64_t defaultValue = 0) {
        return parseInt64(req->getParameter(name), defaultValue);
    }

    // ==================== JSON 校验 ====================

    /**
     * @brief 校验 JSON 字段是否为非空字符串
     * @param json JSON 对象
     * @param field 字段名
     * @return 是否有效
     */
    static bool hasNonEmptyString(const Json::Value& json, const std::string& field) {
        return json.isMember(field) && !json[field].asString().empty();
    }

    /**
     * @brief 校验 JSON 字段是否为正整数
     * @param json JSON 对象
     * @param field 字段名
     * @return 是否有效
     */
    static bool hasPositiveInt(const Json::Value& json, const std::string& field) {
        return json.isMember(field) && json[field].asInt() > 0;
    }

    /**
     * @brief 校验 JSON 字段是否为非负整数
     */
    static bool hasNonNegativeInt(const Json::Value& json, const std::string& field) {
        return json.isMember(field) && json[field].asInt() >= 0;
    }

    /**
     * @brief 校验字符串长度是否满足最小要求
     * @param json JSON 对象
     * @param field 字段名
     * @param minLength 最小长度
     * @return 是否有效
     */
    static bool hasMinLength(const Json::Value& json, const std::string& field, size_t minLength) {
        if (!json.isMember(field)) return false;
        return json[field].asString().length() >= minLength;
    }

    /**
     * @brief 校验字符串值是否在允许的列表中
     * @param json JSON 对象
     * @param field 字段名
     * @param allowedValues 允许的值列表
     * @return 是否有效
     */
    static bool isInList(const Json::Value& json, const std::string& field,
                         const std::vector<std::string>& allowedValues) {
        if (!json.isMember(field)) return false;
        std::string value = json[field].asString();
        for (const auto& allowed : allowedValues) {
            if (value == allowed) return true;
        }
        return false;
    }

    /**
     * @brief 校验字符串值是否在允许的列表中（如果字段存在）
     * @param json JSON 对象
     * @param field 字段名
     * @param allowedValues 允许的值列表
     * @return 字段不存在返回 true，存在则检查是否在列表中
     */
    static bool isInListIfPresent(const Json::Value& json, const std::string& field,
                                   const std::vector<std::string>& allowedValues) {
        if (!json.isMember(field)) return true;
        return isInList(json, field, allowedValues);
    }

    // ==================== 批量校验（返回错误消息） ====================

    /**
     * @brief 校验结果
     */
    struct ValidationResult {
        bool valid = true;
        std::string errorMessage;

        operator bool() const { return valid; }

        static ValidationResult ok() {
            return {true, ""};
        }

        static ValidationResult fail(const std::string& message) {
            return {false, message};
        }

        /**
         * @brief 如果校验失败则抛出 ValidationException
         */
        void throwIfInvalid() const {
            if (!valid) {
                throw ValidationException(errorMessage);
            }
        }
    };

    /**
     * @brief 校验必填的非空字符串字段
     */
    static ValidationResult requireNonEmptyString(const Json::Value& json,
                                                   const std::string& field,
                                                   const std::string& fieldName) {
        if (!hasNonEmptyString(json, field)) {
            return ValidationResult::fail(fieldName + "不能为空");
        }
        return ValidationResult::ok();
    }

    /**
     * @brief 校验必填的正整数字段
     */
    static ValidationResult requirePositiveInt(const Json::Value& json,
                                                const std::string& field,
                                                const std::string& fieldName) {
        if (!hasPositiveInt(json, field)) {
            return ValidationResult::fail("请选择" + fieldName);
        }
        return ValidationResult::ok();
    }

    /**
     * @brief 校验字符串最小长度
     */
    static ValidationResult requireMinLength(const Json::Value& json,
                                              const std::string& field,
                                              size_t minLength,
                                              const std::string& fieldName) {
        if (json.isMember(field) && !json[field].asString().empty()) {
            if (json[field].asString().length() < minLength) {
                return ValidationResult::fail(fieldName + "长度不能小于" + std::to_string(minLength) + "位");
            }
        }
        return ValidationResult::ok();
    }

    /**
     * @brief 校验枚举值
     */
    static ValidationResult requireInList(const Json::Value& json,
                                           const std::string& field,
                                           const std::vector<std::string>& allowedValues,
                                           const std::string& fieldName,
                                           const std::string& allowedDescription) {
        if (!isInList(json, field, allowedValues)) {
            return ValidationResult::fail(fieldName + "只能是 " + allowedDescription);
        }
        return ValidationResult::ok();
    }

    /**
     * @brief 校验枚举值（如果字段存在）
     */
    static ValidationResult requireInListIfPresent(const Json::Value& json,
                                                    const std::string& field,
                                                    const std::vector<std::string>& allowedValues,
                                                    const std::string& fieldName,
                                                    const std::string& allowedDescription) {
        if (!isInListIfPresent(json, field, allowedValues)) {
            return ValidationResult::fail(fieldName + "只能是 " + allowedDescription);
        }
        return ValidationResult::ok();
    }
};
