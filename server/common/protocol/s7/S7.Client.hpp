#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef S7_ENABLE_BLOCKING_CLIENT
#define S7_ENABLE_BLOCKING_CLIENT 0
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

inline constexpr int p_u16_LocalPort = 1;
inline constexpr int p_u16_RemotePort = 2;
inline constexpr int p_i32_PingTimeout = 3;
inline constexpr int p_i32_SendTimeout = 4;
inline constexpr int p_i32_RecvTimeout = 5;
inline constexpr int p_u16_PduRequest = 6;

inline constexpr int S7AreaPE = 0x81;
inline constexpr int S7AreaPA = 0x82;
inline constexpr int S7AreaMK = 0x83;
inline constexpr int S7AreaDB = 0x84;
inline constexpr int S7AreaCT = 0x1C;
inline constexpr int S7AreaTM = 0x1D;

inline constexpr int S7WLBit = 0x01;
inline constexpr int S7WLByte = 0x02;
inline constexpr int S7WLChar = 0x03;
inline constexpr int S7WLWord = 0x04;
inline constexpr int S7WLInt = 0x05;
inline constexpr int S7WLDWord = 0x06;
inline constexpr int S7WLDInt = 0x07;
inline constexpr int S7WLReal = 0x08;
inline constexpr int S7WLCounter = 0x1C;
inline constexpr int S7WLTimer = 0x1D;

namespace s7 {

inline constexpr int kS7Ok = 0;
inline constexpr int kS7ErrInvalidHandle = -1;
inline constexpr int kS7ErrInvalidParams = -2;
inline constexpr int kS7ErrSocketInit = -10;
inline constexpr int kS7ErrResolveFailed = -11;
inline constexpr int kS7ErrConnectFailed = -12;
inline constexpr int kS7ErrTimeout = -13;
inline constexpr int kS7ErrSocketIo = -14;
inline constexpr int kS7ErrProtocol = -15;
inline constexpr int kS7ErrNotConnected = -16;
inline constexpr int kS7ErrPduNegotiation = -17;
inline constexpr int kS7ErrUnsupported = -18;
inline constexpr int kS7ErrResponseTooShort = -19;
inline constexpr int kS7ErrSequenceMismatch = -20;
inline constexpr int kS7ErrFunctionMismatch = -21;
inline constexpr int kS7ErrItemCountMismatch = -22;
inline constexpr int kS7ErrResponseLengthMismatch = -23;

inline constexpr std::uint16_t kDefaultRemotePort = 102;
inline constexpr std::uint16_t kDefaultIsoPduSize = 1024;
inline constexpr std::uint16_t kDefaultS7PduRequest = 960;
inline constexpr std::uint16_t kDefaultSourceRef = 0x0100;
inline constexpr std::array<std::uint16_t, 3> kSourceRefCandidates{
    kDefaultSourceRef,
    0x0001,
    0x0000,
};
inline constexpr int kDefaultTimeoutMs = 5000;

inline constexpr std::uint16_t kConnTypePg = 0x01;
inline constexpr std::uint16_t kConnTypeOp = 0x02;
inline constexpr std::uint16_t kConnTypeBasic = 0x03;

inline constexpr std::uint8_t kIsoTcpVersion = 0x03;
inline constexpr std::uint8_t kCotpCr = 0xE0;
inline constexpr std::uint8_t kCotpCc = 0xD0;
inline constexpr std::uint8_t kCotpDr = 0x80;
inline constexpr std::uint8_t kCotpDt = 0xF0;
inline constexpr std::uint8_t kCotpDtLength = 0x02;
inline constexpr std::uint8_t kCotpEot = 0x80;
inline constexpr std::uint8_t kS7ProtocolId = 0x32;
inline constexpr std::uint8_t kS7Request = 0x01;
inline constexpr std::uint8_t kS7AckData = 0x03;
inline constexpr std::uint8_t kS7FuncNegotiate = 0xF0;
inline constexpr std::uint8_t kS7FuncRead = 0x04;
inline constexpr std::uint8_t kS7FuncWrite = 0x05;
inline constexpr std::uint8_t kTsResBit = 0x03;
inline constexpr std::uint8_t kTsResByte = 0x04;
inline constexpr std::uint8_t kTsResInt = 0x05;
inline constexpr std::uint8_t kTsResReal = 0x07;
inline constexpr std::uint8_t kTsResOctet = 0x09;

enum class ErrorDomain {
    None,
    Client,
    Transport,
    Iso,
    S7Header,
    S7Item,
    Pdu
};

struct ErrorInfo {
    ErrorDomain domain = ErrorDomain::None;
    int code = kS7Ok;
    std::string stage;
};

struct DataItem {
    int area = S7AreaDB;
    int dbNumber = 0;
    int start = 0;
    int amount = 0;
    int wordLen = S7WLByte;
    void* data = nullptr;
    std::size_t capacity = 0;
    std::size_t transferred = 0;
    int result = kS7Ok;
};

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
inline int closeSocket(SocketHandle socket) { return closesocket(socket); }
inline int getSocketError() { return WSAGetLastError(); }
inline bool isInProgress(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL; }
inline int setSocketBlocking(SocketHandle socket, bool blocking) {
    u_long mode = blocking ? 0UL : 1UL;
    return ioctlsocket(socket, FIONBIO, &mode) == 0 ? 0 : getSocketError();
}
inline int shutdownSocket(SocketHandle socket) { return shutdown(socket, SD_BOTH); }
inline int setSocketTimeout(SocketHandle socket, int option, int timeoutMs) {
    DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
    return setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&timeout),
        static_cast<int>(sizeof(timeout)));
}
inline constexpr int kSocketRecvTimeoutOpt = SO_RCVTIMEO;
inline constexpr int kSocketSendTimeoutOpt = SO_SNDTIMEO;
inline int ensureSocketSubsystem() {
    static std::once_flag once;
    static int initResult = 0;
    std::call_once(once, []() {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            initResult = rc;
        }
    });
    return initResult;
}
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
inline int closeSocket(SocketHandle socket) { return ::close(socket); }
inline int getSocketError() { return errno; }
inline bool isInProgress(int error) { return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN; }
inline int setSocketBlocking(SocketHandle socket, bool blocking) {
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return errno;
    }
    const int newFlags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(socket, F_SETFL, newFlags) == 0 ? 0 : errno;
}
inline int shutdownSocket(SocketHandle socket) { return shutdown(socket, SHUT_RDWR); }
inline int setSocketTimeout(SocketHandle socket, int option, int timeoutMs) {
    timeval timeout{};
    timeout.tv_sec = timeoutMs > 0 ? timeoutMs / 1000 : 0;
    timeout.tv_usec = timeoutMs > 0 ? (timeoutMs % 1000) * 1000 : 0;
    return setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout));
}
inline constexpr int kSocketRecvTimeoutOpt = SO_RCVTIMEO;
inline constexpr int kSocketSendTimeoutOpt = SO_SNDTIMEO;
inline int ensureSocketSubsystem() { return 0; }
#endif

inline std::uint16_t readBe16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8)
        | static_cast<std::uint16_t>(bytes[1]));
}

