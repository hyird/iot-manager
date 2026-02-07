#pragma once

#include "SL651.Types.hpp"
#include "SL651.Utils.hpp"
#include "common/utils/AppException.hpp"

namespace sl651 {

/**
 * @brief SL651 报文构建器
 */
class SL651Builder {
public:
    /**
     * @brief 构建下行控制报文
     *
     * 下行报文结构:
     *   7E 7E              - 帧头
     *   XX XX XX XX XX     - 测站编码 (5字节BCD) - 下行时测站在前
     *   XX                 - 中心地址 (1字节HEX) - 下行时中心在后
     *   XX XX              - 密码 (2字节BCD)
     *   XX                 - 功能码 (1字节HEX)
     *   XX XX              - 下行标识(0x8000) + 长度
     *   02                 - STX
     *   XX XX              - 流水号 (2字节HEX)
     *   XX XX XX XX XX XX  - 发报时间 (6字节BCD: YYMMDDHHmmSS)
     *   [要素数据...]      - 可选
     *   03/05              - ETX (0x03=不需应答, 0x05=需要应答)
     *   XX XX              - CRC16
     */
    static std::vector<uint8_t> buildDownFrame(const BuildDownFrameParams& params) {
        std::vector<uint8_t> contentParts;

        // 流水号 (2字节 HEX，Big-Endian)
        auto serialNumber = SL651Utils::encodeSerialNumberHex(params.serialNumber);
        contentParts.insert(contentParts.end(), serialNumber.begin(), serialNumber.end());

        // 发报时间 (6字节 BCD: YYMMDDHHmmSS)
        auto reportTime = SL651Utils::encodeReportTime(params.reportTime);
        contentParts.insert(contentParts.end(), reportTime.begin(), reportTime.end());

        // 要素数据
        for (const auto& elem : params.elements) {
            // 引导符
            auto guideBytes = SL651Utils::hexToBuffer(elem.guideHex);
            contentParts.insert(contentParts.end(), guideBytes.begin(), guideBytes.end());

            // 要素值
            std::vector<uint8_t> valueBytes;
            if (elem.encode == Encode::BCD) {
                double numValue;
                try {
                    numValue = std::stod(elem.value);
                } catch (...) {
                    throw ValidationException("BCD 编码要素值必须为数字: " + elem.value);
                }
                if (!std::isfinite(numValue)) {
                    throw ValidationException("BCD 编码要素值不合法: " + elem.value);
                }
                valueBytes = SL651Utils::encodeBCDValue(numValue, elem.length, elem.digits);
            } else {
                // HEX 编码：校验字符合法性
                for (char c : elem.value) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        throw ValidationException("HEX 编码要素值包含非法字符: " + elem.value);
                    }
                }
                std::string hexStr = elem.value;
                while (hexStr.length() < static_cast<size_t>(elem.length * 2)) {
                    hexStr = "0" + hexStr;
                }
                valueBytes = SL651Utils::hexToBuffer(hexStr);
            }
            contentParts.insert(contentParts.end(), valueBytes.begin(), valueBytes.end());
        }

        // 构建帧头
        std::vector<uint8_t> frame;

        // 帧头 7E 7E
        frame.push_back(0x7E);
        frame.push_back(0x7E);

        // 测站编码 (5字节 BCD) - 下行时测站在前
        auto remoteCode = SL651Utils::encodeBCDAddress(params.remoteCode, 5);
        frame.insert(frame.end(), remoteCode.begin(), remoteCode.end());

        // 中心站地址 (1字节 HEX) - 下行时中心在后
        auto centerCode = SL651Utils::hexToBuffer(params.centerCode);
        frame.insert(frame.end(), centerCode.begin(), centerCode.end());

        // 密码 (2字节 BCD)
        auto password = SL651Utils::encodeBCDAddress(params.password, 2);
        frame.insert(frame.end(), password.begin(), password.end());

        // 功能码 (1字节 HEX)
        auto funcCode = SL651Utils::hexToBuffer(params.funcCode);
        frame.insert(frame.end(), funcCode.begin(), funcCode.end());

        // 长度字段 (下行标识 0x8000 + content 长度，不含 STX/ETX)
        uint16_t lenField = 0x8000 | (static_cast<uint16_t>(contentParts.size()) & 0x0FFF);
        SL651Utils::writeUInt16BE(frame, lenField);

        // STX (下行固定 0x02)
        frame.push_back(0x02);

