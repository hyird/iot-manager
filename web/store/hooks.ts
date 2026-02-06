import type { TypedUseSelectorHook } from "react-redux";
import { useDispatch, useSelector } from "react-redux";
import type { Auth } from "@/types";
import type { AppDispatch, RootState } from "./index";
import { store } from "./index";

import {
  clearAuth,
  refreshAccessToken,
  selectToken,
  selectUser,
  setAuth,
} from "./slices/authSlice";

import {
  addTab,
  clearTabs,
  HOME_TAB,
  removeTab,
  selectActiveKey,
  selectTabs,
  setActiveKey,
  setTabsState,
  type TabItem,
} from "./slices/tabsSlice";

// 类型安全的 hooks
export const useAppDispatch: () => AppDispatch = useDispatch;
export const useAppSelector: TypedUseSelectorHook<RootState> = useSelector;

// ============ Auth Hooks ============

export function useAuthStore() {
  const dispatch = useAppDispatch();
  const token = useAppSelector(selectToken);
  const user = useAppSelector(selectUser);

  return {
    token,
    user,
    setAuth: (token: string, refreshToken: string, user: Auth.UserInfo) =>
      dispatch(setAuth({ token, refreshToken, user })),
    clearAuth: () => dispatch(clearAuth()),
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
