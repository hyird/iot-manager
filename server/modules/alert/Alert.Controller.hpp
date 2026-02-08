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
    ADD_METHOD_TO(AlertController::batchDeleteRules, "/api/alert/rule/batch", Delete, "AuthFilter");
    ADD_METHOD_TO(AlertController::applyTemplate, "/api/alert/rule/apply-template", Post, "AuthFilter");
    // 模板管理
    ADD_METHOD_TO(AlertController::listTemplates, "/api/alert/template", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::createTemplate, "/api/alert/template", Post, "AuthFilter");
    ADD_METHOD_TO(AlertController::templateDetail, "/api/alert/template/{id}", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::updateTemplate, "/api/alert/template/{id}", Put, "AuthFilter");
    ADD_METHOD_TO(AlertController::deleteTemplate, "/api/alert/template/{id}", Delete, "AuthFilter");
    // 告警记录
    ADD_METHOD_TO(AlertController::listRecords, "/api/alert/record", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::acknowledgeRecord, "/api/alert/record/{id}/ack", Post, "AuthFilter");
    ADD_METHOD_TO(AlertController::batchAcknowledge, "/api/alert/record/batch-ack", Post, "AuthFilter");
    ADD_METHOD_TO(AlertController::getGroupedRecords, "/api/alert/record/grouped", Get, "AuthFilter");
    ADD_METHOD_TO(AlertController::exportRecords, "/api/alert/record/export", Get, "AuthFilter");
    // 统计
    ADD_METHOD_TO(AlertController::activeStats, "/api/alert/stats", Get, "AuthFilter");
    METHOD_LIST_END

    // ==================== 规则 CRUD ====================

    Task<HttpResponsePtr> listRules(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        auto page = Pagination::fromRequest(req);
        int deviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        std::string severity = req->getParameter("severity");

        // 参数化 ETag 检查
        std::string params = std::to_string(deviceId) + ":" + severity + ":" +
                             std::to_string(page.page) + ":" + std::to_string(page.pageSize);
        if (auto notModified = ETagUtils::checkParamETag(req, "alert", params)) {
            co_return notModified;
        }

        auto result = co_await service_.listRules(page, deviceId, severity);

        auto resp = Response::ok(result.toJson());
        ETagUtils::addParamETag(resp, "alert", params);
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

        auto page = Pagination::fromRequest(req);
        int deviceId = ValidatorHelper::getIntParam(req, "deviceId", 0);
        int ruleId = ValidatorHelper::getIntParam(req, "ruleId", 0);
        std::string status = req->getParameter("status");
        std::string severity = req->getParameter("severity");

        auto [items, total] = co_await service_.listRecords(page, deviceId, ruleId, status, severity);
        co_return Pagination::buildResponse(items, total, page.page, page.pageSize);
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

    // ==================== 模板管理 ====================

    Task<HttpResponsePtr> listTemplates(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        auto page = Pagination::fromRequest(req);
        std::string category = req->getParameter("category");

        auto [items, total] = co_await service_.listTemplates(page, category);
        co_return Pagination::buildResponse(items, total, page.page, page.pageSize);
    }

    Task<HttpResponsePtr> createTemplate(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requireNonEmptyString(*json, "name", "模板名称").throwIfInvalid();

        (*json)["created_by"] = ControllerUtils::getUserId(req);
        co_await service_.createTemplate(*json);
        co_return Response::created("创建成功");
    }

    Task<HttpResponsePtr> templateDetail(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        co_return Response::ok(co_await service_.getTemplateDetail(id));
    }

    Task<HttpResponsePtr> updateTemplate(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:edit"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        co_await service_.updateTemplate(id, *json);
        co_return Response::updated("更新成功");
    }

    Task<HttpResponsePtr> deleteTemplate(HttpRequestPtr req, int id) {
        if (id <= 0) co_return Response::badRequest("无效的资源ID");
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:delete"});

        co_await service_.deleteTemplate(id);
        co_return Response::deleted("删除成功");
    }

    // ==================== 批量操作 ====================

    Task<HttpResponsePtr> batchDeleteRules(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:delete"});

        auto json = req->getJsonObject();
        if (!json || !json->isMember("ids") || !(*json)["ids"].isArray()) {
            co_return Response::badRequest("请提供 ids 数组");
        }

        std::vector<int> ids;
        for (const auto& idVal : (*json)["ids"]) {
            ids.push_back(idVal.asInt());
        }

        if (ids.empty()) co_return Response::badRequest("ids 不能为空");

        co_await service_.batchDeleteRules(ids);
        co_return Response::ok("批量删除成功");
    }

    Task<HttpResponsePtr> applyTemplate(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:add"});

        auto json = req->getJsonObject();
        if (!json) co_return Response::badRequest("请求体格式错误");

        ValidatorHelper::requirePositiveInt(*json, "template_id", "模板ID").throwIfInvalid();
        if (!json->isMember("device_ids") || !(*json)["device_ids"].isArray()) {
            co_return Response::badRequest("请提供 device_ids 数组");
        }

        int templateId = (*json)["template_id"].asInt();
        std::vector<int> deviceIds;
        for (const auto& idVal : (*json)["device_ids"]) {
            deviceIds.push_back(idVal.asInt());
        }

        if (deviceIds.empty()) co_return Response::badRequest("device_ids 不能为空");

        auto result = co_await service_.applyTemplate(templateId, deviceIds);
        co_return Response::ok(result, "应用模板成功");
    }

    // ==================== 聚合查询 ====================

    Task<HttpResponsePtr> getGroupedRecords(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        auto page = Pagination::fromRequest(req);

        int days = 7;
        auto daysStr = req->getParameter("days");
        if (!daysStr.empty()) {
            try { days = std::stoi(daysStr); } catch (...) {}
        }

        if (days < 1) days = 1;
        if (days > 90) days = 90;

        auto items = co_await service_.getGroupedRecords(days);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        co_return Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
    }

    Task<HttpResponsePtr> exportRecords(HttpRequestPtr req) {
        co_await PermissionChecker::checkPermission(ControllerUtils::getUserId(req), {"iot:alert:query"});

        auto page = Pagination::fromRequest(req);
        std::string startTime = req->getParameter("startTime");
        std::string endTime = req->getParameter("endTime");
        std::string severity = req->getParameter("severity");
        std::string status = req->getParameter("status");

        auto items = co_await service_.exportRecords(startTime, endTime, severity, status);
        auto [pagedItems, total] = Pagination::paginate(items, page);
        co_return Pagination::buildResponse(pagedItems, total, page.page, page.pageSize);
    }
};
