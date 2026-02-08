import { combineReducers, configureStore } from "@reduxjs/toolkit";
import {
  createTransform,
  FLUSH,
  PAUSE,
  PERSIST,
  PURGE,
  persistReducer,
  persistStore,
  REGISTER,
  REHYDRATE,
} from "redux-persist";
import storage from "redux-persist/lib/storage";
import sessionStorage from "redux-persist/lib/storage/session";
import { listenerMiddleware } from "./middleware/listenerMiddleware";
import authReducer from "./slices/authSlice";
import tabsReducer, { HOME_TAB, type TabItem } from "./slices/tabsSlice";

// Tabs 转换器：确保 HOME_TAB 始终存在
// createTransform 的 whitelist 指定 slice 中的子键名，
// outboundState 是该子键的值（TabItem[]），不是整个 slice
const tabsTransform = createTransform(
  (inboundState: TabItem[]) => inboundState,
  (outboundState: TabItem[]) => {
    const tabs = Array.isArray(outboundState) ? outboundState : [];
    const hasHome = tabs.some((t) => t.key === HOME_TAB.key);
    return hasHome ? tabs : [HOME_TAB, ...tabs];
  },
  { whitelist: ["tabs"] }
);

// Auth 持久化配置
const authPersistConfig = {
  key: "auth",
  storage: sessionStorage,
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
