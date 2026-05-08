#pragma once

#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/StreamRegistry.h"
#include "sip/SipServer.h"

#include <string>

class ApiServer {
public:
    ApiServer(AppConfig config, DeviceRegistry& deviceRegistry, StreamRegistry& streamRegistry, SipServer& sipServer);
    void registerRoutes(const std::string& apiPrefix);

private:
    AppConfig config_;
    DeviceRegistry& deviceRegistry_;
    StreamRegistry& streamRegistry_;
    SipServer& sipServer_;
};
