#pragma once

/**
 * @brief 领域驱动设计统一入口
 *
 * 包含所有领域基础设施和聚合根定义。
 *
 * 使用示例：
 * @code
 * #include "common/domain/Domain.hpp"
 *
 * // 创建用户
 * co_await User::create(data)
 *     .require(User::usernameUnique)
 *     .withRoles(roleIds)
 *     .save();
 *
 * // 更新角色
 * co_await Role::of(id)
 *     .require(Role::notSuperadmin)
 *     .update(data)
 *     .save();
 * @endcode
 */

// ==================== 基础设施 ====================

#include "DomainEvent.hpp"       // 领域事件定义
#include "EventBus.hpp"          // 事件发布订阅
#include "Aggregate.hpp"         // 聚合根基类

// ==================== 系统模块聚合根 ====================

#include "modules/system/domain/User.hpp"
#include "modules/system/domain/Role.hpp"
#include "modules/system/domain/Department.hpp"
#include "modules/system/domain/Menu.hpp"

// ==================== 设备模块聚合根 ====================

#include "modules/device/domain/Device.hpp"

// ==================== 链路模块聚合根 ====================

#include "modules/link/domain/Link.hpp"

// ==================== 协议模块聚合根 ====================

#include "modules/protocol/domain/ProtocolConfig.hpp"
