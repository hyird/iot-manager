#pragma once

#include "config/AppConfig.h"
#include "media/MediaTypes.h"

#include <drogon/drogon.h>

#include <atomic>
#include <optional>
#include <string>

class ZlmClient {
public:
    explicit ZlmClient(MediaConfig config);

    drogon::Task<std::optional<OpenRtpServerResult>> openRtpServerCoro(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode = "preview");
    drogon::Task<bool> closeRtpServerCoro(const std::string& streamId);
    PlayUrls buildPlayUrls(const std::string& streamId) const;

private:
    MediaConfig config_;
    std::atomic_uint nextRtpPort_{0};

    std::string makeStreamId(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) const;
    uint16_t allocateRtpPort();
};
