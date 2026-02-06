#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <json/json.h>

namespace modbus {

// ==================== 枚举类型 ====================

/** 寄存器类型 */
enum class RegisterType {
    COIL,               // FC01 Read Coils
    DISCRETE_INPUT,     // FC02 Read Discrete Inputs
    HOLDING_REGISTER,   // FC03 Read Holding Registers
    INPUT_REGISTER      // FC04 Read Input Registers
};

/** 数据类型（决定占用寄存器数量） */
enum class DataType {
    BOOL,       // 1 bit (线圈/离散输入)
    INT16,      // 1 register
    UINT16,     // 1 register
    INT32,      // 2 registers
    UINT32,     // 2 registers
    FLOAT32,    // 2 registers
    INT64,      // 4 registers
    UINT64,     // 4 registers
    DOUBLE      // 4 registers
};

/** 字节序（避免使用 BIG_ENDIAN/LITTLE_ENDIAN，与 Linux <endian.h> 宏冲突） */
enum class ByteOrder {
    Big,          // AB CD（标准 Modbus）
    Little,       // CD AB
    BigSwap,      // BA DC
    LittleSwap    // DC BA
};

/** Modbus 帧模式 */
enum class FrameMode {
    TCP,    // MBAP Header
    RTU     // SlaveAddr + PDU + CRC16
};

// ==================== 功能码常量 ====================

struct FuncCodes {
    static constexpr uint8_t READ_COILS = 0x01;
    static constexpr uint8_t READ_DISCRETE_INPUTS = 0x02;
    static constexpr uint8_t READ_HOLDING_REGISTERS = 0x03;
    static constexpr uint8_t READ_INPUT_REGISTERS = 0x04;
    static constexpr uint8_t WRITE_SINGLE_COIL = 0x05;
    static constexpr uint8_t WRITE_SINGLE_REGISTER = 0x06;
    static constexpr uint8_t WRITE_MULTIPLE_REGISTERS = 0x10;
};

// ==================== 帧结构 ====================

/** Modbus 请求参数 */
struct ModbusRequest {
    uint8_t slaveId;
    uint8_t functionCode;
    uint16_t startAddress;
    uint16_t quantity;
    uint16_t transactionId = 0;  // 仅 TCP 模式
};

/** Modbus 写请求参数 */
struct ModbusWriteRequest {
    uint8_t slaveId;
    uint8_t functionCode;     // FC05/FC06/FC10
    uint16_t address;
    std::vector<uint8_t> data;  // 编码后的值
    uint16_t quantity = 1;      // 寄存器数量（FC10 使用）
    uint16_t transactionId = 0; // 仅 TCP 模式
};

/** 解析后的 Modbus 响应 */
struct ModbusResponse {
    FrameMode mode;
    uint8_t slaveId;
    uint8_t functionCode;
    std::vector<uint8_t> data;    // 有效数据
    bool isException = false;
    uint8_t exceptionCode = 0;
    uint16_t transactionId = 0;   // 仅 TCP 模式
};

// ==================== 寄存器配置 ====================

/** 单个寄存器定义（从 protocol_config.config.registers[] 解析） */
struct RegisterDef {
    std::string id;
    std::string name;
    RegisterType registerType;
    uint16_t address;
    DataType dataType;
    uint16_t quantity;     // 占用的寄存器数
    std::string unit;
    std::string remark;
    int decimals = -1;     // 小数位数，-1 表示不限制（仅 FLOAT32/DOUBLE 生效）
    Json::Value dictConfig;
};

/** 合并后的读取请求组 */
struct ReadGroup {
    RegisterType registerType;
    uint8_t functionCode;
    uint16_t startAddress;
    uint16_t totalQuantity;
    std::vector<const RegisterDef*> registers;
};

/** 设备轮询上下文 */
struct DeviceContext {
    int deviceId;
    std::string deviceName;
    int linkId;
    std::string linkMode;   // 链路模式: "TCP Server" / "TCP Client"
    uint8_t slaveId;
    FrameMode frameMode;
    ByteOrder byteOrder;
    int readInterval;    // 轮询间隔（秒）
    std::vector<RegisterDef> registers;
    std::vector<ReadGroup> readGroups;
};

// ==================== 枚举解析函数 ====================

inline RegisterType parseRegisterType(const std::string& str) {
    if (str == "COIL") return RegisterType::COIL;
    if (str == "DISCRETE_INPUT") return RegisterType::DISCRETE_INPUT;
    if (str == "HOLDING_REGISTER") return RegisterType::HOLDING_REGISTER;
    if (str == "INPUT_REGISTER") return RegisterType::INPUT_REGISTER;
    return RegisterType::HOLDING_REGISTER;  // 默认
}

inline std::string registerTypeToString(RegisterType type) {
    switch (type) {
        case RegisterType::COIL: return "COIL";
        case RegisterType::DISCRETE_INPUT: return "DISCRETE_INPUT";
        case RegisterType::HOLDING_REGISTER: return "HOLDING_REGISTER";
        case RegisterType::INPUT_REGISTER: return "INPUT_REGISTER";
    }
    return "HOLDING_REGISTER";
}

inline DataType parseDataType(const std::string& str) {
    if (str == "BOOL") return DataType::BOOL;
    if (str == "INT16") return DataType::INT16;
    if (str == "UINT16") return DataType::UINT16;
    if (str == "INT32") return DataType::INT32;
    if (str == "UINT32") return DataType::UINT32;
    if (str == "FLOAT32") return DataType::FLOAT32;
    if (str == "INT64") return DataType::INT64;
    if (str == "UINT64") return DataType::UINT64;
    if (str == "DOUBLE") return DataType::DOUBLE;
    return DataType::UINT16;  // 默认
}

inline ByteOrder parseByteOrder(const std::string& str) {
    if (str == "BIG_ENDIAN") return ByteOrder::Big;
    if (str == "LITTLE_ENDIAN") return ByteOrder::Little;
    if (str == "BIG_ENDIAN_BYTE_SWAP") return ByteOrder::BigSwap;
    if (str == "LITTLE_ENDIAN_BYTE_SWAP") return ByteOrder::LittleSwap;
    return ByteOrder::Big;  // 默认
}

inline const char* byteOrderToString(ByteOrder order) {
    switch (order) {
        case ByteOrder::Big: return "BIG_ENDIAN";
        case ByteOrder::Little: return "LITTLE_ENDIAN";
        case ByteOrder::BigSwap: return "BIG_ENDIAN_BYTE_SWAP";
        case ByteOrder::LittleSwap: return "LITTLE_ENDIAN_BYTE_SWAP";
    }
    return "UNKNOWN";
}

inline FrameMode parseFrameMode(const std::string& modbusMode) {
    if (modbusMode == "RTU") return FrameMode::RTU;
    return FrameMode::TCP;  // 默认 TCP
}

/** 寄存器类型 → 读取功能码 */
inline uint8_t registerTypeToFuncCode(RegisterType type) {
    switch (type) {
        case RegisterType::COIL: return FuncCodes::READ_COILS;
        case RegisterType::DISCRETE_INPUT: return FuncCodes::READ_DISCRETE_INPUTS;
        case RegisterType::HOLDING_REGISTER: return FuncCodes::READ_HOLDING_REGISTERS;
        case RegisterType::INPUT_REGISTER: return FuncCodes::READ_INPUT_REGISTERS;
    }
    return FuncCodes::READ_HOLDING_REGISTERS;
}

/** 数据类型 → 占用寄存器数 */
inline uint16_t dataTypeToQuantity(DataType type) {
    switch (type) {
        case DataType::BOOL:
        case DataType::INT16:
        case DataType::UINT16:
            return 1;
        case DataType::INT32:
        case DataType::UINT32:
        case DataType::FLOAT32:
            return 2;
        case DataType::INT64:
        case DataType::UINT64:
        case DataType::DOUBLE:
            return 4;
    }
    return 1;
}

/** 数据类型 → 字节数 */
inline size_t dataTypeToByteSize(DataType type) {
    switch (type) {
        case DataType::BOOL: return 1;
        case DataType::INT16:
        case DataType::UINT16: return 2;
        case DataType::INT32:
        case DataType::UINT32:
        case DataType::FLOAT32: return 4;
        case DataType::INT64:
        case DataType::UINT64:
        case DataType::DOUBLE: return 8;
    }
    return 2;
}

/** 是否为位类型寄存器（线圈/离散输入） */
inline bool isBitRegister(RegisterType type) {
    return type == RegisterType::COIL || type == RegisterType::DISCRETE_INPUT;
}

/** 寄存器类型是否可写 */
inline bool isWritable(RegisterType type) {
    return type == RegisterType::COIL || type == RegisterType::HOLDING_REGISTER;
}

}  // namespace modbus