        // 正文内容
        frame.insert(frame.end(), contentParts.begin(), contentParts.end());

        // ETX (0x05 表示需要应答)
        frame.push_back(0x05);

        // 计算 CRC
        uint16_t crc = SL651Utils::crc16Modbus(frame);
        SL651Utils::writeUInt16BE(frame, crc);

        return frame;
    }

    /**
     * @brief 构建应答报文
     *
     * 上行应答报文结构:
     *   7E 7E              - 帧头
     *   XX                 - 中心地址 (1字节HEX)
     *   XX XX XX XX XX     - 测站编码 (5字节BCD)
     *   XX XX              - 密码 (2字节BCD)
     *   XX                 - 功能码 (1字节HEX)
     *   XX XX              - 上行标识(0x0000) + 长度
     *   02                 - STX
     *   XX XX              - 流水号 (2字节HEX)
     *   XX XX XX XX XX XX  - 发报时间 (6字节BCD)
     *   03                 - ETX
     *   XX XX              - CRC16
     */
    static std::vector<uint8_t> buildAckFrame(const Sl651Frame& originFrame) {
        std::vector<uint8_t> frame;

        // 帧头 7E 7E
        frame.push_back(0x7E);
        frame.push_back(0x7E);

        // 中心站地址 (1字节 HEX)
        auto centerCode = SL651Utils::hexToBuffer(originFrame.centerCode);
        frame.insert(frame.end(), centerCode.begin(), centerCode.end());

        // 测站编码 (5字节 BCD)
        auto remoteCode = SL651Utils::encodeBCDAddress(originFrame.remoteCode, 5);
        frame.insert(frame.end(), remoteCode.begin(), remoteCode.end());

        // 密码 (2字节 BCD)
        auto password = SL651Utils::hexToBuffer(originFrame.password);
        frame.insert(frame.end(), password.begin(), password.end());

        // 功能码 (1字节 HEX)
        auto funcCode = SL651Utils::hexToBuffer(originFrame.funcCode);
        frame.insert(frame.end(), funcCode.begin(), funcCode.end());

        // 正文长度 = 流水号(2) + 发报时间(6) = 8
        uint16_t bodyLength = 8;
        uint16_t lenField = 0x8000 | bodyLength;  // 下行标识
        SL651Utils::writeUInt16BE(frame, lenField);

        // STX
        frame.push_back(0x02);

        // 流水号
        if (originFrame.serialNumber) {
            auto serialNumber = SL651Utils::hexToBuffer(*originFrame.serialNumber);
            frame.insert(frame.end(), serialNumber.begin(), serialNumber.end());
        } else {
            frame.push_back(0x00);
            frame.push_back(0x00);
        }

        // 发报时间
        auto reportTime = SL651Utils::encodeReportTime(std::chrono::system_clock::now());
        frame.insert(frame.end(), reportTime.begin(), reportTime.end());

        // ETX
        frame.push_back(0x03);

        // 计算 CRC
        uint16_t crc = SL651Utils::crc16Modbus(frame);
        SL651Utils::writeUInt16BE(frame, crc);

        return frame;
    }

    /**
     * @brief 构建链路维持应答报文
     */
    static std::vector<uint8_t> buildLinkKeepAck(const std::string& centerCode,
                                                  const std::string& remoteCode,
                                                  const std::string& password) {
        std::vector<uint8_t> frame;

        // 帧头
        frame.push_back(0x7E);
        frame.push_back(0x7E);

        // 中心站地址
        auto center = SL651Utils::hexToBuffer(centerCode);
        frame.insert(frame.end(), center.begin(), center.end());

        // 测站编码
        auto remote = SL651Utils::encodeBCDAddress(remoteCode, 5);
        frame.insert(frame.end(), remote.begin(), remote.end());

        // 密码
        auto pwd = SL651Utils::encodeBCDAddress(password, 2);
        frame.insert(frame.end(), pwd.begin(), pwd.end());

        // 功能码 2F (链路维持)
        frame.push_back(0x2F);

        // 长度 (无正文)
        SL651Utils::writeUInt16BE(frame, 0x8000);

        // STX
        frame.push_back(0x02);

        // ETX
        frame.push_back(0x03);

        // CRC
        uint16_t crc = SL651Utils::crc16Modbus(frame);
        SL651Utils::writeUInt16BE(frame, crc);

        return frame;
    }
};

}  // namespace sl651
