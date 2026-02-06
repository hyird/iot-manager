import { useMemo } from "react";
import { useAuthStore } from "@/store/hooks";
import type { Menu } from "@/types";

interface DynamicRoutesResult {
  pageMenus: Menu.Item[];
  defaultPath: string;
  isLoading: boolean;
}

/**
 * 动态路由 Hook
 * 从用户菜单中提取页面路由信息,并缓存结果
 */
export function useDynamicRoutes(): DynamicRoutesResult {
  const { user, token } = useAuthStore();

  // 使用 useMemo 缓存计算结果,仅在 user.menus 变化时重新计算
  const { pageMenus, defaultPath } = useMemo(() => {
    const menus: Menu.Item[] = user?.menus ?? [];

    // 过滤出所有页面类型的菜单项 (page 类型且有 component)
    const pages = menus.filter(
      (m) => m.type === "page" && m.component && m.path && m.path.trim().length > 0
    );

    // 默认打开第一个页面
    const defaultRoute = pages[0]?.path || "/dashboard";

    return {
      pageMenus: pages,
      defaultPath: defaultRoute,
    };
  }, [user?.menus]);

  const isLoading = !!token && !user;

  return {
    pageMenus,
    defaultPath,
    isLoading,
  };
}
