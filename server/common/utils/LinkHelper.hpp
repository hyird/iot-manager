#pragma once

#include <cctype>
#include <string>
#include <string_view>

#include "common/utils/Constants.hpp"

namespace LinkHelper {

inline std::string normalizeName(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());

    for (unsigned char ch : name) {
        if (std::isspace(ch) || ch == '_' || ch == '-') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }

    return normalized;
}

inline bool isReservedAgentName(std::string_view name) {
    const auto normalized = normalizeName(name);
    return normalized == "agent" || normalized == "agentlink";
}

inline bool isReservedAgentLink(std::string_view name, std::string_view usage) {
    return usage == Constants::LINK_USAGE_AGENT || isReservedAgentName(name);
}

}  // namespace LinkHelper
