/**
 * QueryKey 工厂函数
 * 提供类型安全的 QueryKey 生成
 */

/**
 * 创建模块级别的 QueryKey 工厂
 * @param module 模块名称
 */
export function createQueryKeys<T extends string>(module: T) {
  return {
    /** 模块根 key */
    all: [module] as const,

    /** 列表 key */
    lists: () => [module, "list"] as const,

    /** 带参数的列表 key */
    list: <P extends Record<string, unknown>>(params: P) => [module, "list", params] as const,

    /** 详情 key */
    details: () => [module, "detail"] as const,

    /** 单个详情 key */
    detail: (id: number | string) => [module, "detail", id] as const,

    /** 树形数据 key */
    trees: () => [module, "tree"] as const,

    /** 带参数的树形数据 key */
    tree: <P extends Record<string, unknown>>(params?: P) => [module, "tree", params] as const,

    /** 选项列表 key (用于下拉框) */
    options: () => [module, "options"] as const,
  };
}
