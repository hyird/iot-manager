/**
 * 通用树构建工具
 */

/** 树节点基础接口 */
export interface TreeNode {
  id: number;
  parent_id?: number | null;
  children?: TreeNode[];
}

/** 构建树的选项 */
export interface BuildTreeOptions {
  /** 排序字段 */
  sortBy?: "order" | "id" | "code" | "name";
  /** 是否移除空的 children 数组 */
  removeEmptyChildren?: boolean;
  /** 自定义比较函数 */
  compareFn?: <T>(a: T, b: T) => number;
}

/**
 * 将平铺列表构建成树形结构
 * @param items 平铺的数据列表
 * @param options 配置选项
 */
export function buildTree<T extends TreeNode>(
  items: T[],
  options: BuildTreeOptions = {}
): (T & { children?: T[] })[] {
  const { sortBy = "order", removeEmptyChildren = true, compareFn } = options;

  const map = new Map<number, T & { children: T[] }>();
  const roots: (T & { children: T[] })[] = [];

  // 创建所有节点的映射
  items.forEach((item) => {
    map.set(item.id, { ...item, children: [] });
  });

  // 构建父子关系
  items.forEach((item) => {
    const node = map.get(item.id)!;
    if (item.parent_id && map.has(item.parent_id)) {
      map.get(item.parent_id)!.children.push(node);
    } else {
      roots.push(node);
    }
  });

  // 排序函数
  const defaultCompareFn = (a: T, b: T): number => {
    const aRecord = a as unknown as Record<string, unknown>;
    const bRecord = b as unknown as Record<string, unknown>;
    if (sortBy === "order") {
      return ((aRecord.order as number) ?? 0) - ((bRecord.order as number) ?? 0);
    }
    if (sortBy === "id") {
      return a.id - b.id;
    }
    if (sortBy === "code") {
      return ((aRecord.code as string) ?? "").localeCompare((bRecord.code as string) ?? "");
    }
    if (sortBy === "name") {
      return ((aRecord.name as string) ?? "").localeCompare((bRecord.name as string) ?? "");
    }
    return 0;
  };

  const sortFn = compareFn ?? defaultCompareFn;

  // 递归排序并清理空 children
  const sortTree = (nodes: (T & { children: T[] })[]): (T & { children?: T[] })[] => {
    return nodes.sort(sortFn).map((n) => {
      const result: T & { children?: T[] } = { ...n };
      if (n.children.length > 0) {
        result.children = sortTree(n.children as (T & { children: T[] })[]);
      } else if (removeEmptyChildren) {
        delete result.children;
      } else {
        result.children = [];
      }
      return result;
    });
  };

  return sortTree(roots);
}

/**
 * 将树形结构平铺为列表
 * @param tree 树形数据
 */
export function flattenTree<T extends TreeNode>(tree: T[]): T[] {
  const result: T[] = [];

  const traverse = (nodes: T[]) => {
    nodes.forEach((node) => {
      const { children, ...rest } = node as T & { children?: TreeNode[] };
      result.push(rest as T);
      if (children && children.length > 0) {
        traverse(children as T[]);
      }
    });
  };

  traverse(tree);
  return result;
}

/**
 * 将树形结构转为 id -> node 的映射
 * @param tree 树形数据
 */
export function treeToMap<T extends TreeNode>(tree: T[]): Map<number, T> {
  const map = new Map<number, T>();

  const traverse = (nodes: T[]) => {
    nodes.forEach((node) => {
      map.set(node.id, node);
      if (node.children && node.children.length > 0) {
        traverse(node.children as T[]);
      }
    });
  };

  traverse(tree);
  return map;
}

/**
 * 按关键词过滤树（保留匹配节点及其祖先）
 * @param tree 树形数据
 * @param keyword 关键词
 * @param matchFields 要匹配的字段名数组
 */
export function filterTree<T extends TreeNode>(
  tree: T[],
  keyword: string,
  matchFields: (keyof T)[] = ["name" as keyof T]
): T[] {
  const kw = keyword.trim().toLowerCase();
  if (!kw) return tree;

  const filter = (nodes: T[]): T[] => {
    const result: T[] = [];

    nodes.forEach((node) => {
      // 检查当前节点是否匹配
      const selfMatch = matchFields.some((field) => {
        const value = node[field];
        return typeof value === "string" && value.toLowerCase().includes(kw);
      });

      // 递归过滤子节点
      const children = node.children ? filter(node.children as T[]) : [];

      // 如果自身匹配或有匹配的子节点，保留该节点
      if (selfMatch || children.length > 0) {
        const newNode = { ...node } as T & { children?: T[] };
        if (children.length > 0) {
          newNode.children = children;
        } else {
          delete newNode.children;
        }
        result.push(newNode as T);
      }
    });

    return result;
  };

  return filter(tree);
}

