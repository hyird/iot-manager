#pragma once

#include "common/utils/AppException.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/ValidatorHelper.hpp"

#include <json/json.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ProtocolConfigValidator {

inline const std::vector<std::string>& supportedProtocols() {
    static const std::vector<std::string> protocols = {
        Constants::PROTOCOL_SL651,
        Constants::PROTOCOL_MODBUS,
        Constants::PROTOCOL_MODBUS_TCP,
        Constants::PROTOCOL_MODBUS_RTU,
        Constants::PROTOCOL_S7,
    };
    return protocols;
}

inline bool isSupportedProtocol(const std::string& protocol) {
    const auto& protocols = supportedProtocols();
    return std::find(protocols.begin(), protocols.end(), protocol) != protocols.end();
}

inline void requireSupportedProtocol(const std::string& protocol) {
    if (protocol.empty()) {
        throw ValidationException("协议类型不能为空");
    }
    if (!isSupportedProtocol(protocol)) {
        throw ValidationException("协议类型只能是 SL651、MODBUS、Modbus TCP、Modbus RTU 或 S7");
    }
}

inline std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

inline std::optional<std::string> normalizeTsap(const Json::Value& value);

struct S7ConnectionPreset {
    std::string mode = "RACK_SLOT";
    int rack = 0;
    int slot = 1;
    std::string localTSAP;
    std::string remoteTSAP;
};

inline std::optional<S7ConnectionPreset> resolveS7Preset(const std::string& model) {
    if (model == "S7-200") {
        return S7ConnectionPreset{.mode = "TSAP", .rack = 0, .slot = 1, .localTSAP = "4D57", .remoteTSAP = "4D57"};
    }
    if (model == "S7-300") {
        return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 2, .localTSAP = "", .remoteTSAP = ""};
    }
    if (model == "S7-400") {
        return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 3, .localTSAP = "", .remoteTSAP = ""};
    }
    if (model == "S7-1200" || model == "S7-1500") {
        return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 1, .localTSAP = "", .remoteTSAP = ""};
    }
    return std::nullopt;
}

inline std::string resolveS7ConnectionMode(
    const Json::Value& connection,
    const S7ConnectionPreset& preset
) {
    std::string mode = connection.get("mode", "").asString();
    if (!mode.empty()) {
        return toUpper(mode);
    }
    if (connection.isMember("localTSAP") || connection.isMember("remoteTSAP")) {
        return std::string("TSAP");
    }
    return preset.mode;
}

inline std::string resolveS7ConnectionType(const Json::Value& connection) {
    return toUpper(connection.get("connectionType", "PG").asString());
}

inline void validateSl651Element(const Json::Value& elem, bool isResponseElement) {
    if (!elem.isObject()) {
        return;
    }

    const std::string prefix = isResponseElement ? "应答要素" : "要素";
    const std::string elemName = elem.get("name", "未命名").asString();

    ValidatorHelper::requireStringField(
        elem,
        "guideHex",
        prefix + "「" + elemName + "」的引导符不能为空"
    );

    ValidatorHelper::requirePositiveIntField(
        elem,
        "length",
        prefix + "「" + elemName + "」的长度必须 >= 1"
    );

    if (!isResponseElement) {
        ValidatorHelper::optionalIntRangeField(
            elem,
            "digits",
            0,
            0,
            8,
            "要素「" + elemName + "」的小数位必须在 0-8 之间"
        );
    }
}

inline void validateSl651Func(const Json::Value& func) {
    if (!func.isObject()) {
        return;
    }

    ValidatorHelper::requireStringField(func, "funcCode", "功能码 funcCode 不能为空");

    if (const auto* elements = ValidatorHelper::optionalArrayField(func, "elements")) {
        for (const auto& elem : *elements) {
            validateSl651Element(elem, false);
        }
    }

    if (const auto* responseElements = ValidatorHelper::optionalArrayField(func, "responseElements")) {
        for (const auto& elem : *responseElements) {
            validateSl651Element(elem, true);
        }
    }
}

