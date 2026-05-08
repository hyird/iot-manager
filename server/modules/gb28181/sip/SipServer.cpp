#include "sip/SipServer.h"

#include "common/network/TcpLinkManager.hpp"
#include "sip/DigestAuth.h"
#include "sip/SipMessage.h"

#include <trantor/utils/Logger.h>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <future>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalidSocket = INVALID_SOCKET;
using SockLen = int;
#else
using NativeSocket = int;
constexpr NativeSocket invalidSocket = -1;
using SockLen = socklen_t;
#endif

struct TcpConnectionContext {
    std::string pending;
    SipServer::SipPeer peer;
};

std::string extractUserFromSipUri(const std::string& value) {
    const auto sip = value.find("sip:");
    if (sip == std::string::npos) {
        return {};
    }
    const auto begin = sip + 4;
    const auto end = value.find_first_of("@;>", begin);
    if (end == std::string::npos) {
        return value.substr(begin);
    }
    return value.substr(begin, end - begin);
}

std::string extractTag(const std::string& value) {
    const auto tag = value.find("tag=");
    if (tag == std::string::npos) {
        return {};
    }
    const auto begin = tag + 4;
    const auto end = value.find_first_of(";> \t\r\n", begin);
    if (end == std::string::npos) {
        return value.substr(begin);
    }
    return value.substr(begin, end - begin);
}

unsigned int extractCSeqNumber(const std::string& value) {
    std::istringstream input(value);
    unsigned int cseq = 0;
    input >> cseq;
    return cseq;
}

std::string peerToString(const SipServer::SipPeer& peer) {
    return peer.address + ":" + std::to_string(peer.port);
}