inline void appendBe16(std::vector<std::uint8_t>& buffer, std::uint16_t value) {
    buffer.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

#if S7_ENABLE_BLOCKING_CLIENT
inline int waitForSocket(SocketHandle socket, bool readReady, int timeoutMs) {
    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    if (readReady) {
        FD_SET(socket, &readSet);
    } else {
        FD_SET(socket, &writeSet);
    }

    timeval timeout{};
    timeval* timeoutPtr = nullptr;
    if (timeoutMs >= 0) {
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        timeoutPtr = &timeout;
    }

    const int rc = select(static_cast<int>(socket + 1),
        readReady ? &readSet : nullptr,
        readReady ? nullptr : &writeSet,
        nullptr,
        timeoutPtr);
    if (rc > 0) {
        return kS7Ok;
    }
    if (rc == 0) {
        return kS7ErrTimeout;
    }
    return kS7ErrSocketIo;
}
#endif

inline std::uint8_t encodeTpduSize(std::uint16_t size) {
    switch (size) {
    case 128: return 0x07;
    case 256: return 0x08;
    case 512: return 0x09;
    case 1024: return 0x0A;
    case 2048: return 0x0B;
    case 4096: return 0x0C;
    case 8192: return 0x0D;
    default: return 0x0A;
    }
}

inline int wordLenByteSize(int wordLen) {
    switch (wordLen) {
    case S7WLBit:
    case S7WLByte:
    case S7WLChar:
        return 1;
    case S7WLWord:
    case S7WLInt:
    case S7WLCounter:
    case S7WLTimer:
        return 2;
    case S7WLDWord:
    case S7WLDInt:
    case S7WLReal:
        return 4;
    default:
        return 0;
    }
}

inline std::uint8_t writeTransportSize(int wordLen) {
    switch (wordLen) {
    case S7WLBit:
        return kTsResBit;
    case S7WLInt:
    case S7WLDInt:
        return kTsResInt;
    case S7WLReal:
        return kTsResReal;
    case S7WLChar:
    case S7WLCounter:
    case S7WLTimer:
        return kTsResOctet;
    default:
        return kTsResByte;
    }
}

inline std::size_t decodeResponseDataSize(std::uint8_t transportSize, std::uint16_t dataLength) {
    if (transportSize == kTsResOctet || transportSize == kTsResReal || transportSize == kTsResBit) {
        return dataLength;
    }
    return static_cast<std::size_t>(dataLength / 8);
}

inline int maxElementsPerPdu(std::uint16_t pduLength, int protocolOverhead, int elementSize) {
    if (elementSize <= 0) {
        return 1;
    }
    if (pduLength <= static_cast<std::uint16_t>(protocolOverhead)) {
        return 1;
    }
    return std::max(1, static_cast<int>((pduLength - protocolOverhead) / elementSize));
}

inline std::size_t transferByteSize(int amount, int wordLen) {
    const int elementSize = wordLenByteSize(wordLen);
    if (amount <= 0 || elementSize <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(amount) * static_cast<std::size_t>(elementSize);
}

inline int itemStartAddress(int start, int wordLen) {
    return (wordLen == S7WLBit || wordLen == S7WLCounter || wordLen == S7WLTimer)
        ? start
        : start * 8;
}

inline std::size_t paddedEven(std::size_t value) {
    return value + (value % 2);
}

inline std::size_t readRequestPduSize(std::size_t itemCount) {
    return 10 + 2 + itemCount * 12;
}

inline std::size_t readResponsePduSize(const std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
    std::size_t size = 12 + 2;
    for (std::size_t i = begin; i < end; ++i) {
        size += 4 + paddedEven(transferByteSize(items[i].amount, items[i].wordLen));
    }
    return size;
}

inline std::size_t writeRequestPduSize(const std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
    std::size_t size = 10 + 2 + (end - begin) * 12;
    for (std::size_t i = begin; i < end; ++i) {
        size += 4 + paddedEven(transferByteSize(items[i].amount, items[i].wordLen));
    }
    return size;
}

inline std::size_t writeResponsePduSize(std::size_t itemCount) {
    return 12 + 2 + itemCount;
}

inline bool readItemFitsPdu(const DataItem& item, std::uint16_t pduLength) {
    return readRequestPduSize(1) <= pduLength
        && (12 + 2 + 4 + paddedEven(transferByteSize(item.amount, item.wordLen))) <= pduLength;
}

inline bool writeItemFitsPdu(const DataItem& item, std::uint16_t pduLength) {
    std::vector<DataItem> itemView{item};
    return writeRequestPduSize(itemView, 0, 1) <= pduLength
        && writeResponsePduSize(1) <= pduLength;
}

class Client {
public:
    struct TransportHooks {
        std::function<int(const std::string& remoteAddress,
                          std::uint16_t remotePort,
                          int pingTimeoutMs,
                          int sendTimeoutMs,
                          int recvTimeoutMs,
                          std::uint16_t& localPort)> open;
        std::function<void()> close;
        std::function<bool()> connected;
        std::function<int(const std::uint8_t* data, std::size_t size)> send;
        std::function<int(std::uint8_t* data, std::size_t size, int timeoutMs)> recv;
    };

    using TraceCallback = std::function<void(std::string_view stage, bool outbound,
                                             const std::vector<std::uint8_t>& frame)>;

    Client() = default;
    ~Client() { disconnect(); }

    int setConnectionParams(const char* address, std::uint16_t localTsap, std::uint16_t remoteTsap) {
        if (!address || address[0] == '\0') {
            return kS7ErrInvalidParams;
        }
        remoteAddress_ = address;
        localTsap_ = localTsap;
        remoteTsap_ = remoteTsap;
        return kS7Ok;
    }

    int setConnectionType(std::uint16_t connectionType) {
        if (connectionType < kConnTypePg || connectionType > kConnTypeBasic) {
            return kS7ErrInvalidParams;
        }
        connectionType_ = connectionType;
        return kS7Ok;
    }

    int setParam(int paramNumber, const void* value) {
        if (!value) {
            return kS7ErrInvalidParams;
        }
        switch (paramNumber) {
        case p_u16_RemotePort:
            remotePort_ = *static_cast<const std::uint16_t*>(value);
            return kS7Ok;
        case p_u16_PduRequest:
            pduRequestLength_ = std::clamp(
                *static_cast<const std::uint16_t*>(value),
                static_cast<std::uint16_t>(240),
                static_cast<std::uint16_t>(kDefaultIsoPduSize - 7));
            return kS7Ok;
        case p_i32_PingTimeout:
            pingTimeoutMs_ = *static_cast<const std::int32_t*>(value);
            return kS7Ok;
        case p_i32_SendTimeout:
            sendTimeoutMs_ = *static_cast<const std::int32_t*>(value);
            return kS7Ok;
        case p_i32_RecvTimeout:
            recvTimeoutMs_ = *static_cast<const std::int32_t*>(value);
            return kS7Ok;
        default:
            return kS7ErrUnsupported;
        }
    }

    int getParam(int paramNumber, void* value) const {
        if (!value) {
            return kS7ErrInvalidParams;
        }
        switch (paramNumber) {
        case p_u16_LocalPort:
            *static_cast<std::uint16_t*>(value) = localPort_;
            return kS7Ok;
        case p_u16_RemotePort:
            *static_cast<std::uint16_t*>(value) = remotePort_;
            return kS7Ok;
        case p_u16_PduRequest:
            *static_cast<std::uint16_t*>(value) = pduRequestLength_;
            return kS7Ok;
        case p_i32_PingTimeout:
            *static_cast<std::int32_t*>(value) = pingTimeoutMs_;
            return kS7Ok;
        case p_i32_SendTimeout:
            *static_cast<std::int32_t*>(value) = sendTimeoutMs_;
            return kS7Ok;
        case p_i32_RecvTimeout:
            *static_cast<std::int32_t*>(value) = recvTimeoutMs_;
            return kS7Ok;
        default:
            return kS7ErrUnsupported;
        }
    }

    int connectTo(const char* address, int rack, int slot) {
#if !S7_ENABLE_BLOCKING_CLIENT
        (void)address;
        (void)rack;
        (void)slot;
        return setError(ErrorDomain::Client, kS7ErrUnsupported, "connectTo.blocking-disabled");
#else
        if (rack < 0 || slot < 0) {
            return kS7ErrInvalidParams;
        }
        const auto remoteTsap = static_cast<std::uint16_t>((connectionType_ << 8) + (rack * 0x20) + slot);
        const int rc = setConnectionParams(address, localTsap_, remoteTsap);
        if (rc != kS7Ok) {
            return rc;
        }
        return connect();
#endif
    }

    int connect() {
#if !S7_ENABLE_BLOCKING_CLIENT
        return setError(ErrorDomain::Client, kS7ErrUnsupported, "connect.blocking-disabled");
#else
        std::lock_guard ioLock(ioMutex_);
        if (remoteAddress_.empty()) {
            return kS7ErrInvalidParams;
        }

        closeSocketOnly();
        sequence_ = 0;
        pduLength_ = pduRequestLength_;

        int rc = openSocket();
        if (rc != kS7Ok) {
            closeSocketOnly();
            return rc;
        }

        rc = sendConnectionRequest();
        if (rc != kS7Ok) {
            closeSocketOnly();
            return rc;
        }

        rc = negotiatePduLength();
        if (rc != kS7Ok) {
            closeSocketOnly();
            return rc;
        }

        connected_ = true;
        return kS7Ok;
#endif
    }

    int disconnect() {
        std::lock_guard ioLock(ioMutex_);
        connected_ = false;
#if !S7_ENABLE_BLOCKING_CLIENT
        return closeSocketOnly();
#else
        if (transportHooks_.close) {
            return closeSocketOnly();
        }
        const int rc = sendDisconnectRequest();
        if (rc != kS7Ok) {
            closeSocketOnly();
            return rc;
        }
        return closeSocketOnly();
#endif
    }

    bool connected() const {
        if (transportHooks_.connected) {
            return connected_ && transportHooks_.connected();
        }
        return connected_ && socket_ != kInvalidSocket;
    }

    bool transportOpen() const {
        if (transportHooks_.connected) {
            return transportHooks_.connected();
        }
        return socket_ != kInvalidSocket;
    }

    void setTraceCallback(TraceCallback callback) {
        traceCallback_ = std::move(callback);
    }

    void setConnectionSourceRef(std::uint16_t sourceRef) {
        connectionSourceRef_ = sourceRef;
    }

    void setTransportHooks(TransportHooks hooks) {
        transportHooks_ = std::move(hooks);
    }

    std::uint16_t negotiatedPduLength() const {
        return pduLength_;
    }

    ErrorInfo lastErrorInfo() const {
        return lastErrorInfo_;
    }

    void resetAsyncSessionState() {
        std::lock_guard ioLock(ioMutex_);
        connected_ = false;
        sequence_ = 0;
        pduLength_ = pduRequestLength_;
        disconnectFrame_.clear();
    }

    std::vector<std::uint8_t> buildConnectionRequestFrame() {
        const auto sourceRef = nextSourceRef();
        std::vector<std::uint8_t> frame;
        frame.reserve(22);
        frame.push_back(kIsoTcpVersion);
        frame.push_back(0x00);
        appendBe16(frame, 22);
        frame.push_back(0x11);
        frame.push_back(kCotpCr);
        frame.push_back(0x00);
        frame.push_back(0x00);
        appendBe16(frame, sourceRef);
        frame.push_back(0x00);
        frame.push_back(0xC0);
        frame.push_back(0x01);
        frame.push_back(encodeTpduSize(kDefaultIsoPduSize));
        frame.push_back(0xC1);
        frame.push_back(0x02);
        appendBe16(frame, localTsap_);
        frame.push_back(0xC2);
        frame.push_back(0x02);
        appendBe16(frame, remoteTsap_);
        traceFrame("iso.cr", true, frame);
        return frame;
    }

    int parseConnectionConfirmFrame(const std::vector<std::uint8_t>& frame) {
        traceFrame("iso.cc", false, frame);
        if (frame.size() < 11 || frame[0] != kIsoTcpVersion || frame[5] != kCotpCc) {
            return setError(ErrorDomain::Iso, kS7ErrProtocol, "iso.cc");
        }
        disconnectFrame_ = frame;
        return setError(ErrorDomain::None, kS7Ok, "iso.cc");
    }

    std::vector<std::uint8_t> buildSetupCommunicationRequest() {
        std::vector<std::uint8_t> request;
        request.reserve(18);
        request.push_back(kS7ProtocolId);
        request.push_back(kS7Request);
        appendBe16(request, 0x0000);
        appendBe16(request, nextSequence());
        appendBe16(request, 8);
        appendBe16(request, 0);
        request.push_back(kS7FuncNegotiate);
        request.push_back(0x00);
        appendBe16(request, 1);
        appendBe16(request, 1);
        appendBe16(request, pduRequestLength_);
        return request;
    }

    int parseSetupCommunicationResponse(const std::vector<std::uint8_t>& response) {
        if (response.size() < 20 || response[0] != kS7ProtocolId) {
            return setError(ErrorDomain::Pdu, kS7ErrPduNegotiation, "s7.setup-comm.resp");
        }
        const std::uint16_t error = readBe16(response.data() + 10);
        if (error != 0) {
            return setError(ErrorDomain::Pdu, static_cast<int>(error), "s7.setup-comm.resp");
        }
        const std::uint16_t parLen = readBe16(response.data() + 6);
        if (12 + parLen > response.size() || parLen < 8) {
            return setError(ErrorDomain::Pdu, kS7ErrPduNegotiation, "s7.setup-comm.resp");
        }

        pduLength_ = readBe16(response.data() + 18);
        if (pduLength_ == 0) {
            pduLength_ = kDefaultS7PduRequest;
        }
        connected_ = true;
        return setError(ErrorDomain::None, kS7Ok, "s7.setup-comm.resp");
    }

    int readArea(int area, int dbNumber, int start, int amount, int wordLen, void* data);
    int writeArea(int area, int dbNumber, int start, int amount, int wordLen, void* data);
    int readMultiVars(std::vector<DataItem>& items);
    int writeMultiVars(std::vector<DataItem>& items);
    int dbRead(int dbNumber, int start, int size, void* data) {
        return readArea(S7AreaDB, dbNumber, start, size, S7WLByte, data);
    }
    int dbWrite(int dbNumber, int start, int size, void* data) {
        return writeArea(S7AreaDB, dbNumber, start, size, S7WLByte, data);
    }
    int mbRead(int start, int size, void* data) {
        return readArea(S7AreaMK, 0, start, size, S7WLByte, data);
    }
    int mbWrite(int start, int size, void* data) {
        return writeArea(S7AreaMK, 0, start, size, S7WLByte, data);
    }
    int ebRead(int start, int size, void* data) {
        return readArea(S7AreaPE, 0, start, size, S7WLByte, data);
    }
    int ebWrite(int start, int size, void* data) {
        return writeArea(S7AreaPE, 0, start, size, S7WLByte, data);
    }
    int abRead(int start, int size, void* data) {
        return readArea(S7AreaPA, 0, start, size, S7WLByte, data);
    }
    int abWrite(int start, int size, void* data) {
        return writeArea(S7AreaPA, 0, start, size, S7WLByte, data);
    }

    std::vector<std::uint8_t> buildReadRequestForItems(
        const std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
        return buildReadRequest(items, begin, end);
    }

    int parseReadResponseForItems(
        const std::vector<std::uint8_t>& response,
        std::vector<DataItem>& items,
        std::size_t begin,
        std::size_t end) {
        return parseReadResponse(response, items, begin, end);
    }

    std::vector<std::uint8_t> buildWriteRequestForItems(
        const std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
        return buildWriteRequest(items, begin, end);
    }

    int parseWriteResponseForItems(
        const std::vector<std::uint8_t>& response,
        std::vector<DataItem>& items,
        std::size_t begin,
        std::size_t end) {
        return parseWriteResponse(response, items, begin, end);
    }

private:
    int setError(ErrorDomain domain, int code, std::string_view stage = {});
    int closeSocketOnly();
    int sendDisconnectRequest();
    int openSocket();
    int sendAll(const std::uint8_t* data, std::size_t size);
    int recvAll(std::uint8_t* data, std::size_t size, bool closeOnTimeout = true);
    int recvTpktFrame(std::vector<std::uint8_t>& frame, bool closeOnTimeout = true);
    int sendConnectionRequest();
    int negotiatePduLength();
    int sendPayload(std::string_view stage, const std::vector<std::uint8_t>& payload);
    int recvPayload(std::string_view stage, std::vector<std::uint8_t>& payload);
    int exchangePayload(std::string_view requestStage, std::string_view responseStage,
                        const std::vector<std::uint8_t>& request, std::vector<std::uint8_t>& response,
                        std::uint8_t expectedFunction);
    std::vector<std::uint8_t> buildReadRequest(int area, int dbNumber, int start, int amount, int wordLen);
    std::vector<std::uint8_t> buildReadRequest(const std::vector<DataItem>& items, std::size_t begin, std::size_t end);
    std::vector<std::uint8_t> buildWriteRequest(int area, int dbNumber, int start, int amount,
                                                int wordLen, const std::uint8_t* source, std::size_t size);
    std::vector<std::uint8_t> buildWriteRequest(const std::vector<DataItem>& items, std::size_t begin, std::size_t end);
    int parseReadResponse(const std::vector<std::uint8_t>& response, std::uint8_t* target,
                          std::size_t capacity, std::size_t& copied);
    int parseReadResponse(const std::vector<std::uint8_t>& response,
                          std::vector<DataItem>& items, std::size_t begin, std::size_t end);
    int parseWriteResponse(const std::vector<std::uint8_t>& response) const;
    int parseWriteResponse(const std::vector<std::uint8_t>& response,
                           std::vector<DataItem>& items, std::size_t begin, std::size_t end) const;
    int readAreaUnlocked(int area, int dbNumber, int start, int amount, int wordLen, void* data);
    int writeAreaUnlocked(int area, int dbNumber, int start, int amount, int wordLen, void* data);
    std::vector<std::uint8_t> buildDisconnectFrame(std::uint16_t dstRef,
                                                   std::uint16_t srcRef,
                                                   std::uint8_t reason) const;
    int sendDisconnectProbe(std::string_view stage, std::uint16_t dstRef,
                            std::uint16_t srcRef, std::uint8_t reason);
    std::uint16_t nextSequence() { return sequence_++; }
    std::uint16_t nextSourceRef() {
        if (connectionSourceRef_) {
            return *connectionSourceRef_;
        }
        const auto sourceRef = kSourceRefCandidates[sourceRefIndex_ % kSourceRefCandidates.size()];
        sourceRefIndex_ = (sourceRefIndex_ + 1) % kSourceRefCandidates.size();
        return sourceRef;
    }
    void traceFrame(std::string_view stage, bool outbound,
                    const std::vector<std::uint8_t>& frame) const {
        if (traceCallback_) {
            traceCallback_(stage, outbound, frame);
        }
    }

    SocketHandle socket_ = kInvalidSocket;
    std::string remoteAddress_;
    std::uint16_t localTsap_ = 0x0100;
    std::uint16_t remoteTsap_ = 0x0100;
    std::uint16_t remotePort_ = kDefaultRemotePort;
    std::uint16_t localPort_ = 0;
    std::uint16_t connectionType_ = kConnTypePg;
    int pingTimeoutMs_ = kDefaultTimeoutMs;
    int sendTimeoutMs_ = kDefaultTimeoutMs;
    int recvTimeoutMs_ = kDefaultTimeoutMs;
    std::uint16_t pduRequestLength_ = kDefaultS7PduRequest;
    std::uint16_t pduLength_ = kDefaultS7PduRequest;
    std::uint16_t sequence_ = 0;
    std::size_t sourceRefIndex_ = 0;
    std::optional<std::uint16_t> connectionSourceRef_;
    std::mutex ioMutex_;
    bool connected_ = false;
    int lastError_ = kS7Ok;
    ErrorInfo lastErrorInfo_;
    std::vector<std::uint8_t> disconnectFrame_;
    TraceCallback traceCallback_;
    TransportHooks transportHooks_;
};

inline int Client::setError(ErrorDomain domain, int code, std::string_view stage) {
    lastError_ = code;
    lastErrorInfo_ = ErrorInfo{domain, code, std::string(stage)};
    return code;
}

inline int Client::closeSocketOnly() {
    connected_ = false;
    localPort_ = 0;
    disconnectFrame_.clear();
    if (transportHooks_.close) {
        transportHooks_.close();
    }
    if (socket_ != kInvalidSocket) {
        shutdownSocket(socket_);
        closeSocket(socket_);
        socket_ = kInvalidSocket;
    }
    return kS7Ok;
}

inline int Client::sendDisconnectRequest() {
    if (socket_ == kInvalidSocket) {
        return kS7Ok;
    }

    if (disconnectFrame_.size() >= 11) {
        const auto clientRef = readBe16(disconnectFrame_.data() + 6);
        const auto serverRef = readBe16(disconnectFrame_.data() + 8);
        auto frame = buildDisconnectFrame(serverRef, clientRef, 0x00);
        traceFrame("iso.dr", true, frame);
        return sendAll(frame.data(), frame.size());
    }

    return sendDisconnectProbe("iso.dr", 0x0000, kDefaultSourceRef, 0x00);
}

inline int Client::openSocket() {
    if (transportHooks_.open) {
        disconnectFrame_.clear();
        localPort_ = 0;
        return lastError_ = transportHooks_.open(
            remoteAddress_,
            remotePort_,
            pingTimeoutMs_,
            sendTimeoutMs_,
            recvTimeoutMs_,
            localPort_);
    }

#if !S7_ENABLE_BLOCKING_CLIENT
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "openSocket.blocking-disabled");
#else
    lastError_ = ensureSocketSubsystem();
    if (lastError_ != 0) {
        return kS7ErrSocketInit;
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(remotePort_);
    if (getaddrinfo(remoteAddress_.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
        return lastError_ = kS7ErrResolveFailed;
    }

    const auto guard = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>(result, freeaddrinfo);

    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        SocketHandle candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (candidate == kInvalidSocket) {
            continue;
        }

        if (setSocketBlocking(candidate, false) != 0) {
            closeSocket(candidate);
            continue;
        }

        int rc = ::connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen));
        if (rc != 0) {
            const int error = getSocketError();
            if (!isInProgress(error)) {
                closeSocket(candidate);
                continue;
            }

            rc = waitForSocket(candidate, false, pingTimeoutMs_);
            if (rc != kS7Ok) {
                closeSocket(candidate);
                lastError_ = rc;
                continue;
            }

            int socketError = 0;
            socklen_t socketErrorLen = static_cast<socklen_t>(sizeof(socketError));
            if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &socketErrorLen) != 0
                || socketError != 0) {
                closeSocket(candidate);
                lastError_ = kS7ErrConnectFailed;
                continue;
            }
        }

        if (setSocketBlocking(candidate, true) != 0) {
            closeSocket(candidate);
            continue;
        }

        const int one = 1;
        setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&one), static_cast<int>(sizeof(one)));
        setSocketTimeout(candidate, kSocketSendTimeoutOpt, sendTimeoutMs_);
        setSocketTimeout(candidate, kSocketRecvTimeoutOpt, recvTimeoutMs_);

        sockaddr_storage local{};
        socklen_t localLen = static_cast<socklen_t>(sizeof(local));
        if (getsockname(candidate, reinterpret_cast<sockaddr*>(&local), &localLen) == 0) {
            if (local.ss_family == AF_INET) {
                localPort_ = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
            } else if (local.ss_family == AF_INET6) {
                localPort_ = ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
            }
        }

        socket_ = candidate;
        return lastError_ = kS7Ok;
    }

    return lastError_ == kS7Ok ? (lastError_ = kS7ErrConnectFailed) : lastError_;
