/**
 * 通用验证工具函数
 */

// 验证规则正则表达式
export const VALIDATION_REGEX = {
  /** 手机号（中国大陆）*/
  PHONE: /^1[3-9]\d{9}$/,
  /** 邮箱 */
  EMAIL: /^[^\s@]+@[^\s@]+\.[^\s@]+$/,
  /** 用户名（字母、数字、下划线，4-20位）*/
  USERNAME: /^[a-zA-Z0-9_]{4,20}$/,
  /** 密码（至少包含字母和数字，6-20位）*/
  PASSWORD: /^(?=.*[A-Za-z])(?=.*\d)[A-Za-z\d@$!%*#?&]{6,20}$/,
} as const;

/**
 * 验证手机号
 */
export function validatePhone(phone?: string): boolean {
  if (!phone) return true; // 可选字段
  return VALIDATION_REGEX.PHONE.test(phone);
}

/**
 * 验证邮箱
 */
export function validateEmail(email?: string): boolean {
  if (!email) return true; // 可选字段
  return VALIDATION_REGEX.EMAIL.test(email);
}

/**
 * 验证用户名
 */
export function validateUsername(username: string): boolean {
  return VALIDATION_REGEX.USERNAME.test(username);
}

/**
 * 验证密码强度
 */
export function validatePassword(password: string): boolean {
  return VALIDATION_REGEX.PASSWORD.test(password);
}

/**
 * 验证 ID 是否为正整数
 */
export function validateId(id: number): boolean {
  return Number.isInteger(id) && id > 0;
}

/**
 * 验证字符串长度
 */
export function validateLength(str: string, min: number, max: number): boolean {
  const len = str.length;
  return len >= min && len <= max;
}

/**
 * 验证是否为有效的状态值
 */
export function validateStatus<T extends string>(
  status: string,
  validStatuses: readonly T[]
): status is T {
  return validStatuses.includes(status as T);
}
