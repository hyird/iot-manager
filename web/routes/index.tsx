import "@/pages"; // 触发页面注册
import { type ComponentType, lazy, useMemo } from "react";
import { createHashRouter, Navigate, RouterProvider } from "react-router-dom";
import { AuthGuard } from "@/components/AuthGuard";
import FallbackPage from "@/components/FallbackPage";
import { RootTransition } from "@/components/RootTransition";
import { getComponentLoaderMap } from "@/config/registry";
import { useDynamicRoutes, useInitAuth } from "@/hooks";
import type { Menu } from "@/types";

// 固定的页面
const LoginPage = lazy(() => import("@/pages/system/Login"));
const AdminLayout = lazy(() => import("@/layouts/AdminLayout"));
const HomePage = lazy(() => import("@/pages/Home"));

// 从统一注册中心获取组件映射
const componentMap: Record<string, () => Promise<{ default: ComponentType<unknown> }>> =
  getComponentLoaderMap();

// 懒加载缓存
const lazyCache = new Map<string, ComponentType<unknown>>();

/**
 * 获取懒加载组件
 */
function getLazyComponent(name: string): ComponentType<unknown> | null {
  if (!componentMap[name]) {
    return null;
  }

  if (!lazyCache.has(name)) {
    lazyCache.set(name, lazy(componentMap[name]));
  }
  return lazyCache.get(name)!;
}

/**
 * 获取动态路由组件
 */
function getRouteComponent(menu: Menu.Item): React.ReactNode {
  if (menu.component) {
    const Component = getLazyComponent(menu.component);
    if (Component) {
      return <Component />;
    }
  }
  return <FallbackPage menu={menu} />;
}

export function AppRoutes() {
  useInitAuth();
  const { pageMenus } = useDynamicRoutes();
  const router = useMemo(
    () =>
      createHashRouter([
        {
          element: <RootTransition />,
          children: [
            {
              path: "/login",
              element: <LoginPage />,
            },
            {
              element: <AuthGuard />,
              children: [
                {
                  path: "/",
                  element: <AdminLayout />,
                  children: [
                    {
                      index: true,
                      element: <Navigate to="/home" replace />,
                    },
                    {
                      path: "home",
                      element: <HomePage />,
                    },
                    ...pageMenus.map((menu) => ({
                      path: menu.path!.trim().replace(/^\//, ""),
                      element: getRouteComponent(menu),
                    })),
                  ],
                },
              ],
            },
            {
              path: "*",
              element: <Navigate to="/" replace />,
            },
          ],
        },
      ]),
    [pageMenus]
  );

  return <RouterProvider router={router} />;
}