#endif
}

inline int Client::sendAll(const std::uint8_t* data, std::size_t size) {
    if (transportHooks_.send) {
        const int rc = transportHooks_.send(data, size);
        if (rc != kS7Ok) {
            disconnect();
            return lastError_ = rc;
        }
        return lastError_ = kS7Ok;
    }

#if !S7_ENABLE_BLOCKING_CLIENT
    (void)data;
    (void)size;
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "send.blocking-disabled");
#else
    std::size_t sent = 0;
    while (sent < size) {
        const int waitRc = waitForSocket(socket_, false, sendTimeoutMs_);
        if (waitRc != kS7Ok) {
            closeSocketOnly();
            return lastError_ = waitRc;
        }

        const int rc = send(socket_,
            reinterpret_cast<const char*>(data + sent),
            static_cast<int>(size - sent),
            0);
        if (rc <= 0) {
            closeSocketOnly();
            return lastError_ = kS7ErrSocketIo;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return lastError_ = kS7Ok;
#endif
}

inline int Client::recvAll(std::uint8_t* data, std::size_t size, bool closeOnTimeout) {
    if (transportHooks_.recv) {
        const int rc = transportHooks_.recv(data, size, recvTimeoutMs_);
        if (rc != kS7Ok) {
            if (rc == kS7ErrTimeout && !closeOnTimeout) {
                return lastError_ = rc;
            }
            disconnect();
            return lastError_ = rc;
        }
        return lastError_ = kS7Ok;
    }

#if !S7_ENABLE_BLOCKING_CLIENT
    (void)data;
    (void)size;
    (void)closeOnTimeout;
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "recv.blocking-disabled");
#else
    std::size_t received = 0;
    while (received < size) {
        const int waitRc = waitForSocket(socket_, true, recvTimeoutMs_);
        if (waitRc != kS7Ok) {
            if (waitRc == kS7ErrTimeout && !closeOnTimeout) {
                return lastError_ = waitRc;
            }
            closeSocketOnly();
            return lastError_ = waitRc;
        }

        const int rc = recv(socket_,
            reinterpret_cast<char*>(data + received),
            static_cast<int>(size - received),
            0);
        if (rc <= 0) {
            closeSocketOnly();
            return lastError_ = kS7ErrSocketIo;
        }
        received += static_cast<std::size_t>(rc);
    }
    return lastError_ = kS7Ok;
#endif
}

