#pragma once

/**
 * @brief Modbus 协议处理模块
 *
 * 包含:
 * - Modbus.Types.hpp   - 类型定义、常量、枚举
 * - Modbus.Utils.hpp   - CRC16、帧构建/解析、数据转换、寄存器合并
 * - Modbus.SessionTypes.hpp - DTU/session 重构草案的共享类型
 * - DtuRegistry.hpp - DTU 配置聚合注册表
 * - DtuSessionManager.hpp - DTU 会话与在线路由
 * - RegistrationNormalizer.hpp - 注册码归一化
 * - ModbusSessionEngine.hpp - Session 粒度收发引擎
 * - ModbusPollScheduler.hpp - 轮询任务调度器
 */

#include "Modbus.Types.hpp"
#include "Modbus.Utils.hpp"
#include "Modbus.SessionTypes.hpp"
#include "DtuRegistry.hpp"
#include "DtuSessionManager.hpp"
#include "RegistrationNormalizer.hpp"
#include "ModbusSessionEngine.hpp"
#include "ModbusPollScheduler.hpp"