const char* transportName(SipServer::SipTransport transport) {
    return transport == SipServer::SipTransport::Tcp ? "TCP" : "UDP";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::size_t> contentLengthOf(const std::string& headerText) {
    std::istringstream lines(headerText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (lower(line.substr(0, colon)) == "content-length") {
            try {
                return static_cast<std::size_t>(std::stoul(line.substr(colon + 1)));
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return 0;
}

std::string xmlText(const pugi::xml_node& node, const char* name) {
    return node.child(name).text().as_string();
}

bool statusOnline(const std::string& status) {
    return status == "ON" || status == "ONLINE" || status == "OK";
}

std::string makeToken(const char* prefix) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream output;
    output << prefix << std::chrono::steady_clock::now().time_since_epoch().count() << rng();
    return output.str();
}

std::string ptzCommand(const std::string& action, uint8_t speed) {
    const auto clamped = static_cast<uint8_t>(std::min<unsigned int>(speed, 255));
    uint8_t command = 0x00;
    uint8_t panSpeed = 0x00;
    uint8_t tiltSpeed = 0x00;
    uint8_t zoomSpeed = 0x00;

    if (action == "left") {
        command = 0x02;
        panSpeed = clamped;
    } else if (action == "right") {
        command = 0x01;
        panSpeed = clamped;
    } else if (action == "up") {
        command = 0x08;
        tiltSpeed = clamped;
    } else if (action == "down") {
        command = 0x04;
        tiltSpeed = clamped;
    } else if (action == "zoomin") {
        command = 0x10;
        zoomSpeed = clamped;
    } else if (action == "zoomout") {
        command = 0x20;
        zoomSpeed = clamped;
    }

    std::array<uint8_t, 8> bytes{0xA5, 0x0F, 0x01, command, panSpeed, tiltSpeed, zoomSpeed, 0x00};
    unsigned int checksum = 0;
    for (std::size_t i = 0; i < bytes.size() - 1; ++i) {
        checksum += bytes[i];
    }
    bytes.back() = static_cast<uint8_t>(checksum & 0xFF);

    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

long long gbTimeToUnixSeconds(const std::string& value) {
    std::tm tm{};
    std::istringstream input(value);
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        input.clear();
        input.str(value);
        input >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    }
    if (input.fail()) {
        return 0;
    }
#ifdef _WIN32
    return static_cast<long long>(_mkgmtime(&tm));
#else
    return static_cast<long long>(timegm(&tm));
#endif
}

bool isInvalidSocket(NativeSocket socket) {
    return socket == invalidSocket;
}

void closeSocket(NativeSocket socket) {
    if (isInvalidSocket(socket)) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

std::string socketErrorMessage() {
#ifdef _WIN32
    return "WSA error=" + std::to_string(WSAGetLastError());
#else
    return "errno=" + std::to_string(errno);
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    const auto error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void setNonBlocking(NativeSocket socket) {
#ifdef _WIN32
    u_long mode = 1;
    if (::ioctlsocket(socket, FIONBIO, &mode) != 0) {
        throw std::runtime_error("Could not set UDP socket non-blocking: " + socketErrorMessage());
    }
#else
    const auto flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("Could not set UDP socket non-blocking: " + socketErrorMessage());
    }
#endif
}

sockaddr_in makeSocketAddress(const std::string& host, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("Invalid IPv4 address: " + host);
    }
    return address;
}

std::string socketAddressToIp(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return buffer;
}

} // namespace

SipServer::SipServer(SipConfig sipConfig, MediaConfig mediaConfig, DeviceRegistry& deviceRegistry, ZlmClient& zlmClient)
    : sipConfig_(std::move(sipConfig)),
      mediaConfig_(std::move(mediaConfig)),
      deviceRegistry_(deviceRegistry),
      zlmClient_(zlmClient) {}

SipServer::~SipServer() {
    stop();
}

void SipServer::start() {
    if (running_.exchange(true)) {
        return;
    }

    ioLoop_ = TcpLinkManager::instance().getNextIoLoop();
    if (ioLoop_->isInLoopThread()) {
        try {
            startInLoop();
        } catch (...) {
            running_.store(false);
            throw;
        }
        return;
    }

    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    ioLoop_->queueInLoop([this, promise]() {
        try {
            startInLoop();
            promise->set_value();
        } catch (...) {
            running_.store(false);
            promise->set_exception(std::current_exception());
        }
    });
    future.get();
}

void SipServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (ioLoop_ == nullptr) {
        return;
    }
    if (ioLoop_->isInLoopThread()) {
        stopInLoop();
    } else {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        ioLoop_->queueInLoop([this, promise]() {
            stopInLoop();
            promise->set_value();
        });
        future.get();
    }
}

void SipServer::startInLoop() {
    if (ioLoop_ == nullptr) {
        throw std::runtime_error("GB28181 SIP IO loop is not available");
    }

    udpSocket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (isInvalidSocket(udpSocket_)) {
        throw std::runtime_error("Could not create GB28181 UDP socket: " + socketErrorMessage());
    }
    int reuse = 1;
    ::setsockopt(udpSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    setNonBlocking(udpSocket_);

    const auto udpAddress = makeSocketAddress(sipConfig_.host, sipConfig_.port);
    if (::bind(udpSocket_, reinterpret_cast<const sockaddr*>(&udpAddress), sizeof(udpAddress)) != 0) {
        const auto message = socketErrorMessage();
        closeSocket(udpSocket_);
        udpSocket_ = invalidSocket;
        throw std::runtime_error("Could not bind GB28181 UDP socket: " + message);
    }

    udpChannel_ = std::make_unique<trantor::Channel>(ioLoop_, static_cast<int>(udpSocket_));
    udpChannel_->setReadCallback([this]() { handleUdpReadable(); });
    udpChannel_->enableReading();

    const auto tcpAddress = trantor::InetAddress(sipConfig_.host, sipConfig_.port);
#ifdef _WIN32
    tcpServer_ = std::make_shared<trantor::TcpServer>(ioLoop_, tcpAddress, "Gb28181SipTcpServer", true, false);
#else
    tcpServer_ = std::make_shared<trantor::TcpServer>(ioLoop_, tcpAddress, "Gb28181SipTcpServer");
#endif
    tcpServer_->setConnectionCallback([this](const trantor::TcpConnectionPtr& connection) {
        handleTcpConnection(connection);
    });
    tcpServer_->setRecvMessageCallback([this](const trantor::TcpConnectionPtr& connection, trantor::MsgBuffer* buffer) {
        handleTcpMessage(connection, buffer);
    });
    tcpServer_->start();

    LOG_INFO << "SIP UDP listening on " << sipConfig_.host << ":" << sipConfig_.port << " via TcpIoPool";
    LOG_INFO << "SIP TCP listening on " << sipConfig_.host << ":" << sipConfig_.port << " via TcpIoPool";
}

void SipServer::stopInLoop() {
    if (udpChannel_) {
        udpChannel_->disableAll();
        udpChannel_->remove();
        udpChannel_.reset();
    }
    closeSocket(udpSocket_);
    udpSocket_ = invalidSocket;

    if (tcpServer_) {
        tcpServer_->stop();
        tcpServer_.reset();
    }

    {
        std::lock_guard lock(tcpConnectionsMutex_);
        for (auto& [_, connection] : tcpConnections_) {
            if (connection && connection->connected()) {
                connection->forceClose();
            }
        }
        tcpConnections_.clear();
    }
}

void SipServer::handleUdpReadable() {
    while (running_) {
        std::array<char, 8192> buffer{};
        sockaddr_in remoteAddress{};
        SockLen remoteLength = sizeof(remoteAddress);
        const auto size = ::recvfrom(
            udpSocket_,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&remoteAddress),
            &remoteLength);
        if (size < 0) {
            if (!wouldBlock() && running_) {
                LOG_WARN << "SIP UDP receive failed: " << socketErrorMessage();
            }
            return;
        }

        SipPeer peer;
        peer.transport = SipTransport::Udp;
        peer.udp = remoteAddress;
        peer.address = socketAddressToIp(remoteAddress);
        peer.port = ntohs(remoteAddress.sin_port);
        handlePacket(std::string(buffer.data(), static_cast<size_t>(size)), peer);
    }
}

void SipServer::handleTcpConnection(const trantor::TcpConnectionPtr& connection) {
    const auto key = connection->peerAddr().toIpPort();
    if (connection->connected()) {
        SipPeer peer;
        peer.transport = SipTransport::Tcp;
        peer.tcp = connection;
        peer.address = connection->peerAddr().toIp();
        peer.port = connection->peerAddr().toPort();

        auto context = std::make_shared<TcpConnectionContext>();
        context->peer = peer;
        connection->setContext(context);

        {
            std::lock_guard lock(tcpConnectionsMutex_);
            tcpConnections_[key] = connection;
        }
        LOG_INFO << "SIP TCP connected from " << key;
        return;
    }

    {
        std::lock_guard lock(tcpConnectionsMutex_);
        const auto iter = tcpConnections_.find(key);
        if (iter != tcpConnections_.end() && iter->second == connection) {
            tcpConnections_.erase(iter);
        }
    }
    connection->clearContext();
    LOG_INFO << "SIP TCP disconnected from " << key;
}

void SipServer::handleTcpMessage(const trantor::TcpConnectionPtr& connection, trantor::MsgBuffer* buffer) {
    auto context = connection->getContext<TcpConnectionContext>();
    if (!context) {
        return;
    }

    context->pending.append(buffer->peek(), buffer->readableBytes());
    buffer->retrieveAll();

    const auto key = connection->peerAddr().toIpPort();
    while (true) {
        const auto headerEnd = context->pending.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            break;
        }
        const auto contentLength = contentLengthOf(context->pending.substr(0, headerEnd));
        if (!contentLength.has_value()) {
            LOG_WARN << "SIP TCP message with invalid Content-Length from " << key;
            context->pending.clear();
            break;
        }
        const auto packetSize = headerEnd + 4 + *contentLength;
        if (context->pending.size() < packetSize) {
            break;
        }
        auto packet = context->pending.substr(0, packetSize);
        context->pending.erase(0, packetSize);
        handlePacket(packet, context->peer);
    }
}

