import { useDispatch, useSelector } from "react-redux";
import type { TypedUseSelectorHook } from "react-redux";
import type { RootState, AppDispatch } from "./index";
import { store } from "./index";
import type { Auth, Menu, Role } from "@/types";

import {
  selectToken,
  selectUser,
  selectIsValidating,
  setAuth,
  clearAuth,
  refreshUser,
  refreshAccessToken,
} from "./slices/authSlice";

import {
  selectTabs,
  selectActiveKey,
  addTab,
  removeTab,
  setActiveKey,
  clearTabs,
  setTabsState,
  HOME_TAB,
  type TabItem,
} from "./slices/tabsSlice";

import {
  selectIsSuperAdmin,
  selectHasPermission,
  setPermissions,
  clearPermissions,
} from "./slices/permissionSlice";

// 类型安全的 hooks
export const useAppDispatch: () => AppDispatch = useDispatch;
export const useAppSelector: TypedUseSelectorHook<RootState> = useSelector;

// ============ Auth Hooks ============

export function useAuthStore() {
  const dispatch = useAppDispatch();
  const token = useAppSelector(selectToken);
  const user = useAppSelector(selectUser);
  const isValidating = useAppSelector(selectIsValidating);

  return {
    token,
    user,
    isValidating,
    setAuth: (token: string, refreshToken: string, user: Auth.UserInfo) =>
      dispatch(setAuth({ token, refreshToken, user })),
    clearAuth: () => dispatch(clearAuth()),
    refreshUser: () => dispatch(refreshUser()),
    refreshAccessToken: () => dispatch(refreshAccessToken()),
  };
}

// ============ Tabs Hooks ============

export function useTabsStore() {
  const dispatch = useAppDispatch();
  const tabs = useAppSelector(selectTabs);
  const activeKey = useAppSelector(selectActiveKey);

  return {
    tabs,
    activeKey,
    addTab: (tab: TabItem) => dispatch(addTab(tab)),
    removeTab: (key: string): string | null => {
      if (key === HOME_TAB.key) return null;

      const state = store.getState();
      const currentActiveKey = state.tabs.activeKey;
      const currentTabs = state.tabs.tabs;

      if (currentActiveKey !== key) {
        dispatch(removeTab(key));
        return null;
      }

      const index = currentTabs.findIndex((t) => t.key === key);
      const newTabs = currentTabs.filter((t) => t.key !== key);
      const newIndex = Math.min(index, newTabs.length - 1);
      const newActiveKey = newTabs[newIndex]?.key || HOME_TAB.key;

      dispatch(removeTab(key));
      return newActiveKey;
    },
    setActiveKey: (key: string) => dispatch(setActiveKey(key)),
    clearTabs: () => dispatch(clearTabs()),
    setTabsState: (tabs: TabItem[], activeKey: string) =>
      dispatch(setTabsState({ tabs, activeKey })),
  };
}

export { HOME_TAB, type TabItem };

// ============ Permission Hooks ============

export function usePermissionStore() {
  const dispatch = useAppDispatch();
  const isSuperAdmin = useAppSelector(selectIsSuperAdmin);

  return {
    isSuperAdmin,
    setPermissions: (menus: Menu.Item[], roles: Role.Option[]) =>
      dispatch(setPermissions({ menus, roles })),
    clearPermissions: () => dispatch(clearPermissions()),
    hasPermission: (code: string) => {
      const state = store.getState();
      return selectHasPermission(state, code);
    },
  };
}

export function usePermission(code?: string): boolean {
  const isSuperAdmin = useAppSelector(selectIsSuperAdmin);
  const codes = useAppSelector((state) => state.permission.codes);

  if (!code) return true;
  if (isSuperAdmin) return true;
  return codes.includes(code);
}
