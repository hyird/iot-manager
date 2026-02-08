#pragma once

#include <json/json.h>

#include <optional>
#include <string>

/**
 * @brief 协议解析后的标准化结果（线程间传递）
 *
 * 由 TcpIoPool 线程的协议解析器生成，
 * 通过 queueInLoop 投递到 Drogon IO 线程执行 DB 写入。
 *
 * 这是协议无关的公共结果对象，供 SL651 / Modbus 等协议复用。
 */
struct ParsedFrameResult {
    int deviceId = 0;
    int linkId = 0;
    std::string protocol;       // "SL651" / "Modbus"
    std::string funcCode;
    Json::Value data;           // 完整 JSONB（直接序列化入库）
    std::string reportTime;

    // 解析结果可携带一条命令完成事件，供上层关联下行记录。
    struct CommandCompletion {
        std::string commandKey;
        std::string responseCode;
        bool success = false;
    };
    std::optional<CommandCompletion> commandCompletion;
};
