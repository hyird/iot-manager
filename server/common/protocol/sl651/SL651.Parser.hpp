#pragma once

#include "SL651.Types.hpp"
#include "SL651.Utils.hpp"
#include "SL651.Builder.hpp"
#include "common/protocol/ParsedResult.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/DeviceConnectionCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/utils/Constants.hpp"

namespace sl651 {

/**
 * @brief SL651 协议解析器
 */
class SL651Parser {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    // 使用协程版本的回调，避免同步数据库调用导致死锁
    using DeviceConfigGetter = std::function<Task<std::optional<DeviceConfig>>(int linkId, const std::string& remoteCode)>;
    using ElementsGetter = std::function<std::vector<ElementDef>(const DeviceConfig& config, const std::string& funcCode)>;
    // 指令应答回调：设备编码, 功能码, 是否成功, 应答报文记录 ID
    using CommandResponseCallback = std::function<void(const std::string& deviceCode, const std::string& funcCode, bool success, int64_t responseId)>;
    // 同步版本的设备配置获取器（TcpIoPool 线程使用，从缓存读取）
    using DeviceConfigGetterSync = std::function<std::optional<DeviceConfig>(int linkId, const std::string& remoteCode)>;

private:
    // 链路缓冲区
    std::map<int, std::vector<uint8_t>> buffers_;
    std::mutex bufferMutex_;

    // 多包会话
    std::map<std::string, MultiPacketSession> multiPacketSessions_;
    std::mutex sessionMutex_;

    // 会话超时
    static constexpr int SESSION_TIMEOUT_MS = Constants::SL651_SESSION_TIMEOUT_MS;
    static constexpr size_t MAX_BUFFER_SIZE = 65536;     // 单链路缓冲区上限 64KB
    static constexpr size_t MAX_SESSION_COUNT = 100;      // 多包会话数上限

    // 回调函数
    DeviceConfigGetter getDeviceConfig_;
    ElementsGetter getElements_;
    CommandResponseCallback onCommandResponse_;

    // 数据库服务
    DatabaseService dbService_;

    // 统计计数器（原子操作，无锁）
    std::atomic<int64_t> totalFramesParsed_{0};
    std::atomic<int64_t> totalCrcErrors_{0};
    std::atomic<int64_t> totalMultiPacketCompleted_{0};
    std::atomic<int64_t> totalMultiPacketExpired_{0};
    std::atomic<int64_t> totalParseErrors_{0};

public:
    SL651Parser(DeviceConfigGetter getDeviceConfig, ElementsGetter getElements)
        : getDeviceConfig_(std::move(getDeviceConfig))
        , getElements_(std::move(getElements)) {}

    /**
     * @brief 设置指令应答回调
     */
    void setCommandResponseCallback(CommandResponseCallback callback) {
        onCommandResponse_ = std::move(callback);
    }

    /** SL651 统计数据 */
    struct Sl651Stats {
        int64_t framesParsed;
        int64_t crcErrors;
        int64_t multiPacketCompleted;
        int64_t multiPacketExpired;
        int64_t parseErrors;
    };

    Sl651Stats getSl651Stats() const {
        return {
            totalFramesParsed_.load(std::memory_order_relaxed),
            totalCrcErrors_.load(std::memory_order_relaxed),
            totalMultiPacketCompleted_.load(std::memory_order_relaxed),
            totalMultiPacketExpired_.load(std::memory_order_relaxed),
            totalParseErrors_.load(std::memory_order_relaxed)
        };
    }