inline int Client::recvTpktFrame(std::vector<std::uint8_t>& frame, bool closeOnTimeout) {
    std::array<std::uint8_t, 4> header{};
    int rc = recvAll(header.data(), header.size(), closeOnTimeout);
    if (rc != kS7Ok) {
        return rc;
    }

    if (header[0] != kIsoTcpVersion) {
        closeSocketOnly();
        return lastError_ = kS7ErrProtocol;
    }

    const std::uint16_t totalLength = readBe16(header.data() + 2);
    if (totalLength < 4) {
        closeSocketOnly();
        return lastError_ = kS7ErrProtocol;
    }

    frame.resize(totalLength);
    std::copy(header.begin(), header.end(), frame.begin());
    if (totalLength > 4) {
        rc = recvAll(frame.data() + 4, totalLength - 4, closeOnTimeout);
        if (rc != kS7Ok) {
            return rc;
        }
    }
    return lastError_ = kS7Ok;
}

inline std::vector<std::uint8_t> Client::buildDisconnectFrame(
    std::uint16_t dstRef,
    std::uint16_t srcRef,
    std::uint8_t reason) const {

    std::vector<std::uint8_t> frame;
    frame.reserve(11);
    frame.push_back(kIsoTcpVersion);
    frame.push_back(0x00);
    appendBe16(frame, 11);
    frame.push_back(0x06);
    frame.push_back(kCotpDr);
    appendBe16(frame, dstRef);
    appendBe16(frame, srcRef);
    frame.push_back(reason);
    return frame;
}

