/**
 * 菜单管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { TreeSelectProps } from "antd";
import { useMemo } from "react";
import type { Menu } from "@/types";
import type { PaginatedResult } from "../common";
import * as api from "./api";
import { menuQueryKeys } from "./keys";

/**
 * 获取菜单列表
 */
export function useMenuList(
  params?: Menu.Query,
  options?: Omit<UseQueryOptions<PaginatedResult<Menu.Item>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: menuQueryKeys.list(params),
    queryFn: () => api.getList(params),
    ...options,
  });
}

/**
 * 获取菜单树
 */
export function useMenuTree(
  status?: Menu.Status,
  options?: Omit<UseQueryOptions<Menu.TreeItem[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: menuQueryKeys.tree(status),
    queryFn: () => api.getTree(status),
    staleTime: 5 * 60 * 1000, // 5分钟缓存
    ...options,
  });
}

/**
 * 获取菜单详情
 */
export function useMenuDetail(
  id: number,
  options?: Omit<UseQueryOptions<Menu.Item>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: menuQueryKeys.detail(id),
    queryFn: () => api.getDetail(id),
    enabled: id > 0,
    ...options,
  });
}

/**
 * 获取菜单树（用于 TreeSelect 组件）
 * 自动转换为 TreeSelect 需要的数据格式
 */
export function useMenuTreeSelect(status?: Menu.Status) {
  const { data: tree = [], ...rest } = useMenuTree(status);

  const treeData = useMemo((): TreeSelectProps["treeData"] => {
    const loop = (nodes: Menu.TreeItem[]): TreeSelectProps["treeData"] =>
      nodes.map((n) => ({
        title: n.name,
        value: n.id,
        children: n.children?.length ? loop(n.children) : undefined,
      }));
    return loop(tree);
  }, [tree]);

  return {
    ...rest,
    tree,
    treeData,
  };
}

/** 菜单树节点类型（用于 Ant Design Tree 组件） */
interface MenuTreeNode {
  title: string;
  key: number;
  children?: MenuTreeNode[];
}

/**
 * 获取带 checkbox 的菜单树（用于角色权限分配）
 */
export function useMenuTreeForPermission() {
  const { data: tree = [], ...rest } = useMenuTree("enabled");

  /** 转换为 Tree 组件需要的 treeData */
  const treeData = useMemo(() => {
    const loop = (nodes: Menu.TreeItem[]): MenuTreeNode[] =>
      nodes.map((n) => ({
        title: n.name,
        key: n.id,
        children: n.children?.length ? loop(n.children) : undefined,
      }));
    return loop(tree);
  }, [tree]);

  /** 所有菜单 ID（用于全选） */
  const allMenuIds = useMemo(() => {
    const ids: number[] = [];
    const collect = (nodes: Menu.TreeItem[]) => {
      nodes.forEach((n) => {
        ids.push(n.id);
        if (n.children?.length) collect(n.children);
      });
    };
    collect(tree);
    return ids;
  }, [tree]);

  return {
    ...rest,
    tree,
    treeData,
    allMenuIds,
  };
}
