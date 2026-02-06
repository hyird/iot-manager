#pragma once

#include "Modbus.Types.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <sstream>
#include <iomanip>

namespace modbus {

/**
 * @brief Modbus 工具类
 * CRC16 计算、帧构建/解析、数据转换、寄存器合并
 */
class ModbusUtils {
public:
    // ==================== CRC16 (Modbus RTU) ====================

    static uint16_t crc16(const uint8_t* data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                if (crc & 0x0001) {
                    crc = (crc >> 1) ^ 0xA001;
                } else {
                    crc >>= 1;
                }
            }
        }
        return crc;
    }

    static uint16_t crc16(const std::vector<uint8_t>& data) {
        return crc16(data.data(), data.size());
    }

    // ==================== 帧构建 ====================

    /**
     * @brief 构建 Modbus TCP 请求帧
     * [TransID(2)][ProtocolID(2)=0][Length(2)=6][UnitID(1)][FC(1)][StartAddr(2)][Quantity(2)]
     */
    static std::vector<uint8_t> buildTcpRequest(const ModbusRequest& req) {
        std::vector<uint8_t> frame(12);
        // Transaction ID
        frame[0] = static_cast<uint8_t>(req.transactionId >> 8);
        frame[1] = static_cast<uint8_t>(req.transactionId & 0xFF);
        // Protocol ID = 0
        frame[2] = 0x00;
        frame[3] = 0x00;
        // Length = 6 (UnitID + FC + StartAddr + Quantity)
        frame[4] = 0x00;
        frame[5] = 0x06;
        // Unit ID
        frame[6] = req.slaveId;
        // Function Code
        frame[7] = req.functionCode;
        // Start Address
        frame[8] = static_cast<uint8_t>(req.startAddress >> 8);
        frame[9] = static_cast<uint8_t>(req.startAddress & 0xFF);
        // Quantity
        frame[10] = static_cast<uint8_t>(req.quantity >> 8);
        frame[11] = static_cast<uint8_t>(req.quantity & 0xFF);
        return frame;
    }

    /**
     * @brief 构建 Modbus RTU over TCP 请求帧
     * [SlaveAddr(1)][FC(1)][StartAddr(2)][Quantity(2)][CRC16(2)]
     */
    static std::vector<uint8_t> buildRtuRequest(const ModbusRequest& req) {
        std::vector<uint8_t> frame(8);
        frame[0] = req.slaveId;
        frame[1] = req.functionCode;
        frame[2] = static_cast<uint8_t>(req.startAddress >> 8);
        frame[3] = static_cast<uint8_t>(req.startAddress & 0xFF);
        frame[4] = static_cast<uint8_t>(req.quantity >> 8);
        frame[5] = static_cast<uint8_t>(req.quantity & 0xFF);
        // CRC16 (小端序)
        uint16_t crc = crc16(frame.data(), 6);
        frame[6] = static_cast<uint8_t>(crc & 0xFF);        // CRC Low
        frame[7] = static_cast<uint8_t>((crc >> 8) & 0xFF); // CRC High
        return frame;
    }

    /** 根据 FrameMode 选择构建方式 */
    static std::vector<uint8_t> buildRequest(FrameMode mode, const ModbusRequest& req) {
        if (mode == FrameMode::RTU) {
            return buildRtuRequest(req);
        }
        return buildTcpRequest(req);
    }

    // ==================== 帧解析 ====================

    /**
     * @brief 解析 Modbus TCP 响应帧
     * @return 消耗的字节数（0 = 数据不足）
     *
     * 正常响应: [TransID(2)][ProtocolID(2)][Length(2)][UnitID(1)][FC(1)][ByteCount(1)][Data...]
     * 异常响应: [TransID(2)][ProtocolID(2)][Length(2)][UnitID(1)][FC|0x80(1)][ExceptionCode(1)]
     */
    static size_t parseTcpResponse(const std::vector<uint8_t>& buffer, ModbusResponse& out) {
        // MBAP Header = 7 bytes + at least FC(1) = 8 bytes minimum
        if (buffer.size() < 8) return 0;

        uint16_t transId = (static_cast<uint16_t>(buffer[0]) << 8) | buffer[1];
        uint16_t protoId = (static_cast<uint16_t>(buffer[2]) << 8) | buffer[3];
        uint16_t length  = (static_cast<uint16_t>(buffer[4]) << 8) | buffer[5];

        // Protocol ID 必须为 0
        if (protoId != 0) return 0;

        // Length 包含 UnitID 之后的全部字节
        size_t totalLen = 6 + length;  // MBAP header(6) + payload(length)
        if (buffer.size() < totalLen) return 0;

        out.mode = FrameMode::TCP;
        out.transactionId = transId;
        out.slaveId = buffer[6];
        out.functionCode = buffer[7];

        // 异常响应
        if (out.functionCode & 0x80) {
            out.isException = true;
            out.functionCode &= 0x7F;  // 去掉异常标志
            out.exceptionCode = (totalLen > 8) ? buffer[8] : 0;
            return totalLen;
        }

        // 正常响应: buffer[8] = ByteCount, buffer[9..] = Data
        if (totalLen < 9) return totalLen;  // 无数据

        uint8_t byteCount = buffer[8];
        if (totalLen < static_cast<size_t>(9 + byteCount)) return 0;

        out.isException = false;
        out.data.assign(buffer.begin() + 9, buffer.begin() + 9 + byteCount);
        return totalLen;
    }

    /**
     * @brief 解析 Modbus RTU over TCP 响应帧
     * @return 消耗的字节数（0 = 数据不足）
     *
     * 正常响应: [SlaveAddr(1)][FC(1)][ByteCount(1)][Data...][CRC16(2)]
     * 异常响应: [SlaveAddr(1)][FC|0x80(1)][ExceptionCode(1)][CRC16(2)]
     */
    static size_t parseRtuResponse(const std::vector<uint8_t>& buffer, ModbusResponse& out) {
        // 最小帧 = SlaveAddr(1) + FC(1) + ExceptionCode(1) + CRC(2) = 5 bytes
        if (buffer.size() < 5) return 0;

        uint8_t fc = buffer[1];

        // 异常响应
        if (fc & 0x80) {
            size_t frameLen = 5;  // SlaveAddr + FC + ExcCode + CRC16
            if (buffer.size() < frameLen) return 0;

            // CRC 校验
            uint16_t crcRecv = static_cast<uint16_t>(buffer[frameLen - 2])
                             | (static_cast<uint16_t>(buffer[frameLen - 1]) << 8);
            uint16_t crcCalc = crc16(buffer.data(), frameLen - 2);
            if (crcRecv != crcCalc) return 0;

            out.mode = FrameMode::RTU;
            out.slaveId = buffer[0];
            out.functionCode = fc & 0x7F;
            out.isException = true;
            out.exceptionCode = buffer[2];
            return frameLen;
        }

        // 正常响应: 需要 ByteCount 字段
        if (buffer.size() < 3) return 0;

        uint8_t byteCount = buffer[2];
        size_t frameLen = 3 + byteCount + 2;  // SlaveAddr + FC + ByteCount + Data + CRC16

        if (buffer.size() < frameLen) return 0;

        // CRC 校验
        uint16_t crcRecv = static_cast<uint16_t>(buffer[frameLen - 2])
                         | (static_cast<uint16_t>(buffer[frameLen - 1]) << 8);
        uint16_t crcCalc = crc16(buffer.data(), frameLen - 2);
        if (crcRecv != crcCalc) return 0;

        out.mode = FrameMode::RTU;
        out.slaveId = buffer[0];
        out.functionCode = fc;
        out.isException = false;
        out.data.assign(buffer.begin() + 3, buffer.begin() + 3 + byteCount);
        return frameLen;
    }

    /** 根据 FrameMode 选择解析方式 */
    static size_t parseResponse(FrameMode mode, const std::vector<uint8_t>& buffer, ModbusResponse& out) {
        if (mode == FrameMode::RTU) {
            return parseRtuResponse(buffer, out);
        }
        return parseTcpResponse(buffer, out);
    }

    // ==================== 数据转换 ====================

    /**
     * @brief 从寄存器数据中按字节序提取数值
     * @param data 指向寄存器数据的指针
     * @param dataType 数据类型
     * @param byteOrder 字节序
     * @return 提取的数值
     */
    static double extractValue(const uint8_t* data, DataType dataType, ByteOrder byteOrder) {
        size_t byteSize = dataTypeToByteSize(dataType);

        // 复制并按字节序重排
        std::vector<uint8_t> buf(data, data + byteSize);
        reorderBytes(buf, byteOrder);

        switch (dataType) {
            case DataType::BOOL:
                return (buf[0] & 0x01) ? 1.0 : 0.0;
            case DataType::INT16: {
                int16_t val;
                std::memcpy(&val, buf.data(), 2);
                return static_cast<double>(val);
            }
            case DataType::UINT16: {
                uint16_t val;
                std::memcpy(&val, buf.data(), 2);
                return static_cast<double>(val);
            }
            case DataType::INT32: {
                int32_t val;
                std::memcpy(&val, buf.data(), 4);
                return static_cast<double>(val);
            }
            case DataType::UINT32: {
                uint32_t val;
                std::memcpy(&val, buf.data(), 4);
                return static_cast<double>(val);
            }
            case DataType::FLOAT32: {
                float val;
                std::memcpy(&val, buf.data(), 4);
                return static_cast<double>(val);
            }
            case DataType::INT64: {
                int64_t val;
                std::memcpy(&val, buf.data(), 8);
                return static_cast<double>(val);
            }
            case DataType::UINT64: {
                uint64_t val;
                std::memcpy(&val, buf.data(), 8);
                return static_cast<double>(val);
            }
            case DataType::DOUBLE: {
                double val;
                std::memcpy(&val, buf.data(), 8);
                return val;
            }
        }
        return 0.0;
    }

    /** 从线圈/离散输入位数据中提取布尔值 */
    static bool extractBit(const uint8_t* data, uint16_t bitOffset) {
        uint16_t byteIdx = bitOffset / 8;
        uint8_t bitIdx = bitOffset % 8;
        return (data[byteIdx] >> bitIdx) & 0x01;
    }

    // ==================== 寄存器合并 ====================

    /**
     * @brief 合并连续地址的同类型寄存器
     *
     * 算法：
     * 1. 按 RegisterType 分组
     * 2. 每组内按 address 升序排序
     * 3. 连续且间距 <= maxGap 且合并后总量 <= maxRegsPerRead 则合并
     */
    static std::vector<ReadGroup> mergeRegisters(
        const std::vector<RegisterDef>& registers,
        int maxGap = 10,
        int maxRegsPerRead = 125
    ) {
        // 按类型分组
        std::map<RegisterType, std::vector<const RegisterDef*>> groups;
        for (const auto& reg : registers) {
            groups[reg.registerType].push_back(&reg);
        }

        std::vector<ReadGroup> result;

        for (auto& [regType, regs] : groups) {
            // 按地址排序
            std::sort(regs.begin(), regs.end(), [](const RegisterDef* a, const RegisterDef* b) {
                return a->address < b->address;
            });

            // 位类型寄存器（线圈/离散输入）每个只占 1 bit，最大读取 2000
            int maxQty = isBitRegister(regType) ? 2000 : maxRegsPerRead;

            ReadGroup current;
            current.registerType = regType;
            current.functionCode = registerTypeToFuncCode(regType);
            current.startAddress = regs[0]->address;
            current.totalQuantity = regs[0]->quantity;
            current.registers.push_back(regs[0]);

            for (size_t i = 1; i < regs.size(); ++i) {
                uint16_t nextEnd = regs[i]->address + regs[i]->quantity;
                uint16_t currentEnd = current.startAddress + current.totalQuantity;
                int gap = static_cast<int>(regs[i]->address) - static_cast<int>(currentEnd);
                uint16_t mergedQuantity = nextEnd - current.startAddress;

                if (gap <= maxGap && mergedQuantity <= static_cast<uint16_t>(maxQty)) {
                    // 合并
                    current.totalQuantity = mergedQuantity;
                    current.registers.push_back(regs[i]);
                } else {
                    // 开始新组
                    result.push_back(std::move(current));
                    current = ReadGroup{};
                    current.registerType = regType;
                    current.functionCode = registerTypeToFuncCode(regType);
                    current.startAddress = regs[i]->address;
                    current.totalQuantity = regs[i]->quantity;
                    current.registers.push_back(regs[i]);
                }
            }

            result.push_back(std::move(current));
        }

        return result;
    }

    // ==================== 工具函数 ====================

    static std::string toHexString(const std::vector<uint8_t>& data) {
        std::ostringstream oss;
        for (size_t i = 0; i < data.size(); ++i) {
            if (i > 0) oss << " ";
            oss << std::hex << std::uppercase << std::setw(2)
                << std::setfill('0') << static_cast<int>(data[i]);
        }
        return oss.str();
    }

