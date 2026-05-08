#include "media/ZlmClient.h"

#include <boost/json.hpp>
#include <trantor/utils/Logger.h>
#include <drogon/drogon.h>

#include <chrono>
#include <future>
#include <memory>
#include <sstream>
#include <utility>

namespace {

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string httpToWs(std::string value) {
    if (value.rfind("http://", 0) == 0) {
        value.replace(0, 4, "ws");
    } else if (value.rfind("https://", 0) == 0) {
        value.replace(0, 5, "wss");
    }
    return value;
}

std::string httpToRtsp(std::string value) {
    if (value.rfind("http://", 0) == 0) {
        value.replace(0, 4, "rtsp");
    } else if (value.rfind("https://", 0) == 0) {
        value.replace(0, 5, "rtsp");
    }
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) {
        const auto hostStart = scheme + 3;
        const auto path = value.find('/', hostStart);
        auto authority = value.substr(hostStart, path == std::string::npos ? std::string::npos : path - hostStart);
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            authority = authority.substr(0, colon);
        }
        return value.substr(0, hostStart) + authority + ":8554";
    }
    return value;
}

std::optional<boost::json::object> parseObject(const std::string& body) {
    boost::system::error_code error;
    auto value = boost::json::parse(body, error);
    if (error || !value.is_object()) {
        return std::nullopt;
    }
    return value.as_object();
}

} // namespace

ZlmClient::ZlmClient(MediaConfig config)
    : config_(std::move(config)) {
    nextRtpPort_.store(config_.rtpPortRangeStart);
}

std::optional<OpenRtpServerResult> ZlmClient::openRtpServer(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) {
    const auto baseUrl = trimTrailingSlash(config_.zlmBaseUrl);
    const auto streamId = makeStreamId(deviceId, channelId, ssrc, mode);
    const auto requestedPort = allocateRtpPort();
    auto client = drogon::HttpClient::newHttpClient(baseUrl);

    std::ostringstream path;
    path << "/index/api/openRtpServer?secret=" << config_.zlmSecret
         << "&port=" << requestedPort
         << "&enable_tcp=1"
         << "&stream_id=" << streamId;

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(path.str());

    auto promise = std::make_shared<std::promise<std::optional<OpenRtpServerResult>>>();
    auto future = promise->get_future();

    client->sendRequest(request, [this, streamId, requestedPort, promise](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
        if (result != drogon::ReqResult::Ok || response == nullptr) {
            LOG_WARN << "ZLM openRtpServer request failed";
            promise->set_value(std::nullopt);
            return;
        }

        const auto parsed = parseObject(std::string(response->body()));
        if (!parsed.has_value()) {
            LOG_WARN << "ZLM openRtpServer returned invalid JSON: " << response->body();
            promise->set_value(std::nullopt);
            return;
        }

        const auto& object = *parsed;
        const auto code = object.if_contains("code");
        const auto port = object.if_contains("port");
        if (code == nullptr || !code->is_int64() || code->as_int64() != 0) {
            LOG_WARN << "ZLM openRtpServer failed: " << response->body();
            promise->set_value(std::nullopt);
            return;
        }

        OpenRtpServerResult openResult;
        openResult.streamId = streamId;
        openResult.port = port != nullptr && port->is_int64() ? static_cast<uint16_t>(port->as_int64()) : requestedPort;
        openResult.playUrls = buildPlayUrls(streamId);
        promise->set_value(openResult);
    });

    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        LOG_WARN << "ZLM openRtpServer timed out";
        return std::nullopt;
    }
    return future.get();
}

bool ZlmClient::closeRtpServer(const std::string& streamId) {
    const auto baseUrl = trimTrailingSlash(config_.zlmBaseUrl);
    auto client = drogon::HttpClient::newHttpClient(baseUrl);

    std::ostringstream path;
    path << "/index/api/closeRtpServer?secret=" << config_.zlmSecret
         << "&stream_id=" << streamId;

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(path.str());

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    client->sendRequest(request, [promise](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
        promise->set_value(result == drogon::ReqResult::Ok && response != nullptr);
    });

    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        LOG_WARN << "ZLM closeRtpServer timed out";
        return false;
    }
    return future.get();
}

PlayUrls ZlmClient::buildPlayUrls(const std::string& streamId) const {
    const auto baseUrl = trimTrailingSlash(config_.zlmPublicBaseUrl.empty() ? config_.zlmBaseUrl : config_.zlmPublicBaseUrl);
    const auto wsBaseUrl = httpToWs(baseUrl);
    const auto rtspBaseUrl = httpToRtsp(baseUrl);
    const auto app = std::string("rtp");

    return PlayUrls{
        baseUrl + "/" + app + "/" + streamId + ".live.flv",
        wsBaseUrl + "/" + app + "/" + streamId + ".live.flv",
        baseUrl + "/" + app + "/" + streamId + ".live.ts",
        baseUrl + "/" + app + "/" + streamId + "/hls.m3u8",
        baseUrl + "/index/api/webrtc?app=" + app + "&stream=" + streamId + "&type=play",
        rtspBaseUrl + "/" + app + "/" + streamId,
    };
}

std::string ZlmClient::makeStreamId(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) const {
    const std::string prefix = mode == "playback" ? "gb_playback" : "gb";
    return prefix + "_" + deviceId + "_" + channelId + "_" + ssrc;
}

uint16_t ZlmClient::allocateRtpPort() {
    auto port = nextRtpPort_.fetch_add(2);
    if (port > config_.rtpPortRangeEnd || port < config_.rtpPortRangeStart) {
        nextRtpPort_.store(config_.rtpPortRangeStart + 2);
        port = config_.rtpPortRangeStart;
    }
    if (port % 2 != 0) {
        ++port;
    }
    return static_cast<uint16_t>(port);
}
