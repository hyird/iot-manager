/**
 * 分页工具函数
 */

import type { NormalizedPagination, PageParams } from "../types/common";

/** 分页配置常量 */
const PAGINATION_CONFIG = {
  DEFAULT_PAGE_SIZE: 10,
  MAX_PAGE_SIZE: 100,
  MIN_PAGE_SIZE: 1,
} as const;

/**
 * 标准化分页参数
 */
export function normalizePagination(params: PageParams): NormalizedPagination {
  const page = Math.max(1, Number(params.page) || 1);
  const pageSize = Math.min(
    PAGINATION_CONFIG.MAX_PAGE_SIZE,
    Math.max(
      PAGINATION_CONFIG.MIN_PAGE_SIZE,
      Number(params.pageSize) || PAGINATION_CONFIG.DEFAULT_PAGE_SIZE
    )
  );
  const keyword =
    (typeof params.keyword === "string" ? params.keyword.trim() : undefined) || undefined;

  return {
    page,
    pageSize,
    skip: (page - 1) * pageSize,
    keyword,
  };
}
