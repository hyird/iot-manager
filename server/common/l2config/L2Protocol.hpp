#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace l2config {

// IEEE 802 Local Experimental EtherType 1
inline constexpr uint16_t ETHERTYPE_L2CONFIG = 0x88B5;
inline constexpr uint8_t PROTOCOL_VERSION = 0x01;
inline constexpr size_t ETH_HEADER_SIZE = 14;
inline constexpr size_t L2_HEADER_SIZE = 10;
inline constexpr size_t MAX_ETH_PAYLOAD = 1500;
inline constexpr size_t MAX_FRAG_PAYLOAD = MAX_ETH_PAYLOAD - L2_HEADER_SIZE;  // 1490
inline constexpr size_t MAC_LEN = 6;
inline constexpr double FRAGMENT_TIMEOUT_SEC = 5.0;
inline constexpr double TCP_CONNECT_TIMEOUT_SEC = 5.0;

using MacAddr = std::array<uint8_t, 6>;
inline const MacAddr BROADCAST_MAC = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum class MsgType : uint8_t {
    DISCOVER_REQ = 0x01,
    DISCOVER_RSP = 0x02,
    TCP_CONNECT = 0x10,
    TCP_CONNECTED = 0x11,
    TCP_CONNECT_FAIL = 0x12,
    TCP_DATA = 0x13,
    TCP_CLOSE_MSG = 0x14,
};

#pragma pack(push, 1)
struct L2Header {
    uint8_t version;
    uint8_t msgType;
    uint16_t seqNum;      // network byte order
    uint16_t fragIndex;   // network byte order, 0-based
    uint16_t fragTotal;   // network byte order, >=1
    uint16_t payloadLen;  // network byte order
};
#pragma pack(pop)

static_assert(sizeof(L2Header) == L2_HEADER_SIZE, "L2Header must be 10 bytes");

// ==================== 工具函数 ====================

inline uint16_t nextSeqNum() {
    static std::atomic<uint16_t> seq{1};
    return seq.fetch_add(1, std::memory_order_relaxed);
}

inline std::string macToString(const MacAddr& mac) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

