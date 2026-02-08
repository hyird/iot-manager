/**
 * 部门管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { TreeSelectProps } from "antd";
import { useMemo } from "react";
import type { Department } from "@/types";
import * as api from "./api";
import { departmentQueryKeys } from "./keys";

/**
 * 获取部门列表
 */
export function useDepartmentList(
  params?: Department.Query,
  options?: Omit<UseQueryOptions<Department.Item[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: departmentQueryKeys.list(params),
    queryFn: () => api.getList(params),
    ...options,
  });
}

/**
 * 获取部门树
 */
export function useDepartmentTree(
  status?: Department.Status,
  options?: Omit<UseQueryOptions<Department.TreeItem[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: departmentQueryKeys.tree(status),
    queryFn: () => api.getTree(status),
    staleTime: 5 * 60 * 1000, // 5分钟缓存
    ...options,
  });
}

/**
 * 获取部门详情
 */
export function useDepartmentDetail(
  id: number,
  options?: Omit<UseQueryOptions<Department.Item>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: departmentQueryKeys.detail(id),
    queryFn: () => api.getDetail(id),
    enabled: !!id,
    ...options,
  });
}

/**
 * 获取部门树（用于 TreeSelect 组件）
 * 自动转换为 TreeSelect 需要的数据格式
 */
export function useDepartmentTreeSelect(status?: Department.Status) {
  const { data: tree = [], ...rest } = useDepartmentTree(status);

  const treeData = useMemo((): TreeSelectProps["treeData"] => {
    const loop = (nodes: Department.TreeItem[]): TreeSelectProps["treeData"] =>
      nodes.map((n) => ({
        title: n.name,
        value: n.id,
        children: n.children?.length ? loop(n.children) : undefined,
      }));
    return loop(tree);
  }, [tree]);

  /** 扁平化的部门 Map */
  const departmentMap = useMemo(() => {
    const map = new Map<number, Department.TreeItem>();
    const flatten = (nodes: Department.TreeItem[]) => {
      nodes.forEach((n) => {
        map.set(n.id, n);
        if (n.children?.length) flatten(n.children);
      });
    };
    flatten(tree);
    return map;
  }, [tree]);

  return {
    ...rest,
    tree,
    treeData,
    departmentMap,
  };
}
