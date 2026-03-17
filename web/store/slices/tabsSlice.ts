import { createSlice, type PayloadAction } from "@reduxjs/toolkit";

export const HOME_TAB = {
  key: "/home",
  title: "首页",
} as const;

export interface TabItem {
  key: string;
  title: string;
}

interface TabsState {
  tabs: TabItem[];
  activeKey: string;
}

const initialState: TabsState = {
  tabs: [HOME_TAB],
  activeKey: HOME_TAB.key,
};

const tabsSlice = createSlice({
  name: "tabs",
  initialState,
  reducers: {
    addTab: (state, action: PayloadAction<TabItem>) => {
      const exists = state.tabs.find((t) => t.key === action.payload.key);
      if (!exists) {
        state.tabs.push(action.payload);
      }
      state.activeKey = action.payload.key;
    },
    removeTab: (state, action: PayloadAction<string>) => {
      const key = action.payload;
      // 不能移除首页
      if (key === HOME_TAB.key) return;

      const index = state.tabs.findIndex((t) => t.key === key);
      if (index === -1) return;

      const newTabs = state.tabs.filter((t) => t.key !== key);

      // 如果关闭的是当前激活的 tab，切换到相邻 tab
      if (state.activeKey === key && newTabs.length > 0) {
        const newIndex = Math.max(0, Math.min(index, newTabs.length - 1));
        state.activeKey = newTabs[newIndex]?.key ?? HOME_TAB.key;
      }

      state.tabs = newTabs;
    },
    setActiveKey: (state, action: PayloadAction<string>) => {
      state.activeKey = action.payload;
    },
    clearTabs: (state) => {
      state.tabs = [HOME_TAB];
      state.activeKey = HOME_TAB.key;
    },
    setTabsState: (state, action: PayloadAction<{ tabs: TabItem[]; activeKey: string }>) => {
      state.tabs = action.payload.tabs;
      state.activeKey = action.payload.activeKey;
    },
  },
});

export const { addTab, removeTab, setActiveKey, clearTabs, setTabsState } = tabsSlice.actions;
export default tabsSlice.reducer;

// Selectors
export const selectTabs = (state: { tabs: TabsState }) => state.tabs.tabs;
export const selectActiveKey = (state: { tabs: TabsState }) => state.tabs.activeKey;