void SipServer::handlePacket(const std::string& packet, const SipPeer& remote) {
    const auto message = SipMessage::parse(packet);
    if (!message.has_value()) {
        LOG_WARN << "Ignored malformed SIP packet from " << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }

    if (message->statusCode > 0) {
        handleResponse(*message, remote);
        return;
    }

    if (message->method == "REGISTER") {
        handleRegister(*message, remote);
        return;
    }

    if (message->method == "MESSAGE") {
        handleMessage(*message, remote);
        return;
    }

    sendResponse(*message, remote, 405, "Method Not Allowed");
}

void SipServer::handleResponse(const SipMessage& message, const SipPeer& remote) {
    const auto cseq = message.header("CSeq");
    if (message.statusCode == 200 && cseq.find("INVITE") != std::string::npos) {
        handleInviteOk(message, remote);
        return;
    }

    LOG_DEBUG << "Unhandled SIP response " << message.statusCode << " from " << transportName(remote.transport) << " " << peerToString(remote) << ", CSeq=" << cseq;
}

void SipServer::handleInviteOk(const SipMessage& message, const SipPeer& remote) {
    const auto callId = message.header("Call-ID");
    if (callId.empty()) {
        LOG_WARN << "INVITE 200 OK without Call-ID from " << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }

    PreviewSession session;
    bool found = false;
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& [_, candidate] : previewSessions_) {
            if (candidate.callId == callId) {
                session = candidate;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        LOG_WARN << "INVITE 200 OK for unknown Call-ID: " << callId;
        return;
    }

    const auto to = message.header("To");
    const auto cseq = extractCSeqNumber(message.header("CSeq"));
    const auto toTag = extractTag(to);
    if (!message.body.empty()) {
        LOG_INFO << "INVITE 200 OK SDP, call_id=" << callId << ", body=" << message.body;
    }
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& [_, candidate] : previewSessions_) {
            if (candidate.callId == callId) {
                candidate.toTag = toTag;
                candidate.established = true;
                break;
            }
        }
    }

    const auto host = remote.address;
    const auto port = remote.port;
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("ack");
    const auto transport = transportName(remote.transport);

    std::ostringstream ack;
    ack << "ACK sip:" << session.channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
        << "Via: SIP/2.0/" << transport << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
        << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << session.fromTag << "\r\n"
        << "To: " << to << "\r\n"
        << "Call-ID: " << callId << "\r\n"
        << "CSeq: " << cseq << " ACK\r\n"
        << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
        << "Max-Forwards: 70\r\n"
        << "User-Agent: gb28181-platform-cpp\r\n"
        << "Content-Length: 0\r\n\r\n";

    sendRequest(ack.str(), remote);
    LOG_INFO << "Preview ACK sent, call_id=" << callId << ", to_tag=" << toTag;
}

