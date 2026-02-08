#pragma once

#include "Device.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 设备管理控制器
 */
class DeviceController : public drogon::HttpController<DeviceController> {
private:
    DeviceService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DeviceController::list, "/api/device", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceController::realtime, "/api/device/realtime", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceController::detail, "/api/device/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceController::create, "/api/device", Post, "AuthFilter");
    ADD_METHOD_TO(DeviceController::update, "/api/device/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(DeviceController::remove, "/api/device/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(DeviceController::options, "/api/device/options", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceController::history, "/api/device/history", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceController::command, "/api/device/command/{linkId}", Post, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取设备静态数据列表（支持 ETag 缓存）
     * 异常由全局异常处理器统一处理
     */
    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        // 权限检查优先，防止未授权用户通过 ETag 探测资源变更
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        // ETag 检查
        if (auto notModified = ETagUtils::checkETag(req, "device")) {
            co_return notModified;
        }

        Json::Value result;
        result["list"] = co_await service_.listStatic();

        auto resp = Response::ok(result);
        ETagUtils::addETag(resp, "device");
        co_return resp;
    }

    /**
     * @brief 获取设备实时数据（不缓存，用于轮询）
     */
    Task<HttpResponsePtr> realtime(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        Json::Value result;
        result["list"] = co_await service_.listRealtime();
        co_return Response::ok(result);
    }

    /**
     * @brief 获取设备详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});
        co_return Response::ok(co_await service_.detail(id));
    }

    /**
     * @brief 创建设备
     */
    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "设备名称").throwIfInvalid();
        ValidatorHelper::requirePositiveInt(*json, "link_id", "关联链路").throwIfInvalid();
        ValidatorHelper::requirePositiveInt(*json, "protocol_config_id", "协议配置").throwIfInvalid();

        co_await service_.create(*json);
        co_return Response::created("创建成功");
    }

    /**
     * @brief 更新设备
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除设备
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:delete"});
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }

    /**
     * @brief 获取设备选项（下拉列表，支持 ETag 缓存）
     */
    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        // 权限检查优先
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        // ETag 检查
        if (auto notModified = ETagUtils::checkETag(req, "device")) {
            co_return notModified;
        }

        auto resp = Response::ok(co_await service_.options());
        ETagUtils::addETag(resp, "device");
        co_return resp;
    }

    /**
     * @brief 查询设备历史数据（支持 ETag 缓存）
     * ETag 基于参数哈希 + 设备数据版本号生成
     */
    Task<HttpResponsePtr> history(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:device:query"});

        // 获取查询参数
        std::string code = req->getParameter("code");
        int deviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        std::string funcCode = req->getParameter("funcCode");
        std::string dataType = req->getParameter("dataType");
        std::string startTime = req->getParameter("startTime");
        std::string endTime = req->getParameter("endTime");

        // 使用安全的参数解析
        int page = ValidatorHelper::getIntParam(req, "page", 1);
        int pageSize = ValidatorHelper::getIntParam(req, "pageSize", 10);

        if (code.empty() && deviceId <= 0) {
            co_return Response::badRequest("设备编码或设备ID不能为空");
        }

        // 验证时间范围必填，不允许查询全部数据
        if (startTime.empty() || endTime.empty()) {
            co_return Response::badRequest("必须指定时间范围");
        }

        // 参数化 ETag 检查
        std::string identifier = deviceId > 0 ? std::to_string(deviceId) : code;
        std::string params = identifier + ":" + funcCode + ":" + dataType + ":" +
                             startTime + ":" + endTime + ":" +
                             std::to_string(page) + ":" + std::to_string(pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "device", params)) {
            co_return notModified;
        }

        auto queryResult = co_await service_.queryHistory(
            code, funcCode, dataType, startTime, endTime, page, pageSize, deviceId
        );
        auto [list, total] = queryResult;

        Json::Value result;
        result["list"] = list;
        result["total"] = total;
        result["page"] = page;
        result["pageSize"] = pageSize;

        auto resp = Response::ok(result);
        ETagUtils::addParamETag(resp, "device", params);
        co_return resp;
    }

    /**
     * @brief 下发设备指令
     */
    Task<HttpResponsePtr> command(HttpRequestPtr req, int linkId) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:command"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        std::string deviceCode = (*json).get("deviceCode", "").asString();
        int deviceId = (*json).get("deviceId", 0).asInt();
        std::string funcCode = (*json).get("funcCode", "").asString();

        if (deviceCode.empty() && deviceId == 0) co_return Response::badRequest("设备标识不能为空");
        if (funcCode.empty()) co_return Response::badRequest("功能码不能为空");

        // 校验 elements 数组
        Json::Value elements = (*json).get("elements", Json::arrayValue);
        if (!elements.isArray() || elements.empty()) co_return Response::badRequest("要素列表不能为空");
        for (const auto& elem : elements) {
            if (!elem.isObject()) co_return Response::badRequest("要素格式错误");
            if (elem.get("elementId", "").asString().empty()) co_return Response::badRequest("要素 elementId 不能为空");
            if (elem.get("value", "").asString().empty()) co_return Response::badRequest("要素值不能为空");
        }

        // 通过 Service 层下发指令
        bool success = co_await service_.sendCommand(linkId, deviceCode, funcCode, elements, userId, deviceId);

        if (success) {
            co_return Response::ok(Json::nullValue, "指令下发成功，设备已应答");
        } else {
            co_return Response::badRequest("指令下发失败：设备无应答或应答超时");
        }
    }

};