inline int Client::sendDisconnectProbe(std::string_view stage,
                                       std::uint16_t dstRef,
                                       std::uint16_t srcRef,
                                       std::uint8_t reason) {
    auto frame = buildDisconnectFrame(dstRef, srcRef, reason);
    traceFrame(stage, true, frame);
    return sendAll(frame.data(), frame.size());
}

inline int Client::sendConnectionRequest() {
    const auto sourceRef = nextSourceRef();
    std::vector<std::uint8_t> frame;
    frame.reserve(22);
    frame.push_back(kIsoTcpVersion);
    frame.push_back(0x00);
    appendBe16(frame, 22);
    frame.push_back(0x11);
    frame.push_back(kCotpCr);
    frame.push_back(0x00);
    frame.push_back(0x00);
    appendBe16(frame, sourceRef);
    frame.push_back(0x00);
    frame.push_back(0xC0);
    frame.push_back(0x01);
    frame.push_back(encodeTpduSize(kDefaultIsoPduSize));
    frame.push_back(0xC1);
    frame.push_back(0x02);
    appendBe16(frame, localTsap_);
    frame.push_back(0xC2);
    frame.push_back(0x02);
    appendBe16(frame, remoteTsap_);

    traceFrame("iso.cr", true, frame);
    int rc = sendAll(frame.data(), frame.size());
    if (rc != kS7Ok) {
        return rc;
    }

    std::vector<std::uint8_t> response;
    rc = recvTpktFrame(response, false);
    if (rc != kS7Ok) {
        if (rc == kS7ErrTimeout) {
            return rc;
        }
        closeSocketOnly();
        return rc;
    }
    traceFrame("iso.cc", false, response);

    if (response.size() < 11 || response[5] != kCotpCc) {
        closeSocketOnly();
        return lastError_ = kS7ErrProtocol;
    }
    disconnectFrame_ = response;
    return lastError_ = kS7Ok;
}

inline int Client::negotiatePduLength() {
    std::vector<std::uint8_t> request;
    request.reserve(18);
    request.push_back(kS7ProtocolId);
    request.push_back(kS7Request);
    appendBe16(request, 0x0000);
    appendBe16(request, nextSequence());
    appendBe16(request, 8);
    appendBe16(request, 0);
    request.push_back(kS7FuncNegotiate);
    request.push_back(0x00);
    appendBe16(request, 1);
    appendBe16(request, 1);
    appendBe16(request, pduRequestLength_);

    std::vector<std::uint8_t> response;
    const int rc = exchangePayload(
        "s7.setup-comm.req", "s7.setup-comm.resp", request, response, kS7FuncNegotiate);
    if (rc != kS7Ok) {
        return rc;
    }

    if (response.size() < 20 || response[0] != kS7ProtocolId) {
        return lastError_ = kS7ErrPduNegotiation;
    }
    const std::uint16_t error = readBe16(response.data() + 10);
    if (error != 0) {
        return lastError_ = static_cast<int>(error);
    }
    const std::uint16_t parLen = readBe16(response.data() + 6);
    if (12 + parLen > response.size() || parLen < 8) {
        return lastError_ = kS7ErrPduNegotiation;
    }

    pduLength_ = readBe16(response.data() + 18);
    if (pduLength_ == 0) {
        pduLength_ = kDefaultS7PduRequest;
    }
    return lastError_ = kS7Ok;
}

