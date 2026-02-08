#pragma once

#include "OpenAccess.Repository.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/filters/PermissionFilter.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "modules/device/Device.Service.hpp"

class OpenAccessController : public drogon::HttpController<OpenAccessController> {
private:
    OpenAccessRepository repository_;
    DeviceService deviceService_;

    template<typename T = void>
    using Task = drogon::Task<T>;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;

    Task<OpenAccess::AccessKeySession> authenticateOpenRequest(
        const HttpRequestPtr& req,
        bool requireRealtime,
        bool requireHistory,
        bool requireCommand = false,
        bool requireAlert = false
    ) {
        auto session = co_await repository_.authenticate(
            OpenAccess::extractAccessKey(req),
            OpenAccess::resolveClientIp(req)
        );

        if (requireRealtime && !session.allowRealtime) {
            throw ForbiddenException("AccessKey 未开通实时数据权限");
        }
        if (requireHistory && !session.allowHistory) {
            throw ForbiddenException("AccessKey 未开通历史数据权限");
        }
        if (requireCommand && !session.allowCommand) {
            throw ForbiddenException("AccessKey 未开通控制下发权限");
        }
        if (requireAlert && !session.allowAlert) {
            throw ForbiddenException("AccessKey 未开通告警查询权限");
        }

        co_return session;
    }

    Task<void> writeAccessLogSafe(
        const std::string& direction,
        const std::string& action,
        int accessKeyId = 0,
        int webhookId = 0,
        const std::string& eventType = "",
        const std::string& status = "success",
        const std::string& httpMethod = "",
        const std::string& target = "",
        const std::string& requestIp = "",
        int httpStatus = 0,
        int deviceId = 0,
        const std::string& deviceCode = "",
        const std::string& message = "",
        const Json::Value& requestPayload = Json::Value(Json::objectValue),
        const Json::Value& responsePayload = Json::Value(Json::objectValue)
    ) {
        try {
            co_await OpenAccessRepository::writeAccessLog(
                direction,
                action,
                accessKeyId,
                webhookId,
                eventType,
                status,
                httpMethod,
                target,
                requestIp,
                httpStatus,
                deviceId,
                deviceCode,
                message,
                requestPayload,
                responsePayload
            );
        } catch (const std::exception& e) {
            LOG_WARN << "[OpenAccess] writeAccessLog failed: " << e.what();
        }
    }

    Task<std::string> resolveDeviceCode(int deviceId) {
        auto devices = co_await DeviceCache::instance().getDevices();
        for (const auto& device : devices) {
            if (device.id == deviceId) {
                co_return device.deviceCode;
            }
        }
        co_return "";
    }

    struct CommandTarget {
        int deviceId = 0;
        int linkId = 0;
        std::string deviceCode;
        std::string deviceName;
    };

    Task<CommandTarget> resolveCommandTarget(int deviceId) {
        auto devices = co_await DeviceCache::instance().getDevices();
        for (const auto& device : devices) {
            if (device.id != deviceId) continue;

            if (device.status != "enabled") {
                throw ForbiddenException("设备已被禁用，不能下发控制");
            }
            if (!device.remoteControl) {
                throw ForbiddenException("设备未开启远程控制");
            }
            if (device.linkId <= 0) {
                throw ValidationException("设备未绑定链路，无法下发控制");
            }
            if (device.protocolType == "SL651" && device.deviceCode.empty()) {
                throw ValidationException("设备缺少设备编码，无法下发控制");
            }

            CommandTarget target;
            target.deviceId = device.id;
            target.linkId = device.linkId;
            target.deviceCode = device.deviceCode;
            target.deviceName = device.name;
            co_return target;
        }

        throw NotFoundException("设备不存在");
    }

public:
    using enum drogon::HttpMethod;

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(OpenAccessController::listAccessKeys, "/api/open-access-key", Get, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::createAccessKey, "/api/open-access-key", Post, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::updateAccessKey, "/api/open-access-key/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::rotateAccessKey, "/api/open-access-key/{id}/rotate", Post, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::removeAccessKey, "/api/open-access-key/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::listWebhooks, "/api/open-webhook", Get, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::createWebhook, "/api/open-webhook", Post, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::updateWebhook, "/api/open-webhook/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::removeWebhook, "/api/open-webhook/{id}", Delete, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::listAccessLogs, "/api/open-access-log", Get, "AuthFilter");
    ADD_METHOD_TO(OpenAccessController::openRealtime, "/open-api/device/realtime", Get);
    ADD_METHOD_TO(OpenAccessController::openHistory, "/open-api/device/history", Get);
    ADD_METHOD_TO(OpenAccessController::openCommand, "/open-api/device/command", Post);
    ADD_METHOD_TO(OpenAccessController::openAlert, "/open-api/device/alert", Get);
    METHOD_LIST_END

    Task<HttpResponsePtr> listAccessKeys(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:query"}
        );
        co_return Response::ok(co_await repository_.listAccessKeys());
    }

