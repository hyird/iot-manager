#pragma once

/**
 * @brief 协议解析后的标准化结果（线程间传递）
 *
 * 由 TcpIoPool 线程的协议解析器生成，
 * 通过 queueInLoop 投递到 Drogon IO 线程执行 DB 写入。
 */
struct ParsedFrameResult {
    int deviceId = 0;
    int linkId = 0;
    std::string protocol;       // "SL651" / "Modbus"
    std::string funcCode;
    Json::Value data;           // 完整 JSONB（直接序列化入库）
    std::string reportTime;

    // SL651 指令应答（可选）
    struct CommandResponse {
        std::string deviceCode;
        std::string funcCode;
        bool success = false;
    };
    std::optional<CommandResponse> commandResponse;
};
