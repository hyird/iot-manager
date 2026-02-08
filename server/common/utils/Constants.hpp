#pragma once

/**
 * @brief 全局常量定义
 *
 * 集中管理项目中的魔法数字，提高可维护性和可读性
 */
namespace Constants {

// ==================== 日志相关 ====================

/** 请求体日志截断长度 */
inline constexpr int REQUEST_LOG_MAX_LENGTH = 1000;

// ==================== 缓存相关 ====================

/** 用户会话缓存 TTL（秒）- 1小时 */
inline constexpr int CACHE_TTL_USER_SESSION = 3600;

/** 用户菜单缓存 TTL（秒）- 30分钟 */
inline constexpr int CACHE_TTL_USER_MENUS = 1800;

/** 用户角色缓存 TTL（秒）- 1小时 */
inline constexpr int CACHE_TTL_USER_ROLES = 3600;

/** 登录失败限流窗口（秒）- 15分钟 */
inline constexpr int LOGIN_FAILURE_WINDOW = 900;

// ==================== Redis 相关 ====================

/** 实时数据 Redis TTL（秒）- 24小时 */
inline constexpr int REDIS_TTL_REALTIME_DATA = 86400;

// ==================== 协议相关 ====================

/** SL651 多包会话超时（毫秒）- 15分钟 */
inline constexpr int SL651_SESSION_TIMEOUT_MS = 900000;

// ==================== 设备连接缓存 ====================

/** 设备连接超时时间（秒）- 5分钟 */
inline constexpr int DEVICE_CONNECTION_TIMEOUT = 300;

/** SL651 帧头长度（字节） */
inline constexpr size_t SL651_FRAME_HEADER_SIZE = 13;

// ==================== 数据库查询相关 ====================

/** 实时数据查询回溯天数 */
inline constexpr int DEVICE_DATA_LOOKBACK_DAYS = 7;

/** 历史数据默认查询天数 */
inline constexpr int HISTORY_DEFAULT_QUERY_DAYS = 30;

/** 归档数据阈值天数 */
inline constexpr int ARCHIVE_THRESHOLD_DAYS = 365;

/** 非分页查询最大返回行数（图表等场景的安全上限） */
inline constexpr int MAX_UNPAGED_ROWS = 2000;

// ==================== 链路模式 ====================

/** TCP 服务端模式 */
inline constexpr const char* LINK_MODE_TCP_SERVER = "TCP Server";

/** TCP 客户端模式 */
inline constexpr const char* LINK_MODE_TCP_CLIENT = "TCP Client";

// ==================== 协议类型 ====================

/** SL651 协议 */
inline constexpr const char* PROTOCOL_SL651 = "SL651";

/** Modbus 协议（通用寄存器配置） */
inline constexpr const char* PROTOCOL_MODBUS = "Modbus";

/** Modbus TCP 协议 */
inline constexpr const char* PROTOCOL_MODBUS_TCP = "Modbus TCP";

/** Modbus RTU 协议（通过 TCP 转发）*/
inline constexpr const char* PROTOCOL_MODBUS_RTU = "Modbus RTU";

// ==================== 用户状态 ====================

/** 用户状态 - 启用 */
inline constexpr const char* USER_STATUS_ENABLED = "enabled";

/** 用户状态 - 禁用 */
inline constexpr const char* USER_STATUS_DISABLED = "disabled";

// ==================== 菜单类型 ====================

/** 菜单类型 - 目录（菜单） */
inline constexpr const char* MENU_TYPE_MENU = "menu";

/** 菜单类型 - 页面 */
inline constexpr const char* MENU_TYPE_PAGE = "page";

/** 菜单类型 - 按钮 */
inline constexpr const char* MENU_TYPE_BUTTON = "button";

// ==================== 角色相关 ====================

/** 超级管理员角色编码 */
inline constexpr const char* ROLE_SUPERADMIN = "superadmin";

// ==================== TCP 重连策略 ====================

/** TCP Client 重连基础延迟（秒） */
inline constexpr double RECONNECT_BASE_DELAY_SEC = 2.0;

/** TCP Client 重连最大延迟（秒）- 5分钟 */
inline constexpr double RECONNECT_MAX_DELAY_SEC = 300.0;

/** 重连抖动比例（±20%） */
inline constexpr double RECONNECT_JITTER_RATIO = 0.2;

// ==================== 认证相关 ====================

/** 登录失败最大次数 */
inline constexpr int LOGIN_MAX_FAILURE_COUNT = 5;

/** 密码最小长度 */
inline constexpr size_t PASSWORD_MIN_LENGTH = 6;

}  // namespace Constants
