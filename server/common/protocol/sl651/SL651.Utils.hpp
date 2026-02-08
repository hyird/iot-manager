#pragma once

namespace sl651 {

/**
 * @brief SL651 协议工具函数
 */
class SL651Utils {
private:
    /**
     * @brief 预生成的 HEX 查找表（编译期初始化，O(1) 查表替代格式化）
     * 避免每次 HEX 转换都经过 ostringstream + setfill + setw
     */
    struct HexTable {
        char table[256][2]{};
        HexTable() {
            const char* hex = "0123456789ABCDEF";
            for (int i = 0; i < 256; ++i) {
                table[i][0] = hex[(i >> 4) & 0x0F];
                table[i][1] = hex[i & 0x0F];
            }
        }
    };
    static inline const HexTable hexTable_{};

public:
    /** O(1) 单字节 HEX 转换（查表） */
    static std::string toHexFast(uint8_t byte) {
        return {hexTable_.table[byte][0], hexTable_.table[byte][1]};
    }

    /** 批量字节转 HEX 字符串（查表，无格式化开销） */
    static std::string bufferToHexFast(const uint8_t* data, size_t len) {
        std::string result;
        result.reserve(len * 2);
        for (size_t i = 0; i < len; ++i) {
            result.append(hexTable_.table[data[i]], 2);
        }
        return result;
    }

    static std::string bufferToHexFast(const std::vector<uint8_t>& data) {
        return bufferToHexFast(data.data(), data.size());
    }
    /**
     * @brief 读取 BCD 编码数据
     * @param data 数据缓冲区
     * @param offset 起始偏移
     * @param len 字节长度
     * @return BCD 字符串，如 "1234"
     */
    static std::string readBCD(const std::vector<uint8_t>& data, size_t offset, size_t len) {
        std::string result;
        for (size_t i = 0; i < len && (offset + i) < data.size(); ++i) {
            uint8_t byte = data[offset + i];
            uint8_t high = (byte >> 4) & 0x0F;
            uint8_t low = byte & 0x0F;
            // 防御非法 BCD 半字节（>9 截断为 0，避免产生非数字字符）
            result += static_cast<char>('0' + std::min(high, static_cast<uint8_t>(9)));
            result += static_cast<char>('0' + std::min(low, static_cast<uint8_t>(9)));
        }
        return result;
    }

    /**
     * @brief 读取 BCD 编码数据（从原始指针）
     * @param bufSize 缓冲区总大小（用于边界检查）
     */
    static std::string readBCD(const uint8_t* data, size_t offset, size_t len, size_t bufSize) {
        std::string result;
        for (size_t i = 0; i < len && (offset + i) < bufSize; ++i) {
            uint8_t byte = data[offset + i];
            uint8_t high = (byte >> 4) & 0x0F;
            uint8_t low = byte & 0x0F;
            result += static_cast<char>('0' + std::min(high, static_cast<uint8_t>(9)));
            result += static_cast<char>('0' + std::min(low, static_cast<uint8_t>(9)));
        }
        return result;
    }

    /**
     * @brief 字符串转 BCD 编码
     * @param str 数字字符串，如 "1234"
     * @return BCD 编码的字节数组
     */
    static std::vector<uint8_t> stringToBCD(const std::string& str) {
        std::string padded = str;
        if (padded.length() % 2 != 0) {
            padded = "0" + padded;
        }

        std::vector<uint8_t> result(padded.length() / 2);
        for (size_t i = 0; i < result.size(); ++i) {
            uint8_t high = padded[i * 2] - '0';
            uint8_t low = padded[i * 2 + 1] - '0';
            result[i] = (high << 4) | low;
        }
        return result;
    }

    /**
     * @brief 编码 BCD 地址
     * @param addr 地址字符串
     * @param byteLen 目标字节长度
     */
    static std::vector<uint8_t> encodeBCDAddress(const std::string& addr, size_t byteLen) {
        std::string padded = addr;
        while (padded.length() < byteLen * 2) {
            padded = "0" + padded;
        }
        return stringToBCD(padded);
    }

    /**
     * @brief 编码 BCD 数值
     * @param value 数值
     * @param length 字节长度
     * @param digits 小数位数
     */
    static std::vector<uint8_t> encodeBCDValue(double value, int length, int digits = 0) {
        // 防御：限制 digits 范围，避免 pow 溢出
        if (digits < 0) digits = 0;
        if (digits > 8) digits = 8;

        int64_t scaledValue = static_cast<int64_t>(std::round(value * std::pow(10, digits)));
        // BCD 编码无符号，取绝对值
        if (scaledValue < 0) scaledValue = -scaledValue;

        std::string bcdStr = std::to_string(scaledValue);
        size_t maxDigits = static_cast<size_t>(length) * 2;

        // 截断到目标长度（防止溢出）
        if (bcdStr.length() > maxDigits) {
            bcdStr = bcdStr.substr(bcdStr.length() - maxDigits);
        }
        while (bcdStr.length() < maxDigits) {
            bcdStr = "0" + bcdStr;
        }
        return stringToBCD(bcdStr);
    }

    /**
     * @brief 解析 BCD 时间字符串为可读格式
     * @param timeBCD BCD 时间字符串，如 "221229102215"
     * @return 格式化时间，如 "2022-12-29 10:22:15"
     */
    static std::string parseBCDTime(const std::string& timeBCD) {
        if (timeBCD.length() < 10) {
            return timeBCD;
        }

        int year = 2000 + std::stoi(timeBCD.substr(0, 2));
        std::string month = timeBCD.substr(2, 2);
        std::string day = timeBCD.substr(4, 2);
        std::string hour = timeBCD.substr(6, 2);
        std::string minute = timeBCD.substr(8, 2);
        std::string second = timeBCD.length() >= 12 ? timeBCD.substr(10, 2) : "00";

        std::ostringstream oss;
        oss << year << "-" << month << "-" << day << " "
            << hour << ":" << minute << ":" << second;
        return oss.str();
    }

