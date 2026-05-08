#include "api/ApiServer.h"

#include "api/JsonResponse.h"

#include <boost/json.hpp>
#include <trantor/utils/Logger.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <utility>

namespace {

std::string jsonString(const boost::json::object& object, const char* key, const std::string& fallback = {}) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        return fallback;
    }
    return std::string(value->as_string());
}

bool jsonBoolish(const boost::json::object& object, const char* key, bool fallback = false) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_bool()) {
        return value->as_bool();
    }
    if (value->is_int64()) {
        return value->as_int64() != 0;
    }
    return fallback;
}

boost::json::object deviceToJson(const Device& device) {
    boost::json::array channels;
    for (const auto& channel : device.channels) {
        channels.push_back({
            {"id", channel.id},
            {"name", channel.name},
            {"manufacturer", channel.manufacturer},
            {"online", channel.online},
        });
    }
    boost::json::array records;
    for (const auto& record : device.records) {
        records.push_back({
            {"device_id", record.deviceId},
            {"name", record.name},
            {"file_path", record.filePath},
            {"address", record.address},
            {"start_time", record.startTime},
            {"end_time", record.endTime},
            {"type", record.type},
            {"recorder_id", record.recorderId},
        });
    }

    return {
        {"id", device.id},
        {"name", device.name},
        {"manufacturer", device.manufacturer},
        {"remote_address", device.remoteAddress},
        {"online", device.online},
        {"channels", channels},
        {"records", records},
    };
}

boost::json::object streamToJson(const StreamStatus& status) {
    return {
        {"app", status.app},
        {"stream", status.stream},
        {"schema", status.schema},
        {"online", status.online},
        {"reader_count", status.readerCount},
    };
}

} // namespace

std::string normalizePrefix(std::string prefix) {
    if (prefix.empty()) {
        prefix = "/api/gb28181";
    }
    if (prefix.front() != '/') {
        prefix.insert(prefix.begin(), '/');
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.pop_back();
    }
    return prefix;
}

ApiServer::ApiServer(AppConfig config, DeviceRegistry& deviceRegistry, StreamRegistry& streamRegistry, SipServer& sipServer)
    : config_(std::move(config)), deviceRegistry_(deviceRegistry), streamRegistry_(streamRegistry), sipServer_(sipServer) {}

