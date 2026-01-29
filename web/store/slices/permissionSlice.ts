import { createSlice, type PayloadAction } from "@reduxjs/toolkit";
import type { Menu, Role } from "@/types";

interface PermissionState {
  codes: string[];
  isSuperAdmin: boolean;
}

const initialState: PermissionState = {
  codes: [],
  isSuperAdmin: false,
};

const permissionSlice = createSlice({
  name: "permission",
  initialState,
  reducers: {
    setPermissions: (
      state,
      action: PayloadAction<{ menus: Menu.Item[]; roles: Role.Option[] }>
    ) => {
      const { menus, roles } = action.payload;

      // 检查是否是超级管理员
      state.isSuperAdmin = roles.some((r) => r.code === "superadmin");

      // 收集 button 类型菜单的权限码
      const codesSet = new Set<string>();
      menus.forEach((m) => {
        if (m.type === "button" && m.permission_code) {
          codesSet.add(m.permission_code);
        }
      });
      state.codes = Array.from(codesSet);
    },
    clearPermissions: (state) => {
      state.codes = [];
      state.isSuperAdmin = false;
    },
  },
});

export const { setPermissions, clearPermissions } = permissionSlice.actions;
export default permissionSlice.reducer;

// Selectors
export const selectIsSuperAdmin = (state: { permission: PermissionState }) =>
  state.permission.isSuperAdmin;

export const selectPermissionCodes = (state: { permission: PermissionState }) =>
  state.permission.codes;

// 权限检查 selector (使用 Set 优化查找)
const createHasPermissionSelector = () => {
  let cachedCodes: string[] = [];
  let cachedSet: Set<string> = new Set();

  return (state: { permission: PermissionState }, code: string): boolean => {
    const { isSuperAdmin, codes } = state.permission;

    // 超级管理员拥有所有权限
    if (isSuperAdmin) return true;

    // 缓存 Set 以避免重复创建
    if (codes !== cachedCodes) {
      cachedCodes = codes;
      cachedSet = new Set(codes);
    }

    return cachedSet.has(code);
  };
};

export const selectHasPermission = createHasPermissionSelector();
