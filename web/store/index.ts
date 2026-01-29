import { configureStore, combineReducers } from "@reduxjs/toolkit";
import {
  persistStore,
  persistReducer,
  createTransform,
  FLUSH,
  REHYDRATE,
  PAUSE,
  PERSIST,
  PURGE,
  REGISTER,
} from "redux-persist";
import storage from "redux-persist/lib/storage";

import authReducer from "./slices/authSlice";
import tabsReducer, { HOME_TAB, type TabItem } from "./slices/tabsSlice";
import permissionReducer from "./slices/permissionSlice";
import { listenerMiddleware } from "./middleware/listenerMiddleware";

// Tabs 转换器：确保 HOME_TAB 始终存在
const tabsTransform = createTransform(
  (inboundState: { tabs: TabItem[]; activeKey: string }) => inboundState,
  (outboundState: { tabs: TabItem[]; activeKey: string }) => {
    const hasHome = outboundState.tabs?.some((t) => t.key === HOME_TAB.key);
    if (!hasHome) {
      outboundState.tabs = [HOME_TAB, ...(outboundState.tabs || [])];
    }
    if (!outboundState.activeKey) {
      outboundState.activeKey = HOME_TAB.key;
    }
    return outboundState;
  },
  { whitelist: ["tabs"] }
);

// Auth 持久化配置
const authPersistConfig = {
  key: "auth",
  storage,
  whitelist: ["token", "refreshToken", "user"],
};

// Tabs 持久化配置
const tabsPersistConfig = {
  key: "tabs",
  storage,
  whitelist: ["tabs", "activeKey"],
  transforms: [tabsTransform],
};

const rootReducer = combineReducers({
  auth: persistReducer(authPersistConfig, authReducer),
  tabs: persistReducer(tabsPersistConfig, tabsReducer),
  permission: permissionReducer,
});

export const store = configureStore({
  reducer: rootReducer,
  middleware: (getDefaultMiddleware) =>
    getDefaultMiddleware({
      serializableCheck: {
        ignoredActions: [FLUSH, REHYDRATE, PAUSE, PERSIST, PURGE, REGISTER],
      },
    }).prepend(listenerMiddleware.middleware),
});

export const persistor = persistStore(store);

export type RootState = ReturnType<typeof store.getState>;
export type AppDispatch = typeof store.dispatch;
