#pragma once

#include "OpenAccess.Repository.hpp"
#include "modules/alert/domain/Events.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/protocol/FrameResult.hpp"
#include "common/utils/DeviceSummaryHelper.hpp"
#include "common/utils/OperationFieldHelper.hpp"

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

                    for (const auto& target : targets) {
                        if (!target.deviceIds.contains(frame.deviceId)) continue;

                        for (const auto& eventType : events) {
                            if (!target.supportsEvent(eventType)) continue;

                            Json::Value payload = buildBasePayload(frame, device, eventType);
                            payload["deliveryId"] = drogon::utils::getUuid();
                            payload["webhookId"] = target.id;
                            payload["accessKeyId"] = target.accessKeyId;
                            payload["accessKeyName"] = target.accessKeyName;

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
        alert["recordId"] = static_cast<Json::Int64>(event.recordId);
        alert["ruleId"] = event.ruleId;
        alert["severity"] = event.severity;
        alert["message"] = event.message;
        dispatchAlertEvent(event.deviceId, OpenAccess::WEBHOOK_EVENT_DEVICE_ALERT_TRIGGERED, std::move(alert));
    }

    void dispatchAlertResolved(const AlertResolved& event) {
        Json::Value alert;
        alert["recordId"] = static_cast<Json::Int64>(event.recordId);
        alert["ruleId"] = event.ruleId;
        alert["reason"] = event.reason;
        alert["recoveryData"] = event.recoveryData;
        dispatchAlertEvent(event.deviceId, OpenAccess::WEBHOOK_EVENT_DEVICE_ALERT_RESOLVED, std::move(alert));
    }

private:
    static Json::Value stripProtocolOperationFields(const Json::Value& value) {
        if (value.isArray()) {
            Json::Value cleaned(Json::arrayValue);
            for (const auto& item : value) {
                cleaned.append(stripProtocolOperationFields(item));
            }
            return cleaned;
        }

        if (!value.isObject()) {
            return value;
        }

        Json::Value cleaned(Json::objectValue);
        for (const auto& memberName : value.getMemberNames()) {
            if (memberName == OperationFieldHelper::FIELD_OPERATION_KEY
                || memberName == OperationFieldHelper::FIELD_OPERATION_NAME
                || memberName == OperationFieldHelper::LEGACY_FIELD_FUNC_CODE
                || memberName == OperationFieldHelper::LEGACY_FIELD_FUNC_NAME) {
                continue;
            }
            cleaned[memberName] = stripProtocolOperationFields(value[memberName]);
        }
        return cleaned;
    }

    static Json::Value buildDeviceJson(
        int deviceId,
        const DeviceCache::CachedDevice* device
    ) {
        return DeviceSummaryHelper::build(device, deviceId);
    }

    static Json::Value buildBasePayload(
        const ParsedFrameResult& frame,
        const DeviceCache::CachedDevice* device,
        const std::string& eventType
    ) {
        Json::Value payload;
        payload["event"] = eventType;
        payload["timestamp"] = OpenAccess::nowIso8601();
        payload["reportTime"] = frame.reportTime.empty()
            ? Json::nullValue
            : Json::Value(frame.reportTime);
        payload["data"] = stripProtocolOperationFields(
            OperationFieldHelper::normalizedOperationFields(frame.data, true)
        );
        payload["device"] = buildDeviceJson(frame.deviceId, device);

        if (frame.commandCompletion) {
            Json::Value command;
            command["commandKey"] = frame.commandCompletion->commandKey;
            command["responseCode"] = frame.commandCompletion->responseCode;
            command["success"] = frame.commandCompletion->success;
            payload["command"] = std::move(command);
        }

        return payload;
    }

    static Json::Value buildAlertPayload(
        int deviceId,
        const DeviceCache::CachedDevice* device,
        const std::string& eventType,
        Json::Value alert
    ) {
        Json::Value payload;
        payload["event"] = eventType;
        payload["timestamp"] = OpenAccess::nowIso8601();
        payload["device"] = buildDeviceJson(deviceId, device);
        payload["alert"] = std::move(alert);
        return payload;
    }

    static Json::Value buildCommandPayload(
        int deviceId,
        const DeviceCache::CachedDevice* device,
        const std::string& eventType,
        const Json::Value& elements
    ) {
        Json::Value payload;
        payload["event"] = eventType;
        payload["timestamp"] = OpenAccess::nowIso8601();
        payload["device"] = buildDeviceJson(deviceId, device);

        Json::Value command(Json::objectValue);
        command["elements"] = elements;
        payload["command"] = command;

        Json::Value data(Json::objectValue);
        data["direction"] = "DOWN";
        data["elements"] = elements;
        payload["data"] = std::move(data);

        return payload;
    }

    static std::set<std::string> resolveEvents(const ParsedFrameResult& frame) {
        std::set<std::string> events = {OpenAccess::WEBHOOK_EVENT_DEVICE_DATA};

        if (frame.data.isMember("image") && frame.data["image"].isObject()) {
            const auto& image = frame.data["image"];
            if (image.isMember("data") && !image["data"].asString().empty()) {
                events.insert(OpenAccess::WEBHOOK_EVENT_DEVICE_IMAGE);
            }
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

                    Json::Value payload = buildAlertPayload(deviceId, targetDevice, eventType, alert);
                    payload["deliveryId"] = drogon::utils::getUuid();
                    payload["webhookId"] = target.id;
                    payload["accessKeyId"] = target.accessKeyId;
                    payload["accessKeyName"] = target.accessKeyName;

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
                    payload["deliveryId"] = drogon::utils::getUuid();
                    payload["webhookId"] = target.id;
                    payload["accessKeyId"] = target.accessKeyId;
                    payload["accessKeyName"] = target.accessKeyName;

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
                auto client = drogon::HttpClient::newHttpClient(parsedUrl.baseUrl, drogon::app().getLoop());
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
