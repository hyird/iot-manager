import { createAsyncThunk, createSlice, type PayloadAction } from "@reduxjs/toolkit";
import type { Auth } from "@/types";

interface AuthState {
  token: string | null;
  refreshToken: string | null;
  user: Auth.UserInfo | null;
}

const initialState: AuthState = {
  token: null,
  refreshToken: null,
  user: null,
};

// 刷新 Access Token
export const refreshAccessToken = createAsyncThunk<
  { token: string; refreshToken: string; user: Auth.UserInfo },
  void,
  { state: { auth: AuthState }; rejectValue: string }
>("auth/refreshAccessToken", async (_, { getState, rejectWithValue }) => {
  const { refreshToken: currentRefreshToken } = getState().auth;

  if (!currentRefreshToken) {
    return rejectWithValue("未找到刷新令牌");
  }

  try {
    const response = await fetch("/api/auth/refresh", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ refreshToken: currentRefreshToken }),
    });

    if (!response.ok) {
      return rejectWithValue("刷新令牌失败");
    }

    const data = await response.json();
    const { token, refreshToken } = data.data;

    // 获取最新用户信息
    const userResponse = await fetch("/api/auth/me", {
      headers: { Authorization: `Bearer ${token}` },
    });

    const userData = await userResponse.json();

    return {
      token,
      refreshToken,
      user: userData.data,
    };
  } catch {
    return rejectWithValue("刷新令牌失败");
  }
});

const authSlice = createSlice({
  name: "auth",
  initialState,
  reducers: {
    setAuth: (
      state,
      action: PayloadAction<{
        token: string;
        refreshToken: string;
        user: Auth.UserInfo;
      }>
    ) => {
      state.token = action.payload.token;
      state.refreshToken = action.payload.refreshToken;
      state.user = action.payload.user;
    },
    clearAuth: (state) => {
      state.token = null;
      state.refreshToken = null;
      state.user = null;
    },
    setUser: (state, action: PayloadAction<Auth.UserInfo>) => {
      state.user = action.payload;
    },
  },
  extraReducers: (builder) => {
    builder
      .addCase(refreshAccessToken.fulfilled, (state, action) => {
        state.token = action.payload.token;
        state.refreshToken = action.payload.refreshToken;
        state.user = action.payload.user;
      })
      .addCase(refreshAccessToken.rejected, (state) => {
        state.token = null;
        state.refreshToken = null;
        state.user = null;
      });
  },
});

export const { setAuth, clearAuth, setUser } = authSlice.actions;
export default authSlice.reducer;

// Selectors
export const selectToken = (state: { auth: AuthState }) => state.auth.token;
export const selectUser = (state: { auth: AuthState }) => state.auth.user;
export const selectRefreshToken = (state: { auth: AuthState }) => state.auth.refreshToken;