void SipServer::handleRegister(const SipMessage& message, const SipPeer& remote) {
    if (!DigestAuth::verifyRegister(message, sipConfig_.domain, sipConfig_.password)) {
        const auto nonce = DigestAuth::makeNonce();
        std::ostringstream auth;
        auth << "WWW-Authenticate: Digest realm=\"" << sipConfig_.domain
             << "\", nonce=\"" << nonce
             << "\", algorithm=MD5, qop=\"auth\"\r\n";
        sendResponse(message, remote, 401, "Unauthorized", auth.str());
        return;
    }

    auto deviceId = extractUserFromSipUri(message.header("From"));
    if (deviceId.empty()) {
        deviceId = extractUserFromSipUri(message.header("Contact"));
    }
    if (deviceId.empty()) {
        deviceId = remote.address;
    }

    deviceRegistry_.upsertRegistration(deviceId, peerToString(remote));
    LOG_INFO << "GB28181 device registered: " << deviceId << " from " << transportName(remote.transport) << " " << peerToString(remote);

    sendResponse(message, remote, 200, "OK");

    scheduleCatalogQuery(deviceId);
}

void SipServer::handleMessage(const SipMessage& message, const SipPeer& remote) {
    sendResponse(message, remote, 200, "OK");

    pugi::xml_document document;
    const auto result = document.load_string(message.body.c_str());
    if (!result) {
        LOG_WARN << "Ignored MESSAGE with invalid XML from " << transportName(remote.transport) << " " << peerToString(remote) << ": " << result.description();
        return;
    }

    auto root = document.first_child();
    const auto cmdType = xmlText(root, "CmdType");
    auto deviceId = xmlText(root, "DeviceID");
    if (deviceId.empty()) {
        LOG_WARN << "Ignored MESSAGE without DeviceID from " << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }
    const auto snText = xmlText(root, "SN");

    if (cmdType == "Keepalive") {
        const auto status = xmlText(root, "Status");
        if (statusOnline(status)) {
            const auto existing = deviceRegistry_.findDevice(deviceId);
            const auto shouldQueryCatalog = !existing.has_value() || existing->channels.empty();
            deviceRegistry_.updateKeepalive(deviceId, peerToString(remote));
            if (shouldQueryCatalog) {
                scheduleCatalogQuery(deviceId);
            }
        } else {
            deviceRegistry_.markOffline(deviceId);
        }
        LOG_DEBUG << "Keepalive from " << deviceId << ", status=" << status;
        return;
    }

    if (cmdType == "Catalog") {
        std::vector<Channel> channels;
        for (auto item : root.child("DeviceList").children("Item")) {
            Channel channel;
            channel.id = xmlText(item, "DeviceID");
            channel.name = xmlText(item, "Name");
            channel.manufacturer = xmlText(item, "Manufacturer");
            channel.online = statusOnline(xmlText(item, "Status"));
            if (!channel.id.empty()) {
                channels.push_back(std::move(channel));
            }
        }
        deviceRegistry_.updateCatalog(deviceId, std::move(channels));
        LOG_INFO << "Catalog updated for " << deviceId;
        return;
    }

    if (cmdType == "RecordInfo") {
        if (!snText.empty()) {
            try {
                const auto sn = static_cast<unsigned int>(std::stoul(snText));
                std::lock_guard lock(sessionMutex_);
                const auto iter = pendingRecordQueries_.find(sn);
                if (iter != pendingRecordQueries_.end()) {
                    deviceId = iter->second;
                    pendingRecordQueries_.erase(iter);
                }
            } catch (...) {
            }
        }
        std::vector<RecordItem> records;
        for (auto item : root.child("RecordList").children("Item")) {
            RecordItem record;
            record.deviceId = xmlText(item, "DeviceID");
            record.name = xmlText(item, "Name");
            record.filePath = xmlText(item, "FilePath");
            record.address = xmlText(item, "Address");
            record.startTime = xmlText(item, "StartTime");
            record.endTime = xmlText(item, "EndTime");
            record.type = xmlText(item, "Type");
            record.recorderId = xmlText(item, "RecorderID");
            if (!record.deviceId.empty()) {
                records.push_back(std::move(record));
            }
        }
        deviceRegistry_.updateRecords(deviceId, std::move(records));
        LOG_INFO << "RecordInfo updated for " << deviceId;
        return;
    }

    LOG_DEBUG << "Unhandled MESSAGE CmdType=" << cmdType << " from " << deviceId;
}