    Task<HttpResponsePtr> createAccessKey(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:add"}
        );

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        auto data = co_await repository_.createAccessKey(*json, ControllerUtils::getUserId(req));
        co_return Response::ok(data, "创建成功");
    }

    Task<HttpResponsePtr> updateAccessKey(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的 AccessKey ID");
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:edit"}
        );

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await repository_.updateAccessKey(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> rotateAccessKey(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的 AccessKey ID");
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:edit"}
        );

        auto data = co_await repository_.rotateAccessKey(id);
        co_return Response::ok(data, "轮换成功");
    }

    Task<HttpResponsePtr> removeAccessKey(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的 AccessKey ID");
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:delete"}
        );
        co_await repository_.removeAccessKey(id);
        co_return Response::deleted("删除成功");
    }

    Task<HttpResponsePtr> listWebhooks(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:query"}
        );
        int accessKeyId = ValidatorHelper::getIntParam(req, "accessKeyId", 0);
        co_return Response::ok(co_await repository_.listWebhooks(accessKeyId));
    }

    Task<HttpResponsePtr> createWebhook(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:add"}
        );

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        auto data = co_await repository_.createWebhook(*json);
        co_return Response::ok(data, "创建成功");
    }

    Task<HttpResponsePtr> updateWebhook(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的 Webhook ID");
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:edit"}
        );

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await repository_.updateWebhook(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> removeWebhook(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的 Webhook ID");
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:delete"}
        );
        co_await repository_.removeWebhook(id);
        co_return Response::deleted("删除成功");
    }

    Task<HttpResponsePtr> listAccessLogs(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(
            ControllerUtils::getUserId(req),
            {"iot:open-access:query"}
        );

        auto page = Pagination::fromRequest(req);
        int accessKeyId = ValidatorHelper::getIntParam(req, "accessKeyId", 0);
        int webhookId = ValidatorHelper::getIntParam(req, "webhookId", 0);
        int deviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        std::string direction = req->getParameter("direction");
        std::string action = req->getParameter("action");
        std::string status = req->getParameter("status");
        std::string eventType = req->getParameter("eventType");

        auto [items, total] = co_await repository_.listAccessLogs(
            page.page,
            page.pageSize,
            accessKeyId,
            webhookId,
            deviceId,
            direction,
            action,
            status,
            eventType
        );
        co_return Pagination::buildResponse(items, total, page.page, page.pageSize);
    }

    Task<HttpResponsePtr> openRealtime(HttpRequestPtr req) {
        std::string rawAccessKey = OpenAccess::extractAccessKey(req);
        std::string clientIp = OpenAccess::resolveClientIp(req);
        int logAccessKeyId = 0;
        std::string code = req->getParameter("code");
        int requestedDeviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        auto page = Pagination::fromRequest(req);

        Json::Value requestPayload;
        requestPayload["code"] = code;
        requestPayload["deviceId"] = requestedDeviceId;
        requestPayload["page"] = page.page;
        requestPayload["pageSize"] = page.pageSize;
        std::exception_ptr capturedError;
        std::string errorMessage;

        try {
            auto session = co_await authenticateOpenRequest(req, true, false);
            logAccessKeyId = session.id;

            int deviceId = co_await repository_.resolveDeviceId(code, requestedDeviceId);
            if ((!code.empty() || requestedDeviceId > 0) && deviceId <= 0) {
                throw NotFoundException("设备不存在");
            }
            if (deviceId > 0 && !session.canAccessDevice(deviceId)) {
                throw ForbiddenException("AccessKey 无权访问该设备");
            }

            auto items = co_await deviceService_.listRealtimeForOpenApi();
            Json::Value filtered(Json::arrayValue);
            for (const auto& item : items) {
                int currentId = item.get("device", Json::objectValue).get("id", 0).asInt();
                if (!session.canAccessDevice(currentId)) continue;
                if (deviceId > 0 && currentId != deviceId) continue;
                filtered.append(item);
            }

            auto [pagedItems, total] = Pagination::paginate(filtered, page);
            Json::Value responsePayload;
            responsePayload["total"] = total;
            responsePayload["returned"] = static_cast<Json::Int64>(pagedItems.size());

            co_await writeAccessLogSafe(
                "pull",
                "realtime",
                logAccessKeyId,
                0,
                "",
                "success",
                "GET",
                "/open-api/device/realtime",
                clientIp,
                200,
                deviceId,
                code,
                "",
                requestPayload,
                responsePayload
            );
            co_return Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
        } catch (const std::exception& e) {
            errorMessage = e.what();
            capturedError = std::current_exception();
        }

        if (capturedError) {
            if (logAccessKeyId <= 0) {
                logAccessKeyId = co_await repository_.tryResolveAccessKeyId(rawAccessKey);
            }
            co_await writeAccessLogSafe(
                "pull",
                "realtime",
                logAccessKeyId,
                0,
                "",
                "failed",
                "GET",
                "/open-api/device/realtime",
                clientIp,
                0,
                requestedDeviceId,
                code,
                errorMessage,
                requestPayload
            );
            std::rethrow_exception(capturedError);
        }

        co_return Response::internalError("请求处理失败");
    }

    Task<HttpResponsePtr> openHistory(HttpRequestPtr req) {
        std::string rawAccessKey = OpenAccess::extractAccessKey(req);
        std::string clientIp = OpenAccess::resolveClientIp(req);
        int logAccessKeyId = 0;
        int requestedDeviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        std::string code = req->getParameter("code");
        std::string dataType = req->getParameter("dataType");
        std::string startTime = req->getParameter("startTime");
        std::string endTime = req->getParameter("endTime");
        auto page = Pagination::fromRequest(req);

        Json::Value requestPayload;
        requestPayload["deviceId"] = requestedDeviceId;
        requestPayload["dataType"] = dataType;
        requestPayload["startTime"] = startTime;
        requestPayload["endTime"] = endTime;
        requestPayload["page"] = page.page;
        requestPayload["pageSize"] = page.pageSize;
        std::exception_ptr capturedError;
        std::string errorMessage;

        try {
            auto session = co_await authenticateOpenRequest(req, false, true);
            logAccessKeyId = session.id;

            if (code.empty() && requestedDeviceId <= 0) {
                throw ValidationException("设备编码或设备ID不能为空");
            }
            if (startTime.empty() || endTime.empty()) {
                throw ValidationException("必须指定时间范围");
            }

            int deviceId = co_await repository_.resolveDeviceId(code, requestedDeviceId);
            if (deviceId <= 0) {
                throw NotFoundException("设备不存在");
            }
            if (!session.canAccessDevice(deviceId)) {
                throw ForbiddenException("AccessKey 无权访问该设备");
            }

            if (!page.isPaged() && !dataType.empty() && dataType != "IMAGE") {
                auto rawBody = co_await deviceService_.queryHistoryRaw(
                    startTime, endTime, deviceId
                );
                Json::Value responsePayload;
                responsePayload["mode"] = "raw";
                responsePayload["bodyLength"] = static_cast<Json::Int64>(rawBody.size());
                co_await writeAccessLogSafe(
                    "pull",
                    "history",
                    logAccessKeyId,
                    0,
                    "",
                    "success",
                    "GET",
                    "/open-api/device/history",
                    clientIp,
                    200,
                    deviceId,
                    code,
                    "",
                    requestPayload,
                    responsePayload
                );
                co_return Response::rawJson(std::move(rawBody));
            }

            auto [items, total] = co_await deviceService_.queryHistory(
                dataType,
                startTime,
                endTime,
                page.page,
                page.pageSize,
                deviceId
            );

            Json::Value responsePayload;
            responsePayload["total"] = total;
            responsePayload["returned"] = static_cast<Json::Int64>(items.size());
            co_await writeAccessLogSafe(
                "pull",
                "history",
                logAccessKeyId,
                0,
                "",
                "success",
                "GET",
                "/open-api/device/history",
                clientIp,
                200,
                deviceId,
                code,
                "",
                requestPayload,
                responsePayload
            );
            co_return Pagination::buildResponse(items, total, page.page, page.pageSize);
        } catch (const std::exception& e) {
            errorMessage = e.what();
            capturedError = std::current_exception();
        }

        if (capturedError) {
            if (logAccessKeyId <= 0) {
                logAccessKeyId = co_await repository_.tryResolveAccessKeyId(rawAccessKey);
            }
            co_await writeAccessLogSafe(
                "pull",
                "history",
                logAccessKeyId,
                0,
                "",
                "failed",
                "GET",
                "/open-api/device/history",
                clientIp,
                0,
                requestedDeviceId,
                code,
                errorMessage,
                requestPayload
            );
            std::rethrow_exception(capturedError);
        }

        co_return Response::internalError("请求处理失败");
    }

    Task<HttpResponsePtr> openCommand(HttpRequestPtr req) {
        std::string rawAccessKey = OpenAccess::extractAccessKey(req);
        std::string clientIp = OpenAccess::resolveClientIp(req);
        int logAccessKeyId = 0;

        auto json = req->getJsonObject();
        Json::Value requestPayload = json ? *json : Json::Value(Json::objectValue);
        std::exception_ptr capturedError;
        std::string errorMessage;

        try {
            auto session = co_await authenticateOpenRequest(req, false, false, true, false);
            logAccessKeyId = session.id;

            if (!json) {
                throw ValidationException("请求体格式错误");
            }

            std::string deviceCode = (*json).get("deviceCode", "").asString();
            int requestedDeviceId = (*json).get("deviceId", 0).asInt();
            Json::Value elements = (*json).get("elements", Json::arrayValue);

            if (deviceCode.empty() && requestedDeviceId <= 0) {
                throw ValidationException("设备标识不能为空");
            }
            if (!elements.isArray() || elements.empty()) {
                throw ValidationException("要素列表不能为空");
            }
            for (const auto& elem : elements) {
                if (!elem.isObject()) {
                    throw ValidationException("要素格式错误");
                }
                if (elem.get("elementId", "").asString().empty()) {
                    throw ValidationException("要素 elementId 不能为空");
                }
                if (elem.get("value", "").asString().empty()) {
                    throw ValidationException("要素值不能为空");
                }
            }

            int deviceId = co_await repository_.resolveDeviceId(deviceCode, requestedDeviceId);
            if (deviceId <= 0) {
                throw NotFoundException("设备不存在");
            }
            if (!session.canAccessDevice(deviceId)) {
                throw ForbiddenException("AccessKey 无权控制该设备");
            }

            auto commandTarget = co_await resolveCommandTarget(deviceId);
            auto result = co_await deviceService_.sendCommand(
                commandTarget.linkId,
                commandTarget.deviceCode,
                elements,
                0,
                deviceId
            );

            Json::Value responsePayload;
            responsePayload["success"] = result.ok();
            responsePayload["deviceId"] = deviceId;
            responsePayload["deviceCode"] = commandTarget.deviceCode;

            if (result.ok()) {
                co_await writeAccessLogSafe(
                    "pull",
                    "command",
                    logAccessKeyId,
                    0,
                    "",
                    "success",
                    "POST",
                    "/open-api/device/command",
                    clientIp,
                    200,
                    deviceId,
                    commandTarget.deviceCode,
                    "",
                    requestPayload,
                    responsePayload
                );
                co_return Response::ok(responsePayload, result.message);
            }

            co_await writeAccessLogSafe(
                "pull",
                "command",
                logAccessKeyId,
                0,
                "",
                "failed",
                "POST",
                "/open-api/device/command",
                clientIp,
                400,
                deviceId,
                commandTarget.deviceCode,
                result.message,
                requestPayload,
                responsePayload
            );
            co_return Response::badRequest(result.message);
        } catch (const std::exception& e) {
            errorMessage = e.what();
            capturedError = std::current_exception();
        }

        if (capturedError) {
            if (logAccessKeyId <= 0) {
                logAccessKeyId = co_await repository_.tryResolveAccessKeyId(rawAccessKey);
            }
            co_await writeAccessLogSafe(
                "pull",
                "command",
                logAccessKeyId,
                0,
                "",
                "failed",
                "POST",
                "/open-api/device/command",
                clientIp,
                0,
                json ? (*json).get("deviceId", 0).asInt() : 0,
                json ? (*json).get("deviceCode", "").asString() : "",
                errorMessage,
                requestPayload
            );
            std::rethrow_exception(capturedError);
        }

        co_return Response::internalError("请求处理失败");
    }

    Task<HttpResponsePtr> openAlert(HttpRequestPtr req) {
        std::string rawAccessKey = OpenAccess::extractAccessKey(req);
        std::string clientIp = OpenAccess::resolveClientIp(req);
        int logAccessKeyId = 0;
        auto page = Pagination::fromRequest(req);

        std::string code = req->getParameter("code");
        int requestedDeviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        int ruleId = ValidatorHelper::getIntParam(req, "ruleId", 0);
        std::string status = req->getParameter("status");
        std::string severity = req->getParameter("severity");

        Json::Value requestPayload;
        requestPayload["code"] = code;
        requestPayload["deviceId"] = requestedDeviceId;
        requestPayload["ruleId"] = ruleId;
        requestPayload["status"] = status;
        requestPayload["severity"] = severity;
        requestPayload["page"] = page.page;
        requestPayload["pageSize"] = page.pageSize;
        std::exception_ptr capturedError;
        std::string errorMessage;

        try {
            auto session = co_await authenticateOpenRequest(req, false, false, false, true);
            logAccessKeyId = session.id;

            int deviceId = co_await repository_.resolveDeviceId(code, requestedDeviceId);
            if ((!code.empty() || requestedDeviceId > 0) && deviceId <= 0) {
                throw NotFoundException("设备不存在");
            }
            if (deviceId > 0 && !session.canAccessDevice(deviceId)) {
                throw ForbiddenException("AccessKey 无权访问该设备告警");
            }

            auto [items, total] = co_await repository_.listAlertRecords(
                session.deviceIds,
                page.page,
                page.pageSize,
                deviceId,
                ruleId,
                status,
                severity
            );

            Json::Value responsePayload;
            responsePayload["total"] = total;
            responsePayload["returned"] = static_cast<Json::Int64>(items.size());
            co_await writeAccessLogSafe(
                "pull",
                "alert",
                logAccessKeyId,
                0,
                "",
                "success",
                "GET",
                "/open-api/device/alert",
                clientIp,
                200,
                deviceId,
                code,
                "",
                requestPayload,
                responsePayload
            );
            co_return Pagination::buildResponse(items, total, page.page, page.pageSize);
        } catch (const std::exception& e) {
            errorMessage = e.what();
            capturedError = std::current_exception();
        }

        if (capturedError) {
            if (logAccessKeyId <= 0) {
                logAccessKeyId = co_await repository_.tryResolveAccessKeyId(rawAccessKey);
            }
            co_await writeAccessLogSafe(
                "pull",
                "alert",
                logAccessKeyId,
                0,
                "",
                "failed",
                "GET",
                "/open-api/device/alert",
                clientIp,
                0,
                requestedDeviceId,
                code,
                errorMessage,
                requestPayload
            );
            std::rethrow_exception(capturedError);
        }

        co_return Response::internalError("请求处理失败");
    }
};
