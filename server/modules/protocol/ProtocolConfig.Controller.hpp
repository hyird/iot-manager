#pragma once

#include "ProtocolConfig.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/filters/PermissionFilter.hpp"

namespace {
    // 允许的协议配置类型列表
    const std::vector<std::string> ALLOWED_CONFIG_PROTOCOLS = {
        Constants::PROTOCOL_SL651,
        Constants::PROTOCOL_MODBUS,
        Constants::PROTOCOL_MODBUS_TCP,
        Constants::PROTOCOL_MODBUS_RTU
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
            "协议类型", "SL651、MODBUS、Modbus TCP 或 Modbus RTU").throwIfInvalid();

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
            "协议类型", "SL651、MODBUS、Modbus TCP 或 Modbus RTU").throwIfInvalid();

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