bool SipServer::queryCatalog(const std::string& deviceId) {
    const auto device = deviceRegistry_.findDevice(deviceId);
    if (!device.has_value() || !device->online || device->remoteAddress.empty()) {
        return false;
    }

    const auto colon = device->remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }

    const auto host = device->remoteAddress.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(device->remoteAddress.substr(colon + 1)));
    auto remote = peerFromAddress(device->remoteAddress);
    if (!remote.has_value()) {
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Query>\r\n"
         << "<CmdType>Catalog</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << deviceId << "</DeviceID>\r\n"
         << "</Query>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("branch");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("catalog") + "@" + sipConfig_.domain;

    std::ostringstream request;
    request << "MESSAGE sip:" << deviceId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << deviceId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    return true;
}

bool SipServer::queryRecords(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime) {
    const auto device = deviceRegistry_.findDevice(deviceId);
    if (!device.has_value() || !device->online || device->remoteAddress.empty()) {
        return false;
    }

    const auto colon = device->remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }

    const auto host = device->remoteAddress.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(device->remoteAddress.substr(colon + 1)));
    auto remote = peerFromAddress(device->remoteAddress);
    if (!remote.has_value()) {
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Query>\r\n"
         << "<CmdType>RecordInfo</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << channelId << "</DeviceID>\r\n"
         << "<StartTime>" << startTime << "</StartTime>\r\n"
         << "<EndTime>" << endTime << "</EndTime>\r\n"
         << "<Secrecy>0</Secrecy>\r\n"
         << "<Type>all</Type>\r\n"
         << "</Query>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("record");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("record") + "@" + sipConfig_.domain;
    {
        std::lock_guard lock(sessionMutex_);
        pendingRecordQueries_[sn] = deviceId;
    }

    std::ostringstream request;
    request << "MESSAGE sip:" << channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    return true;
}

bool SipServer::sendPtzControl(const std::string& deviceId, const std::string& channelId, const std::string& action, uint8_t speed) {
    const auto device = deviceRegistry_.findDevice(deviceId);
    if (!device.has_value() || !device->online || device->remoteAddress.empty()) {
        return false;
    }

    const auto colon = device->remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }

    const auto host = device->remoteAddress.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(device->remoteAddress.substr(colon + 1)));
    auto remote = peerFromAddress(device->remoteAddress);
    if (!remote.has_value()) {
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Control>\r\n"
         << "<CmdType>DeviceControl</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << channelId << "</DeviceID>\r\n"
         << "<PTZCmd>" << ptzCommand(action, speed) << "</PTZCmd>\r\n"
         << "<Info>\r\n"
         << "<ControlPriority>5</ControlPriority>\r\n"
         << "</Info>\r\n"
         << "</Control>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("ptz");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("ptz") + "@" + sipConfig_.domain;

    std::ostringstream request;
    request << "MESSAGE sip:" << channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    return true;
}

