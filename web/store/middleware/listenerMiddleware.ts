import { createListenerMiddleware } from "@reduxjs/toolkit";
import { PURGE } from "redux-persist";
import { clearAuth } from "../slices/authSlice";
import { clearPermissions } from "../slices/permissionSlice";
import { clearTabs } from "../slices/tabsSlice";
import type { RootState, AppDispatch } from "../index";

export const listenerMiddleware = createListenerMiddleware();

const startAppListening = listenerMiddleware.startListening.withTypes<RootState, AppDispatch>();

// 当清除认证时，同时清除权限、标签页和持久化存储
startAppListening({
  actionCreator: clearAuth,
  effect: async (_, listenerApi) => {
    listenerApi.dispatch(clearPermissions());
    listenerApi.dispatch(clearTabs());
    // 清除 tabs 的持久化存储，防止刷新后恢复
    listenerApi.dispatch({ type: PURGE, key: "tabs", result: () => {} });
  },
});
