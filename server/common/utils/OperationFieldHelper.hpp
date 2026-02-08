#pragma once

/**
 * @brief 统一处理对外 operation 字段与协议层 legacy 字段之间的兼容映射
 */
class OperationFieldHelper {
public:
    static constexpr const char* FIELD_OPERATION_KEY = "operationKey";
    static constexpr const char* FIELD_OPERATION_NAME = "operationName";
    static constexpr const char* LEGACY_FIELD_FUNC_CODE = "funcCode";
    static constexpr const char* LEGACY_FIELD_FUNC_NAME = "funcName";

    static std::string getOperationKey(const drogon::HttpRequestPtr& req) {
        std::string operationKey = req->getParameter(FIELD_OPERATION_KEY);
        if (operationKey.empty()) {
            operationKey = req->getParameter(LEGACY_FIELD_FUNC_CODE);
        }
        return operationKey;
    }

    static std::string getOperationKey(const Json::Value& json) {
        std::string operationKey = json.get(FIELD_OPERATION_KEY, "").asString();
        if (operationKey.empty()) {
            operationKey = json.get(LEGACY_FIELD_FUNC_CODE, "").asString();
        }
        return operationKey;
    }

    static std::string getOperationName(const Json::Value& json) {
        std::string operationName = json.get(FIELD_OPERATION_NAME, "").asString();
        if (operationName.empty()) {
            operationName = json.get(LEGACY_FIELD_FUNC_NAME, "").asString();
        }
        return operationName;
    }

    static void setOperationKey(Json::Value& value, const std::string& operationKey) {
        ensureObject(value);
        value.removeMember(LEGACY_FIELD_FUNC_CODE);
        if (operationKey.empty()) {
            value.removeMember(FIELD_OPERATION_KEY);
            return;
        }
        value[FIELD_OPERATION_KEY] = operationKey;
    }

    static void setOperationName(Json::Value& value, const std::string& operationName) {
        ensureObject(value);
        value.removeMember(LEGACY_FIELD_FUNC_NAME);
        if (operationName.empty()) {
            value.removeMember(FIELD_OPERATION_NAME);
            return;
        }
        value[FIELD_OPERATION_NAME] = operationName;
    }

    static void copyOperationFields(Json::Value& target, const Json::Value& source) {
        if (!source.isObject()) return;

        if (source.isMember(FIELD_OPERATION_KEY) || source.isMember(LEGACY_FIELD_FUNC_CODE)) {
            setOperationKey(target, getOperationKey(source));
        }
        if (source.isMember(FIELD_OPERATION_NAME) || source.isMember(LEGACY_FIELD_FUNC_NAME)) {
            setOperationName(target, getOperationName(source));
        }
    }

    static void normalizeOperationFields(Json::Value& value, bool recursive = false) {
        value = normalizedOperationFields(value, recursive);
    }

    static Json::Value normalizedOperationFields(const Json::Value& value, bool recursive = true) {
        if (value.isArray()) {
            Json::Value normalized(Json::arrayValue);
            for (const auto& item : value) {
                normalized.append(
                    recursive ? normalizedOperationFields(item, true) : item
                );
            }
            return normalized;
        }

        if (!value.isObject()) {
            return value;
        }

        Json::Value normalized(Json::objectValue);
        for (const auto& memberName : value.getMemberNames()) {
            const bool isLegacyOperationKey = memberName == LEGACY_FIELD_FUNC_CODE;
            const bool isLegacyOperationName = memberName == LEGACY_FIELD_FUNC_NAME;
            std::string normalizedName = memberName;
            if (isLegacyOperationKey) {
                normalizedName = FIELD_OPERATION_KEY;
            } else if (isLegacyOperationName) {
                normalizedName = FIELD_OPERATION_NAME;
            }

            if ((isLegacyOperationKey || isLegacyOperationName)
                && value.isMember(normalizedName)) {
                continue;
            }

            normalized[normalizedName] = recursive
                ? normalizedOperationFields(value[memberName], true)
                : value[memberName];
        }
        return normalized;
    }

private:
    static void ensureObject(Json::Value& value) {
        if (!value.isObject()) {
            value = Json::Value(Json::objectValue);
        }
    }
};
