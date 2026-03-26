#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "common/protocol/s7/S7.Client.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using TestClient = s7::Client;

struct Options {
    std::string host = "192.168.1.12";
    std::uint16_t localTsap = 0x4D57;
    std::uint16_t remoteTsap = 0x4D57;
    std::size_t ioBytes = 1;
    std::size_t cycles = 0;
    std::uint32_t intervalMs = 500;
    bool reverse = false;
};

enum class ParseResult {
    Ok,
    Help,
    Error,
};

std::string normalizeHex(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) || c == '.' || c == ':' || c == '-' || c == '_';
    }), text.end());

    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        text.erase(0, 2);
    }

    return text;
}

bool parseTsap(const std::string& text, std::uint16_t& value) {
    const std::string normalized = normalizeHex(text);
    if (normalized.empty() || normalized.size() > 4) {
        return false;
    }
    if (!std::all_of(normalized.begin(), normalized.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        return false;
    }

    try {
        const unsigned long raw = std::stoul(normalized, nullptr, 16);
        if (raw > 0xFFFF) {
            return false;
        }
        value = static_cast<std::uint16_t>(raw);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseSizeT(const std::string& text, std::size_t& value, bool allowZero = false) {
    try {
        std::size_t pos = 0;
        const unsigned long long raw = std::stoull(text, &pos, 10);
        if (pos != text.size() || raw > std::numeric_limits<std::size_t>::max() ||
            (!allowZero && raw == 0)) {
            return false;
        }
        value = static_cast<std::size_t>(raw);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseUInt32(const std::string& text, std::uint32_t& value) {
    try {
        std::size_t pos = 0;
        const unsigned long long raw = std::stoull(text, &pos, 10);
        if (pos != text.size() || raw > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        value = static_cast<std::uint32_t>(raw);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

ParseResult parseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            return ParseResult::Help;
        }

        auto takeNext = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg.rfind("--host=", 0) == 0) {
            options.host = arg.substr(7);
            continue;
        }
        if (arg == "--host") {
            if (!takeNext(options.host)) {
                return ParseResult::Error;
            }
            continue;
        }

        if (arg.rfind("--local-tsap=", 0) == 0) {
            if (!parseTsap(arg.substr(13), options.localTsap)) {
                return ParseResult::Error;
            }
            continue;
        }
        if (arg == "--local-tsap") {
            std::string value;
            if (!takeNext(value) || !parseTsap(value, options.localTsap)) {
                return ParseResult::Error;
            }
            continue;
        }

        if (arg.rfind("--remote-tsap=", 0) == 0) {
            if (!parseTsap(arg.substr(14), options.remoteTsap)) {
                return ParseResult::Error;
            }
            continue;
        }
        if (arg == "--remote-tsap") {
            std::string value;
            if (!takeNext(value) || !parseTsap(value, options.remoteTsap)) {
                return ParseResult::Error;
            }
            continue;
        }

        if (arg.rfind("--io-bytes=", 0) == 0) {
            if (!parseSizeT(arg.substr(11), options.ioBytes)) {
                return ParseResult::Error;
            }
            continue;
        }
        if (arg == "--io-bytes") {
            std::string value;
            if (!takeNext(value) || !parseSizeT(value, options.ioBytes)) {
                return ParseResult::Error;
            }
            continue;
        }

        if (arg.rfind("--cycles=", 0) == 0) {
            if (!parseSizeT(arg.substr(9), options.cycles, true)) {
                return ParseResult::Error;
            }
            continue;
        }
        if (arg == "--cycles") {
            std::string value;
            if (!takeNext(value) || !parseSizeT(value, options.cycles, true)) {
                return ParseResult::Error;
            }
            continue;
        }

        if (arg.rfind("--interval-ms=", 0) == 0) {
            if (!parseUInt32(arg.substr(14), options.intervalMs)) {
                return ParseResult::Error;
            }
            continue;
        }
        if (arg == "--interval-ms") {
            std::string value;
            if (!takeNext(value) || !parseUInt32(value, options.intervalMs)) {
                return ParseResult::Error;
            }
            continue;
        }

        if (arg == "--reverse") {
            options.reverse = true;
            continue;
        }

        std::cerr << "Unknown argument: " << arg << '\n';
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

void printUsage(const char* exe) {
    std::cout
        << "Usage: " << exe
        << " [--host IP] [--local-tsap HEX] [--remote-tsap HEX]\n"
        << "       [--io-bytes N] [--cycles N] [--interval-ms N]\n"
        << "Defaults: host=192.168.1.12, port=102, local-tsap=4D57, remote-tsap=4D57,\n"
        << "          io-bytes=1, cycles=0(forever), interval-ms=500\n"
        << "Mode: clear-first running light\n"
        << "Reads V area: DB1 start=100 -> VW100 / VW105\n"
        << "Flags: --reverse\n";
}

std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

void printHexDump(const std::vector<uint8_t>& bytes, const char* indent = "    ") {
    constexpr std::size_t bytesPerLine = 16;
    const auto oldFlags = std::cout.flags();
    const auto oldFill = std::cout.fill();
    for (std::size_t offset = 0; offset < bytes.size(); offset += bytesPerLine) {
        std::cout << indent << std::uppercase << std::hex
                  << std::setw(4) << std::setfill('0') << offset << " : ";
        const std::size_t lineEnd = std::min(offset + bytesPerLine, bytes.size());
        for (std::size_t i = offset; i < lineEnd; ++i) {
            if (i != offset) {
                std::cout << ' ';
            }
            std::cout << std::setw(2) << std::setfill('0')
                      << static_cast<int>(bytes[i]);
        }
        std::cout << '\n';
    }
    std::cout.flags(oldFlags);
    std::cout.fill(oldFill);
}

void printPacketTrace(std::size_t index, std::string_view stage, bool outbound,
                      const std::vector<uint8_t>& frame) {
    std::cout << "[packet " << index << "] "
              << (outbound ? "TX " : "RX ")
              << stage
              << " len=" << frame.size() << '\n';
    printHexDump(frame);
}

std::string byteToBits(uint8_t value) {
    std::string bits;
    bits.reserve(8);
    for (int bit = 7; bit >= 0; --bit) {
        bits.push_back(((value >> bit) & 0x01) ? '1' : '0');
    }
    return bits;
}

std::string bytesToBits(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << byteToBits(bytes[i]);
    }
    return oss.str();
}

uint16_t readU16BE(const std::vector<uint8_t>& bytes, std::size_t offset) {
    if (offset + 1 >= bytes.size()) {
        return 0;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
                                 static_cast<uint16_t>(bytes[offset + 1]));
}

int16_t readI16BE(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<int16_t>(readU16BE(bytes, offset));
}

std::vector<uint8_t> buildRunningLightPattern(std::size_t byteCount, std::size_t step) {
    std::vector<uint8_t> pattern(byteCount, 0);
    if (byteCount == 0) {
        return pattern;
    }

    const std::size_t bitCount = byteCount * 8;
    const std::size_t index = step % bitCount;
    const std::size_t byteIndex = index / 8;
    const std::size_t bitIndex = index % 8;
    pattern[byteIndex] = static_cast<uint8_t>(1U << bitIndex);
    return pattern;
}

void printBuffer(const char* title, const std::vector<uint8_t>& buffer) {
    std::cout << title << " hex=[" << bytesToHex(buffer) << "] bits=[" << bytesToBits(buffer)
              << "]\n";
}

void printVAreaSnapshot(const std::vector<uint8_t>& buffer) {
    std::cout << "V area snapshot (DB1, offsets 100/105)\n";
    std::cout << "  raw hex=[" << bytesToHex(buffer) << "]\n";
    std::cout << "  VW100=" << readU16BE(buffer, 0) << '\n';
    std::cout << "  VW105=" << readI16BE(buffer, 5) << '\n';
}

bool readAreaBytes(TestClient& client, int area, int start, std::vector<uint8_t>& buffer,
                   const char* label) {
    if (buffer.empty()) {
        std::cerr << "Empty buffer for " << label << '\n';
        return false;
    }

    std::cout << "[step] Read " << label
              << " area=0x" << std::uppercase << std::hex << area
              << " start=" << std::dec << start
              << " size=" << buffer.size() << '\n';
    const int rc = client.readArea(
        area, 0, start, static_cast<int>(buffer.size()), S7WLByte, buffer.data());
    if (rc != 0) {
        std::cerr << "Read " << label << " failed, rc=" << rc << '\n';
        return false;
    }

    return true;
}

bool writeAreaBytes(TestClient& client, int area, int start, const std::vector<uint8_t>& buffer,
                    const char* label) {
    if (buffer.empty()) {
        std::cerr << "Empty buffer for " << label << '\n';
        return false;
    }

    std::vector<uint8_t> writable = buffer;
    std::cout << "[step] Write " << label
              << " area=0x" << std::uppercase << std::hex << area
              << " start=" << std::dec << start
              << " size=" << writable.size()
              << " data=[" << bytesToHex(writable) << "]\n";
    const int rc = client.writeArea(
        area, 0, start, static_cast<int>(writable.size()), S7WLByte, writable.data());
    if (rc != 0) {
        std::cerr << "Write " << label << " failed, rc=" << rc << '\n';
        return false;
    }

    return true;
}

struct OutputRestoreGuard {
    TestClient* client = nullptr;
    int start = 0;
    std::vector<uint8_t> snapshot;
    bool active = false;

    ~OutputRestoreGuard() {
        restore();
    }

    void restore() {
        if (!active || snapshot.empty() || client == nullptr || !client->connected()) {
            return;
        }
        (void)writeAreaBytes(*client, S7AreaPA, start, snapshot, "DO restore");
        active = false;
    }
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    const ParseResult parseResult = parseArgs(argc, argv, options);
    if (parseResult == ParseResult::Help) {
        printUsage(argv[0]);
        return 0;
    }
    if (parseResult == ParseResult::Error) {
        printUsage(argv[0]);
        return 1;
    }

    if (options.ioBytes == 0) {
        std::cerr << "--io-bytes must be greater than 0\n";
        return 1;
    }
    if (options.cycles == 0) {
        std::cout << "Cycles: forever\n";
    } else {
        std::cout << "Cycles: " << options.cycles << '\n';
    }

    std::cout << "S7 connection test" << '\n';
    std::cout << "Target: " << options.host << ":102" << '\n';
    std::cout << "Local TSAP: 0x" << std::hex << std::uppercase << options.localTsap << '\n';
    std::cout << "Remote TSAP: 0x" << std::hex << std::uppercase << options.remoteTsap << '\n';
    std::cout << std::dec;
    std::cout << "IO bytes: " << options.ioBytes
              << ", interval: " << options.intervalMs << "ms\n";
    std::cout << "Mode: clear-first running light";
    if (options.reverse) {
        std::cout << " (reverse)";
    }
    std::cout << '\n';

    TestClient client;
    std::size_t packetIndex = 0;
    client.setTraceCallback(
        [&packetIndex](std::string_view stage, bool outbound, const std::vector<uint8_t>& frame) {
            printPacketTrace(++packetIndex, stage, outbound, frame);
        }
    );

    // S7-200 走 TSAP，不使用 rack/slot 模式。
    if (client.setConnectionParams(
            options.host.c_str(), options.localTsap, options.remoteTsap) != 0) {
        std::cerr << "Failed to set connection params\n";
        return 2;
    }

    std::cout << "[step] Connect\n";
    const int rc = client.connect();
    if (rc != 0 || !client.connected()) {
        std::cerr << "Connect failed, rc=" << rc << '\n';
        return 3;
    }

    std::cout << "Connected successfully\n";

    std::vector<uint8_t> inputs(options.ioBytes, 0);
    std::vector<uint8_t> outputs(options.ioBytes, 0);

    if (!readAreaBytes(client, S7AreaPE, 0, inputs, "DI")) {
        return 4;
    }
    if (!readAreaBytes(client, S7AreaPA, 0, outputs, "DO")) {
        return 5;
    }

    std::cout << "Initial states:\n";
    printBuffer("  DI", inputs);
    printBuffer("  DO", outputs);

    // S7-200/LOGO 的 V 区通常映射为 DB1，这里按 DB1 读取。
    std::vector<uint8_t> vSnapshot(7, 0);
    std::cout << "[step] Read V snapshot area=0x" << std::uppercase << std::hex << S7AreaDB
              << " db=1 start=" << std::dec << 100
              << " size=" << vSnapshot.size() << '\n';
    if (client.readArea(S7AreaDB, 1, 100, static_cast<int>(vSnapshot.size()),
                        S7WLByte, vSnapshot.data()) == 0) {
        printVAreaSnapshot(vSnapshot);
    } else {
        std::cerr << "Read V area snapshot failed (DB1, start=100, size=7)\n";
    }

    OutputRestoreGuard restoreGuard;
    restoreGuard.client = &client;
    restoreGuard.start = 0;
    restoreGuard.snapshot = outputs;
    restoreGuard.active = true;

    std::size_t round = 0;
    std::size_t step = 0;
    while (options.cycles == 0 || round < options.cycles) {
        ++round;
        const std::size_t bitCount = options.ioBytes * 8;
        const std::size_t rawIndex = step++ % bitCount;
        const std::size_t lightIndex = options.reverse ? (bitCount - 1 - rawIndex) : rawIndex;
        auto writeBuffer = buildRunningLightPattern(options.ioBytes, lightIndex);
        const std::size_t byteIndex = lightIndex / 8;
        const std::size_t bitIndex = lightIndex % 8;

        std::cout << "Round " << round;
        if (options.cycles != 0) {
            std::cout << "/" << options.cycles;
        }
        std::cout << ": write running light DO Q" << byteIndex << "." << bitIndex << '\n';

        if (!writeAreaBytes(client, S7AreaPA, 0, writeBuffer, "DO")) {
            return 6;
        }

        if (options.intervalMs != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.intervalMs));
        }

        if (!readAreaBytes(client, S7AreaPE, 0, inputs, "DI")) {
            return 7;
        }
        if (!readAreaBytes(client, S7AreaPA, 0, outputs, "DO")) {
            return 8;
        }

        printBuffer("  DI", inputs);
        printBuffer("  DO", outputs);
    }

    restoreGuard.restore();
    if (readAreaBytes(client, S7AreaPA, 0, outputs, "DO")) {
        std::cout << "Restored DO:\n";
        printBuffer("  DO", outputs);
    }

    client.disconnect();
    return 0;
}