    /**
     * @brief 处理接收到的数据
     * @param linkId 链路ID
     * @param clientAddr 客户端地址（用于建立设备连接映射）
     * @param data 接收到的数据
     */
    Task<void> handleData(int linkId, const std::string& clientAddr, const std::vector<uint8_t>& data) {
        try {
            std::vector<uint8_t> buffer;
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                auto& linkBuffer = buffers_[linkId];
                linkBuffer.insert(linkBuffer.end(), data.begin(), data.end());
                if (linkBuffer.size() > MAX_BUFFER_SIZE) {
                    LOG_WARN << "[SL651] Buffer overflow for linkId=" << linkId
                             << ", size=" << linkBuffer.size() << ", clearing";
                    linkBuffer.clear();
                    co_return;
                }
                buffer = linkBuffer;
            }

            constexpr size_t HEADER_LEN = Constants::SL651_FRAME_HEADER_SIZE;

            while (buffer.size() >= HEADER_LEN) {
                // 查找帧头 0x7E 0x7E
                int start = -1;
                for (size_t i = 0; i + 1 < buffer.size(); ++i) {
                    if (buffer[i] == 0x7E && buffer[i + 1] == 0x7E) {
                        start = static_cast<int>(i);
                        break;
                    }
                }

                if (start == -1) {
                    std::lock_guard<std::mutex> lock(bufferMutex_);
                    buffers_[linkId].clear();
                    break;
                }

                if (start > 0) {
                    buffer.erase(buffer.begin(), buffer.begin() + start);
                }

                if (buffer.size() < HEADER_LEN) {
                    break;
                }

                // 解析长度字段
                uint16_t lenField = SL651Utils::readUInt16BE(buffer, 11);
                uint16_t bodyLength = lenField & 0x0FFF;

                if (buffer.size() < HEADER_LEN + 1) {
                    break;
                }

                // 完整帧长度 = 帧头(13) + STX(1) + 正文 + ETX(1) + CRC(2)
                size_t fullLen = 13 + 1 + bodyLength + 1 + 2;

                if (buffer.size() < fullLen) {
                    break;
                }

                // 提取完整帧
                std::vector<uint8_t> frameBuf(buffer.begin(), buffer.begin() + fullLen);
                buffer.erase(buffer.begin(), buffer.begin() + fullLen);

                {
                    std::lock_guard<std::mutex> lock(bufferMutex_);
                    buffers_[linkId] = buffer;
                }

                co_await parseFrame(linkId, clientAddr, frameBuf);
            }

            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                buffers_[linkId] = buffer;
            }

        } catch (const std::exception& e) {
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR << "[SL651] handleData error: " << e.what();
        }
    }

    /**
     * @brief 清除链路缓存
     */
    void clearCache(int linkId) {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        buffers_.erase(linkId);
    }

    /**
     * @brief 构建下行指令报文
     */
    std::string buildCommand(const std::string& deviceCode, const std::string& funcCode,
                             const Json::Value& elements, const DeviceConfig& config) {
        try {
            // 获取功能码对应的要素定义
            auto elemDefs = getElements_(config, funcCode);

            // 构建要素数据
            std::vector<BuildDownFrameParams::ElementValue> elemParams;

            if (elements.isArray()) {
                for (const auto& elem : elements) {
                    std::string elementId = elem.get("elementId", "").asString();
                    std::string value = elem.get("value", "").asString();

                    // 查找对应的要素定义
                    for (const auto& def : elemDefs) {
                        if (def.id == elementId) {
                            BuildDownFrameParams::ElementValue e;
                            e.guideHex = def.guideHex;
                            e.encode = def.encode;
                            e.length = def.length;
                            e.digits = def.digits;
                            e.value = value;
                            elemParams.push_back(e);
                            break;
                        }
                    }
                }
            }

            // 生成流水号
            static std::atomic<uint16_t> serialCounter{0};
            std::string serialNumber = std::to_string(++serialCounter);

            // 构建参数
            BuildDownFrameParams params;
            params.centerCode = "01";  // 默认中心站地址
            params.remoteCode = deviceCode;
            params.password = "0000";  // 默认密码
            params.funcCode = funcCode;
            params.serialNumber = serialNumber;
            params.reportTime = std::chrono::system_clock::now();
            params.elements = elemParams;

            // 构建报文
            auto frameBytes = SL651Builder::buildDownFrame(params);

            // 转换为字符串
            return std::string(frameBytes.begin(), frameBytes.end());

        } catch (const ValidationException&) {
            throw;  // 校验异常直接传播，由上层返回给用户
        } catch (const std::exception& e) {
            LOG_ERROR << "[SL651] buildCommand error: " << e.what();
            return "";
        }
    }

    // ==================== 同步解析接口（TcpIoPool 线程使用） ====================

    /**
     * @brief 同步处理接收到的数据（TcpIoPool 线程调用）
     * @return 解析结果列表，由调用方投递到 Drogon IO 线程保存
     */
    std::vector<ParsedFrameResult> parseDataSync(int linkId, const std::string& clientAddr,
                                                  const std::vector<uint8_t>& data,
                                                  const DeviceConfigGetterSync& getConfigSync) {
        std::vector<ParsedFrameResult> results;
        try {
            std::vector<uint8_t> buffer;
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                auto& linkBuffer = buffers_[linkId];
                linkBuffer.insert(linkBuffer.end(), data.begin(), data.end());
                if (linkBuffer.size() > MAX_BUFFER_SIZE) {
                    LOG_WARN << "[SL651] Buffer overflow (sync) for linkId=" << linkId
                             << ", size=" << linkBuffer.size() << ", clearing";
                    linkBuffer.clear();
                    return results;
                }
                buffer = linkBuffer;
            }

            constexpr size_t HEADER_LEN = Constants::SL651_FRAME_HEADER_SIZE;

            while (buffer.size() >= HEADER_LEN) {
                int start = -1;
                for (size_t i = 0; i + 1 < buffer.size(); ++i) {
                    if (buffer[i] == 0x7E && buffer[i + 1] == 0x7E) {
                        start = static_cast<int>(i);
                        break;
                    }
                }

                if (start == -1) {
                    std::lock_guard<std::mutex> lock(bufferMutex_);
                    buffers_[linkId].clear();
                    break;
                }

                if (start > 0) {
                    buffer.erase(buffer.begin(), buffer.begin() + start);
                }

                if (buffer.size() < HEADER_LEN) break;

                uint16_t lenField = SL651Utils::readUInt16BE(buffer, 11);
                uint16_t bodyLength = lenField & 0x0FFF;

                if (buffer.size() < HEADER_LEN + 1) break;

                size_t fullLen = 13 + 1 + bodyLength + 1 + 2;
                if (buffer.size() < fullLen) break;

                std::vector<uint8_t> frameBuf(buffer.begin(), buffer.begin() + fullLen);
                buffer.erase(buffer.begin(), buffer.begin() + fullLen);

                {
                    std::lock_guard<std::mutex> lock(bufferMutex_);
                    buffers_[linkId] = buffer;
                }

                auto frameResults = parseFrameSync(linkId, clientAddr, frameBuf, getConfigSync);
                results.insert(results.end(),
                              std::make_move_iterator(frameResults.begin()),
                              std::make_move_iterator(frameResults.end()));
            }

            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                buffers_[linkId] = buffer;
            }

        } catch (const std::exception& e) {
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR << "[SL651] parseDataSync error: " << e.what();
        }
        return results;
    }

