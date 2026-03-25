#pragma once

#include "ProtocolConfig.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
    // 允许的协议配置类型列表
    const std::vector<std::string> ALLOWED_CONFIG_PROTOCOLS = {
        Constants::PROTOCOL_SL651,
        Constants::PROTOCOL_MODBUS,
        Constants::PROTOCOL_MODBUS_TCP,
        Constants::PROTOCOL_MODBUS_RTU,
        Constants::PROTOCOL_S7
    };
}

/**
 * @brief 协议配置控制器
 * 提供协议配置的 CRUD 接口
 */
class ProtocolConfigController : public drogon::HttpController<ProtocolConfigController> {
private:
    ProtocolConfigService service_;

    /**
     * @brief 校验协议配置 config JSON 的结构合法性
     */
    static void validateConfig(const std::string& protocol, const Json::Value& config) {
        if (protocol == Constants::PROTOCOL_SL651) {
            // SL651: funcs 必须是数组
            if (config.isMember("funcs")) {
                if (!config["funcs"].isArray()) {
                    throw ValidationException("SL651 配置的 funcs 必须是数组");
                }
                for (const auto& func : config["funcs"]) {
                    if (!func.isObject()) continue;
                    std::string funcCode = func.get("funcCode", "").asString();
                    if (funcCode.empty()) {
                        throw ValidationException("功能码 funcCode 不能为空");
                    }
                    // 校验要素
                    if (func.isMember("elements") && func["elements"].isArray()) {
                        for (const auto& elem : func["elements"]) {
                            if (!elem.isObject()) continue;
                            if (elem.get("guideHex", "").asString().empty()) {
                                std::string elemName = elem.get("name", "未命名").asString();
                                throw ValidationException("要素「" + elemName + "」的引导符不能为空");
                            }
                            int length = elem.get("length", 0).asInt();
                            if (length < 1) {
                                std::string elemName = elem.get("name", "未命名").asString();
                                throw ValidationException("要素「" + elemName + "」的长度必须 >= 1");
                            }
                            int digits = elem.get("digits", 0).asInt();
                            if (digits < 0 || digits > 8) {
                                std::string elemName = elem.get("name", "未命名").asString();
                                throw ValidationException("要素「" + elemName + "」的小数位必须在 0-8 之间");
                            }
                        }
                    }
                    // 校验应答要素（同样的规则）
                    if (func.isMember("responseElements") && func["responseElements"].isArray()) {
                        for (const auto& elem : func["responseElements"]) {
                            if (!elem.isObject()) continue;
                            if (elem.get("guideHex", "").asString().empty()) {
                                std::string elemName = elem.get("name", "未命名").asString();
                                throw ValidationException("应答要素「" + elemName + "」的引导符不能为空");
                            }
                            int length = elem.get("length", 0).asInt();
                            if (length < 1) {
                                std::string elemName = elem.get("name", "未命名").asString();
                                throw ValidationException("应答要素「" + elemName + "」的长度必须 >= 1");
                            }
                        }
                    }
                }
            }
        } else if (protocol == Constants::PROTOCOL_MODBUS ||
                   protocol == Constants::PROTOCOL_MODBUS_TCP ||
                   protocol == Constants::PROTOCOL_MODBUS_RTU) {
            // Modbus: readInterval 范围检查
            if (config.isMember("readInterval")) {
                int interval = config["readInterval"].asInt();
                if (interval < 1 || interval > 3600) {
                    throw ValidationException("读取间隔必须在 1-3600 秒之间");
                }
            }
            // Modbus: registers 必须是数组
            if (config.isMember("registers")) {
                if (!config["registers"].isArray()) {
                    throw ValidationException("Modbus 配置的 registers 必须是数组");
                }
                for (const auto& reg : config["registers"]) {
                    if (!reg.isObject()) continue;
                    // 地址范围
                    unsigned int address = reg.get("address", 0).asUInt();
                    if (address > 65535) {
                        std::string regName = reg.get("name", "未命名").asString();
                        throw ValidationException("寄存器「" + regName + "」的地址超出范围 (0-65535)");
                    }
                    // quantity 范围（Modbus 协议限制）
                    unsigned int quantity = reg.get("quantity", 1).asUInt();
                    if (quantity < 1 || quantity > 125) {
                        std::string regName = reg.get("name", "未命名").asString();
                        throw ValidationException("寄存器「" + regName + "」的数量超出范围 (1-125)");
                    }
                    // 地址 + 数量不能溢出 uint16
                    if (address + quantity - 1 > 65535) {
                        std::string regName = reg.get("name", "未命名").asString();
                        throw ValidationException("寄存器「" + regName + "」的地址 + 数量超出范围（末地址不能超过 65535）");
                    }
                }
            }
        } else if (protocol == Constants::PROTOCOL_S7) {
            if (config.get("deviceType", "").asString().empty()) {
                throw ValidationException("S7 配置的 deviceType 不能为空");
            }
            std::string plcModel = config.get("plcModel", "").asString();
            if (plcModel.empty()) {
                throw ValidationException("S7 配置的 plcModel 不能为空");
            }

            struct S7ConnectionPreset {
                std::string mode = "RACK_SLOT";
                int rack = 0;
                int slot = 1;
                std::string localTSAP;
                std::string remoteTSAP;
            };
            auto toUpper = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                return value;
            };
            auto resolvePreset = [](const std::string& model) -> std::optional<S7ConnectionPreset> {
                if (model == "S7-200") return S7ConnectionPreset{.mode = "TSAP", .rack = 0, .slot = 1, .localTSAP = "4D57", .remoteTSAP = "4D57"};
                if (model == "S7-300") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 2};
                if (model == "S7-400") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 3};
                if (model == "S7-1200" || model == "S7-1500") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 1};
                return std::nullopt;
            };
            auto preset = resolvePreset(plcModel);
            if (!preset) {
                throw ValidationException("S7 配置的 plcModel 仅支持 S7-200、S7-300、S7-400、S7-1200、S7-1500");
            }

            if (config.isMember("connection") && !config["connection"].isObject()) {
                throw ValidationException("S7 配置的 connection 必须是对象");
            }

            const auto& conn = config.isMember("connection") && config["connection"].isObject()
                ? config["connection"]
                : Json::Value::nullSingleton();
            auto normalizeTsap = [&](const Json::Value& value) -> std::optional<std::string> {
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
            };
            auto normalizeConnectionMode = [&](const Json::Value& connection) {
                std::string mode = connection.get("mode", "").asString();
                if (!mode.empty()) {
                    return toUpper(mode);
                }
                if (connection.isMember("localTSAP") || connection.isMember("remoteTSAP")) {
                    return std::string("TSAP");
                }
                return preset->mode;
            };
            auto normalizeConnectionType = [&](const Json::Value& connection) {
                return toUpper(connection.get("connectionType", "PG").asString());
            };

            const std::string connectionMode = normalizeConnectionMode(conn);
            if (connectionMode != "RACK_SLOT" && connectionMode != "TSAP") {
                throw ValidationException("S7 connection.mode 仅支持 RACK_SLOT 或 TSAP");
            }
            if (connectionMode == "RACK_SLOT") {
                const int rack = conn.get("rack", preset->rack).asInt();
                const int slot = conn.get("slot", preset->slot).asInt();
                if (rack < 0 || slot < 0) {
                    throw ValidationException("S7 的 rack/slot 不能小于 0");
                }
                const std::string connectionType = normalizeConnectionType(conn);
                if (connectionType != "PG" && connectionType != "OP" && connectionType != "S7_BASIC") {
                    throw ValidationException("S7 connection.connectionType 仅支持 PG、OP、S7_BASIC");
                }
            } else {
                Json::Value localValue;
                Json::Value remoteValue;
                if (conn.isMember("localTSAP")) {
                    localValue = conn["localTSAP"];
                } else if (!preset->localTSAP.empty()) {
                    localValue = Json::Value(preset->localTSAP);
                }
                if (conn.isMember("remoteTSAP")) {
                    remoteValue = conn["remoteTSAP"];
                } else if (!preset->remoteTSAP.empty()) {
                    remoteValue = Json::Value(preset->remoteTSAP);
                }
                if (!normalizeTsap(localValue) || !normalizeTsap(remoteValue)) {
                    throw ValidationException("S7 TSAP 模式下必须提供合法的 localTSAP / remoteTSAP（例如 4D57 或 0200）");
                }
            }

            if (config.isMember("areas")) {
                if (!config["areas"].isArray()) {
                    throw ValidationException("S7 配置的 areas 必须是数组");
                }
                const bool isS7_200 = plcModel == "S7-200";
                for (const auto& area : config["areas"]) {
                    if (!area.isObject()) continue;
                    if (area.get("id", "").asString().empty()) {
                        throw ValidationException("S7 配置的区域 id 不能为空");
                    }
                    if (area.get("name", "").asString().empty()) {
                        throw ValidationException("S7 配置的区域名称不能为空");
                    }
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
                    if (areaType == "DB" && area.get("dbNumber", 0).asInt() < 0) {
                        throw ValidationException("S7 DB 区域的 dbNumber 不能小于 0");
                    }
                    if (area.get("size", 0).asInt() < 1) {
                        throw ValidationException("S7 区域的 size 必须 >= 1");
                    }
                    if (area.get("start", 0).asInt() < 0) {
                        throw ValidationException("S7 区域的 start 不能小于 0");
                    }
                }
            }
        }
    }

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ProtocolConfigController::list, "/api/protocol/configs", Get, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::detail, "/api/protocol/configs/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::create, "/api/protocol/configs", Post, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::update, "/api/protocol/configs/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::remove, "/api/protocol/configs/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(ProtocolConfigController::options, "/api/protocol/configs/options", Get, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取配置列表（支持参数化 ETag 缓存）
     * @param protocol 协议类型筛选（可选）
     */
    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});

        auto page = Pagination::fromRequest(req);
        std::string protocol = req->getParameter("protocol");

        // 参数化 ETag 检查
        std::string params = protocol + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "protocol", params)) {
            co_return notModified;
        }

        auto result = co_await service_.list(page, protocol);
        auto [items, total] = result;
        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "protocol", params);
        co_return resp;
    }

    /**
     * @brief 获取配置详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    /**
     * @brief 创建配置
     */
    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "protocol", "协议类型").throwIfInvalid();
        ValidatorHelper::requireNonEmptyString(*json, "name", "配置名称").throwIfInvalid();
        ValidatorHelper::requireInList(*json, "protocol", ALLOWED_CONFIG_PROTOCOLS,
            "协议类型", "SL651、MODBUS、Modbus TCP、Modbus RTU 或 S7").throwIfInvalid();

        // 校验 config 字段结构
        if (json->isMember("config") && (*json)["config"].isObject()) {
            validateConfig((*json)["protocol"].asString(), (*json)["config"]);
        }

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    /**
     * @brief 更新配置
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireInListIfPresent(*json, "protocol", ALLOWED_CONFIG_PROTOCOLS,
            "协议类型", "SL651、MODBUS、Modbus TCP、Modbus RTU 或 S7").throwIfInvalid();

        co_await service_.update(id, *json, [](const std::string& proto, const Json::Value& cfg) {
            validateConfig(proto, cfg);
        });
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除配置
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }

    /**
     * @brief 获取指定协议的配置选项（用于下拉选择，支持 ETag 缓存 + 可选分页）
     */
    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:protocol:query"});

        std::string protocol = req->getParameter("protocol");
        if (protocol.empty()) {
            co_return Response::badRequest("协议类型不能为空");
        }

        auto page = Pagination::fromRequest(req);

        std::string params = protocol + ":" + std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "protocol", params)) {
            co_return notModified;
        }

        auto items = co_await service_.options(protocol);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        auto resp = Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "protocol", params);
        co_return resp;
    }
};