inline int Client::sendPayload(std::string_view stage, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    frame.reserve(7 + payload.size());
    frame.push_back(kIsoTcpVersion);
    frame.push_back(0x00);
    appendBe16(frame, static_cast<std::uint16_t>(7 + payload.size()));
    frame.push_back(kCotpDtLength);
    frame.push_back(kCotpDt);
    frame.push_back(kCotpEot);
    frame.insert(frame.end(), payload.begin(), payload.end());
    traceFrame(stage, true, frame);
    return sendAll(frame.data(), frame.size());
}

inline int Client::recvPayload(std::string_view stage, std::vector<std::uint8_t>& payload) {
    payload.clear();
    while (true) {
        std::vector<std::uint8_t> frame;
        const int rc = recvTpktFrame(frame);
        if (rc != kS7Ok) {
            return rc;
        }
        traceFrame(stage, false, frame);

        if (frame.size() < 7 || frame[4] != kCotpDtLength || frame[5] != kCotpDt) {
            closeSocketOnly();
            return lastError_ = kS7ErrProtocol;
        }

        payload.insert(payload.end(), frame.begin() + 7, frame.end());
        if ((frame[6] & kCotpEot) == kCotpEot) {
            break;
        }
    }
    return lastError_ = kS7Ok;
}

inline int Client::exchangePayload(std::string_view requestStage, std::string_view responseStage,
                                   const std::vector<std::uint8_t>& request,
                                   std::vector<std::uint8_t>& response,
                                   std::uint8_t expectedFunction) {
    int rc = sendPayload(requestStage, request);
    if (rc != kS7Ok) {
        return rc;
    }
    rc = recvPayload(responseStage, response);
    if (rc != kS7Ok) {
        return rc;
    }
    if (response.size() < 12 || response[0] != kS7ProtocolId
        || (response[1] != kS7AckData && response[1] != 0x02)) {
        return setError(ErrorDomain::S7Header, kS7ErrProtocol, responseStage);
    }

    if (request.size() < 13 || readBe16(request.data() + 4) != readBe16(response.data() + 4)) {
        return setError(ErrorDomain::S7Header, kS7ErrSequenceMismatch, responseStage);
    }

    const std::uint16_t parLen = readBe16(response.data() + 6);
    if (parLen > 0) {
        const std::size_t functionOffset = response[1] == kS7AckData ? 12 : 10;
        if (functionOffset + parLen > response.size() || response[functionOffset] != expectedFunction) {
            return setError(ErrorDomain::S7Header, kS7ErrFunctionMismatch, responseStage);
        }
    }

    return setError(ErrorDomain::None, kS7Ok, responseStage);
}

inline std::vector<std::uint8_t> Client::buildReadRequest(int area, int dbNumber, int start, int amount, int wordLen) {
    DataItem item{
        .area = area,
        .dbNumber = dbNumber,
        .start = start,
        .amount = amount,
        .wordLen = wordLen
    };
    std::vector<DataItem> items{item};
    return buildReadRequest(items, 0, 1);
}

inline std::vector<std::uint8_t> Client::buildReadRequest(
    const std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
    std::vector<std::uint8_t> payload;
    payload.reserve(readRequestPduSize(end - begin));
    payload.push_back(kS7ProtocolId);
    payload.push_back(kS7Request);
    appendBe16(payload, 0x0000);
    appendBe16(payload, nextSequence());
    appendBe16(payload, static_cast<std::uint16_t>(2 + (end - begin) * 12));
    appendBe16(payload, 0);
    payload.push_back(kS7FuncRead);
    payload.push_back(static_cast<std::uint8_t>(end - begin));
    for (std::size_t index = begin; index < end; ++index) {
        const auto& item = items[index];
        payload.push_back(0x12);
        payload.push_back(0x0A);
        payload.push_back(0x10);
        payload.push_back(static_cast<std::uint8_t>(item.wordLen & 0xFF));
        appendBe16(payload, static_cast<std::uint16_t>(item.amount));
        appendBe16(payload, static_cast<std::uint16_t>(item.area == S7AreaDB ? item.dbNumber : 0));
        payload.push_back(static_cast<std::uint8_t>(item.area & 0xFF));

        const int address = itemStartAddress(item.start, item.wordLen);
        payload.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>(address & 0xFF));
    }
    return payload;
}

inline std::vector<std::uint8_t> Client::buildWriteRequest(int area, int dbNumber, int start, int amount,
                                                           int wordLen, const std::uint8_t* source,
                                                           std::size_t size) {
    DataItem item{
        .area = area,
        .dbNumber = dbNumber,
        .start = start,
        .amount = amount,
        .wordLen = wordLen,
        .data = const_cast<std::uint8_t*>(source),
        .capacity = size
    };
    std::vector<DataItem> items{item};
    return buildWriteRequest(items, 0, 1);
}

inline std::vector<std::uint8_t> Client::buildWriteRequest(
    const std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
    std::vector<std::uint8_t> payload;
    payload.reserve(writeRequestPduSize(items, begin, end));
    payload.push_back(kS7ProtocolId);
    payload.push_back(kS7Request);
    appendBe16(payload, 0x0000);
    appendBe16(payload, nextSequence());
    appendBe16(payload, static_cast<std::uint16_t>(2 + (end - begin) * 12));
    const auto dataLengthOffset = payload.size();
    appendBe16(payload, 0);
    payload.push_back(kS7FuncWrite);
    payload.push_back(static_cast<std::uint8_t>(end - begin));
    for (std::size_t index = begin; index < end; ++index) {
        const auto& item = items[index];
        payload.push_back(0x12);
        payload.push_back(0x0A);
        payload.push_back(0x10);
        payload.push_back(static_cast<std::uint8_t>(item.wordLen & 0xFF));
        appendBe16(payload, static_cast<std::uint16_t>(item.amount));
        appendBe16(payload, static_cast<std::uint16_t>(item.area == S7AreaDB ? item.dbNumber : 0));
        payload.push_back(static_cast<std::uint8_t>(item.area & 0xFF));

        const int address = itemStartAddress(item.start, item.wordLen);
        payload.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>(address & 0xFF));
    }

    const auto dataStart = payload.size();
    for (std::size_t index = begin; index < end; ++index) {
        const auto& item = items[index];
        const auto* source = static_cast<const std::uint8_t*>(item.data);
        const std::size_t size = transferByteSize(item.amount, item.wordLen);
        payload.push_back(0x00);
        const std::uint8_t transportSize = writeTransportSize(item.wordLen);
        payload.push_back(transportSize);
        if (transportSize != kTsResOctet && transportSize != kTsResReal && transportSize != kTsResBit) {
            appendBe16(payload, static_cast<std::uint16_t>(size * 8));
        } else if (transportSize == kTsResBit) {
            appendBe16(payload, static_cast<std::uint16_t>(item.amount));
        } else {
            appendBe16(payload, static_cast<std::uint16_t>(size));
        }
        payload.insert(payload.end(), source, source + size);
        if (index + 1 < end && payload.size() % 2 != 0) {
            payload.push_back(0x00);
        }
    }
    const auto dataLen = static_cast<std::uint16_t>(payload.size() - dataStart);
    payload[dataLengthOffset] = static_cast<std::uint8_t>((dataLen >> 8) & 0xFF);
    payload[dataLengthOffset + 1] = static_cast<std::uint8_t>(dataLen & 0xFF);
    return payload;
}

