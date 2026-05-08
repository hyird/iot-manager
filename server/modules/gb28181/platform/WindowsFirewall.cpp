#include "platform/WindowsFirewall.h"

#include <trantor/utils/Logger.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#endif

namespace {

#ifdef _WIN32

struct FirewallRule {
    std::string name;
    std::string protocol;
    std::string localPort;
};

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            quoted += "''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

bool firewallRuleExists(const std::string& name) {
    const auto command = "netsh advfirewall firewall show rule name=\"" + name + "\" >nul 2>&1";
    return std::system(command.c_str()) == 0;
}

uint16_t extractPortFromUrl(const std::string& url) {
    const auto schemeEnd = url.find("://");
    const auto hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
    const auto pathStart = url.find('/', hostStart);
    const auto authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos || colon + 1 >= authority.size()) {
        return 0;
    }

    try {
        const auto port = std::stoul(authority.substr(colon + 1));
        if (port > 0 && port <= 65535) {
            return static_cast<uint16_t>(port);
        }
    } catch (...) {
    }
    return 0;
}

std::vector<FirewallRule> buildFirewallRules(const AppConfig& config) {
    std::vector<FirewallRule> rules{
        {"GB28181 Platform HTTP TCP " + std::to_string(config.server.port), "TCP", std::to_string(config.server.port)},
        {"GB28181 Platform SIP TCP " + std::to_string(config.sip.port), "TCP", std::to_string(config.sip.port)},
        {"GB28181 Platform SIP UDP " + std::to_string(config.sip.port), "UDP", std::to_string(config.sip.port)},
    };

    if (config.media.rtpPortRangeStart <= config.media.rtpPortRangeEnd) {
        const auto range = std::to_string(config.media.rtpPortRangeStart) + "-" + std::to_string(config.media.rtpPortRangeEnd);
        rules.push_back({"GB28181 Platform RTP TCP " + range, "TCP", range});
        rules.push_back({"GB28181 Platform RTP UDP " + range, "UDP", range});
    }

    const auto zlmPort = extractPortFromUrl(config.media.zlmPublicBaseUrl.empty() ? config.media.zlmBaseUrl : config.media.zlmPublicBaseUrl);
    if (zlmPort != 0) {
        rules.push_back({"GB28181 Platform ZLM HTTP TCP " + std::to_string(zlmPort), "TCP", std::to_string(zlmPort)});
    }

    rules.push_back({"GB28181 Platform ZLM RTSP TCP 8554", "TCP", "8554"});
    rules.push_back({"GB28181 Platform ZLM RTC TCP 8000", "TCP", "8000"});
    rules.push_back({"GB28181 Platform ZLM RTC UDP 8000", "UDP", "8000"});
    rules.push_back({"GB28181 Platform ZLM Signal TCP 3000-3001", "TCP", "3000-3001"});
    rules.push_back({"GB28181 Platform ZLM STUN TCP 3478", "TCP", "3478"});
    rules.push_back({"GB28181 Platform ZLM STUN UDP 3478", "UDP", "3478"});

    return rules;
}

std::string buildPowerShellScript(const std::vector<FirewallRule>& rules) {
    std::ostringstream script;
    script << "$ErrorActionPreference='Stop';";
    script << "$rules=@(";
    for (const auto& rule : rules) {
        script << "@{Name=" << shellQuote(rule.name)
               << ";Protocol=" << shellQuote(rule.protocol)
               << ";LocalPort=" << shellQuote(rule.localPort) << "};";
    }
    script << ");";
    script << "foreach($rule in $rules){";
    script << "if(-not (Get-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue)){";
    script << "New-NetFirewallRule -DisplayName $rule.Name -Direction Inbound -Action Allow -Protocol $rule.Protocol -LocalPort $rule.LocalPort -Profile Any | Out-Null";
    script << "}}";
    return script.str();
}

#endif

} // namespace

void ensureWindowsFirewallRules(const AppConfig& config) {
#ifdef _WIN32
    const auto rules = buildFirewallRules(config);
    std::vector<FirewallRule> missing;
    for (const auto& rule : rules) {
        if (!firewallRuleExists(rule.name)) {
            missing.push_back(rule);
        }
    }

    if (missing.empty()) {
        LOG_INFO << "Windows firewall rules already exist";
        return;
    }

    LOG_INFO << "Requesting Windows firewall authorization for " << missing.size() << " inbound rule(s)";

    const auto script = buildPowerShellScript(missing);
    const auto parameters = std::string("-NoProfile -ExecutionPolicy Bypass -Command \"") + script + "\"";
    const auto wideParameters = widen(parameters);

    const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(
        nullptr,
        L"runas",
        L"powershell.exe",
        wideParameters.c_str(),
        nullptr,
        SW_SHOWNORMAL));

    if (result <= 32) {
        LOG_WARN << "Could not request Windows firewall authorization, ShellExecute error: " << static_cast<long long>(result);
        return;
    }

    LOG_INFO << "Windows firewall authorization prompt started";
#else
    (void)config;
#endif
}
