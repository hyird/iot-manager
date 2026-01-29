/**
 * 统一注册中心
 * 集中管理页面组件和权限标识的注册
 */

import type { ComponentType } from "react";

// ============================================================
// 类型定义
// ============================================================

export interface PageConfig {
  /**
   * 组件标识 (对应后端菜单的 component 字段)
   */
  component: string;

  /**
   * 页面名称
   */
  name: string;

  /**
   * 页面描述
   */
  description?: string;

  /**
   * 所属模块
   */
  module: string;

  /**
   * 组件导入函数 (懒加载)
   */
  loader: () => Promise<{ default: ComponentType<unknown> }>;

  /**
   * 页面权限列表
   */
  permissions?: Omit<PermissionConfig, "module" | "resource">[];
}

export interface PermissionConfig {
  /**
   * 权限标识 (对应后端菜单按钮的 permissionCode 字段)
   * 格式: module:resource:action
   * 例如: system:user:add
   */
  code: string;

  /**
   * 权限名称
   */
  name: string;

  /**
   * 权限描述
   */
  description?: string;

  /**
   * 所属模块
   */
  module: string;

  /**
   * 所属资源
   */
  resource: string;

  /**
   * 操作类型
   */
  action: "query" | "add" | "edit" | "delete" | "perm" | "export" | "import" | string;
}

/**
 * 模块注册配置
 * 统一注册一个模块的所有页面和权限
 */
export interface ModuleConfig {
  /**
   * 模块名称
   */
  name: string;

  /**
   * 页面列表 (module 字段会自动填充)
   */
  pages: Omit<PageConfig, "module">[];
}

// ============================================================
// 注册表
// ============================================================

const pageRegistry: PageConfig[] = [];
const permissionRegistry: PermissionConfig[] = [];

// ============================================================
// 页面注册 API
// ============================================================

export function registerPage(config: PageConfig): void {
  const exists = pageRegistry.find((p) => p.component === config.component);
  if (exists) {
    console.warn(`Page component "${config.component}" already registered`);
    return;
  }
  pageRegistry.push(config);
}

export function registerPages(...configs: PageConfig[]): void {
  configs.forEach(registerPage);
}

export function getRegisteredPages(): PageConfig[] {
  return [...pageRegistry];
}

export function getPagesByModule(module: string): PageConfig[] {
  return pageRegistry.filter((p) => p.module === module);
}

export function getPageConfig(component: string): PageConfig | undefined {
  return pageRegistry.find((p) => p.component === component);
}

export function getComponentLoaderMap(): Record<
  string,
  () => Promise<{ default: ComponentType<unknown> }>
> {
  return Object.fromEntries(pageRegistry.map((page) => [page.component, page.loader]));
}

// ============================================================
// 权限注册 API
// ============================================================

export function registerPermission(config: PermissionConfig): void {
  const exists = permissionRegistry.find((p) => p.code === config.code);
  if (exists) {
    console.warn(`Permission "${config.code}" already registered`);
    return;
  }
  permissionRegistry.push(config);
}

export function registerPermissions(...configs: PermissionConfig[]): void {
  configs.forEach(registerPermission);
}

export function getRegisteredPermissions(): PermissionConfig[] {
  return [...permissionRegistry];
}

export function getPermissionsByModule(module: string): PermissionConfig[] {
  return permissionRegistry.filter((p) => p.module === module);
}

export function getPermissionsByResource(resource: string): PermissionConfig[] {
  return permissionRegistry.filter((p) => p.resource === resource);
}

export function getPermissionConfig(code: string): PermissionConfig | undefined {
  return permissionRegistry.find((p) => p.code === code);
}

// ============================================================
// 统一注册 API
// ============================================================

/**
 * 注册整个模块 (页面 + 权限)
 */
export function registerModule(config: ModuleConfig): void {
  let totalPermissions = 0;

  // 注册页面及其权限
  config.pages.forEach((page) => {
    // 注册页面
    registerPage({ ...page, module: config.name });

    // 注册页面的权限
    if (page.permissions) {
      page.permissions.forEach((permission) => {
        registerPermission({
          ...permission,
          module: config.name,
          resource: page.name, // 自动填充资源为页面名称
        });
        totalPermissions++;
      });
    }
  });

  if (import.meta.env.DEV) {
    // eslint-disable-next-line no-console
    console.log(
      `✅ Module "${config.name}" registered: ${config.pages.length} pages, ${totalPermissions} permissions`
    );
  }
}
