import { createListenerMiddleware } from "@reduxjs/toolkit";
import { PURGE } from "redux-persist";
import type { AppDispatch, RootState } from "../index";
import { clearAuth } from "../slices/authSlice";
import { clearTabs } from "../slices/tabsSlice";

export const listenerMiddleware = createListenerMiddleware();

const startAppListening = listenerMiddleware.startListening.withTypes<RootState, AppDispatch>();

// 当清除认证时，同时清除标签页和持久化存储
startAppListening({
  actionCreator: clearAuth,
  effect: async (_, listenerApi) => {
    listenerApi.dispatch(clearTabs());
    // 清除 tabs 的持久化存储，防止刷新后恢复
    listenerApi.dispatch({ type: PURGE, key: "tabs", result: () => {} });
  },
});
