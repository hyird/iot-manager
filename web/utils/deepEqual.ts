/**
 * 通用深度比较函数
 * 支持基本类型、对象、数组、Date、RegExp、Map、Set
 */
export function deepEqual(a: unknown, b: unknown): boolean {
  // 严格相等（处理基本类型、null、undefined、同一引用）
  if (a === b) return true;

  // 类型不同
  if (typeof a !== typeof b) return false;

  // null / undefined 检查
  if (a == null || b == null) return false;

  // 非对象类型且不严格相等
  if (typeof a !== "object") {
    // 处理 NaN === NaN 的情况
    if (Number.isNaN(a) && Number.isNaN(b)) return true;
    return false;
  }

  // 以下都是 object 类型，且 a、b 都不为 null/undefined
  const objA = a as object;
  const objB = b as object;

  // 构造函数不同
  if (objA.constructor !== objB.constructor) return false;

  // Date 比较
  if (objA instanceof Date && objB instanceof Date) {
    return objA.getTime() === objB.getTime();
  }

  // RegExp 比较
  if (objA instanceof RegExp && objB instanceof RegExp) {
    return objA.source === objB.source && objA.flags === objB.flags;
  }

  // Map 比较
  if (objA instanceof Map && objB instanceof Map) {
    if (objA.size !== objB.size) return false;
    for (const [key, value] of objA) {
      if (!objB.has(key) || !deepEqual(value, objB.get(key))) {
        return false;
      }
    }
    return true;
  }

  // Set 比较
  if (objA instanceof Set && objB instanceof Set) {
    if (objA.size !== objB.size) return false;
    for (const value of objA) {
      let found = false;
      for (const bValue of objB) {
        if (deepEqual(value, bValue)) {
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
    return true;
  }

  // 数组比较
  if (Array.isArray(objA) && Array.isArray(objB)) {
    if (objA.length !== objB.length) return false;
    return objA.every((item, index) => deepEqual(item, objB[index]));
  }

  // 普通对象比较
  const recordA = objA as Record<string, unknown>;
  const recordB = objB as Record<string, unknown>;

  const keysA = Object.keys(recordA);
  const keysB = Object.keys(recordB);

  if (keysA.length !== keysB.length) return false;

  return keysA.every((key) => {
    return key in recordB && deepEqual(recordA[key], recordB[key]);
  });
}
