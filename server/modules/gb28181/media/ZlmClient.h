#pragma once

#include "config/AppConfig.h"
#include "media/MediaTypes.h"

#include <atomic>
#include <optional>
#include <string>

class ZlmClient {
public:
    explicit ZlmClient(MediaConfig config);

    std::optional<OpenRtpServerResult> openRtpServer(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode = "preview");
    bool closeRtpServer(const std::string& streamId);
    PlayUrls buildPlayUrls(const std::string& streamId) const;

private:
    MediaConfig config_;
    std::atomic_uint nextRtpPort_{0};

    std::string makeStreamId(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) const;
    uint16_t allocateRtpPort();
};