inline std::optional<int> readS7PayloadErrorCode(const std::vector<std::uint8_t>& response) {
    if (response.size() < 12 || response[0] != kS7ProtocolId) {
        return std::nullopt;
    }
    return static_cast<int>(readBe16(response.data() + 10));
}

inline int Client::parseReadResponse(const std::vector<std::uint8_t>& response, std::uint8_t* target,
                                     std::size_t capacity, std::size_t& copied) {
    copied = 0;
    // PLC rejections can arrive as short S7 error frames with no data section.
    if (const auto error = readS7PayloadErrorCode(response); error && *error != 0) {
        return *error;
    }
    if (response.size() < 18) {
        return kS7ErrResponseTooShort;
    }

    const std::uint16_t parLen = readBe16(response.data() + 6);
    const std::uint16_t dataLen = readBe16(response.data() + 8);
    const std::size_t dataOffset = 12 + parLen;
    if (dataOffset + 4 > response.size() || dataOffset + dataLen > response.size()) {
        return kS7ErrProtocol;
    }
    if (parLen < 2 || response[12] != kS7FuncRead || response[13] != 0x01 || dataLen < 4) {
        return kS7ErrProtocol;
    }

    const std::uint8_t returnCode = response[dataOffset];
    if (returnCode != 0xFF) {
        return static_cast<int>(returnCode);
    }

    const std::uint8_t transportSize = response[dataOffset + 1];
    const std::uint16_t payloadLength = readBe16(response.data() + dataOffset + 2);
    copied = decodeResponseDataSize(transportSize, payloadLength);
    if (copied > capacity || dataOffset + 4 + copied > response.size()) {
        return kS7ErrProtocol;
    }

    std::memcpy(target, response.data() + dataOffset + 4, copied);
    return kS7Ok;
}

inline int Client::parseReadResponse(const std::vector<std::uint8_t>& response,
                                     std::vector<DataItem>& items, std::size_t begin, std::size_t end) {
    if (const auto error = readS7PayloadErrorCode(response); error && *error != 0) {
        for (std::size_t index = begin; index < end; ++index) {
            items[index].result = *error;
        }
        return *error;
    }
    if (response.size() < 18) {
        return kS7ErrResponseTooShort;
    }

    const std::uint16_t parLen = readBe16(response.data() + 6);
    const std::uint16_t dataLen = readBe16(response.data() + 8);
    const std::size_t itemCount = end - begin;
    const std::size_t dataOffset = 12 + parLen;
    if (dataOffset + dataLen > response.size()) {
        return kS7ErrResponseLengthMismatch;
    }
    if (parLen < 2 || response[12] != kS7FuncRead || response[13] != itemCount) {
        return kS7ErrItemCountMismatch;
    }

    std::size_t cursor = dataOffset;
    int firstError = kS7Ok;
    for (std::size_t index = begin; index < end; ++index) {
        auto& item = items[index];
        item.transferred = 0;
        item.result = kS7Ok;
        if (cursor + 4 > response.size() || cursor + 4 > dataOffset + dataLen) {
            item.result = kS7ErrResponseLengthMismatch;
            return item.result;
        }

        const std::uint8_t returnCode = response[cursor];
        if (returnCode != 0xFF) {
            item.result = static_cast<int>(returnCode);
            if (firstError == kS7Ok) {
                firstError = item.result;
            }
            cursor += 4;
            continue;
        }

        const std::uint8_t transportSize = response[cursor + 1];
        const std::uint16_t payloadLength = readBe16(response.data() + cursor + 2);
        const std::size_t copied = decodeResponseDataSize(transportSize, payloadLength);
        if (copied > item.capacity || cursor + 4 + copied > response.size() || cursor + 4 + copied > dataOffset + dataLen) {
            item.result = kS7ErrResponseLengthMismatch;
            return item.result;
        }
        std::memcpy(item.data, response.data() + cursor + 4, copied);
        item.transferred = copied;
        cursor += 4 + copied;
        if (index + 1 < end && cursor < dataOffset + dataLen && cursor % 2 != 0) {
            ++cursor;
        }
    }

    return firstError;
}

inline int Client::parseWriteResponse(const std::vector<std::uint8_t>& response) const {
    if (const auto error = readS7PayloadErrorCode(response); error && *error != 0) {
        return *error;
    }
    if (response.size() < 15) {
        return kS7ErrResponseTooShort;
    }

    const std::uint16_t parLen = readBe16(response.data() + 6);
    const std::uint16_t dataLen = readBe16(response.data() + 8);
    const std::size_t dataOffset = 12 + parLen;
    if (dataOffset >= response.size() || dataOffset + dataLen > response.size()) {
        return kS7ErrProtocol;
    }
    if (parLen < 2 || response[12] != kS7FuncWrite || response[13] != 0x01 || dataLen < 1) {
        return kS7ErrProtocol;
    }

    const std::uint8_t returnCode = response[dataOffset];
    return returnCode == 0xFF ? kS7Ok : static_cast<int>(returnCode);
}

inline int Client::parseWriteResponse(const std::vector<std::uint8_t>& response,
                                      std::vector<DataItem>& items, std::size_t begin, std::size_t end) const {
    if (const auto error = readS7PayloadErrorCode(response); error && *error != 0) {
        for (std::size_t index = begin; index < end; ++index) {
            items[index].result = *error;
        }
        return *error;
    }
    if (response.size() < 15) {
        return kS7ErrResponseTooShort;
    }

    const std::uint16_t parLen = readBe16(response.data() + 6);
    const std::uint16_t dataLen = readBe16(response.data() + 8);
    const std::size_t itemCount = end - begin;
    const std::size_t dataOffset = 12 + parLen;
    if (dataOffset + dataLen > response.size()) {
        return kS7ErrResponseLengthMismatch;
    }
    if (parLen < 2 || response[12] != kS7FuncWrite || response[13] != itemCount || dataLen < itemCount) {
        return kS7ErrItemCountMismatch;
    }

    int firstError = kS7Ok;
    for (std::size_t index = begin; index < end; ++index) {
        const auto returnCode = response[dataOffset + (index - begin)];
        items[index].transferred = returnCode == 0xFF ? transferByteSize(items[index].amount, items[index].wordLen) : 0;
        items[index].result = returnCode == 0xFF ? kS7Ok : static_cast<int>(returnCode);
        if (firstError == kS7Ok && items[index].result != kS7Ok) {
            firstError = items[index].result;
        }
    }
    return firstError;
}