inline std::optional<MacAddr> stringToMac(const std::string& str) {
    MacAddr mac{};
    unsigned int bytes[6];
    if (std::sscanf(str.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                    &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return std::nullopt;
    }
    for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(bytes[i]);
    return mac;
}

// ==================== 帧构建 ====================

/**
 * @brief 构建完整的以太网帧（含分片）
 *
 * 返回的每个 vector<uint8_t> 是一个完整的以太网帧:
 * [DstMAC 6B][SrcMAC 6B][EtherType 2B][L2Header 10B][Payload]
 */
inline std::vector<std::vector<uint8_t>> buildFrames(
    const MacAddr& dstMac, const MacAddr& srcMac,
    MsgType type, uint16_t seqNum,
    const uint8_t* payload, size_t payloadLen) {

    std::vector<std::vector<uint8_t>> frames;
    const size_t fragCount = payloadLen == 0 ? 1 : (payloadLen + MAX_FRAG_PAYLOAD - 1) / MAX_FRAG_PAYLOAD;

    for (size_t i = 0; i < fragCount; ++i) {
        const size_t offset = i * MAX_FRAG_PAYLOAD;
        const size_t thisLen = std::min(MAX_FRAG_PAYLOAD, payloadLen - offset);
        const size_t frameSize = ETH_HEADER_SIZE + L2_HEADER_SIZE + thisLen;

        std::vector<uint8_t> frame(frameSize);
        uint8_t* p = frame.data();

        // Ethernet header
        std::memcpy(p, dstMac.data(), MAC_LEN);
        p += MAC_LEN;
        std::memcpy(p, srcMac.data(), MAC_LEN);
        p += MAC_LEN;
        uint16_t etherType = htons(ETHERTYPE_L2CONFIG);
        std::memcpy(p, &etherType, 2);
        p += 2;

        // L2Config header
        auto* hdr = reinterpret_cast<L2Header*>(p);
        hdr->version = PROTOCOL_VERSION;
        hdr->msgType = static_cast<uint8_t>(type);
        hdr->seqNum = htons(seqNum);
        hdr->fragIndex = htons(static_cast<uint16_t>(i));
        hdr->fragTotal = htons(static_cast<uint16_t>(fragCount));
        hdr->payloadLen = htons(static_cast<uint16_t>(thisLen));
        p += L2_HEADER_SIZE;

        // Payload
        if (thisLen > 0 && payload) {
            std::memcpy(p, payload + offset, thisLen);
        }

        frames.push_back(std::move(frame));
    }

    return frames;
}

inline std::vector<std::vector<uint8_t>> buildFrames(
    const MacAddr& dstMac, const MacAddr& srcMac,
    MsgType type, uint16_t seqNum,
    const std::string& payload) {
    return buildFrames(dstMac, srcMac, type, seqNum,
                       reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

inline std::vector<std::vector<uint8_t>> buildFrames(
    const MacAddr& dstMac, const MacAddr& srcMac,
    MsgType type, uint16_t seqNum) {
    return buildFrames(dstMac, srcMac, type, seqNum, nullptr, 0);
}

// ==================== 帧解析 ====================

struct ParsedFrame {
    MacAddr srcMac;
    MacAddr dstMac;
    L2Header header;
    const uint8_t* payload;
    size_t payloadLen;
};

/**
 * @brief 解析收到的以太网帧
 *
 * 不复制 payload 数据，只返回指针。调用者需保证 data 生命周期。
 */
inline std::optional<ParsedFrame> parseFrame(const uint8_t* data, size_t len) {
    if (len < ETH_HEADER_SIZE + L2_HEADER_SIZE) return std::nullopt;

    // 检查 EtherType
    uint16_t etherType;
    std::memcpy(&etherType, data + 12, 2);
    if (ntohs(etherType) != ETHERTYPE_L2CONFIG) return std::nullopt;

    ParsedFrame frame{};
    std::memcpy(frame.dstMac.data(), data, MAC_LEN);
    std::memcpy(frame.srcMac.data(), data + MAC_LEN, MAC_LEN);

    const auto* hdr = reinterpret_cast<const L2Header*>(data + ETH_HEADER_SIZE);
    if (hdr->version != PROTOCOL_VERSION) return std::nullopt;

    frame.header.version = hdr->version;
    frame.header.msgType = hdr->msgType;
    frame.header.seqNum = ntohs(hdr->seqNum);
    frame.header.fragIndex = ntohs(hdr->fragIndex);
    frame.header.fragTotal = ntohs(hdr->fragTotal);
    frame.header.payloadLen = ntohs(hdr->payloadLen);

    if (frame.header.fragTotal == 0) return std::nullopt;
    if (frame.header.fragIndex >= frame.header.fragTotal) return std::nullopt;

    const size_t expectedPayload = len - ETH_HEADER_SIZE - L2_HEADER_SIZE;
    if (frame.header.payloadLen > expectedPayload) return std::nullopt;

    frame.payload = data + ETH_HEADER_SIZE + L2_HEADER_SIZE;
    frame.payloadLen = frame.header.payloadLen;

    return frame;
}

// ==================== 分片重组 ====================

class FragmentAssembler {
public:
    /**
     * @brief 输入一个解析后的帧，如果重组完成则返回完整 payload
     *
     * 单帧消息直接返回，多帧消息等待所有分片到齐后拼接返回。
     */
    std::optional<std::vector<uint8_t>> feed(const ParsedFrame& frame) {
        // 单帧消息，直接返回
        if (frame.header.fragTotal == 1) {
            return std::vector<uint8_t>(frame.payload, frame.payload + frame.payloadLen);
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto key = makeKey(frame.srcMac, frame.header.seqNum);
        auto& pending = pending_[key];

        if (pending.fragments.empty()) {
            pending.fragTotal = frame.header.fragTotal;
            pending.firstSeen = std::chrono::steady_clock::now();
        }

        pending.fragments[frame.header.fragIndex] =
            std::vector<uint8_t>(frame.payload, frame.payload + frame.payloadLen);

        // 检查是否所有分片已到齐
        if (pending.fragments.size() == pending.fragTotal) {
            std::vector<uint8_t> assembled;
            for (uint16_t i = 0; i < pending.fragTotal; ++i) {
                auto it = pending.fragments.find(i);
                if (it == pending.fragments.end()) {
                    pending_.erase(key);
                    return std::nullopt;
                }
                assembled.insert(assembled.end(), it->second.begin(), it->second.end());
            }
            pending_.erase(key);
            return assembled;
        }

        return std::nullopt;
    }

    /**
     * @brief 清理超时的不完整分片
     */
    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto elapsed = std::chrono::duration<double>(now - it->second.firstSeen).count();
            if (elapsed > FRAGMENT_TIMEOUT_SEC) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    using Key = std::pair<uint64_t, uint16_t>;  // (MAC as uint64, seqNum)

    static uint64_t macToUint64(const MacAddr& mac) {
        uint64_t val = 0;
        for (int i = 0; i < 6; ++i) {
            val = (val << 8) | mac[i];
        }
        return val;
    }

    static Key makeKey(const MacAddr& mac, uint16_t seqNum) {
        return {macToUint64(mac), seqNum};
    }

    struct PendingMessage {
        uint16_t fragTotal = 0;
        std::map<uint16_t, std::vector<uint8_t>> fragments;
        std::chrono::steady_clock::time_point firstSeen;
    };

    std::map<Key, PendingMessage> pending_;
    std::mutex mutex_;
};

}  // namespace l2config