std::optional<SipServer::PreviewStartResult> SipServer::startPreview(const std::string& deviceId, const std::string& channelId) {
    const auto device = deviceRegistry_.findDevice(deviceId);
    if (!device.has_value() || !device->online || device->remoteAddress.empty()) {
        return std::nullopt;
    }

    const auto channelExists = std::any_of(device->channels.begin(), device->channels.end(), [&](const Channel& channel) {
        return channel.id == channelId;
    });
    if (!device->channels.empty() && !channelExists) {
        return std::nullopt;
    }

    const auto colon = device->remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    const auto host = device->remoteAddress.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(device->remoteAddress.substr(colon + 1)));
    auto remote = peerFromAddress(device->remoteAddress);
    if (!remote.has_value()) {
        return std::nullopt;
    }

    LOG_INFO << "Preview start requested, device=" << deviceId << ", channel=" << channelId;
    std::vector<std::string> staleSessionIds;
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& [_, session] : previewSessions_) {
            if (session.mode == "preview" && session.deviceId == deviceId && session.channelId == channelId) {
                if (!session.mediaOnline) {
                    staleSessionIds.push_back(session.sessionId);
                    continue;
                }
                const auto viewerId = makeToken("viewer");
                ++session.viewerCount;
                previewViewers_[viewerId] = session.sessionId;
                LOG_INFO << "Preview stream reused, device=" << deviceId << ", channel=" << channelId << ", viewer_session=" << viewerId << ", stream_session=" << session.sessionId << ", viewers=" << session.viewerCount;
                return PreviewStartResult{
                    viewerId,
                    session.deviceId,
                    session.channelId,
                    session.streamId,
                    session.ssrc,
                    session.rtpPort,
                    session.playUrls,
                };
            }
        }
    }

    for (const auto& staleSessionId : staleSessionIds) {
        const auto result = stopPreview(staleSessionId);
        if (result.has_value()) {
            LOG_INFO << "Closed stale preview session before restart, session=" << staleSessionId << ", stream_id=" << result->streamId;
        }
    }

    const auto cseq = cseq_.fetch_add(1);
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto mediaHost = mediaConfig_.rtpPublicIp.empty() || mediaConfig_.rtpPublicIp == "YOUR_PUBLIC_SERVER_IP" ? publicHost : mediaConfig_.rtpPublicIp;
    const auto sessionId = makeToken("preview");
    const auto branch = "z9hG4bK-" + makeToken("branch");
    const auto fromTag = makeToken("tag");
    const auto callId = sessionId + "@" + sipConfig_.domain;
    const auto nowMs = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto ssrcNumber = 1000000000ULL + ((nowMs + cseq) % 899999999ULL);
    const auto ssrc = std::to_string(ssrcNumber);
    const auto rtpServer = zlmClient_.openRtpServer(deviceId, channelId, ssrc);
    if (!rtpServer.has_value()) {
        return std::nullopt;
    }
    LOG_INFO << "ZLM RTP server opened for preview, stream_id=" << rtpServer->streamId << ", rtp_port=" << rtpServer->port;

    std::ostringstream body;
    body << "v=0\r\n"
         << "o=" << channelId << " 0 0 IN IP4 " << mediaHost << "\r\n"
         << "s=Play\r\n"
         << "c=IN IP4 " << mediaHost << "\r\n"
         << "t=0 0\r\n"
         << "m=video " << rtpServer->port << " TCP/RTP/AVP 96\r\n"
         << "a=recvonly\r\n"
         << "a=setup:passive\r\n"
         << "a=connection:new\r\n"
         << "a=rtpmap:96 PS/90000\r\n"
         << "y=" << ssrc << "\r\n";

    const auto bodyText = body.str();
    std::ostringstream request;
    request << "INVITE sip:" << channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << fromTag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << cseq << " INVITE\r\n"
            << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
            << "Max-Forwards: 70\r\n"
            << "Subject: " << channelId << ":" << ssrc << "," << sipConfig_.id << ":0\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: application/sdp\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    PreviewSession session;
    session.sessionId = sessionId;
    session.deviceId = deviceId;
    session.channelId = channelId;
    session.streamId = rtpServer->streamId;
    session.callId = callId;
    session.fromTag = fromTag;
    session.branch = branch;
    session.ssrc = ssrc;
    session.inviteCseq = cseq;
    session.rtpPort = rtpServer->port;
    session.playUrls = rtpServer->playUrls;
    session.remote = *remote;
    session.viewerCount = 1;

    const auto viewerId = makeToken("viewer");

    {
        std::lock_guard lock(sessionMutex_);
        previewSessions_.emplace(sessionId, session);
        previewViewers_.emplace(viewerId, sessionId);
    }
    LOG_INFO << "Preview session stored, viewer_session=" << viewerId << ", stream_session=" << sessionId;

    sendRequest(request.str(), *remote);
    LOG_INFO << "Preview INVITE sent, device=" << deviceId << ", channel=" << channelId << ", stream_id=" << rtpServer->streamId << ", rtp_port=" << rtpServer->port << ", ssrc=" << ssrc;

    return PreviewStartResult{
        viewerId,
        session.deviceId,
        session.channelId,
        session.streamId,
        session.ssrc,
        session.rtpPort,
        session.playUrls,
    };
}

void SipServer::markStreamOnline(const std::string& streamId, bool online) {
    std::lock_guard lock(sessionMutex_);
    for (auto& [_, session] : previewSessions_) {
        if (session.streamId == streamId) {
            session.mediaOnline = online;
            LOG_INFO << "Preview session media state changed, stream_id=" << streamId << ", online=" << online;
        }
    }
}

