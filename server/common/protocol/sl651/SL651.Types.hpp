#pragma once

namespace sl651 {

/**
 * @brief 编码类型
 */
enum class Encode {
    BCD,                    // BCD 编码
    TIME_YYMMDDHHMMSS,      // 时间 BCD 编码
    JPEG,                   // JPEG 图片
    DICT,                   // 字典值
    HEX                     // 十六进制
};

/**
 * @brief 传输方向
 */
enum class Direction {
    UP,     // 上行（设备->中心）
    DOWN    // 下行（中心->设备）
};

/**
 * @brief 帧控制字符
 */
struct FrameControl {
    static constexpr uint8_t FRAME_HEADER = 0x7E;       // 帧头
    static constexpr uint8_t STX_SINGLE = 0x02;         // 单包起始
    static constexpr uint8_t STX_MULTI = 0x16;          // 多包起始
    static constexpr uint8_t ETX_END = 0x03;            // 结束符（不需应答）
    static constexpr uint8_t ETX_INQUIRY = 0x05;        // 结束符（需要应答）
};

/**
 * @brief 功能码定义
 */
struct FuncCodes {
    static constexpr const char* LINK_KEEP = "2F";      // 链路维持
    static constexpr const char* TEST = "30";           // 测试
    static constexpr const char* EVEN_TIME = "31";      // 均匀时段水文信息
    static constexpr const char* HOUR = "32";           // 遥测站定时报
    static constexpr const char* TIMED_REPORT = "32";   // 定时报（HOUR 的别名）
    static constexpr const char* ADD_REPORT = "33";     // 遥测站加报报
    static constexpr const char* HOUR_RAIN = "34";      // 小时雨量
    static constexpr const char* DAY_DATA = "35";       // 日报
    static constexpr const char* QUERY_TIME = "40";     // 查询遥测站时钟
    static constexpr const char* SET_TIME = "41";       // 设置遥测站时钟
    static constexpr const char* QUERY_ADDR = "42";     // 查询遥测站地址
    static constexpr const char* SET_ADDR = "43";       // 设置遥测站地址
    static constexpr const char* QUERY_PARAMS = "44";   // 查询遥测站参数
    static constexpr const char* SET_PARAMS = "45";     // 设置遥测站参数
    static constexpr const char* QUERY_REALTIME = "46"; // 查询实时数据
    static constexpr const char* QUERY_HISTORY = "47";  // 查询历史数据
    static constexpr const char* QUERY_MANUAL = "48";   // 查询人工置数
    static constexpr const char* SET_MANUAL = "49";     // 设置人工置数
    static constexpr const char* SET_PASSWORD = "4A";   // 修改密码
    static constexpr const char* SET_IC = "4B";         // 设置 IC 卡状态
    static constexpr const char* PUMP_CONTROL = "4C";   // 水泵控制
    static constexpr const char* VALVE_CONTROL = "4D";  // 阀门控制
    static constexpr const char* GATE_CONTROL = "4E";   // 闸门控制
    static constexpr const char* IMAGE = "53";          // 图片数据
    static constexpr const char* INIT = "80";           // 初始化固态存储
    static constexpr const char* RESET = "81";          // 恢复出厂设置
    static constexpr const char* TRANSFER = "82";       // 数据传输
    static constexpr const char* EVENT = "E0";          // 事件报告
    static constexpr const char* ACK_OK = "E1";         // 确认帧
    static constexpr const char* ACK_ERR = "E2";        // 否认帧
};

/**
 * @brief SL651 原始帧结构
 */
struct Sl651Frame {
    Direction direction;        // 传输方向
    std::string centerCode;     // 中心站地址（2位 HEX）
    std::string remoteCode;     // 遥测站地址（10位 BCD）
    std::string password;       // 密码（4位 BCD）
    std::string funcCode;       // 功能码（2位 HEX）
    uint8_t stx;                // 起始符
    uint8_t etx;                // 结束符
    std::vector<uint8_t> body;  // 正文数据
    uint16_t crcRecv;           // 接收的 CRC
    uint16_t crcCalc;           // 计算的 CRC
    bool crcValid;              // CRC 是否有效
    std::vector<uint8_t> raw;   // 原始报文

