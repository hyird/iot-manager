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

std::string firstJsonString(const boost::json::object& object, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        const auto value = jsonString(object, key);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

bool isGb28181StreamId(const std::string& streamId) {
    return streamId.rfind("gb_", 0) == 0 || streamId.rfind("gb_playback_", 0) == 0;
}

std::string remoteIp(const std::string& remoteAddress) {
    const auto colon = remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return remoteAddress;
    }
    return remoteAddress.substr(0, colon);
}

std::string remotePort(const std::string& remoteAddress) {
    const auto colon = remoteAddress.rfind(':');
    if (colon == std::string::npos || colon + 1 >= remoteAddress.size()) {
        return {};
    }
    return remoteAddress.substr(colon + 1);
}

boost::json::object deviceToJson(const Device& device) {
    boost::json::array channels;
    for (const auto& channel : device.channels) {
        channels.push_back({
            {"id", channel.id},
            {"name", channel.name},
            {"manufacturer", channel.manufacturer},
            {"online", channel.online},
            {"ptz_type", channel.ptzType},
            {"ptz_capable", channel.ptzType > 0},
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
        {"remote_ip", remoteIp(device.remoteAddress)},
        {"remote_port", remotePort(device.remoteAddress)},
        {"registration_source", device.registrationSource},
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
            deviceRegistry_.forEachDevice([&](const Device& device) {
                devices.push_back(deviceToJson(device));
            });
            callback(jsonResponse({{"items", devices}}));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/devices/{1}",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId) {
            boost::json::object device;
            if (!deviceRegistry_.visitDevice(deviceId, [&](const Device& existing) {
                    device = deviceToJson(existing);
                })) {
                callback(jsonNotFound("设备不存在"));
                return;
            }
            callback(jsonResponse(device));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        prefix + "/devices/mock-register",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto deviceId = request->getParameter("device_id");
            if (deviceId.empty()) {
                deviceId = "34020000001320000001";
            }
            deviceRegistry_.upsertRegistration(deviceId, request->peerAddr().toIpPort(), "mock");
            callback(jsonResponse({{"registered", true}, {"device_id", deviceId}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/catalog/query",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId) {
            const auto sent = sipServer_.queryCatalog(deviceId);
            if (!sent) {
                callback(jsonNotFound("设备不在线"));
                return;
            }
            callback(jsonResponse({{"sent", true}, {"device_id", deviceId}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/devices/{1}/channels/{2}/preview/start",
        [this](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId, const std::string& channelId) {
            const auto result = sipServer_.startPreview(deviceId, channelId);
            if (!result.has_value()) {
                callback(jsonNotFound("设备或通道不可用"));
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
        prefix + "/devices/{1}/channels/{2}/ptz/{3}",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& deviceId, const std::string& channelId, const std::string& action) {
            const auto speedText = request->getParameter("speed");
            uint8_t speed = 80;
            if (!speedText.empty()) {
                speed = static_cast<uint8_t>(std::clamp(std::stoi(speedText), 0, 255));
            }
            const auto sent = sipServer_.sendPtzControl(deviceId, channelId, action, speed);
            if (!sent) {
                callback(jsonNotFound("设备或通道不可用"));
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
                callback(jsonBadRequest("缺少时间范围"));
                return;
            }
            const auto sent = sipServer_.queryRecords(deviceId, channelId, startTime, endTime);
            if (!sent) {
                callback(jsonNotFound("设备或通道不可用"));
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
                callback(jsonBadRequest("缺少时间范围"));
                return;
            }
            const auto result = sipServer_.startPlayback(deviceId, channelId, startTime, endTime);
            if (!result.has_value()) {
                callback(jsonNotFound("设备或通道不可用"));
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
                callback(jsonNotFound("预览会话不存在"));
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
                callback(jsonNotFound("流不存在"));
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
            callback(jsonBody({{"code", 0}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/zlm/hook/on_stream_none_reader",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            bool closeStream = false;
            boost::system::error_code error;
            const auto value = boost::json::parse(std::string(request->body()), error);
            if (!error && value.is_object()) {
                const auto object = value.as_object();
                const auto app = jsonString(object, "app");
                const auto stream = jsonString(object, "stream");
                const auto schema = jsonString(object, "schema");
                if (!stream.empty()) {
                    streamRegistry_.updateNoneReader(app, stream, schema);
                    const auto stopResult = sipServer_.stopPreviewByStream(stream);
                    if (stopResult.has_value()) {
                        closeStream = true;
                        LOG_INFO << "ZLM stream none reader closed GB28181 session, stream=" << stream
                                 << ", bye_sent=" << stopResult->byeSent
                                 << ", rtp_server_closed=" << stopResult->rtpServerClosed;
                    } else if (app == "rtp" && isGb28181StreamId(stream)) {
                        closeStream = sipServer_.forceCloseRtpServer(stream);
                        LOG_INFO << "ZLM stream none reader closed orphan GB28181 RTP stream, stream=" << stream;
                    } else {
                        LOG_INFO << "ZLM stream none reader for unmanaged stream, stream=" << stream;
                    }
                }
            }
            callback(jsonBody({{"code", 0}, {"close", closeStream}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/zlm/hook/on_rtp_server_timeout",
        [this](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            bool closed = false;
            boost::system::error_code error;
            const auto value = boost::json::parse(std::string(request->body()), error);
            if (!error && value.is_object()) {
                const auto object = value.as_object();
                const auto stream = firstJsonString(object, {"stream_id", "stream"});
                const auto ssrc = jsonString(object, "ssrc");
                const auto localPort = object.if_contains("local_port");
                if (!stream.empty()) {
                    const auto stopResult = sipServer_.stopPreviewByStream(stream);
                    if (stopResult.has_value()) {
                        closed = true;
                        LOG_WARN << "ZLM RTP server timeout closed GB28181 session, stream=" << stream
                                 << ", ssrc=" << ssrc
                                 << ", bye_sent=" << stopResult->byeSent
                                 << ", rtp_server_closed=" << stopResult->rtpServerClosed;
                    } else if (isGb28181StreamId(stream)) {
                        closed = sipServer_.forceCloseRtpServer(stream);
                        LOG_WARN << "ZLM RTP server timeout closed orphan GB28181 RTP stream, stream=" << stream
                                 << ", ssrc=" << ssrc;
                    } else {
                        LOG_INFO << "ZLM RTP server timeout for unmanaged stream, stream=" << stream
                                 << ", ssrc=" << ssrc;
                    }
                    if (localPort != nullptr && localPort->is_int64()) {
                        LOG_INFO << "ZLM RTP server timeout local_port=" << localPort->as_int64()
                                 << ", stream=" << stream;
                    }
                }
            }
            callback(jsonBody({{"code", 0}, {"closed", closed}}));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        prefix + "/zlm/hook/on_send_rtp_stopped",
        [](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            boost::system::error_code error;
            const auto value = boost::json::parse(std::string(request->body()), error);
            if (!error && value.is_object()) {
                const auto object = value.as_object();
                const auto stream = firstJsonString(object, {"stream_id", "stream"});
                const auto app = jsonString(object, "app");
                const auto ssrc = jsonString(object, "ssrc");
                const auto dstUrl = firstJsonString(object, {"dst_url", "dstUrl", "dst_ip"});
                LOG_INFO << "ZLM send RTP stopped, app=" << app
                         << ", stream=" << stream
                         << ", ssrc=" << ssrc
                         << ", dst=" << dstUrl;
            }
            callback(jsonBody({{"code", 0}}));
        },
        {drogon::Post});
}
