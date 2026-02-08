#pragma once

/**
 * @brief Modbus 协议处理模块
 *
 * 包含:
 * - Modbus.Types.hpp   - 类型定义、常量、枚举
 * - Modbus.Utils.hpp   - CRC16、帧构建/解析、数据转换、寄存器合并
 * - Modbus.Handler.hpp - 主处理器：轮询、请求-响应关联、数据存储
 */

#include "Modbus.Types.hpp"
#include "Modbus.Utils.hpp"
#include "Modbus.Handler.hpp"
