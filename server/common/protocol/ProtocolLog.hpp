#pragma once

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace protocol_log {

inline std::string prefix(std::string_view protocol, std::string_view component, std::string_view event = {}) {
    std::ostringstream oss;
    oss << '[' << protocol << "][" << component << ']';
    if (!event.empty()) {
        oss << '[' << event << ']';
    }
    return oss.str();
}

inline std::string device(int deviceId, std::string_view deviceName = {}) {
    std::ostringstream oss;
    oss << "deviceId=" << deviceId;
    if (!deviceName.empty()) {
        oss << ", deviceName=" << deviceName;
    }
    return oss.str();
}

inline std::string bytesToHex(std::string_view data, std::size_t limit = 512) {
    std::ostringstream oss;
    const auto count = std::min(data.size(), limit);
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) oss << ' ';
        const auto byte = static_cast<unsigned char>(data[i]);
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(byte);
    }
    if (data.size() > limit) {
        oss << " ...";
    }
    return oss.str();
}

inline std::string bytesToPrintableAscii(std::string_view data, std::size_t limit = 256) {
    std::string out;
    const auto count = std::min(data.size(), limit);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(data[i]);
        switch (byte) {
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                out.push_back(std::isprint(byte) ? static_cast<char>(byte) : '.');
                break;
        }
    }
    if (data.size() > limit) {
        out += "...";
    }
    return out;
}

inline std::string bytesSummary(std::string_view data) {
    std::ostringstream oss;
    oss << "bytes=" << data.size()
        << ", hex=" << bytesToHex(data)
        << ", ascii=\"" << bytesToPrintableAscii(data) << '"';
    return oss.str();
}

}  // namespace protocol_log