    // 多包信息
    bool isMultiPacket;         // 是否多包
    int totalPk;                // 总包数
    int seqPk;                  // 当前包序号
    bool isLastPacket;          // 是否最后一包

    // 流水号（仅上行有）
    std::optional<std::string> serialNumber;
};

/**
 * @brief 字典映射类型
 */
enum class DictMapType {
    VALUE,  // 值映射
    BIT     // 位映射
};

/**
 * @brief 字典映射依赖条件运算符
 */
enum class DictDependencyOperator {
    AND,  // 所有条件都必须满足
    OR    // 至少一个条件满足
};

/**
 * @brief 字典映射依赖条件
 */
struct DictDependency {
    std::string bitIndex;  // 依赖的位号
    std::string bitValue;  // 依赖位的期望值（"0"或"1"）
};

/**
 * @brief 字典映射项依赖配置
 */
struct DictDependsOn {
    DictDependencyOperator op;              // 条件运算符
    std::vector<DictDependency> conditions; // 条件列表
};

/**
 * @brief 字典映射项
 */
struct DictMapItem {
    std::string key;    // 值或位号（VALUE模式下为值，BIT模式下为位号0-31）
    std::string label;  // 映射的文本
    std::string value;  // 触发值（仅BIT模式使用，"0"或"1"，表示该位为此值时触发，默认"1"）
    std::optional<DictDependsOn> dependsOn;  // 依赖条件（仅BIT模式使用）
};

/**
 * @brief 字典配置
 */
struct DictConfig {
    DictMapType mapType;            // 映射类型
    std::vector<DictMapItem> items; // 映射项列表
};

/**
 * @brief 要素定义
 */
struct ElementDef {
    std::string id;             // 要素 ID（UUID 字符串）
    std::string name;           // 要素名称
    std::string funcCode;       // 功能码
    std::string guideHex;       // 引导符（HEX）
    Encode encode;              // 编码类型
    int length;                 // 数据长度（字节）
    int digits;                 // 小数位数
    std::string unit;           // 单位
    std::string remark;         // 备注
    std::optional<DictConfig> dictConfig;  // 字典配置（仅 DICT 编码类型使用）
};

/**
 * @brief 解析后的要素数据
 */
struct ParsedElement {
    std::string name;           // 要素名称
    std::string guideHex;       // 引导符
    std::string rawValue;       // 原始值（HEX 字符串）
    std::string value;          // 解析后的值
    std::string unit;           // 单位
    std::string elementId;      // 要素 ID（UUID 字符串）
    Encode encode;              // 编码类型
};

/**
 * @brief 解析后的正文数据
 */
struct ParsedBody {
    std::vector<ParsedElement> data;        // 解析的要素列表
    std::vector<uint8_t> unparsed;          // 未解析的数据
};

/**
 * @brief 设备配置
 */
struct DeviceConfig {
    int deviceId;               // 设备 ID
    std::string deviceName;     // 设备名称
    std::string deviceCode;     // 设备编码（遥测站地址）
    int protocolConfigId;       // 协议配置 ID
    int linkId;                 // 链路 ID
    std::string timezone = "+08:00";  // 设备时区（用于报文时间转换）
    std::map<std::string, std::vector<ElementDef>> elementsByFunc;  // 按功能码分组的要素
    std::map<std::string, std::vector<ElementDef>> responseElementsByFunc;  // 下行功能码的应答要素
    std::map<std::string, std::string> funcNames;  // 功能码名称映射 (funcCode -> name)
    std::map<std::string, Direction> funcDirections;  // 功能码方向映射 (funcCode -> direction)
};

/**
 * @brief 多包会话（中间包仅在内存缓存，所有包收齐后一次性入库）
 */
struct MultiPacketSession {
    std::string remoteCode;     // 遥测站地址
    std::string funcCode;       // 功能码
    int totalPk;                // 总包数
    std::set<int> receivedPk;   // 已接收的包序号
    std::map<int, std::vector<uint8_t>> packets;  // 各包正文数据
    std::map<int, std::vector<uint8_t>> rawFrames; // 各包原始报文
    std::chrono::steady_clock::time_point startTime;  // 开始时间
};

/**
 * @brief 下行报文构建参数
 */
struct BuildDownFrameParams {
    std::string centerCode;     // 中心站地址
    std::string remoteCode;     // 遥测站地址
    std::string password;       // 密码
    std::string funcCode;       // 功能码
    std::string serialNumber;   // 流水号
    std::chrono::system_clock::time_point reportTime;  // 发报时间

