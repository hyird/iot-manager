#pragma once

#include "DtuRegistry.hpp"
#include "DtuSessionManager.hpp"

#include <algorithm>
#include <vector>

namespace modbus {

/**
 * @brief 注册码归一化器
 *
 * 兼容两种 DTU 工作模式：
 * - 独立注册码帧：bytes == registrationBytes
 * - 查询响应前缀：bytes startsWith registrationBytes
 */
class RegistrationNormalizer {
public:
    RegistrationNormalizer(DtuRegistry& registry, DtuSessionManager& sessions)
        : registry_(registry), sessions_(sessions) {}

    /** 识别注册码并在成功时绑定 session，返回去前缀后的纯 Modbus payload */
    RegistrationMatchResult normalize(
        int linkId,
        const std::string& clientAddr,
        const std::vector<uint8_t>& bytes);

private:
    DtuRegistry& registry_;
    DtuSessionManager& sessions_;
};

namespace detail {

inline bool startsWithBytes(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& prefix) {
    return bytes.size() >= prefix.size()
        && std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

inline bool isSessionBoundToDifferentDtu(
    const std::optional<DtuSession>& sessionOpt,
    const std::string& dtuKey) {
    return sessionOpt
        && sessionOpt->bindState == SessionBindState::Bound
        && !sessionOpt->dtuKey.empty()
        && sessionOpt->dtuKey != dtuKey;
}

inline RegistrationMatchResult makeConflictResult(const std::vector<uint8_t>& bytes) {
    RegistrationMatchResult result;
    result.kind = RegistrationMatchKind::Conflict;
    result.payload = bytes;
    return result;
}

}  // namespace detail

inline RegistrationMatchResult RegistrationNormalizer::normalize(
    int linkId,
    const std::string& clientAddr,
    const std::vector<uint8_t>& bytes) {

    sessions_.onConnected(linkId, clientAddr);
    sessions_.touch(linkId, clientAddr);

    RegistrationMatchResult result;
    result.payload = bytes;

    if (bytes.empty()) {
        return result;
    }

    auto sessionOpt = sessions_.getSession(linkId, clientAddr);
    auto definitions = registry_.getDefinitionsByLink(linkId);
    if (definitions.empty()) {
        return result;
    }

    const DtuDefinition* exactMatch = nullptr;
    const DtuDefinition* prefixMatch = nullptr;

    for (const auto& dtu : definitions) {
        if (dtu.registrationBytes.empty()) continue;

        if (dtu.supportsStandaloneRegistration && bytes == dtu.registrationBytes) {
            if (exactMatch && exactMatch->dtuKey != dtu.dtuKey) {
                return detail::makeConflictResult(bytes);
            }
            exactMatch = &dtu;
            continue;
        }

        if (dtu.supportsPrefixedPayloadRegistration
            && bytes.size() > dtu.registrationBytes.size()
            && detail::startsWithBytes(bytes, dtu.registrationBytes)) {
            if (prefixMatch && prefixMatch->dtuKey != dtu.dtuKey) {
                return detail::makeConflictResult(bytes);
            }
            prefixMatch = &dtu;
        }
    }

    if (exactMatch) {
        if (detail::isSessionBoundToDifferentDtu(sessionOpt, exactMatch->dtuKey)) {
            return detail::makeConflictResult(bytes);
        }

        bool bound = false;
        if (!sessionOpt || sessionOpt->bindState != SessionBindState::Bound
            || sessionOpt->dtuKey != exactMatch->dtuKey) {
            bound = sessions_.bindSession(linkId, clientAddr, *exactMatch);
        }

        result.kind = RegistrationMatchKind::StandaloneFrame;
        result.sessionBound = bound;
        result.dtuKey = exactMatch->dtuKey;
        result.registrationBytes = exactMatch->registrationBytes;
        result.payload.clear();
        return result;
    }

    if (prefixMatch) {
        if (detail::isSessionBoundToDifferentDtu(sessionOpt, prefixMatch->dtuKey)) {
            return detail::makeConflictResult(bytes);
        }

        bool bound = false;
        if (!sessionOpt || sessionOpt->bindState != SessionBindState::Bound
            || sessionOpt->dtuKey != prefixMatch->dtuKey) {
            bound = sessions_.bindSession(linkId, clientAddr, *prefixMatch);
        }

        result.kind = RegistrationMatchKind::PrefixedPayload;
        result.sessionBound = bound;
        result.dtuKey = prefixMatch->dtuKey;
        result.registrationBytes = prefixMatch->registrationBytes;
        result.payload.assign(
            bytes.begin() + static_cast<ptrdiff_t>(prefixMatch->registrationBytes.size()),
            bytes.end());
        return result;
    }

    return result;
}

}  // namespace modbus