private:
    /**
     * @brief 按字节序重排字节，将设备数据转为本机字节序以便 memcpy
     *
     * 以 DOUBLE 1.1 (0x3FF199999999999A) 为例，各字节序在通信中的字节顺序：
     *   BIG_ENDIAN:             3F F1 99 99 99 99 99 9A  (标准大端 AB CD EF GH)
     *   LITTLE_ENDIAN:          9A 99 99 99 99 99 F1 3F  (完全反转 HG FE DC BA)
     *   BIG_ENDIAN_BYTE_SWAP:   F1 3F 99 99 99 99 9A 99  (word 内字节交换 BA DC FE HG)
     *   LITTLE_ENDIAN_BYTE_SWAP:99 9A 99 99 99 99 3F F1  (word 顺序反转 GH EF CD AB)
     *
     * 算法：先转为标准大端，再根据本机字节序转换
     */
    static void reorderBytes(std::vector<uint8_t>& buf, ByteOrder order) {
        // Step 1: 将输入从指定字节序转为标准大端
        switch (order) {
            case ByteOrder::BIG_ENDIAN:
                break;
            case ByteOrder::LITTLE_ENDIAN:
                // 完全反转: HG FE DC BA → AB CD EF GH
                std::reverse(buf.begin(), buf.end());
                break;
            case ByteOrder::BIG_ENDIAN_BYTE_SWAP:
                // Word 内字节交换: BA DC FE HG → AB CD EF GH
                swapBytesInWords(buf);
                break;
            case ByteOrder::LITTLE_ENDIAN_BYTE_SWAP:
                // Word 顺序反转: GH EF CD AB → AB CD EF GH
                reverseWords(buf);
                break;
        }

        // Step 2: 从大端转为本机字节序
        if constexpr (std::endian::native == std::endian::little) {
            std::reverse(buf.begin(), buf.end());
        }
    }

    /** 反转 word (16-bit) 顺序: [W0][W1]...[Wn] → [Wn]...[W1][W0] */
    static void reverseWords(std::vector<uint8_t>& buf) {
        size_t numWords = buf.size() / 2;
        if (numWords < 2) return;

        for (size_t i = 0; i < numWords / 2; ++i) {
            size_t j = numWords - 1 - i;
            std::swap(buf[i * 2], buf[j * 2]);
            std::swap(buf[i * 2 + 1], buf[j * 2 + 1]);
        }
    }

    /** Word 内字节交换: [AB][CD] → [BA][DC] */
    static void swapBytesInWords(std::vector<uint8_t>& buf) {
        for (size_t i = 0; i + 1 < buf.size(); i += 2) {
            std::swap(buf[i], buf[i + 1]);
        }
    }
};

}  // namespace modbus