/**
 * 查找树中的节点
 * @param tree 树形数据
 * @param predicate 匹配函数
 */
export function findInTree<T extends TreeNode>(
  tree: T[],
  predicate: (node: T) => boolean
): T | undefined {
  for (const node of tree) {
    if (predicate(node)) {
      return node;
    }
    if (node.children && node.children.length > 0) {
      const found = findInTree(node.children as T[], predicate);
      if (found) return found;
    }
  }
  return undefined;
}

/**
 * 获取节点的所有子孙 ID
 * @param tree 树形数据
 * @param nodeId 节点 ID
 */
export function getDescendantIds<T extends TreeNode>(tree: T[], nodeId: number): number[] {
  const ids: number[] = [];

  const collectIds = (nodes: T[]) => {
    nodes.forEach((node) => {
      ids.push(node.id);
      if (node.children && node.children.length > 0) {
        collectIds(node.children as T[]);
      }
    });
  };

  const node = findInTree(tree, (n) => n.id === nodeId);
  if (node?.children) {
    collectIds(node.children as T[]);
  }

  return ids;
}

/**
 * 获取节点的祖先路径（从根到父节点）
 * @param tree 树形数据
 * @param nodeId 节点 ID
 */
export function getAncestorPath<T extends TreeNode>(tree: T[], nodeId: number): T[] {
  const path: T[] = [];

  const find = (nodes: T[], ancestors: T[]): boolean => {
    for (const node of nodes) {
      if (node.id === nodeId) {
        path.push(...ancestors);
        return true;
      }
      if (node.children && node.children.length > 0) {
        if (find(node.children as T[], [...ancestors, node])) {
          return true;
        }
      }
    }
    return false;
  };

  find(tree, []);
  return path;
}

// ============ 菜单专用函数 ============

import type { Menu } from "../types/system";

/**
 * 将平铺的菜单列表构建成树形结构
 * @param menus 菜单列表
 * @param filterButton 是否过滤掉 button 类型，默认 true
 */
export function buildMenuTree(menus: Menu.Item[], filterButton = true): Menu.TreeItem[] {
  const items = filterButton ? menus.filter((m) => m.type !== "button") : menus;
  const tree = buildTree(items, { sortBy: "order" }) as Menu.TreeItem[];

  // 计算完整路径
  const computeFullPath = (nodes: Menu.TreeItem[], parentPath = "") => {
    for (const node of nodes) {
      if (node.path) {
        // 如果路径以 / 开头，使用绝对路径；否则拼接父路径
        node.full_path = node.path.startsWith("/") ? node.path : `${parentPath}/${node.path}`;
      } else {
        node.full_path = parentPath;
      }
      if (node.children) {
        computeFullPath(node.children, node.full_path);
      }
    }
  };

  computeFullPath(tree);
  return tree;
}

/**
 * 按关键词过滤菜单树
 * @param tree 菜单树
 * @param keyword 关键词
 */
export function filterMenuTree(tree: Menu.TreeItem[], keyword: string): Menu.TreeItem[] {
  return filterTree(tree, keyword, ["name", "path"]) as Menu.TreeItem[];
}

/**
 * 获取路径段（相对于父级的路径）
 * @param record 当前菜单项
 * @param menuMap 菜单映射表
 */
export function getPathSegment(
  record: Menu.TreeItem,
  menuMap: Map<number, Menu.TreeItem> | Record<number, Menu.TreeItem>
): string {
  const fullPath = (record.path || "").trim();
  if (!fullPath) return "";
  if (!record.parent_id) {
    return fullPath.replace(/^\/+/, "");
  }
  const parent = menuMap instanceof Map ? menuMap.get(record.parent_id) : menuMap[record.parent_id];
  const parentPath = (parent?.path || "").trim();
  if (!parentPath) {
    return fullPath.replace(/^\/+/, "");
  }
  if (fullPath.startsWith(parentPath)) {
    const seg = fullPath.slice(parentPath.length);
    return seg.replace(/^\/+/, "");
  }
  return fullPath.replace(/^\/+/, "");
}
