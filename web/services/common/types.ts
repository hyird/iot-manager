/**
 * Service 层通用类型定义
 */

/** 分页参数 */
export interface PaginationParams {
  page?: number;
  pageSize?: number;
}

/** 分页响应 */
export interface PaginatedResult<T> {
  list: T[];
  total: number;
  page: number;
  pageSize: number;
}

/** 通用查询参数 */
export interface BaseQuery extends PaginationParams {
  keyword?: string;
}

/** 通用状态 */
export type Status = "enabled" | "disabled";