inline void validateModbusRegister(const Json::Value& reg) {
    if (!reg.isObject()) {
        return;
    }

    const std::string regName = reg.get("name", "未命名").asString();

    const int address = ValidatorHelper::optionalIntRangeField(
        reg,
        "address",
        0,
        0,
        65535,
        "寄存器「" + regName + "」的地址超出范围 (0-65535)"
    );

    const int quantity = ValidatorHelper::optionalIntRangeField(
        reg,
        "quantity",
        1,
        1,
        125,
        "寄存器「" + regName + "」的数量超出范围 (1-125)"
    );

    if (address + quantity - 1 > 65535) {
        throw ValidationException("寄存器「" + regName + "」的地址 + 数量超出范围（末地址不能超过 65535）");
    }
}

inline void validateS7Connection(
    const Json::Value& config,
    const S7ConnectionPreset& preset,
    bool isS7_200
) {
    const Json::Value* connection = ValidatorHelper::optionalObjectField(
        config,
        "connection",
        "S7 配置的 connection 必须是对象"
    );

    const Json::Value& conn = connection ? *connection : Json::Value::nullSingleton();
    const std::string connectionMode = resolveS7ConnectionMode(conn, preset);
    if (isS7_200 && connectionMode != "TSAP") {
        throw ValidationException("S7-200 仅支持 TSAP 连接模式");
    }
    if (connectionMode != "RACK_SLOT" && connectionMode != "TSAP") {
        throw ValidationException("S7 connection.mode 仅支持 RACK_SLOT 或 TSAP");
    }

    if (connectionMode == "RACK_SLOT") {
        ValidatorHelper::optionalNonNegativeIntField(
            conn,
            "rack",
            preset.rack,
            "S7 的 rack/slot 不能小于 0"
        );
        ValidatorHelper::optionalNonNegativeIntField(
            conn,
            "slot",
            preset.slot,
            "S7 的 rack/slot 不能小于 0"
        );
        const std::string connectionType = resolveS7ConnectionType(conn);
        if (connectionType != "PG" && connectionType != "OP" && connectionType != "S7_BASIC") {
            throw ValidationException("S7 connection.connectionType 仅支持 PG、OP、S7_BASIC");
        }
        return;
    }

    Json::Value localValue;
    Json::Value remoteValue;
    if (conn.isMember("localTSAP")) {
        localValue = conn["localTSAP"];
    } else if (!preset.localTSAP.empty()) {
        localValue = Json::Value(preset.localTSAP);
    }
    if (conn.isMember("remoteTSAP")) {
        remoteValue = conn["remoteTSAP"];
    } else if (!preset.remoteTSAP.empty()) {
        remoteValue = Json::Value(preset.remoteTSAP);
    }
    if (!normalizeTsap(localValue) || !normalizeTsap(remoteValue)) {
        throw ValidationException("S7 TSAP 模式下必须提供合法的 localTSAP / remoteTSAP（例如 4D57 或 0200）");
    }
}

inline void validateS7Area(const Json::Value& area, bool isS7_200) {
    if (!area.isObject()) {
        return;
    }

    ValidatorHelper::requireStringField(area, "id", "S7 配置的区域 id 不能为空");
    ValidatorHelper::requireStringField(area, "name", "S7 配置的区域名称不能为空");

    std::string areaType = toUpper(area.get("area", "").asString());
    std::string dataType = area.get("dataType", (areaType == "CT" || areaType == "TM") ? "UINT16" : "INT16").asString();
    const bool areaAllowed = isS7_200
        ? (areaType == "DB" || areaType == "V" || areaType == "MK" ||
           areaType == "PE" || areaType == "PA" || areaType == "CT" ||
           areaType == "TM")
        : (areaType == "DB" || areaType == "MK" || areaType == "PE" ||
           areaType == "PA" || areaType == "CT" || areaType == "TM");
    if (!areaAllowed) {
        if (isS7_200) {
            throw ValidationException("S7-200 区域 area 仅支持 DB、V、MK、PE、PA、CT、TM");
        }
        throw ValidationException("S7 区域 area 仅支持 DB、MK、PE、PA、CT、TM");
    }
    if ((areaType == "PE" || areaType == "PA") && dataType != "BOOL") {
        throw ValidationException("S7 区域 PE/PA 仅支持 BOOL 数据类型");
    }
    if ((areaType == "CT" || areaType == "TM") && dataType != "UINT16") {
        throw ValidationException("S7 区域 CT/TM 仅支持 UINT16 数据类型");
    }
    if (areaType == "DB") {
        ValidatorHelper::optionalNonNegativeIntField(
            area,
            "dbNumber",
            0,
            "S7 DB 区域的 dbNumber 不能小于 0"
        );
    }
    ValidatorHelper::requirePositiveIntField(area, "size", "S7 区域的 size 必须 >= 1");
    ValidatorHelper::optionalNonNegativeIntField(
        area,
        "start",
        0,
        "S7 区域的 start 不能小于 0"
    );
}