std::optional<SipServer::PreviewStartResult> SipServer::startPlayback(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime) {
    const auto device = deviceRegistry_.findDevice(deviceId);
    if (!device.has_value() || !device->online || device->remoteAddress.empty()) {
        return std::nullopt;
    }

    const auto colon = device->remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    const auto host = device->remoteAddress.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(device->remoteAddress.substr(colon + 1)));
    auto remote = peerFromAddress(device->remoteAddress);
    if (!remote.has_value()) {
        return std::nullopt;
    }

    const auto cseq = cseq_.fetch_add(1);
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto mediaHost = mediaConfig_.rtpPublicIp.empty() || mediaConfig_.rtpPublicIp == "YOUR_PUBLIC_SERVER_IP" ? publicHost : mediaConfig_.rtpPublicIp;
    const auto sessionId = makeToken("playback");
    const auto branch = "z9hG4bK-" + makeToken("branch");
    const auto fromTag = makeToken("tag");
    const auto callId = sessionId + "@" + sipConfig_.domain;
    const auto nowMs = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto ssrcNumber = 2000000000ULL + ((nowMs + cseq) % 899999999ULL);
    const auto ssrc = std::to_string(ssrcNumber);
    const auto rtpServer = zlmClient_.openRtpServer(deviceId, channelId, ssrc, "playback");
    if (!rtpServer.has_value()) {
        return std::nullopt;
    }

    const auto startSeconds = gbTimeToUnixSeconds(startTime);
    const auto endSeconds = gbTimeToUnixSeconds(endTime);

    std::ostringstream body;
    body << "v=0\r\n"
         << "o=" << channelId << " 0 0 IN IP4 " << mediaHost << "\r\n"
         << "s=Playback\r\n"
         << "u=" << channelId << ":0\r\n"
         << "c=IN IP4 " << mediaHost << "\r\n"
         << "t=" << startSeconds << " " << endSeconds << "\r\n"
         << "m=video " << rtpServer->port << " TCP/RTP/AVP 96\r\n"
         << "a=recvonly\r\n"
         << "a=setup:passive\r\n"
         << "a=connection:new\r\n"
         << "a=rtpmap:96 PS/90000\r\n"
         << "y=" << ssrc << "\r\n";

    const auto bodyText = body.str();
    std::ostringstream request;
    request << "INVITE sip:" << channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << fromTag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << cseq << " INVITE\r\n"
            << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
            << "Max-Forwards: 70\r\n"
            << "Subject: " << channelId << ":" << ssrc << "," << sipConfig_.id << ":0\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: application/sdp\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    PreviewSession session;
    session.sessionId = sessionId;
    session.deviceId = deviceId;
    session.channelId = channelId;
    session.streamId = rtpServer->streamId;
    session.callId = callId;
    session.fromTag = fromTag;
    session.branch = branch;
    session.ssrc = ssrc;
    session.inviteCseq = cseq;
    session.rtpPort = rtpServer->port;
    session.playUrls = rtpServer->playUrls;
    session.remote = *remote;
    session.mode = "playback";

    {
        std::lock_guard lock(sessionMutex_);
        previewSessions_[sessionId] = session;
    }

    sendRequest(request.str(), *remote);
    LOG_INFO << "Playback INVITE sent, device=" << deviceId << ", channel=" << channelId << ", stream_id=" << rtpServer->streamId << ", rtp_port=" << rtpServer->port << ", ssrc=" << ssrc;

    return PreviewStartResult{
        session.sessionId,
        session.deviceId,
        session.channelId,
        session.streamId,
        session.ssrc,
        session.rtpPort,
        session.playUrls,
    };
}