    /**
     * @brief 编码发报时间（6字节BCD: YYMMDDHHmmSS）
     */
    static std::vector<uint8_t> encodeReportTime(std::chrono::system_clock::time_point tp) {
        auto zoned = std::chrono::zoned_time{std::chrono::current_zone(), tp};
        auto local = zoned.get_local_time();
        auto dp = std::chrono::floor<std::chrono::days>(local);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{local - dp};

        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(2) << (static_cast<int>(ymd.year()) % 100)
            << std::setw(2) << static_cast<unsigned>(ymd.month())
            << std::setw(2) << static_cast<unsigned>(ymd.day())
            << std::setw(2) << hms.hours().count()
            << std::setw(2) << hms.minutes().count()
            << std::setw(2) << hms.seconds().count();

        return stringToBCD(oss.str());
    }

    /**
     * @brief CRC16 Modbus 校验
     * @param data 数据
     * @return CRC16 值
     */
    static uint16_t crc16Modbus(const std::vector<uint8_t>& data) {
        uint16_t crc = 0xFFFF;
        for (uint8_t byte : data) {
            crc ^= byte;
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

    /**
     * @brief CRC16 Modbus 校验（从原始指针）
     */
    static uint16_t crc16Modbus(const uint8_t* data, size_t len) {
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

    /**
     * @brief 字节数组转十六进制字符串（带空格）- 使用查表优化
     */
    static std::string bufferToHexWithSpaces(const std::vector<uint8_t>& data) {
        if (data.empty()) return {};
        std::string result;
        result.reserve(data.size() * 3 - 1);  // "XX XX XX"
        for (size_t i = 0; i < data.size(); ++i) {
            if (i > 0) result += ' ';
            result.append(hexTable_.table[data[i]], 2);
        }
        return result;
    }

    /**
     * @brief 字节数组转十六进制字符串（不带空格）- 使用查表优化
     */
    static std::string bufferToHex(const std::vector<uint8_t>& data) {
        return bufferToHexFast(data);
    }

    /**
     * @brief 十六进制字符串转字节数组
     */
    static std::vector<uint8_t> hexToBuffer(const std::string& hex) {
        std::vector<uint8_t> result;
        for (size_t i = 0; i < hex.length(); i += 2) {
            result.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
        return result;
    }

    /**
     * @brief 格式化流水号为4位字符串
     */
    static std::string formatSerialNumber(int num) {
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << num;
        return oss.str();
    }

    /**
     * @brief 编码流水号为 HEX（2字节 Big-Endian）
     */
    static std::vector<uint8_t> encodeSerialNumberHex(const std::string& str) {
        int num = std::stoi(str);
        return {
            static_cast<uint8_t>((num >> 8) & 0xFF),
            static_cast<uint8_t>(num & 0xFF)
        };
    }

    /**
     * @brief 读取 Big-Endian 16位整数
     */
    static uint16_t readUInt16BE(const std::vector<uint8_t>& data, size_t offset) {
        return (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    }

    /**
     * @brief 读取 Big-Endian 16位整数（从原始指针）
     */
    static uint16_t readUInt16BE(const uint8_t* data, size_t offset) {
        return (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    }

    /**
     * @brief 写入 Big-Endian 16位整数
     */
    static void writeUInt16BE(std::vector<uint8_t>& data, uint16_t value) {
        data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    /**
     * @brief 在数据中查找字节序列
     * @return 找到的位置，未找到返回 -1
     */
    static int indexOf(const std::vector<uint8_t>& data, const std::vector<uint8_t>& pattern, size_t start = 0) {
        if (pattern.empty() || data.size() < pattern.size()) {
            return -1;
        }
        for (size_t i = start; i <= data.size() - pattern.size(); ++i) {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (data[i + j] != pattern[j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /**
     * @brief 解析 BCD 数值
     * @param bcdStr BCD 字符串
     * @param digits 小数位数
     * @return 解析后的数值
     */
    static double parseBCDValue(const std::string& bcdStr, int digits) {
        try {
            int64_t intValue = std::stoll(bcdStr);
            if (digits > 0) {
                return static_cast<double>(intValue) / std::pow(10, digits);
            }
            return static_cast<double>(intValue);
        } catch (...) {
            return 0.0;
        }
    }

    /**
     * @brief 字节数组转 Base64 编码
     * @param data 字节数组
     * @return Base64 编码字符串
     */
    static std::string toBase64(const std::vector<uint8_t>& data) {
        static constexpr char base64Chars[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string result;
        result.reserve((data.size() + 2) / 3 * 4);

        size_t i = 0;
        while (i + 2 < data.size()) {
            uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                         (static_cast<uint32_t>(data[i + 1]) << 8) |
                         static_cast<uint32_t>(data[i + 2]);
            result += base64Chars[(n >> 18) & 0x3F];
            result += base64Chars[(n >> 12) & 0x3F];
            result += base64Chars[(n >> 6) & 0x3F];
            result += base64Chars[n & 0x3F];
            i += 3;
        }

        if (i + 1 == data.size()) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            result += base64Chars[(n >> 18) & 0x3F];
            result += base64Chars[(n >> 12) & 0x3F];
            result += '=';
            result += '=';
        } else if (i + 2 == data.size()) {
            uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                         (static_cast<uint32_t>(data[i + 1]) << 8);
            result += base64Chars[(n >> 18) & 0x3F];
            result += base64Chars[(n >> 12) & 0x3F];
            result += base64Chars[(n >> 6) & 0x3F];
            result += '=';
        }

        return result;
    }
};

}  // namespace sl651
