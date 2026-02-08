/**
 * 文本工具函数
 */

/** 计算加权长度（中文字符 1.5，其他字符 1） */
export const calcWeightedLength = (str: string) => {
  let len = 0;
  for (const ch of str) {
    if (/[\u4e00-\u9fa5]/.test(ch)) len += 1.5;
    else len += 1;
  }
  return len;
};