std::optional<SipServer::PreviewStopResult> SipServer::stopPreview(const std::string& sessionId) {
    PreviewSession session;
    std::string streamSessionId = sessionId;
    {
        std::lock_guard lock(sessionMutex_);
        const auto viewerIter = previewViewers_.find(sessionId);
        if (viewerIter != previewViewers_.end()) {
            streamSessionId = viewerIter->second;
            previewViewers_.erase(viewerIter);
        }

        const auto iter = previewSessions_.find(streamSessionId);
        if (iter == previewSessions_.end()) {
            return std::nullopt;
        }

        if (iter->second.mode == "preview" && iter->second.viewerCount > 1 && streamSessionId != sessionId) {
            --iter->second.viewerCount;
            LOG_INFO << "Preview viewer released, viewer_session=" << sessionId << ", stream_session=" << streamSessionId << ", viewers=" << iter->second.viewerCount;
            return PreviewStopResult{
                sessionId,
                iter->second.streamId,
                false,
                false,
            };
        }

        session = iter->second;
        for (auto viewer = previewViewers_.begin(); viewer != previewViewers_.end();) {
            if (viewer->second == streamSessionId) {
                viewer = previewViewers_.erase(viewer);
            } else {
                ++viewer;
            }
        }
        previewSessions_.erase(iter);
    }

    bool byeSent = false;
    if (session.established) {
        const auto cseq = cseq_.fetch_add(1);
        const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
        const auto branch = "z9hG4bK-" + makeToken("bye");
        const auto host = session.remote.address;
        const auto port = session.remote.port;

        std::ostringstream bye;
        bye << "BYE sip:" << session.channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(session.remote.transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << session.fromTag << "\r\n"
            << "To: <sip:" << session.channelId << "@" << sipConfig_.domain << ">;tag=" << session.toTag << "\r\n"
            << "Call-ID: " << session.callId << "\r\n"
            << "CSeq: " << cseq << " BYE\r\n"
            << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Length: 0\r\n\r\n";

        sendRequest(bye.str(), session.remote);
        byeSent = true;
        LOG_INFO << "Preview BYE sent, session=" << streamSessionId << ", stream_id=" << session.streamId;
    } else {
        LOG_INFO << "Preview session was not established; skip BYE, session=" << streamSessionId;
    }

    closeRtpServerAsync(session.streamId);

    return PreviewStopResult{
        sessionId,
        session.streamId,
        byeSent,
        true,
    };
}

void SipServer::sendResponse(const SipMessage& request, const SipPeer& remote, int statusCode, const std::string& reason, const std::string& extraHeaders) {
    std::ostringstream response;
    response << "SIP/2.0 " << statusCode << ' ' << reason << "\r\n";

    const auto via = request.header("Via");
    const auto from = request.header("From");
    const auto to = request.header("To");
    const auto callId = request.header("Call-ID");
    const auto cseq = request.header("CSeq");

    if (!via.empty()) {
        response << "Via: " << via << "\r\n";
    }
    if (!from.empty()) {
        response << "From: " << from << "\r\n";
    }
    if (!to.empty()) {
        response << "To: " << to << "\r\n";
    }
    if (!callId.empty()) {
        response << "Call-ID: " << callId << "\r\n";
    }
    if (!cseq.empty()) {
        response << "CSeq: " << cseq << "\r\n";
    }

    response << "User-Agent: gb28181-platform-cpp\r\n";
    response << extraHeaders;
    response << "Content-Length: 0\r\n\r\n";

    const auto data = response.str();
    sendRequest(data, remote);
}

void SipServer::sendRequest(const std::string& request, const SipPeer& remote) {
    std::lock_guard lock(sendMutex_);
    if (remote.transport == SipTransport::Tcp) {
        if (!remote.tcp || !remote.tcp->connected()) {
            LOG_WARN << "SIP TCP send failed: connection is closed";
            return;
        }
        remote.tcp->send(request);
        return;
    }

    if (ioLoop_ == nullptr || isInvalidSocket(udpSocket_)) {
        LOG_WARN << "SIP UDP send failed: socket is not ready";
        return;
    }

    ioLoop_->queueInLoop([this, request, remoteAddress = remote.udp]() {
        if (!running_ || isInvalidSocket(udpSocket_)) {
            return;
        }
        const auto sent = ::sendto(
            udpSocket_,
            request.data(),
            static_cast<int>(request.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remoteAddress),
            sizeof(remoteAddress));
        if (sent < 0) {
            LOG_WARN << "SIP UDP send failed: " << socketErrorMessage();
        }
    });
}

std::optional<SipServer::SipPeer> SipServer::peerFromAddress(const std::string& remoteAddress) const {
    const auto colon = remoteAddress.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    SipPeer peer;
    peer.address = remoteAddress.substr(0, colon);
    peer.port = static_cast<uint16_t>(std::stoi(remoteAddress.substr(colon + 1)));
    {
        std::lock_guard lock(tcpConnectionsMutex_);
        const auto iter = tcpConnections_.find(remoteAddress);
        if (iter != tcpConnections_.end() && iter->second && iter->second->connected()) {
            peer.transport = SipTransport::Tcp;
            peer.tcp = iter->second;
            return peer;
        }
    }

    peer.transport = SipTransport::Udp;
    peer.udp = makeSocketAddress(peer.address, peer.port);
    return peer;
}

void SipServer::scheduleCatalogQuery(const std::string& deviceId) {
    if (ioLoop_ == nullptr) {
        return;
    }
    ioLoop_->runAfter(0.5, [this, deviceId]() {
        if (!running_) {
            return;
        }
        if (queryCatalog(deviceId)) {
            LOG_INFO << "Catalog query sent, device=" << deviceId;
        }
    });
}

void SipServer::closeRtpServerAsync(const std::string& streamId) {
    if (ioLoop_ == nullptr) {
        zlmClient_.closeRtpServer(streamId);
        return;
    }
    ioLoop_->queueInLoop([this, streamId]() {
        zlmClient_.closeRtpServer(streamId);
    });
}
