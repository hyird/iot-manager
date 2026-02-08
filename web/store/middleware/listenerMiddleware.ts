import { createListenerMiddleware } from "@reduxjs/toolkit";
import { PURGE } from "redux-persist";
import { queryClient } from "@/services/common/queryClient";
import type { AppDispatch, RootState } from "../index";
import { clearAuth } from "../slices/authSlice";
import { clearTabs } from "../slices/tabsSlice";

export const listenerMiddleware = createListenerMiddleware();

const startAppListening = listenerMiddleware.startListening.withTypes<RootState, AppDispatch>();

// 当清除认证时，同时清除标签页和持久化存储
startAppListening({
  actionCreator: clearAuth,
  effect: async (_, listenerApi) => {
    // 避免退出登录或切换账号后沿用上一位用户的查询缓存
    queryClient.clear();
    listenerApi.dispatch(clearTabs());
    // 清除 tabs 的持久化存储，防止刷新后恢复
    listenerApi.dispatch({ type: PURGE, key: "tabs", result: () => {} });
  },
});
