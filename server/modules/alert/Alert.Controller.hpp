#pragma once

#include "Alert.Service.hpp"
#include "common/utils/Response.hpp"
#include "common/utils/Pagination.hpp"
#include "common/utils/ControllerMacros.hpp"
#include "common/utils/ValidatorHelper.hpp"
#include "common/filters/PermissionFilter.hpp"

/**
 * @brief 告警管理控制器
 */
class AlertController : public drogon::HttpController<AlertController> {
private:
    AlertService service_;

public:
    using enum drogon::HttpMethod;
    using HttpRequestPtr = drogon::HttpRequestPtr;
    using HttpResponsePtr = drogon::HttpResponsePtr;
    template<typename T = void> using Task = drogon::Task<T>;

    METHOD_LIST_BEGIN
    // 规则管理
    ADD_METHOD_TO(AlertController::listRules, "/api/alert/rule", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::createRule, "/api/alert/rule", Post, "AuthFilter");
    ADD_METHOD_TO(AlertController::ruleDetail, "/api/alert/rule/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::updateRule, "/api/alert/rule/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(AlertController::deleteRule, "/api/alert/rule/{id}", Delete, "AuthFilter");
    // 告警记录
    ADD_METHOD_TO(AlertController::listRecords, "/api/alert/record", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::acknowledgeRecord, "/api/alert/record/{id}/ack", Post, "AuthFilter");
    ADD_METHOD_TO(AlertController::batchAcknowledge, "/api/alert/record/batch-ack", Post, "AuthFilter");
    // 统计
    ADD_METHOD_TO(AlertController::activeStats, "/api/alert/stats", Get, "AuthFilter");
    METHOD_LIST_END

    // ==================== 规则 CRUD ====================

    Task<HttpResponsePtr> listRules(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        if (auto notModified = ETagUtils::checkETag(req, "alert")) {
            co_return notModified;
        }

        auto page = Pagination::fromRequest(req);
        int deviceId = 0;
        std::string severity;

        auto deviceIdStr = req->getParameter("deviceId");
        if (!deviceIdStr.empty()) {
            try { deviceId = std::stoi(deviceIdStr); } catch (...) {}
        }
        severity = req->getParameter("severity");

        auto result = co_await service_.listRules(page, deviceId, severity);

        auto resp = Response::ok(result.toJson());
        ETagUtils::addETag(resp, "alert");
        co_return resp;
    }

    Task<HttpResponsePtr> createRule(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "规则名称").throwIfInvalid();
        ValidatorHelper::requirePositiveInt(*json, "device_id", "关联设备").throwIfInvalid();

        co_await service_.createRule(*json);
        co_return Response::created("创建成功");
    }

    Task<HttpResponsePtr> ruleDetail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        co_return Response::ok(co_await service_.getRuleDetail(id));
    }

    Task<HttpResponsePtr> updateRule(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await service_.updateRule(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> deleteRule(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:delete"});

        co_await service_.deleteRule(id);
        co_return Response::deleted("删除成功");
    }

    // ==================== 告警记录 ====================

    Task<HttpResponsePtr> listRecords(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        int page = 1, pageSize = 20;
        int deviceId = 0, ruleId = 0;
        std::string status, severity;

        auto pageStr = req->getParameter("page");
        if (!pageStr.empty()) { try { page = std::stoi(pageStr); } catch (...) {} }
        auto pageSizeStr = req->getParameter("pageSize");
        if (!pageSizeStr.empty()) { try { pageSize = std::stoi(pageSizeStr); } catch (...) {} }
        auto deviceIdStr = req->getParameter("deviceId");
        if (!deviceIdStr.empty()) { try { deviceId = std::stoi(deviceIdStr); } catch (...) {} }
        auto ruleIdStr = req->getParameter("ruleId");
        if (!ruleIdStr.empty()) { try { ruleId = std::stoi(ruleIdStr); } catch (...) {} }
        status = req->getParameter("status");
        severity = req->getParameter("severity");

        if (pageSize < 1) pageSize = 20;
        if (pageSize > 100) pageSize = 100;
        if (page < 1) page = 1;

        auto [list, total] = co_await service_.listRecords(page, pageSize, deviceId, ruleId, status, severity);

        Json::Value data;
        data["list"] = list;
        data["total"] = total;
        data["page"] = page;
        data["pageSize"] = pageSize;
        data["totalPages"] = pageSize > 0 ? static_cast<int>(std::ceil(static_cast<double>(total) / pageSize)) : 0;

        co_return Response::ok(data);
    }

    Task<HttpResponsePtr> acknowledgeRecord(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:ack"});

        int userId = ControllerUtils::getUserId(req);
        co_await service_.acknowledgeRecord(static_cast<int64_t>(id), userId);
        co_return Response::ok("确认成功");
    }

    Task<HttpResponsePtr> batchAcknowledge(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:ack"});

        auto json = req->getJsonObject();
        if (!json || !json->isMember("ids") || !(*json)["ids"].isArray()) {
            co_return Response::badRequest("请提供 ids 数组");
        }

        std::vector<int64_t> ids;
        for (const auto& idVal : (*json)["ids"]) {
            ids.push_back(idVal.asInt64());
        }

        if (ids.empty()) co_return Response::badRequest("ids 不能为空");

        int userId = ControllerUtils::getUserId(req);
        co_await service_.batchAcknowledge(ids, userId);
        co_return Response::ok("批量确认成功");
    }

    // ==================== 统计 ====================

    Task<HttpResponsePtr> activeStats(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        co_return Response::ok(co_await service_.activeAlertStats());
    }
};