private:
    // ==================== 同步解析内部方法 ====================

    /**
     * @brief 同步解析单个帧
     */
    std::vector<ParsedFrameResult> parseFrameSync(int linkId, const std::string& clientAddr,
                                                    const std::vector<uint8_t>& frame,
                                                    const DeviceConfigGetterSync& getConfigSync) {
        std::vector<ParsedFrameResult> results;
        try {
            size_t offset = 0;
            offset += 2;

            std::string centerCode = toHex(frame[offset++]);
            std::string remoteCode = SL651Utils::readBCD(frame, offset, 5);
            offset += 5;
            std::string password = SL651Utils::readBCD(frame, offset, 2);
            offset += 2;
            std::string funcCode = toHex(frame[offset++]);

            uint16_t lenField = SL651Utils::readUInt16BE(frame, offset);
            Direction direction = (lenField & 0xF000) == 0 ? Direction::UP : Direction::DOWN;
            uint16_t bodyLength = lenField & 0x0FFF;
            offset += 2;

            if (direction == Direction::UP && !clientAddr.empty()) {
                DeviceConnectionCache::instance().registerConnection(remoteCode, linkId, clientAddr);
            }

            uint8_t stx = frame[offset++];
            bool isMultiPacket = (stx == FrameControl::STX_MULTI);

            int totalPk = 0, seqPk = 0;
            uint16_t actualBodyLength = bodyLength;

            if (isMultiPacket) {
                uint8_t byte1 = frame[offset];
                uint8_t byte2 = frame[offset + 1];
                uint8_t byte3 = frame[offset + 2];
                uint32_t value24 = (static_cast<uint32_t>(byte1) << 16) |
                                   (static_cast<uint32_t>(byte2) << 8) | byte3;
                totalPk = (value24 >> 12) & 0x0FFF;
                seqPk = value24 & 0x0FFF;
                offset += 3;
                actualBodyLength = bodyLength - 3;
            }

            std::vector<uint8_t> body(frame.begin() + offset, frame.begin() + offset + actualBodyLength);
            offset += actualBodyLength;

            std::optional<std::string> serialNumber;
            if (direction == Direction::UP && body.size() >= 2) {
                serialNumber = SL651Utils::bufferToHex({body[0], body[1]});
            }

            uint8_t etx = frame[offset++];
            bool isLastPacket = (etx == FrameControl::ETX_END);

            uint16_t crcRecv = SL651Utils::readUInt16BE(frame, offset);
            std::vector<uint8_t> crcData(frame.begin(), frame.end() - 2);
            uint16_t crcCalc = SL651Utils::crc16Modbus(crcData);

            Sl651Frame parsed;
            parsed.direction = direction;
            parsed.centerCode = centerCode;
            parsed.remoteCode = remoteCode;
            parsed.password = password;
            parsed.funcCode = funcCode;
            parsed.stx = stx;
            parsed.etx = etx;
            parsed.body = body;
            parsed.crcRecv = crcRecv;
            parsed.crcCalc = crcCalc;
            parsed.crcValid = (crcCalc == crcRecv);
            parsed.raw = frame;
            parsed.isMultiPacket = isMultiPacket;
            parsed.totalPk = totalPk;
            parsed.seqPk = seqPk;
            parsed.isLastPacket = isLastPacket;
            parsed.serialNumber = serialNumber;

            LOG_DEBUG << "[SL651] 帧: " << parsed.remoteCode << " | " << parsed.funcCode
                     << " | CRC:" << (parsed.crcValid ? "OK" : "FAIL")
                     << (isMultiPacket ? " | 多包" + std::to_string(parsed.seqPk) + "/" + std::to_string(parsed.totalPk) : "");

            if (!parsed.crcValid) {
                totalCrcErrors_.fetch_add(1, std::memory_order_relaxed);
            }

            if (!isMultiPacket) {
                auto configOpt = getConfigSync(linkId, parsed.remoteCode);
                auto parsedBody = parseBodySync(parsed, configOpt);
                if (parsedBody) {
                    printParsedBody(*parsedBody);
                }
                auto result = buildFrameResult(linkId, parsed, parsedBody, configOpt);
                if (result) {
                    totalFramesParsed_.fetch_add(1, std::memory_order_relaxed);
                    results.push_back(std::move(*result));
                }
            } else {
                auto multiResults = handleMultiPacketSync(linkId, parsed, getConfigSync);
                results.insert(results.end(),
                              std::make_move_iterator(multiResults.begin()),
                              std::make_move_iterator(multiResults.end()));
            }

        } catch (const std::exception& e) {
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR << "[SL651] parseFrameSync error: " << e.what();
        }
        return results;
    }

    /**
     * @brief 同步解析正文数据（不涉及 DB 查询，从缓存获取配置）
     */
    std::optional<ParsedBody> parseBodySync(const Sl651Frame& frame,
                                             const std::optional<DeviceConfig>& configOpt) {
        if (!configOpt) {
            LOG_WARN << "[SL651] parseBodySync: 无设备配置, code=" << frame.remoteCode;
            return std::nullopt;
        }

        auto elements = getElements_(*configOpt, frame.funcCode);

        if (frame.direction == Direction::UP && isDownFunc(*configOpt, frame.funcCode)) {
            auto responseElements = getResponseElements(*configOpt, frame.funcCode);
            if (!responseElements.empty()) {
                LOG_DEBUG << "[SL651] 下行功能码 " << frame.funcCode << " 的上行应答，使用 responseElements 解析";
                elements = responseElements;
            } else if (elements.empty()) {
                LOG_DEBUG << "[SL651] 下行功能码 " << frame.funcCode << " 的上行应答，无应答要素配置，不解析";
                return ParsedBody{};
            }
        }

        if (elements.empty()) {
            LOG_WARN << "[SL651] 未找到功能码要素定义: funcCode=" << frame.funcCode;
            return ParsedBody{};
        }

        ParsedBody result;
        size_t offset = 0;

        for (const auto& elem : elements) {
            auto guideBytes = SL651Utils::hexToBuffer(elem.guideHex);
            int guideIndex = SL651Utils::indexOf(frame.body, guideBytes, offset);

            if (guideIndex == -1) continue;

            offset = guideIndex + guideBytes.size();

            if (elem.length == 0) {
                std::vector<uint8_t> dataToEnd(frame.body.begin() + offset, frame.body.end());
                std::string rawValue = SL651Utils::bufferToHex(dataToEnd);
                std::string value = parseElementValue(dataToEnd, elem);
                result.data.push_back({elem.name, elem.guideHex, rawValue, value, elem.unit, elem.id, elem.encode});
                offset = frame.body.size();
                break;
            }

            if (offset + elem.length > frame.body.size()) {
                LOG_WARN << "[SL651] 数据长度不足: " << elem.name;
                break;
            }

            std::vector<uint8_t> valueBuffer(frame.body.begin() + offset, frame.body.begin() + offset + elem.length);
            offset += elem.length;

            std::string rawValue = SL651Utils::bufferToHex(valueBuffer);
            std::string value = parseElementValue(valueBuffer, elem);
            result.data.push_back({elem.name, elem.guideHex, rawValue, value, elem.unit, elem.id, elem.encode});
        }

        if (offset < frame.body.size()) {
            result.unparsed.assign(frame.body.begin() + offset, frame.body.end());
        }

        return result;
    }

    /**
     * @brief 构建 ParsedFrameResult（单帧版本）
     * 从 saveFrameData 提取 JSON 构建逻辑，不涉及 DB 操作
     */
    std::optional<ParsedFrameResult> buildFrameResult(int linkId, const Sl651Frame& frame,
                                                       const std::optional<ParsedBody>& parsedBody,
                                                       const std::optional<DeviceConfig>& configOpt) {
        if (!configOpt) return std::nullopt;

        ParsedFrameResult result;
        result.deviceId = configOpt->deviceId;
        result.linkId = linkId;
        result.protocol = Constants::PROTOCOL_SL651;
        result.funcCode = frame.funcCode;
        result.reportTime = extractReportTime(frame.body);

        if (!result.reportTime.empty()) {
            result.reportTime += configOpt->timezone;
        }

        // 构建 JSONB 数据（与 saveFrameData 相同逻辑）
        Json::Value data;
        data["funcCode"] = frame.funcCode;
        auto funcNameIt = configOpt->funcNames.find(frame.funcCode);
        if (funcNameIt != configOpt->funcNames.end()) {
            data["funcName"] = funcNameIt->second;
        }
        data["direction"] = directionToString(frame.direction);

        Json::Value rawArr(Json::arrayValue);
        rawArr.append(SL651Utils::bufferToHex(frame.raw));
        data["raw"] = rawArr;

        Json::Value frameMeta;
        frameMeta["centerCode"] = frame.centerCode;
        frameMeta["remoteCode"] = frame.remoteCode;
        frameMeta["password"] = frame.password;
        frameMeta["crcValid"] = frame.crcValid;
        if (frame.serialNumber) {
            frameMeta["serialNumber"] = *frame.serialNumber;
        }
        data["frame"] = frameMeta;

        Json::Value dataObj(Json::objectValue);
        if (parsedBody) {
            for (const auto& elem : parsedBody->data) {
                Json::Value e;
                e["value"] = elem.value;
                if (!elem.name.empty()) e["name"] = elem.name;
                if (!elem.unit.empty()) e["unit"] = elem.unit;
                e["type"] = encodeToString(elem.encode);
                std::string key = frame.funcCode + "_" + elem.guideHex;
                dataObj[key] = e;
            }
        }
        data["data"] = dataObj;

        result.data = data;

        // 指令应答信息（上行帧）
        if (frame.direction == Direction::UP) {
            ParsedFrameResult::CommandResponse cmdResp;
            cmdResp.deviceCode = frame.remoteCode;
            cmdResp.funcCode = frame.funcCode;
            cmdResp.success = (frame.funcCode != FuncCodes::ACK_ERR);
            result.commandResponse = cmdResp;
        }

        return result;
    }

    /**
     * @brief 同步处理多包帧
     */
    std::vector<ParsedFrameResult> handleMultiPacketSync(int linkId, const Sl651Frame& frame,
                                                          const DeviceConfigGetterSync& getConfigSync) {
        std::string sessionKey = frame.remoteCode + "_" + frame.funcCode;

        cleanExpiredSessions();

        bool complete = false;
        MultiPacketSession completedSession;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            auto it = multiPacketSessions_.find(sessionKey);
            if (it == multiPacketSessions_.end() || it->second.totalPk != frame.totalPk) {
                if (it == multiPacketSessions_.end() && multiPacketSessions_.size() >= MAX_SESSION_COUNT) {
                    LOG_WARN << "[SL651] Session limit reached (" << MAX_SESSION_COUNT
                             << "), dropping: " << sessionKey;
                    return {};
                }
                MultiPacketSession newSession;
                newSession.remoteCode = frame.remoteCode;
                newSession.funcCode = frame.funcCode;
                newSession.totalPk = frame.totalPk;
                newSession.startTime = std::chrono::steady_clock::now();
                multiPacketSessions_[sessionKey] = newSession;
            }
            auto& session = multiPacketSessions_[sessionKey];
            session.receivedPk.insert(frame.seqPk);
            session.packets[frame.seqPk] = frame.body;
            session.rawFrames[frame.seqPk] = frame.raw;

            LOG_DEBUG << "[SL651] 多包缓存: " << sessionKey << " (" << session.receivedPk.size() << "/" << session.totalPk << ")";

            if (static_cast<int>(session.receivedPk.size()) == session.totalPk) {
                complete = true;
                completedSession = std::move(session);
                multiPacketSessions_.erase(sessionKey);
            }
        }

        if (complete) {
            totalMultiPacketCompleted_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG << "[SL651] 多包完成: " << sessionKey << " (" << completedSession.totalPk << "包)";
            auto result = mergeAndBuildMultiPacketResult(linkId, completedSession, frame, getConfigSync);
            if (result) {
                totalFramesParsed_.fetch_add(1, std::memory_order_relaxed);
                return {std::move(*result)};
            }
        }

        return {};
    }

    /**
     * @brief 合并多包数据并构建 ParsedFrameResult
     */
    std::optional<ParsedFrameResult> mergeAndBuildMultiPacketResult(
            int linkId, const MultiPacketSession& session, const Sl651Frame& lastFrame,
            const DeviceConfigGetterSync& getConfigSync) {
        try {
            std::vector<uint8_t> mergedBody;
            for (int i = 1; i <= session.totalPk; ++i) {
                auto it = session.packets.find(i);
                if (it != session.packets.end()) {
                    mergedBody.insert(mergedBody.end(), it->second.begin(), it->second.end());
                }
            }

            Sl651Frame virtualFrame;
            virtualFrame.direction = Direction::UP;
            virtualFrame.remoteCode = session.remoteCode;
            virtualFrame.funcCode = session.funcCode;
            virtualFrame.body = mergedBody;
            virtualFrame.isMultiPacket = false;

            auto configOpt = getConfigSync(linkId, session.remoteCode);
            auto parsedBody = parseBodySync(virtualFrame, configOpt);
            if (parsedBody) {
                printParsedBody(*parsedBody);
            }

            if (!configOpt) return std::nullopt;

            ParsedFrameResult result;
            result.deviceId = configOpt->deviceId;
            result.linkId = linkId;
            result.protocol = Constants::PROTOCOL_SL651;
            result.funcCode = session.funcCode;
            result.reportTime = extractReportTime(mergedBody);

            if (!result.reportTime.empty()) {
                result.reportTime += configOpt->timezone;
            }

            Json::Value data;
            data["funcCode"] = session.funcCode;
            auto funcNameIt = configOpt->funcNames.find(session.funcCode);
            if (funcNameIt != configOpt->funcNames.end()) {
                data["funcName"] = funcNameIt->second;
            }
            data["direction"] = directionToString(Direction::UP);
            data["isMultiPacket"] = true;
            data["totalPackets"] = session.totalPk;

            Json::Value frameMeta;
            frameMeta["centerCode"] = lastFrame.centerCode;
            frameMeta["remoteCode"] = session.remoteCode;
            frameMeta["password"] = lastFrame.password;
            data["frame"] = frameMeta;

            Json::Value rawArr(Json::arrayValue);
            for (int i = 1; i <= session.totalPk; ++i) {
                auto rawIt = session.rawFrames.find(i);
                if (rawIt != session.rawFrames.end()) {
                    rawArr.append(SL651Utils::bufferToHex(rawIt->second));
                }
            }
            data["raw"] = rawArr;

            Json::Value dataObj(Json::objectValue);
            if (parsedBody) {
                for (const auto& elem : parsedBody->data) {
                    Json::Value e;
                    e["value"] = elem.value;
                    if (!elem.name.empty()) e["name"] = elem.name;
                    if (!elem.unit.empty()) e["unit"] = elem.unit;
                    e["type"] = encodeToString(elem.encode);
                    std::string key = session.funcCode + "_" + elem.guideHex;
                    dataObj[key] = e;
                }
            }
            data["data"] = dataObj;

            result.data = data;
            return result;

        } catch (const std::exception& e) {
            LOG_ERROR << "[SL651] mergeAndBuildMultiPacketResult error: " << e.what();
            return std::nullopt;
        }
    }

    // ==================== 原有异步方法（sendCommand 等仍使用） ====================

