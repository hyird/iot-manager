#pragma once

/**
 * @brief 统一错误码定义
 *
 * 错误码规则：
 * - 0: 成功
 * - 1xxx: 客户端错误（请求参数、资源不存在、权限等）
 * - 2xxx: 认证/授权错误
 * - 5xxx: 服务器内部错误
 */
namespace ErrorCodes {

// ==================== 成功 ====================

/** 操作成功 */
inline constexpr int SUCCESS = 0;

// ==================== 客户端错误 (1xxx) ====================

/** 资源不存在 */
inline constexpr int NOT_FOUND = 1001;

/** 请求参数错误 */
inline constexpr int BAD_REQUEST = 1002;

/** 无权限访问 */
inline constexpr int FORBIDDEN = 1003;

/** 数据冲突（如唯一约束） */
inline constexpr int CONFLICT = 1004;

/** 数据验证失败 */
inline constexpr int VALIDATION_FAILED = 1005;

// ==================== 认证错误 (2xxx) ====================

/** 未认证/Token 无效 */
inline constexpr int UNAUTHORIZED = 2004;

/** Token 已过期 */
inline constexpr int TOKEN_EXPIRED = 2001;

/** Token 已被加入黑名单 */
inline constexpr int TOKEN_BLACKLISTED = 2002;

/** 用户名或密码错误 */
inline constexpr int PASSWORD_INCORRECT = 2003;

/** 用户已被禁用 */
inline constexpr int USER_DISABLED = 2005;

/** 用户不存在 */
inline constexpr int USER_NOT_FOUND = 2006;

// ==================== 服务器错误 (5xxx) ====================

/** 服务器内部错误 */
inline constexpr int INTERNAL_ERROR = 5000;

/** 数据库错误 */
inline constexpr int DATABASE_ERROR = 5001;

/** 外部服务错误 */
inline constexpr int EXTERNAL_SERVICE_ERROR = 5002;

}  // namespace ErrorCodes
