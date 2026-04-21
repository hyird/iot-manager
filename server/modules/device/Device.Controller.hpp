#pragma once

#include "Device.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/filters/ResourcePermission.hpp"

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
    ADD_METHOD_TO(DeviceController::shares, "/api/device/{id}/shares", Get, "AuthFilter");
    ADD_METHOD_TO(DeviceController::shareUpsert, "/api/device/{id}/shares", Post, "AuthFilter");
    ADD_METHOD_TO(DeviceController::shareRemove, "/api/device/{id}/shares/{targetType}/{targetId}", Delete, "AuthFilter");
    METHOD_LIST_END

    /**
     * @brief 获取设备静态数据列表（支持 ETag 缓存 + 可选分页）
     */
    Task<HttpResponsePtr> list(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:query"});

        auto page = Pagination::fromRequest(req);

        // 参数化 ETag 检查
        std::string params = std::to_string(userId) + ":" +
                             std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "device", params)) {
            co_return notModified;
        }

        auto items = co_await service_.listStatic(userId);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        auto resp = Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "device", params);
        co_return resp;
    }

    /**
     * @brief 获取设备实时数据（不缓存，用于轮询，支持可选分页）
     */
    Task<HttpResponsePtr> realtime(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:query"});

        auto page = Pagination::fromRequest(req);
        auto items = co_await service_.listRealtime(userId);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        co_return Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
    }

    /**
     * @brief 获取设备详情
     */
    Task<HttpResponsePtr> detail(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:query"});
        co_return Response::ok(co_await service_.detail(id, userId));
    }

    /**
     * @brief 创建设备
     */
    Task<HttpResponsePtr> create(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:add"});

        auto json = ControllerUtils::requireJson(req);

        ValidatorHelper::requireNonEmptyString(*json, "name", "设备名称").throwIfInvalid();
        ValidatorHelper::requirePositiveInt(*json, "protocol_config_id", "协议配置").throwIfInvalid();

        // link_id: 0 = Agent 模式, > 0 = 本地链路模式
        const int linkId = json->get("link_id", -1).asInt();
        ControllerUtils::requireNonNegativeValue(linkId, "缺少 link_id 参数");
        if (linkId == 0) {
            // Agent 模式必须指定 agent_id 和 agent_endpoint_id
            ValidatorHelper::requirePositiveInt(*json, "agent_id", "采集Agent").throwIfInvalid();
            ValidatorHelper::requirePositiveInt(*json, "agent_endpoint_id", "接入端点").throwIfInvalid();
        }

        co_await service_.create(*json, userId);
        co_return Response::created("创建成功");
    }

    /**
     * @brief 更新设备
     */
    Task<HttpResponsePtr> update(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:edit"});
        co_await ResourcePermission::ensureDeviceOwnerOrSuperAdmin(id, userId);

        auto json = ControllerUtils::requireJson(req);

        co_await service_.update(id, *json);
        co_return Response::updated("更新成功");
    }

    /**
     * @brief 删除设备
     */
    Task<HttpResponsePtr> remove(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:delete"});
        co_await ResourcePermission::ensureDeviceOwnerOrSuperAdmin(id, userId);
        co_await service_.remove(id);
        co_return Response::deleted("删除成功");
    }

    /**
     * @brief 获取设备选项（下拉列表，支持 ETag 缓存 + 可选分页）
     */
    Task<HttpResponsePtr> options(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:query"});

        auto page = Pagination::fromRequest(req);

        std::string params = std::to_string(userId) + ":" +
                             std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "device", params)) {
            co_return notModified;
        }

        auto items = co_await service_.options(userId);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        auto resp = Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "device", params);
        co_return resp;
    }

    /**
     * @brief 查询设备历史数据（支持 ETag 缓存 + 可选分页）
     */
    Task<HttpResponsePtr> history(HttpRequestPtr req) {
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:query"});

        // 获取查询参数（协议无关：deviceId + dataType + 时间范围）
        int deviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        std::string dataType = req->getParameter("dataType");
        std::string startTime = req->getParameter("startTime");
        std::string endTime = req->getParameter("endTime");

        auto page = Pagination::fromRequest(req);

        ControllerUtils::requirePositiveId(deviceId, "设备ID不能为空");
        ControllerUtils::requireNonEmptyString(dataType, "数据类型不能为空");

        // 验证时间范围必填，不允许查询全部数据
        ControllerUtils::requireTimeRange(startTime, endTime);
        co_await ResourcePermission::ensureDeviceViewPermission(deviceId, userId);

        // 参数化 ETag 检查
        std::string deviceIdStr = std::to_string(deviceId);
        std::string params = std::to_string(userId) + ":" + deviceIdStr + ":" + dataType + ":" +
                             startTime + ":" + endTime + ":" +
                             std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "device", params)) {
            co_return notModified;
        }

        // 非分页要素查询：Raw JSONB 透传（图表场景，零 JSON 解析）
        if (!page.isPaged() && dataType != "IMAGE") {
            auto rawBody = co_await service_.queryHistoryRaw(
                startTime, endTime, deviceId, userId
            );
            auto resp = Response::rawJson(std::move(rawBody));
            ETagUtils::addParamETag(resp, "device", params);
            co_return resp;
        }

        auto [items, total] = co_await service_.queryHistory(
            dataType, startTime, endTime, page.page, page.pageSize, deviceId, userId
        );

        auto resp = Pagination::buildResponse(items, total, page.page, page.pageSize);
        ETagUtils::addParamETag(resp, "device", params);
        co_return resp;
    }

    /**
     * @brief 下发设备指令
     */
    Task<HttpResponsePtr> command(HttpRequestPtr req, int linkId) {
        int userId = ControllerUtils::getUserId(req);

        auto json = ControllerUtils::requireJson(req);

        std::string deviceCode = (*json).get("deviceCode", "").asString();
        int deviceId = (*json).get("deviceId", 0).asInt();

        ControllerUtils::requireDeviceSelector(deviceCode, deviceId, "设备标识不能为空");
        const auto& elements = ControllerUtils::requireCommandElements(*json);

        // 通过 Service 层下发指令
        auto result = co_await service_.sendCommand(linkId, deviceCode, elements, userId, deviceId);

        if (result.ok()) {
            co_return Response::message(result.message);
        }
        throw result.toException();
    }

    /**
     * @brief 获取设备分享列表
     */
    Task<HttpResponsePtr> shares(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:query"});
        co_return Response::ok(co_await service_.listShares(id, userId));
    }

    /**
     * @brief 新增或更新设备分享
     */
    Task<HttpResponsePtr> shareUpsert(HttpRequestPtr req, int id) {
        ControllerUtils::requirePositiveId(id);
        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:edit"});

        auto json = ControllerUtils::requireJson(req);
        co_await service_.upsertShare(id, userId, *json);
        co_return Response::updated("分享权限已更新");
    }

    /**
     * @brief 删除设备分享
     */
    Task<HttpResponsePtr> shareRemove(HttpRequestPtr req, int id, std::string targetType, int targetId) {
        ControllerUtils::requirePositiveId(id);
        ControllerUtils::requirePositiveId(targetId, "分享目标ID无效");

        int userId = ControllerUtils::getUserId(req);
        co_await PermissionChecker::checkPermission(userId, {"iot:device:edit"});

        co_await service_.removeShare(id, targetType, targetId, userId);
        co_return Response::deleted("已取消分享");
    }

};