private:
    /**
     * @brief 解析单个帧
     * @param linkId 链路ID
     * @param clientAddr 客户端地址（用于建立设备连接映射）
     * @param frame 帧数据
     */
    Task<void> parseFrame(int linkId, const std::string& clientAddr, const std::vector<uint8_t>& frame) {
        try {
            size_t offset = 0;

            // 跳过帧头
            offset += 2;

            // 中心站地址（1字节 HEX）
            std::string centerCode = toHex(frame[offset++]);

            // 遥测站地址（5字节 BCD）
            std::string remoteCode = SL651Utils::readBCD(frame, offset, 5);
            offset += 5;

            // 密码（2字节 BCD）
            std::string password = SL651Utils::readBCD(frame, offset, 2);
            offset += 2;

            // 功能码（1字节 HEX）
            std::string funcCode = toHex(frame[offset++]);

            // 长度字段
            uint16_t lenField = SL651Utils::readUInt16BE(frame, offset);
            Direction direction = (lenField & 0xF000) == 0 ? Direction::UP : Direction::DOWN;
            uint16_t bodyLength = lenField & 0x0FFF;
            offset += 2;

            // 注册设备连接映射（仅上行帧）
            if (direction == Direction::UP && !clientAddr.empty()) {
                DeviceConnectionCache::instance().registerConnection(remoteCode, linkId, clientAddr);
            }

            // STX
            uint8_t stx = frame[offset++];
            bool isMultiPacket = (stx == FrameControl::STX_MULTI);

            // 多包头处理
            int totalPk = 0;
            int seqPk = 0;
            uint16_t actualBodyLength = bodyLength;

            if (isMultiPacket) {
                uint8_t byte1 = frame[offset];
                uint8_t byte2 = frame[offset + 1];
                uint8_t byte3 = frame[offset + 2];

                uint32_t value24 = (static_cast<uint32_t>(byte1) << 16) |
                                   (static_cast<uint32_t>(byte2) << 8) | byte3;
                totalPk = (value24 >> 12) & 0x0FFF;
                seqPk = value24 & 0x0FFF;

                offset += 3;
                actualBodyLength = bodyLength - 3;

            }

            // 正文数据
            std::vector<uint8_t> body(frame.begin() + offset, frame.begin() + offset + actualBodyLength);
            offset += actualBodyLength;

            // 流水号（仅上行有）
            std::optional<std::string> serialNumber;
            if (direction == Direction::UP && body.size() >= 2) {
                serialNumber = SL651Utils::bufferToHex({body[0], body[1]});
            }

            // ETX
            uint8_t etx = frame[offset++];
            bool isLastPacket = (etx == FrameControl::ETX_END);

            // CRC 校验
            uint16_t crcRecv = SL651Utils::readUInt16BE(frame, offset);
            std::vector<uint8_t> crcData(frame.begin(), frame.end() - 2);
            uint16_t crcCalc = SL651Utils::crc16Modbus(crcData);

            // 构建帧结构
            Sl651Frame parsed;
            parsed.direction = direction;
            parsed.centerCode = centerCode;
            parsed.remoteCode = remoteCode;
            parsed.password = password;
            parsed.funcCode = funcCode;
            parsed.stx = stx;
            parsed.etx = etx;
            parsed.body = body;
            parsed.crcRecv = crcRecv;
            parsed.crcCalc = crcCalc;
            parsed.crcValid = (crcCalc == crcRecv);
            parsed.raw = frame;
            parsed.isMultiPacket = isMultiPacket;
            parsed.totalPk = totalPk;
            parsed.seqPk = seqPk;
            parsed.isLastPacket = isLastPacket;
            parsed.serialNumber = serialNumber;

            // 打印解析结果（精简版）
            LOG_DEBUG << "[SL651] 帧: " << parsed.remoteCode << " | " << parsed.funcCode
                     << " | CRC:" << (parsed.crcValid ? "OK" : "FAIL")
                     << (isMultiPacket ? " | 多包" + std::to_string(parsed.seqPk) + "/" + std::to_string(parsed.totalPk) : "");

            if (!parsed.crcValid) {
                totalCrcErrors_.fetch_add(1, std::memory_order_relaxed);
            }

            // 处理帧数据
            if (!isMultiPacket) {
                totalFramesParsed_.fetch_add(1, std::memory_order_relaxed);
                co_await onSingleFrameParsed(linkId, parsed);
            } else {
                co_await handleMultiPacket(linkId, parsed);
            }

        } catch (const std::exception& e) {
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR << "[SL651] parseFrame error: " << e.what();
        }
    }

    /**
     * @brief 处理单包帧
     */
    Task<void> onSingleFrameParsed(int linkId, const Sl651Frame& frame) {
        // 解析正文
        auto parsedBody = co_await parseBody(linkId, frame);

        // 保存帧数据并获取记录 ID
        int64_t recordId = 0;
        if (parsedBody) {
            printParsedBody(*parsedBody);
            recordId = co_await saveFrameData(linkId, frame, parsedBody->data);
        } else {
            recordId = co_await saveFrameData(linkId, frame, {});
        }

        // 检查是否为指令应答（上行帧），在保存后通知以便关联应答 ID
        if (frame.direction == Direction::UP && onCommandResponse_) {
            // 上行帧视为对下行指令的应答
            // E1 = 确认成功, E2 = 否认失败, 其他功能码的上行 = 对应指令的应答
            bool success = (frame.funcCode != FuncCodes::ACK_ERR);
            onCommandResponse_(frame.remoteCode, frame.funcCode, success, recordId);
        }
    }

    /**
     * @brief 处理多包帧
     */
    Task<void> handleMultiPacket(int linkId, const Sl651Frame& frame) {
        std::string sessionKey = frame.remoteCode + "_" + frame.funcCode;

        cleanExpiredSessions();

        bool complete = false;
        MultiPacketSession completedSession;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            auto it = multiPacketSessions_.find(sessionKey);
            if (it == multiPacketSessions_.end() || it->second.totalPk != frame.totalPk) {
                if (it == multiPacketSessions_.end() && multiPacketSessions_.size() >= MAX_SESSION_COUNT) {
                    LOG_WARN << "[SL651] Session limit reached (" << MAX_SESSION_COUNT
                             << "), dropping: " << sessionKey;
                    co_return;
                }
                MultiPacketSession newSession;
                newSession.remoteCode = frame.remoteCode;
                newSession.funcCode = frame.funcCode;
                newSession.totalPk = frame.totalPk;
                newSession.startTime = std::chrono::steady_clock::now();
                multiPacketSessions_[sessionKey] = newSession;
            }
            auto& session = multiPacketSessions_[sessionKey];
            session.receivedPk.insert(frame.seqPk);
            session.packets[frame.seqPk] = frame.body;
            session.rawFrames[frame.seqPk] = frame.raw;

            LOG_DEBUG << "[SL651] 多包缓存: " << sessionKey << " (" << session.receivedPk.size() << "/" << session.totalPk << ")";

            if (static_cast<int>(session.receivedPk.size()) == session.totalPk) {
                complete = true;
                completedSession = std::move(session);
                multiPacketSessions_.erase(sessionKey);
            }
        }

        if (complete) {
            totalMultiPacketCompleted_.fetch_add(1, std::memory_order_relaxed);
            totalFramesParsed_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG << "[SL651] 多包完成: " << sessionKey << " (" << completedSession.totalPk << "包)";
            co_await mergeAndParseMultiPacket(linkId, completedSession, frame);
        }
    }

    /**
     * @brief 解析正文数据（协程版本）
     */
    Task<std::optional<ParsedBody>> parseBody(int linkId, const Sl651Frame& frame) {
        auto configOpt = co_await getDeviceConfig_(linkId, frame.remoteCode);
        if (!configOpt) {
            LOG_WARN << "[SL651] 未找到设备配置: linkId=" << linkId << ", code=" << frame.remoteCode;
            co_return std::nullopt;
        }

        auto elements = getElements_(*configOpt, frame.funcCode);

        // 如果是下行功能码收到上行帧（应答帧），优先使用 responseElements
        if (frame.direction == Direction::UP && isDownFunc(*configOpt, frame.funcCode)) {
            auto responseElements = getResponseElements(*configOpt, frame.funcCode);
            if (!responseElements.empty()) {
                LOG_DEBUG << "[SL651] 下行功能码 " << frame.funcCode << " 的上行应答，使用 responseElements 解析";
                elements = responseElements;
            } else if (elements.empty()) {
                // 没有配置 responseElements 且没有 elements，不解析要素
                LOG_DEBUG << "[SL651] 下行功能码 " << frame.funcCode << " 的上行应答，无应答要素配置，不解析";
                co_return ParsedBody{};
            }
        }

        if (elements.empty()) {
            LOG_WARN << "[SL651] 未找到功能码要素定义: funcCode=" << frame.funcCode;
            co_return ParsedBody{};
        }


        ParsedBody result;
        size_t offset = 0;

        for (const auto& elem : elements) {
            auto guideBytes = SL651Utils::hexToBuffer(elem.guideHex);
            int guideIndex = SL651Utils::indexOf(frame.body, guideBytes, offset);

            if (guideIndex == -1) {
                continue;
            }

            offset = guideIndex + guideBytes.size();

            // 可变长度要素
            if (elem.length == 0) {
                std::vector<uint8_t> dataToEnd(frame.body.begin() + offset, frame.body.end());
                std::string rawValue = SL651Utils::bufferToHex(dataToEnd);
                std::string value = parseElementValue(dataToEnd, elem);

                result.data.push_back({
                    elem.name, elem.guideHex, rawValue, value, elem.unit, elem.id, elem.encode
                });

                offset = frame.body.size();
                break;
            }

            // 定长要素
            if (offset + elem.length > frame.body.size()) {
                LOG_WARN << "[SL651] 数据长度不足: " << elem.name;
                break;
            }

            std::vector<uint8_t> valueBuffer(frame.body.begin() + offset, frame.body.begin() + offset + elem.length);
            offset += elem.length;

            std::string rawValue = SL651Utils::bufferToHex(valueBuffer);
            std::string value = parseElementValue(valueBuffer, elem);

            result.data.push_back({
                elem.name, elem.guideHex, rawValue, value, elem.unit, elem.id, elem.encode
            });
        }

        if (offset < frame.body.size()) {
            result.unparsed.assign(frame.body.begin() + offset, frame.body.end());
        }

        co_return result;
    }

    /**
     * @brief 解析要素值
     */
    std::string parseElementValue(const std::vector<uint8_t>& data, const ElementDef& elem) {
        switch (elem.encode) {
            case Encode::TIME_YYMMDDHHMMSS: {
                std::string timeBCD = SL651Utils::readBCD(data, 0, data.size());
                return SL651Utils::parseBCDTime(timeBCD);
            }
            case Encode::JPEG: {
                bool isValid = data.size() > 2 && data[0] == 0xFF && data[1] == 0xD8;
                if (!isValid) {
                    return "INVALID_JPEG";
                }
                // 返回 Base64 编码的图片数据，带 data URL 前缀
                return "data:image/jpeg;base64," + SL651Utils::toBase64(data);
            }
            case Encode::DICT: {
                // DICT 编码：直接返回原始十六进制值，映射逻辑由前端处理
                return SL651Utils::bufferToHex(data);
            }
            case Encode::BCD:
            default: {
                std::string bcdStr = SL651Utils::readBCD(data, 0, data.size());
                double value = SL651Utils::parseBCDValue(bcdStr, elem.digits);
                std::ostringstream oss;
                if (elem.digits > 0) {
                    oss << std::fixed << std::setprecision(elem.digits) << value;
                } else {
                    oss << static_cast<int64_t>(value);
                }
                return oss.str();
            }
        }
    }

    /**
     * @brief 打印解析后的正文
     */
    void printParsedBody(const ParsedBody& parsedBody) {
        if (parsedBody.data.empty()) {
            LOG_DEBUG << "[SL651] 正文解析: 无要素数据";
            return;
        }

        std::ostringstream oss;
        oss << "[SL651] 正文解析 (" << parsedBody.data.size() << "个要素): ";
        for (size_t i = 0; i < parsedBody.data.size(); ++i) {
            const auto& elem = parsedBody.data[i];
            if (i > 0) oss << ", ";
            oss << elem.name << "=" << elem.value;
            if (!elem.unit.empty()) oss << elem.unit;
        }
        LOG_DEBUG << oss.str();

        if (!parsedBody.unparsed.empty()) {
            LOG_DEBUG << "[SL651] 未解析数据: " << parsedBody.unparsed.size() << " 字节";
        }
    }

    /**
     * @brief 保存帧数据到数据库（统一表结构）
     * @return 返回插入记录的 ID
     */
    Task<int64_t> saveFrameData(int linkId, const Sl651Frame& frame, const std::vector<ParsedElement>& elements) {
        try {
            std::string reportTime = extractReportTime(frame.body);

            // 获取设备配置（用于获取 funcName）
            auto configOpt = co_await getDeviceConfig_(linkId, frame.remoteCode);
            if (!configOpt) {
                LOG_WARN << "[SL651] 未找到设备: " << frame.remoteCode;
                co_return 0;
            }

            int deviceId = configOpt->deviceId;

            // 拼接设备时区，确保 TIMESTAMPTZ 正确存储
            if (!reportTime.empty()) {
                reportTime += configOpt->timezone;
            }

            // 构建 JSONB 数据
            Json::Value data;

            // SL651 协议特有字段
            data["funcCode"] = frame.funcCode;
            // 从配置中获取功能码名称
            auto funcNameIt = configOpt->funcNames.find(frame.funcCode);
            if (funcNameIt != configOpt->funcNames.end()) {
                data["funcName"] = funcNameIt->second;
            }
            data["direction"] = directionToString(frame.direction);

            // 原始报文（统一为数组格式，与多包保持一致）
            Json::Value rawArr(Json::arrayValue);
            rawArr.append(SL651Utils::bufferToHex(frame.raw));
            data["raw"] = rawArr;

            // 帧头信息
            Json::Value frameMeta;
            frameMeta["centerCode"] = frame.centerCode;
            frameMeta["remoteCode"] = frame.remoteCode;
            frameMeta["password"] = frame.password;
            frameMeta["crcValid"] = frame.crcValid;
            if (frame.serialNumber) {
                frameMeta["serialNumber"] = *frame.serialNumber;
            }
            data["frame"] = frameMeta;

            // 解析的要素（以 funcCode_guideHex 为 key，确保删除再添加后历史数据仍可关联）
            Json::Value dataObj(Json::objectValue);
            for (const auto& elem : elements) {
                Json::Value e;
                e["value"] = elem.value;
                if (!elem.name.empty()) {
                    e["name"] = elem.name;
                }
                if (!elem.unit.empty()) {
                    e["unit"] = elem.unit;
                }
                e["type"] = encodeToString(elem.encode);
                // 使用 funcCode + guideHex 作为复合 key
                std::string key = frame.funcCode + "_" + elem.guideHex;
                dataObj[key] = e;
            }
            data["data"] = dataObj;

            // 序列化 JSON（保留 UTF-8 字符，不转义为 unicode）
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            writer["emitUTF8"] = true;
            std::string jsonStr = Json::writeString(writer, data);

            // 插入并返回 ID
            auto result = co_await dbService_.execSqlCoro(R"(
                INSERT INTO device_data (device_id, link_id, protocol, data, report_time)
                VALUES (?, ?, 'SL651', ?::jsonb, ?::timestamptz)
                RETURNING id
            )", {std::to_string(deviceId), std::to_string(linkId), jsonStr, reportTime});

            int64_t id = 0;
            if (!result.empty()) {
                id = result[0]["id"].as<int64_t>();
            }

            // 更新实时数据缓存
            RealtimeDataCache::instance().update(deviceId, frame.funcCode, data, reportTime);

            // 更新资源版本号，让前端知道有新数据（历史数据、设备列表等都会刷新）
            ResourceVersion::instance().incrementVersion("device");

            LOG_DEBUG << "[SL651] 已保存: id=" << id << ", device=" << deviceId << ", 要素=" << elements.size();
            co_return id;

        } catch (const std::exception& e) {
            LOG_ERROR << "[SL651] 保存数据失败: " << e.what();
            co_return 0;
        }
    }

    /**
     * @brief 拼接并解析多包数据（只在所有包收齐后才入库）
     */
    Task<void> mergeAndParseMultiPacket(int linkId, const MultiPacketSession& session, const Sl651Frame& lastFrame) {
        try {
            // 拼接所有包的正文
            std::vector<uint8_t> mergedBody;
            for (int i = 1; i <= session.totalPk; ++i) {
                auto it = session.packets.find(i);
                if (it != session.packets.end()) {
                    mergedBody.insert(mergedBody.end(), it->second.begin(), it->second.end());
                }
            }

            // 构建虚拟帧用于解析
            Sl651Frame virtualFrame;
            virtualFrame.direction = Direction::UP;
            virtualFrame.remoteCode = session.remoteCode;
            virtualFrame.funcCode = session.funcCode;
            virtualFrame.body = mergedBody;
            virtualFrame.isMultiPacket = false;

            auto parsedBody = co_await parseBody(linkId, virtualFrame);
            if (parsedBody) {
                printParsedBody(*parsedBody);
            }

            // 获取设备配置（用于获取 funcName 和 deviceId）
            std::string reportTime = extractReportTime(mergedBody);
            auto configOpt = co_await getDeviceConfig_(linkId, session.remoteCode);
            if (!configOpt) {
                LOG_WARN << "[SL651] 未找到设备: " << session.remoteCode;
                co_return;
            }

            int deviceId = configOpt->deviceId;

            // 拼接设备时区，确保 TIMESTAMPTZ 正确存储
            if (!reportTime.empty()) {
                reportTime += configOpt->timezone;
            }

            // 构建 JSONB 数据
            Json::Value data;
            data["funcCode"] = session.funcCode;
            // 从配置中获取功能码名称
            auto funcNameIt = configOpt->funcNames.find(session.funcCode);
            if (funcNameIt != configOpt->funcNames.end()) {
                data["funcName"] = funcNameIt->second;
            }
            data["direction"] = directionToString(Direction::UP);
            data["isMultiPacket"] = true;
            data["totalPackets"] = session.totalPk;

            // 帧头信息
            Json::Value frameMeta;
            frameMeta["centerCode"] = lastFrame.centerCode;
            frameMeta["remoteCode"] = session.remoteCode;
            frameMeta["password"] = lastFrame.password;
            data["frame"] = frameMeta;

            // 所有包的原始报文数组
            Json::Value rawArr(Json::arrayValue);
            for (int i = 1; i <= session.totalPk; ++i) {
                auto rawIt = session.rawFrames.find(i);
                if (rawIt != session.rawFrames.end()) {
                    rawArr.append(SL651Utils::bufferToHex(rawIt->second));
                }
            }
            data["raw"] = rawArr;

            // 解析的要素（以 funcCode_guideHex 为 key，确保删除再添加后历史数据仍可关联）
            Json::Value dataObj(Json::objectValue);
            if (parsedBody) {
                for (const auto& elem : parsedBody->data) {
                    Json::Value e;
                    e["value"] = elem.value;
                    if (!elem.name.empty()) {
                        e["name"] = elem.name;
                    }
                    if (!elem.unit.empty()) {
                        e["unit"] = elem.unit;
                    }
                    e["type"] = encodeToString(elem.encode);
                    // 使用 funcCode + guideHex 作为复合 key
                    std::string key = session.funcCode + "_" + elem.guideHex;
                    dataObj[key] = e;
                }
            }
            data["data"] = dataObj;

            // 序列化并入库（保留 UTF-8 字符，不转义为 unicode）
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            writer["emitUTF8"] = true;
            std::string jsonStr = Json::writeString(writer, data);

            co_await dbService_.execSqlCoro(R"(
                INSERT INTO device_data (device_id, link_id, protocol, data, report_time)
                VALUES (?, ?, 'SL651', ?::jsonb, ?::timestamptz)
            )", {std::to_string(deviceId), std::to_string(linkId), jsonStr, reportTime});

            // 更新实时数据缓存
            RealtimeDataCache::instance().update(deviceId, session.funcCode, data, reportTime);

            // 更新资源版本号，让前端知道有新数据
            ResourceVersion::instance().incrementVersion("device");

            LOG_DEBUG << "[SL651] 多包数据已保存: device=" << deviceId << ", 总包数=" << session.totalPk;

        } catch (const std::exception& e) {
            LOG_ERROR << "[SL651] 保存多包数据失败: " << e.what();
        }
    }

    /**
     * @brief 获取设备 ID
     */
    Task<int> getDeviceId(int linkId, const std::string& deviceCode) {
        try {
            auto result = co_await dbService_.execSqlCoro(R"(
                SELECT id FROM device WHERE link_id = ? AND protocol_params->>'device_code' = ? AND deleted_at IS NULL
            )", {std::to_string(linkId), deviceCode});

            if (!result.empty()) {
                co_return result[0]["id"].as<int>();
            }
        } catch (const std::exception&) {}
        co_return 0;
    }

    /**
     * @brief 清理过期会话
     */
    void cleanExpiredSessions() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(sessionMutex_);

        for (auto it = multiPacketSessions_.begin(); it != multiPacketSessions_.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.startTime).count();
            if (elapsed > SESSION_TIMEOUT_MS) {
                totalMultiPacketExpired_.fetch_add(1, std::memory_order_relaxed);
                it = multiPacketSessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief 提取发报时间
     */
    std::string extractReportTime(const std::vector<uint8_t>& body) {
        constexpr size_t offset = 2;  // 跳过流水号
        constexpr size_t len = 6;

        if (body.size() < offset + len) {
            return "";
        }

        std::string timeBCD = SL651Utils::readBCD(body, offset, len);
        if (timeBCD.length() < 12) {
            return "";
        }

        return SL651Utils::parseBCDTime(timeBCD);
    }

    /**
     * @brief 字节转十六进制字符串
     */
    static std::string toHex(uint8_t byte) {
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        return oss.str();
    }

    /**
     * @brief 判断功能码是否为下行定义
     */
    static bool isDownFunc(const DeviceConfig& config, const std::string& funcCode) {
        auto it = config.funcDirections.find(funcCode);
        if (it != config.funcDirections.end()) {
            return it->second == Direction::DOWN;
        }
        return false;
    }

    /**
     * @brief 获取下行功能码的应答要素定义
     */
    static std::vector<ElementDef> getResponseElements(const DeviceConfig& config, const std::string& funcCode) {
        auto it = config.responseElementsByFunc.find(funcCode);
        if (it != config.responseElementsByFunc.end()) {
            return it->second;
        }
        return {};
    }
};

}  // namespace sl651