    struct ElementValue {
        std::string guideHex;   // 引导符
        std::string value;      // 值
        Encode encode;          // 编码类型
        int length;             // 长度
        int digits;             // 小数位数
    };
    std::vector<ElementValue> elements;  // 要素列表
};

/**
 * @brief 下行控制请求
 */
struct SendControlRequest {
    std::string deviceCode;     // 设备编码
    std::string funcCode;       // 功能码
    struct ElementData {
        std::string elementId;  // 要素 ID（UUID 字符串）
        std::string value;      // 值
    };
    std::vector<ElementData> elements;  // 要素数据
};

/**
 * @brief 下行控制响应
 */
struct SendControlResponse {
    bool success;               // 是否成功
    std::string message;        // 消息
    std::string serialNumber;   // 流水号
    std::string rawFrame;       // 原始报文（HEX）
    int frameId;                // 帧 ID
    std::string ackStatus;      // 应答状态
};

/**
 * @brief 字符串转方向枚举
 */
inline Direction parseDirection(const std::string& str) {
    return str == "DOWN" ? Direction::DOWN : Direction::UP;
}

/**
 * @brief 方向枚举转字符串
 */
inline std::string directionToString(Direction dir) {
    return dir == Direction::DOWN ? "DOWN" : "UP";
}

/**
 * @brief 字符串转编码类型
 */
inline Encode parseEncode(const std::string& str) {
    if (str == "TIME_YYMMDDHHMMSS") return Encode::TIME_YYMMDDHHMMSS;
    if (str == "JPEG") return Encode::JPEG;
    if (str == "DICT") return Encode::DICT;
    if (str == "HEX") return Encode::HEX;
    return Encode::BCD;
}

/**
 * @brief 编码类型转字符串
 */
inline std::string encodeToString(Encode encode) {
    switch (encode) {
        case Encode::TIME_YYMMDDHHMMSS: return "TIME";
        case Encode::JPEG: return "JPEG";
        case Encode::DICT: return "DICT";
        case Encode::HEX: return "HEX";
        case Encode::BCD:
        default: return "BCD";
    }
}

/**
 * @brief 字符串转字典映射类型
 */
inline DictMapType parseDictMapType(const std::string& str) {
    return str == "BIT" ? DictMapType::BIT : DictMapType::VALUE;
}

/**
 * @brief 字典映射类型转字符串
 */
inline std::string dictMapTypeToString(DictMapType type) {
    return type == DictMapType::BIT ? "BIT" : "VALUE";
}

/**
 * @brief 字符串转依赖条件运算符
 */
inline DictDependencyOperator parseDictDependencyOperator(const std::string& str) {
    return str == "OR" ? DictDependencyOperator::OR : DictDependencyOperator::AND;
}

/**
 * @brief 依赖条件运算符转字符串
 */
inline std::string dictDependencyOperatorToString(DictDependencyOperator op) {
    return op == DictDependencyOperator::OR ? "OR" : "AND";
}

}  // namespace sl651