inline std::optional<std::string> normalizeTsap(const Json::Value& value) {
    if (value.isNull()) {
        return std::nullopt;
    }

    if (value.isInt() || value.isUInt()) {
        const unsigned int tsap = value.asUInt();
        if (tsap > 0xFFFF) {
            return std::nullopt;
        }
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << tsap;
        return oss.str();
    }

    if (!value.isString()) {
        return std::nullopt;
    }

    std::string text = value.asString();
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) || c == '.' || c == ':' || c == '-' || c == '_';
    }), text.end());
    text = toUpper(text);
    if (text.rfind("0X", 0) == 0) {
        text.erase(0, 2);
    }
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return std::nullopt;
    }

    std::ostringstream oss;
    oss << std::uppercase << std::setw(4) << std::setfill('0') << text;
    return oss.str();
}

inline void validateSl651(const Json::Value& config) {
    if (!config.isMember("funcs")) {
        return;
    }

    ValidatorHelper::requireArrayField(config, "funcs", "SL651 配置的 funcs 必须是数组");

    for (const auto& func : config["funcs"]) {
        validateSl651Func(func);
    }
}

inline void validateModbus(const Json::Value& config) {
    if (config.isMember("readInterval")) {
        ValidatorHelper::requireIntRangeField(
            config,
            "readInterval",
            1,
            3600,
            "读取间隔必须在 1-3600 秒之间"
        );
    }

    if (!config.isMember("registers")) {
        return;
    }

    ValidatorHelper::requireArrayField(config, "registers", "Modbus 配置的 registers 必须是数组");

    for (const auto& reg : config["registers"]) {
        validateModbusRegister(reg);
    }
}

inline void validateS7(const Json::Value& config) {
    ValidatorHelper::requireStringField(config, "deviceType", "S7 配置的 deviceType 不能为空");

    std::string plcModel = ValidatorHelper::requireStringField(config, "plcModel", "S7 配置的 plcModel 不能为空");
    auto preset = resolveS7Preset(plcModel);
    if (!preset) {
        throw ValidationException("S7 配置的 plcModel 仅支持 S7-200、S7-300、S7-400、S7-1200、S7-1500");
    }

    const bool isS7_200 = plcModel == "S7-200";
    validateS7Connection(config, *preset, isS7_200);

    if (!config.isMember("areas")) {
        return;
    }

    ValidatorHelper::requireArrayField(config, "areas", "S7 配置的 areas 必须是数组");

    for (const auto& area : config["areas"]) {
        validateS7Area(area, isS7_200);
    }
}

inline void validate(const std::string& protocol, const Json::Value& config) {
    if (protocol == Constants::PROTOCOL_SL651) {
        validateSl651(config);
    } else if (protocol == Constants::PROTOCOL_MODBUS ||
               protocol == Constants::PROTOCOL_MODBUS_TCP ||
               protocol == Constants::PROTOCOL_MODBUS_RTU) {
        validateModbus(config);
    } else if (protocol == Constants::PROTOCOL_S7) {
        validateS7(config);
    }
}

}  // namespace ProtocolConfigValidator