void ApiServer::registerRoutes(const std::string& apiPrefix) {
    const auto prefix = normalizePrefix(apiPrefix);

    drogon::app().registerHandler(
        prefix + "/health",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(jsonResponse({
                {"status", "ok"},
                {"service", "iot-manager-gb28181"},
            }));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/config/sip",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(jsonResponse({
                {"domain", config_.sip.domain},
                {"id", config_.sip.id},
                {"host", config_.sip.host},
                {"public_ip", config_.sip.publicIp},
                {"port", config_.sip.port},
                {"transport", config_.sip.transport},
            }));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/devices",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            boost::json::array devices;
            for (const auto& device : deviceRegistry_.listDevices()) {
                devices.push_back(deviceToJson(device));
            }
            callback(jsonResponse({{"items", devices}}));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/devices/{1}",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId) {
            const auto device = deviceRegistry_.findDevice(deviceId);
            if (!device.has_value()) {
                auto response = jsonResponse({{"error", "device_not_found"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse(deviceToJson(*device)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/devices/mock-register",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto deviceId = request->getParameter("device_id");
            if (deviceId.empty()) {
                deviceId = "34020000001320000001";
            }
            deviceRegistry_.upsertRegistration(deviceId, request->peerAddr().toIpPort());
            callback(jsonResponse({{"registered", true}, {"device_id", deviceId}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/catalog/query",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId) {
            const auto sent = sipServer_.queryCatalog(deviceId);
            if (!sent) {
                auto response = jsonResponse({{"error", "device_not_online"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse({{"sent", true}, {"device_id", deviceId}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/channels/{2}/preview/start",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId, const std::string& channelId) {
            const auto previousSessionId = request->getParameter("previous_session_id");
            const auto result = sipServer_.startPreview(deviceId, channelId);
            if (!result.has_value()) {
                auto response = jsonResponse({{"error", "device_or_channel_not_available"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            if (!previousSessionId.empty() && previousSessionId != result->sessionId) {
                sipServer_.stopPreview(previousSessionId);
            }
            callback(jsonResponse({
                {"sent", true},
                {"session_id", result->sessionId},
                {"device_id", result->deviceId},
                {"channel_id", result->channelId},
                {"stream_id", result->streamId},
                {"ssrc", result->ssrc},
                {"rtp_port", result->rtpPort},
                {"play_urls", {
                    {"http_flv", result->playUrls.httpFlv},
                    {"ws_flv", result->playUrls.wsFlv},
                    {"http_ts", result->playUrls.httpTs},
                    {"hls", result->playUrls.hls},
                    {"webrtc", result->playUrls.webRtc},
                    {"rtsp", result->playUrls.rtsp},
                }},
            }));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/channels/{2}/ptz/{3}",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId, const std::string& channelId, const std::string& action) {
            const auto speedText = request->getParameter("speed");
            uint8_t speed = 80;
            if (!speedText.empty()) {
                speed = static_cast<uint8_t>(std::clamp(std::stoi(speedText), 0, 255));
            }
            const auto sent = sipServer_.sendPtzControl(deviceId, channelId, action, speed);
            if (!sent) {
                auto response = jsonResponse({{"error", "device_or_channel_not_available"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse({{"sent", true}, {"action", action}, {"speed", speed}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/channels/{2}/records/query",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId, const std::string& channelId) {
            const auto startTime = request->getParameter("start_time");
            const auto endTime = request->getParameter("end_time");
            if (startTime.empty() || endTime.empty()) {
                auto response = jsonResponse({{"error", "missing_time_range"}});
                response->setStatusCode(drogon::k400BadRequest);
                callback(response);
                return;
            }
            const auto sent = sipServer_.queryRecords(deviceId, channelId, startTime, endTime);
            if (!sent) {
                auto response = jsonResponse({{"error", "device_or_channel_not_available"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse({{"sent", true}, {"device_id", deviceId}, {"channel_id", channelId}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/channels/{2}/playback/start",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId, const std::string& channelId) {
            const auto startTime = request->getParameter("start_time");
            const auto endTime = request->getParameter("end_time");
            if (startTime.empty() || endTime.empty()) {
                auto response = jsonResponse({{"error", "missing_time_range"}});
                response->setStatusCode(drogon::k400BadRequest);
                callback(response);
                return;
            }
            const auto result = sipServer_.startPlayback(deviceId, channelId, startTime, endTime);
            if (!result.has_value()) {
                auto response = jsonResponse({{"error", "device_or_channel_not_available"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse({
                {"sent", true},
                {"session_id", result->sessionId},
                {"device_id", result->deviceId},
                {"channel_id", result->channelId},
                {"stream_id", result->streamId},
                {"ssrc", result->ssrc},
                {"rtp_port", result->rtpPort},
                {"play_urls", {
                    {"http_flv", result->playUrls.httpFlv},
                    {"ws_flv", result->playUrls.wsFlv},
                    {"http_ts", result->playUrls.httpTs},
                    {"hls", result->playUrls.hls},
                    {"webrtc", result->playUrls.webRtc},
                    {"rtsp", result->playUrls.rtsp},
                }},
            }));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/streams",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            boost::json::array streams;
            for (const auto& stream : streamRegistry_.listStreams()) {
                streams.push_back(streamToJson(stream));
            }
            callback(jsonResponse({{"items", streams}}));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/previews/{1}/stop",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& sessionId) {
            const auto result = sipServer_.stopPreview(sessionId);
            if (!result.has_value()) {
                auto response = jsonResponse({{"error", "preview_session_not_found"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse({
                {"stopped", true},
                {"session_id", result->sessionId},
                {"stream_id", result->streamId},
                {"bye_sent", result->byeSent},
                {"rtp_server_closed", result->rtpServerClosed},
            }));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/streams/{1}",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& streamId) {
            const auto stream = streamRegistry_.findStream(streamId);
            if (!stream.has_value()) {
                auto response = jsonResponse({{"error", "stream_not_found"}});
                response->setStatusCode(drogon::k404NotFound);
                callback(response);
                return;
            }
            callback(jsonResponse(streamToJson(*stream)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/zlm/hook/on_stream_changed",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            boost::system::error_code error;
            const auto value = boost::json::parse(std::string(request->body()), error);
            if (!error && value.is_object()) {
                const auto object = value.as_object();
                const auto app = jsonString(object, "app");
                const auto stream = jsonString(object, "stream");
                const auto schema = jsonString(object, "schema");
                const auto online = jsonBoolish(object, "regist", true);
                if (!stream.empty()) {
                    streamRegistry_.updateStreamChanged(app, stream, schema, online);
                    sipServer_.markStreamOnline(stream, online);
                    LOG_INFO << "ZLM stream changed, stream=" << stream << ", online=" << online;
                }
            }
            callback(jsonResponse({{"code", 0}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/zlm/hook/on_stream_none_reader",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            boost::system::error_code error;
            const auto value = boost::json::parse(std::string(request->body()), error);
            if (!error && value.is_object()) {
                const auto object = value.as_object();
                const auto app = jsonString(object, "app");
                const auto stream = jsonString(object, "stream");
                const auto schema = jsonString(object, "schema");
                if (!stream.empty()) {
                    streamRegistry_.updateNoneReader(app, stream, schema);
                    LOG_INFO << "ZLM stream none reader, stream=" << stream;
                }
            }
            callback(jsonResponse({{"code", 0}, {"close", false}}));
        },
        {drogon::Post});
}
