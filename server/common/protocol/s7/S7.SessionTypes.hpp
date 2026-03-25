#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace s7 {

struct S7DtuDefinition {
    std::string dtuKey;
    int deviceId = 0;
    int linkId = 0;
    std::string linkMode;
    std::string name;
    std::vector<uint8_t> registrationBytes;
    bool supportsStandaloneRegistration = true;
    bool supportsPrefixedPayloadRegistration = true;
};

enum class SessionBindState {
    Unknown,
    Bound
};

struct S7OnlineRoute {
    std::string sessionKey;
    std::string dtuKey;
    int linkId = 0;
    std::string clientAddr;
    int deviceId = 0;
};

enum class RegistrationMatchKind {
    None,
    StandaloneFrame,
    PrefixedPayload,
    Conflict
};

struct RegistrationMatchResult {
    RegistrationMatchKind kind = RegistrationMatchKind::None;
    bool sessionBound = false;
    std::string dtuKey;
    std::vector<uint8_t> registrationBytes;
    std::vector<uint8_t> payload;
};

struct S7DtuSession {
    int linkId = 0;
    std::string clientAddr;
    std::string sessionKey;
    SessionBindState bindState = SessionBindState::Unknown;
    std::string dtuKey;
    int deviceId = 0;
    std::chrono::steady_clock::time_point lastSeen;
};

inline std::string makeS7DtuSessionKey(int linkId, const std::string& clientAddr) {
    return std::to_string(linkId) + ":" + clientAddr;
}

}  // namespace s7
