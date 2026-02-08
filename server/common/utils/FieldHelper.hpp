#pragma once

/**
 * @brief Field 值获取辅助类
 * 兼容 Drogon 新版 API
 */
class FieldHelper {
public:
    template<typename T>
    static T get(const drogon::orm::Field& field) {
        return field.as<T>();
    }

    template<typename T>
    static T get(const drogon::orm::Field& field, const T& defaultValue) {
        if (field.isNull()) {
            return defaultValue;
        }
        return field.as<T>();
    }

    static std::string getString(const drogon::orm::Field& field, const std::string& defaultValue = "") {
        if (field.isNull()) {
            return defaultValue;
        }
        return field.as<std::string>();
    }

    static int getInt(const drogon::orm::Field& field, int defaultValue = 0) {
        if (field.isNull()) {
            return defaultValue;
        }
        return field.as<int>();
    }

    static int64_t getInt64(const drogon::orm::Field& field, int64_t defaultValue = 0) {
        if (field.isNull()) {
            return defaultValue;
        }
        return field.as<int64_t>();
    }

    static bool getBool(const drogon::orm::Field& field, bool defaultValue = false) {
        if (field.isNull()) {
            return defaultValue;
        }
        return field.as<bool>();
    }

    static double getDouble(const drogon::orm::Field& field, double defaultValue = 0.0) {
        if (field.isNull()) {
            return defaultValue;
        }
        return field.as<double>();
    }
};
