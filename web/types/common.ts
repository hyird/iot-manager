/**
 * 通用类型定义
 */

// API 响应
export interface ApiResponse<T = unknown> {
  code: number | string;
  message?: string;
  data: T;
}

export interface ApiError {
  code: string | number;
  message: string;
  status?: number;
}

// 分页参数 (支持 string 类型，用于 HTTP 请求)
export interface PageParams {
  page?: number | string;
  pageSize?: number | string;
  keyword?: string;
}

// 分页结果 (不传 pageSize 时，page/pageSize/totalPages 不返回)
export interface PageResult<T> {
  list: T[];
  total: number;
  page?: number;
  pageSize?: number;
  totalPages?: number;
}

// 标准化后的分页参数
export interface NormalizedPagination {
  page: number;
  pageSize: number;
  skip: number;
  keyword?: string;
}

// 树节点通用接口
export interface TreeNode {
  id: number;
  parent_id?: number | null;
  children?: TreeNode[];
}

// 构建树的选项
export interface BuildTreeOptions<T> {
  idKey?: keyof T;
  parentKey?: keyof T;
  childrenKey?: string;
  rootParentValue?: unknown;
}
