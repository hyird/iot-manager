#pragma once

/**
 * @brief SL651 水文数据采集协议
 *
 * 包含:
 * - SL651.Types.hpp   - 类型定义
 * - SL651.Utils.hpp   - 工具函数（BCD编解码、CRC16）
 * - SL651.Parser.hpp  - 协议解析器
 * - SL651.Builder.hpp - 报文构建器
 */

#include "SL651.Types.hpp"
#include "SL651.Utils.hpp"
#include "SL651.Parser.hpp"
#include "SL651.Builder.hpp"
