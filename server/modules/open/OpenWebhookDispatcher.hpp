#pragma once

#include "OpenAccess.DataTransformer.hpp"
#include "OpenAccess.Repository.hpp"
#include "modules/alert/domain/Events.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/protocol/FrameResult.hpp"
#include "common/utils/DrogonLoopSelector.hpp"

#include <map>
#include <set>

class OpenWebhookDispatcher {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    static OpenWebhookDispatcher& instance() {
        static OpenWebhookDispatcher dispatcher;
        return dispatcher;
    }

    void dispatch(const std::vector<ParsedFrameResult>& batch) {
        if (batch.empty()) return;

        drogon::async_run([batch]() -> Task<void> {
            try {
                std::set<int> deviceIds;
                for (const auto& item : batch) {
                    if (item.deviceId > 0) {
                        deviceIds.insert(item.deviceId);
                    }
                }
                if (deviceIds.empty()) co_return;

                OpenAccessRepository repository;
                auto targets = co_await repository.listActiveWebhookTargets(deviceIds);
                if (targets.empty()) co_return;

                auto devices = co_await DeviceCache::instance().getDevices();
                std::map<int, const DeviceCache::CachedDevice*> deviceMap;
                for (const auto& device : devices) {
                    deviceMap[device.id] = &device;
                }

                for (const auto& frame : batch) {
                    auto deviceIt = deviceMap.find(frame.deviceId);
                    const DeviceCache::CachedDevice* device =
                        deviceIt == deviceMap.end() ? nullptr : deviceIt->second;
                    auto events = resolveEvents(frame);
                    if (events.empty()) continue;

                    for (const auto& target : targets) {
                        if (!target.deviceIds.contains(frame.deviceId)) continue;

                        for (const auto& eventType : events) {
                            if (!target.supportsEvent(eventType)) continue;

                            Json::Value payload = buildWebhookPayload(
                                eventType,
                                buildFrameEventData(frame, device, eventType)
                            );

                            std::string deviceCode = device ? device->deviceCode : "";
                            std::string body = JsonHelper::serialize(payload);
                            scheduleDelivery(target, eventType, frame.deviceId, deviceCode, std::move(body));
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN << "[OpenWebhook] dispatch failed: " << e.what();
            }
        });
    }

    void dispatchMergedDataReports(const std::vector<ParsedFrameResult>& batch) {
        if (batch.empty()) return;

        drogon::async_run([batch]() -> Task<void> {
            try {
                std::set<int> deviceIds;
                for (const auto& item : batch) {
                    if (item.deviceId > 0 && isElementDataReport(item)) {
                        deviceIds.insert(item.deviceId);
                    }
                }
                if (deviceIds.empty()) co_return;

                OpenAccessRepository repository;
                auto targets = co_await repository.listActiveWebhookTargets(deviceIds);
                if (targets.empty()) co_return;

                std::vector<int> deviceIdList(deviceIds.begin(), deviceIds.end());
                auto devices = co_await DeviceCache::instance().getDevices();
                auto dataMap = co_await RealtimeDataCache::instance().getBatch(deviceIdList);

                std::map<int, const DeviceCache::CachedDevice*> deviceMap;
                for (const auto& device : devices) {
                    deviceMap[device.id] = &device;
                }

                const std::string eventType = OpenAccess::WEBHOOK_EVENT_DEVICE_DATA;
                for (int deviceId : deviceIds) {
                    auto deviceIt = deviceMap.find(deviceId);
                    if (deviceIt == deviceMap.end()) continue;

                    RealtimeDataCache::DeviceRealtimeData emptyData;
                    auto dataIt = dataMap.find(deviceId);
                    const auto* device = deviceIt->second;
                    const auto& deviceData = dataIt == dataMap.end() ? emptyData : dataIt->second;
                    Json::Value mergedData = OpenAccessDataTransformer::buildDataItem(*device, deviceData);

                    for (const auto& target : targets) {
                        if (!target.deviceIds.contains(deviceId)) continue;
                        if (!target.supportsEvent(eventType)) continue;

                        Json::Value payload = buildWebhookPayload(eventType, mergedData);

                        std::string body = JsonHelper::serialize(payload);
                        scheduleDelivery(target, eventType, deviceId, device->deviceCode, std::move(body));
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN << "[OpenWebhook] merged data dispatch failed: " << e.what();
            }
        });
    }

    void dispatchCommandDispatched(int deviceId, const std::string&, Json::Value elements) {
        if (deviceId <= 0) return;
        dispatchCommandEvent(
            deviceId,
            OpenAccess::WEBHOOK_EVENT_DEVICE_COMMAND_DISPATCHED,
            std::move(elements)
        );
    }

    void dispatchAlertTriggered(const AlertTriggered& event) {
        Json::Value alert;
        alert["id"] = static_cast<Json::Int64>(event.recordId);
        alert["ruleId"] = event.ruleId;
        alert["severity"] = event.severity;
        alert["status"] = "active";
        alert["message"] = event.message;
        alert["time"] = OpenAccess::nowIso8601();
        dispatchAlertEvent(event.deviceId, OpenAccess::WEBHOOK_EVENT_DEVICE_ALERT_TRIGGERED, std::move(alert));
    }

    void dispatchAlertResolved(const AlertResolved& event) {
        Json::Value alert;
        alert["id"] = static_cast<Json::Int64>(event.recordId);
        alert["ruleId"] = event.ruleId;
        alert["severity"] = "";
        alert["status"] = "resolved";
        alert["message"] = event.reason;
        alert["time"] = OpenAccess::nowIso8601();
        dispatchAlertEvent(event.deviceId, OpenAccess::WEBHOOK_EVENT_DEVICE_ALERT_RESOLVED, std::move(alert));
    }

private:
    static Json::Value buildDeviceRef(
        int deviceId,
        const DeviceCache::CachedDevice* device
    ) {
        if (device) {
            return OpenAccessDataTransformer::buildDeviceRef(*device);
        }
        return OpenAccessDataTransformer::buildDeviceRef(deviceId, "", "");
    }

    static Json::Value buildWebhookPayload(const std::string& eventType, Json::Value data) {
        Json::Value payload;
        payload["event"] = eventType;
        payload["time"] = OpenAccess::nowIso8601();
        payload["deliveryId"] = drogon::utils::getUuid();
        payload["data"] = std::move(data);
        return payload;
    }

    static bool isImageFrame(const ParsedFrameResult& frame) {
        if (frame.data.isMember("image") && frame.data["image"].isObject()) {
            const auto& image = frame.data["image"];
            return image.isMember("data") && !image["data"].asString().empty();
        }

        if (!frame.data.isMember("data") || !frame.data["data"].isObject()) {
            return false;
        }

        for (const auto& key : frame.data["data"].getMemberNames()) {
            const auto& element = frame.data["data"][key];
            if (!element.isObject()) continue;
            if (element.get("type", "").asString() == "JPEG") return true;
            const std::string value = element.get("value", "").asString();
            if (value.rfind("data:image/", 0) == 0) return true;
        }

        return false;
    }

    static bool isElementDataReport(const ParsedFrameResult& frame) {
        if (frame.data.get("direction", "UP").asString() == "DOWN") return false;
        if (frame.data.get("funcCode", "").asString() == "36"
            || frame.data.get("funcCode", "").asString() == "B6") {
            return false;
        }
        if (isImageFrame(frame)) return false;
        return frame.data.isMember("data")
            && frame.data["data"].isObject()
            && !frame.data["data"].empty();
    }

    static Json::Value buildFrameEventData(
        const ParsedFrameResult& frame,
        const DeviceCache::CachedDevice* device,
        const std::string& eventType
    ) {
        if (eventType == OpenAccess::WEBHOOK_EVENT_DEVICE_IMAGE) {
            return buildImageData(frame, device);
        }
        return buildCommandResponseData(frame, device);
    }

    static Json::Value buildImageData(
        const ParsedFrameResult& frame,
        const DeviceCache::CachedDevice* device
    ) {
        Json::Value data(Json::objectValue);
        data["device"] = buildDeviceRef(frame.deviceId, device);

        Json::Value image(Json::objectValue);
        if (frame.data.isMember("image") && frame.data["image"].isObject()) {
            const auto& source = frame.data["image"];
            if (source.isMember("data")) {
                image["data"] = source["data"];
            }
            image["id"] = source.get("id", frame.funcCode);
            image["name"] = source.get("name", "image");
        } else if (frame.data.isMember("data") && frame.data["data"].isObject()) {
            for (const auto& key : frame.data["data"].getMemberNames()) {
                const auto& element = frame.data["data"][key];
                if (!element.isObject()) continue;
                const bool isImage = element.get("type", "").asString() == "JPEG"
                    || element.get("value", "").asString().rfind("data:image/", 0) == 0;
                if (!isImage) continue;

                image["id"] = element.get("elementId", key);
                image["name"] = element.get("name", "image");
                image["data"] = element.get("value", "");
                break;
            }
        }
        image["time"] = frame.reportTime.empty()
            ? Json::Value(Json::nullValue)
            : Json::Value(frame.reportTime);
        data["image"] = std::move(image);
        return data;
    }

    static Json::Value buildCommandResponseData(
        const ParsedFrameResult& frame,
        const DeviceCache::CachedDevice* device
    ) {
        Json::Value data(Json::objectValue);
        data["device"] = buildDeviceRef(frame.deviceId, device);

        Json::Value command(Json::objectValue);
        if (frame.commandCompletion) {
            command["key"] = frame.commandCompletion->commandKey;
            command["responseCode"] = frame.commandCompletion->responseCode;
            command["success"] = frame.commandCompletion->success;
        } else {
            command["success"] = frame.data.get("direction", "").asString() != "DOWN";
            if (frame.data.isMember("responseId")) {
                command["responseId"] = frame.data["responseId"];
            }
        }
        data["command"] = std::move(command);

        if (device && frame.data.isMember("data") && frame.data["data"].isObject()) {
            data["points"] = OpenAccessDataTransformer::buildDataItem(
                *device,
                frame.data["data"],
                frame.reportTime
            )["points"];
        } else {
            data["points"] = Json::Value(Json::arrayValue);
        }
        return data;
    }

    static Json::Value buildCommandPayload(
        int deviceId,
        const DeviceCache::CachedDevice* device,
        const std::string& eventType,
        const Json::Value& elements
    ) {
        Json::Value data(Json::objectValue);
        data["accepted"] = true;
        data["device"] = buildDeviceRef(deviceId, device);
        Json::Value command(Json::objectValue);
        command["elements"] = elements;
        data["command"] = command;
        return buildWebhookPayload(eventType, std::move(data));
    }

    static std::set<std::string> resolveEvents(const ParsedFrameResult& frame) {
        std::set<std::string> events;

        if (isImageFrame(frame)) {
            events.insert(OpenAccess::WEBHOOK_EVENT_DEVICE_IMAGE);
        }

        std::string direction = frame.data.get("direction", "").asString();
        if (frame.commandCompletion || direction == "DOWN" || frame.data.isMember("responseId")) {
            events.insert(OpenAccess::WEBHOOK_EVENT_DEVICE_COMMAND_RESPONSE);
        }

        return events;
    }

    static void dispatchAlertEvent(
        int deviceId,
        const std::string& eventType,
        Json::Value alert
    ) {
        if (deviceId <= 0) return;

        drogon::async_run([deviceId, eventType, alert = std::move(alert)]() -> Task<void> {
            try {
                OpenAccessRepository repository;
                auto targets = co_await repository.listActiveWebhookTargets({deviceId});
                if (targets.empty()) co_return;

                auto devices = co_await DeviceCache::instance().getDevices();
                const DeviceCache::CachedDevice* targetDevice = nullptr;
                for (const auto& device : devices) {
                    if (device.id == deviceId) {
                        targetDevice = &device;
                        break;
                    }
                }

                for (const auto& target : targets) {
                    if (!target.deviceIds.contains(deviceId)) continue;
                    if (!target.supportsEvent(eventType)) continue;

                    Json::Value data = OpenAccessDataTransformer::buildAlertItem(
                        alert.get("id", 0).asInt64(),
                        deviceId,
                        targetDevice ? targetDevice->deviceCode : "",
                        targetDevice ? targetDevice->name : "",
                        alert.get("ruleId", 0).asInt(),
                        alert.get("severity", "").asString(),
                        alert.get("status", "").asString(),
                        alert.get("message", "").asString(),
                        alert.get("time", "").asString()
                    );
                    Json::Value payload = buildWebhookPayload(eventType, std::move(data));

                    std::string deviceCode = targetDevice ? targetDevice->deviceCode : "";
                    std::string body = JsonHelper::serialize(payload);
                    scheduleDelivery(target, eventType, deviceId, deviceCode, std::move(body));
                }
            } catch (const std::exception& e) {
                LOG_WARN << "[OpenWebhook] alert dispatch failed: " << e.what();
            }
        });
    }

    static void dispatchCommandEvent(
        int deviceId,
        const std::string& eventType,
        Json::Value elements
    ) {
        if (deviceId <= 0) return;

        drogon::async_run([
            deviceId,
            eventType,
            elements = std::move(elements)
        ]() -> Task<void> {
            try {
                OpenAccessRepository repository;
                auto targets = co_await repository.listActiveWebhookTargets({deviceId});
                if (targets.empty()) co_return;

                auto devices = co_await DeviceCache::instance().getDevices();
                const DeviceCache::CachedDevice* targetDevice = nullptr;
                for (const auto& device : devices) {
                    if (device.id == deviceId) {
                        targetDevice = &device;
                        break;
                    }
                }

                for (const auto& target : targets) {
                    if (!target.deviceIds.contains(deviceId)) continue;
                    if (!target.supportsEvent(eventType)) continue;

                    Json::Value payload = buildCommandPayload(
                        deviceId,
                        targetDevice,
                        eventType,
                        elements
                    );
                    std::string deviceCode = targetDevice ? targetDevice->deviceCode : "";
                    std::string body = JsonHelper::serialize(payload);
                    scheduleDelivery(target, eventType, deviceId, deviceCode, std::move(body));
                }
            } catch (const std::exception& e) {
                LOG_WARN << "[OpenWebhook] command dispatch failed: " << e.what();
            }
        });
    }

    static void scheduleDelivery(
        const OpenAccess::WebhookTarget& target,
        const std::string& eventType,
        int deviceId,
        const std::string& deviceCode,
        std::string body
    ) {
        drogon::async_run([target, eventType, deviceId, deviceCode, body = std::move(body)]() -> Task<void> {
            int httpStatus = 0;
            std::string failureReason;
            Json::Value requestPayload = OpenAccess::parseJsonOrDefault(
                body,
                Json::Value(Json::objectValue)
            );
            try {
                auto parsedUrl = OpenAccess::parseWebhookUrl(target.url);
                auto client = drogon::HttpClient::newHttpClient(parsedUrl.baseUrl, DrogonLoopSelector::getNext());
                client->setUserAgent("iot-manager-webhook/1.0");

                auto req = drogon::HttpRequest::newHttpRequest();
                req->setMethod(drogon::Post);
                req->setPath(parsedUrl.path);
                req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                req->setBody(body);

                if (target.headers.isObject()) {
                    for (const auto& name : target.headers.getMemberNames()) {
                        const auto& value = target.headers[name];
                        if (value.isObject() || value.isArray() || value.isNull()) continue;
                        req->addHeader(
                            name,
                            value.isString() ? value.asString() : JsonHelper::serialize(value)
                        );
                    }
                }

                std::string requestTs = OpenAccess::nowIso8601();
                req->addHeader("X-IOT-Event", eventType);
                req->addHeader("X-IOT-Timestamp", requestTs);
                if (!target.secret.empty()) {
                    req->addHeader(
                        "X-IOT-Signature",
                        "sha256=" + OpenAccess::hmacSha256Hex(target.secret, body)
                    );
                }

                auto resp = co_await client->sendRequestCoro(req, static_cast<double>(target.timeoutSeconds));
                if (!resp) {
                    failureReason = "Webhook 无响应";
                } else {
                    httpStatus = static_cast<int>(resp->getStatusCode());
                    if (httpStatus >= 200 && httpStatus < 300) {
                        co_await OpenAccessRepository::markWebhookSuccess(target.id, httpStatus);
                        Json::Value responsePayload;
                        responsePayload["httpStatus"] = httpStatus;
                        co_await OpenAccessRepository::writeAccessLog(
                            "push",
                            "webhook",
                            target.accessKeyId,
                            target.id,
                            eventType,
                            "success",
                            "POST",
                            target.url,
                            "",
                            httpStatus,
                            deviceId,
                            deviceCode,
                            "",
                            requestPayload,
                            responsePayload
                        );
                        co_return;
                    }

                    failureReason = "HTTP " + std::to_string(httpStatus) + " " +
                        OpenAccess::sanitizeError(std::string(resp->getBody()));
                }
            } catch (const std::exception& e) {
                failureReason = e.what();
            }

            if (!failureReason.empty()) {
                co_await OpenAccessRepository::markWebhookFailure(
                    target.id, failureReason, httpStatus);
                Json::Value responsePayload;
                if (httpStatus > 0) {
                    responsePayload["httpStatus"] = httpStatus;
                }
                responsePayload["error"] = failureReason;
                co_await OpenAccessRepository::writeAccessLog(
                    "push",
                    "webhook",
                    target.accessKeyId,
                    target.id,
                    eventType,
                    "failed",
                    "POST",
                    target.url,
                    "",
                    httpStatus,
                    deviceId,
                    deviceCode,
                    failureReason,
                    requestPayload,
                    responsePayload
                );
            }
        });
    }
};