inline int Client::readMultiVars(std::vector<DataItem>& items) {
#if !S7_ENABLE_BLOCKING_CLIENT
    for (auto& item : items) {
        item.result = kS7ErrUnsupported;
        item.transferred = 0;
    }
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "readMultiVars.blocking-disabled");
#else
    std::lock_guard ioLock(ioMutex_);
    if (!connected()) {
        return setError(ErrorDomain::Client, kS7ErrNotConnected, "readMultiVars");
    }
    if (items.empty()) {
        return kS7Ok;
    }

    for (auto& item : items) {
        item.transferred = 0;
        item.result = kS7Ok;
        const std::size_t size = transferByteSize(item.amount, item.wordLen);
        if (!item.data || size == 0 || item.capacity < size || item.start < 0 || item.dbNumber < 0) {
            item.result = kS7ErrInvalidParams;
            return setError(ErrorDomain::Client, kS7ErrInvalidParams, "readMultiVars");
        }
    }

    std::size_t begin = 0;
    int firstError = kS7Ok;
    while (begin < items.size()) {
        if (!readItemFitsPdu(items[begin], pduLength_)) {
            auto& item = items[begin];
            const int rc = readAreaUnlocked(item.area, item.dbNumber, item.start, item.amount, item.wordLen, item.data);
            item.result = rc;
            item.transferred = rc == kS7Ok ? transferByteSize(item.amount, item.wordLen) : 0;
            if (rc != kS7Ok && firstError == kS7Ok) {
                firstError = rc;
            }
            ++begin;
            continue;
        }

        std::size_t end = begin + 1;
        while (end < items.size()) {
            const std::size_t nextCount = end - begin + 1;
            if (readRequestPduSize(nextCount) > pduLength_
                || readResponsePduSize(items, begin, end + 1) > pduLength_) {
                break;
            }
            ++end;
        }

        auto request = buildReadRequest(items, begin, end);
        std::vector<std::uint8_t> response;
        const int rc = exchangePayload("s7.read.multi.req", "s7.read.multi.resp", request, response, kS7FuncRead);
        if (rc != kS7Ok) {
            return rc;
        }

        const int parseRc = parseReadResponse(response, items, begin, end);
        if (parseRc != kS7Ok && firstError == kS7Ok) {
            firstError = parseRc;
        }
        begin = end;
    }

    return firstError == kS7Ok ? kS7Ok : setError(ErrorDomain::S7Item, firstError, "readMultiVars");
#endif
}

inline int Client::writeMultiVars(std::vector<DataItem>& items) {
#if !S7_ENABLE_BLOCKING_CLIENT
    for (auto& item : items) {
        item.result = kS7ErrUnsupported;
        item.transferred = 0;
    }
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "writeMultiVars.blocking-disabled");
#else
    std::lock_guard ioLock(ioMutex_);
    if (!connected()) {
        return setError(ErrorDomain::Client, kS7ErrNotConnected, "writeMultiVars");
    }
    if (items.empty()) {
        return kS7Ok;
    }

    for (auto& item : items) {
        item.transferred = 0;
        item.result = kS7Ok;
        const std::size_t size = transferByteSize(item.amount, item.wordLen);
        if (!item.data || size == 0 || item.capacity < size || item.start < 0 || item.dbNumber < 0) {
            item.result = kS7ErrInvalidParams;
            return setError(ErrorDomain::Client, kS7ErrInvalidParams, "writeMultiVars");
        }
    }

    std::size_t begin = 0;
    int firstError = kS7Ok;
    while (begin < items.size()) {
        if (!writeItemFitsPdu(items[begin], pduLength_)) {
            auto& item = items[begin];
            const int rc = writeAreaUnlocked(item.area, item.dbNumber, item.start, item.amount, item.wordLen, item.data);
            item.result = rc;
            item.transferred = rc == kS7Ok ? transferByteSize(item.amount, item.wordLen) : 0;
            if (rc != kS7Ok && firstError == kS7Ok) {
                firstError = rc;
            }
            ++begin;
            continue;
        }

        std::size_t end = begin + 1;
        while (end < items.size()) {
            const std::size_t nextCount = end - begin + 1;
            if (writeRequestPduSize(items, begin, end + 1) > pduLength_
                || writeResponsePduSize(nextCount) > pduLength_) {
                break;
            }
            ++end;
        }

        auto request = buildWriteRequest(items, begin, end);
        std::vector<std::uint8_t> response;
        const int rc = exchangePayload("s7.write.multi.req", "s7.write.multi.resp", request, response, kS7FuncWrite);
        if (rc != kS7Ok) {
            return rc;
        }

        const int parseRc = parseWriteResponse(response, items, begin, end);
        if (parseRc != kS7Ok && firstError == kS7Ok) {
            firstError = parseRc;
        }
        begin = end;
    }

    return firstError == kS7Ok ? kS7Ok : setError(ErrorDomain::S7Item, firstError, "writeMultiVars");
#endif
}

inline int Client::readArea(int area, int dbNumber, int start, int amount, int wordLen, void* data) {
#if !S7_ENABLE_BLOCKING_CLIENT
    (void)area;
    (void)dbNumber;
    (void)start;
    (void)amount;
    (void)wordLen;
    (void)data;
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "readArea.blocking-disabled");
#else
    std::lock_guard ioLock(ioMutex_);
    return readAreaUnlocked(area, dbNumber, start, amount, wordLen, data);
#endif
}

inline int Client::readAreaUnlocked(int area, int dbNumber, int start, int amount, int wordLen, void* data) {
    if (!connected() || !data) {
        return connected() ? kS7ErrInvalidParams : kS7ErrNotConnected;
    }

    const int elementSize = wordLenByteSize(wordLen);
    if (elementSize <= 0 || amount <= 0 || start < 0 || dbNumber < 0) {
        return kS7ErrInvalidParams;
    }

    auto* target = static_cast<std::uint8_t*>(data);
    int remaining = amount;
    int currentStart = start;
    std::size_t offset = 0;

    const int maxElements = maxElementsPerPdu(pduLength_, 18, elementSize);

    while (remaining > 0) {
        const int sliceElements = std::min(remaining, maxElements);
        auto request = buildReadRequest(area, dbNumber, currentStart, sliceElements, wordLen);
        std::vector<std::uint8_t> response;
        const int rc = exchangePayload("s7.read.req", "s7.read.resp", request, response, kS7FuncRead);
        if (rc != kS7Ok) {
            return rc;
        }

        std::size_t copied = 0;
        const int parseRc = parseReadResponse(
            response,
            target + offset,
            static_cast<std::size_t>(sliceElements) * elementSize,
            copied);
        if (parseRc != kS7Ok) {
            return parseRc;
        }

        offset += copied;
        remaining -= sliceElements;
        currentStart += sliceElements * elementSize;
    }

    return kS7Ok;
}

inline int Client::writeArea(int area, int dbNumber, int start, int amount, int wordLen, void* data) {
#if !S7_ENABLE_BLOCKING_CLIENT
    (void)area;
    (void)dbNumber;
    (void)start;
    (void)amount;
    (void)wordLen;
    (void)data;
    return setError(ErrorDomain::Client, kS7ErrUnsupported, "writeArea.blocking-disabled");
#else
    std::lock_guard ioLock(ioMutex_);
    return writeAreaUnlocked(area, dbNumber, start, amount, wordLen, data);
#endif
}

inline int Client::writeAreaUnlocked(int area, int dbNumber, int start, int amount, int wordLen, void* data) {
    if (!connected() || !data) {
        return connected() ? kS7ErrInvalidParams : kS7ErrNotConnected;
    }

    const int elementSize = wordLenByteSize(wordLen);
    if (elementSize <= 0 || amount <= 0 || start < 0 || dbNumber < 0) {
        return kS7ErrInvalidParams;
    }

    const auto* source = static_cast<const std::uint8_t*>(data);
    int remaining = amount;
    int currentStart = start;
    std::size_t offset = 0;

    const int maxElements = maxElementsPerPdu(pduLength_, 28, elementSize);

    while (remaining > 0) {
        const int sliceElements = std::min(remaining, maxElements);
        const std::size_t sliceBytes = static_cast<std::size_t>(sliceElements) * elementSize;
        auto request = buildWriteRequest(area, dbNumber, currentStart, sliceElements,
            wordLen, source + offset, sliceBytes);
        std::vector<std::uint8_t> response;
        const int rc = exchangePayload("s7.write.req", "s7.write.resp", request, response, kS7FuncWrite);
        if (rc != kS7Ok) {
            return rc;
        }

        const int parseRc = parseWriteResponse(response);
        if (parseRc != kS7Ok) {
            return parseRc;
        }

        offset += sliceBytes;
        remaining -= sliceElements;
        currentStart += sliceElements * elementSize;
    }

    return kS7Ok;
}

}  // namespace s7
