import { createAsyncThunk, createSlice, type PayloadAction } from "@reduxjs/toolkit";
import type { Auth } from "@/types";
import { fetchCurrentUser, refreshToken as refreshAuthToken } from "@/services/auth/api";

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
    const { token, refreshToken } = await refreshAuthToken(currentRefreshToken, {
      _silent: true,
    });

    const user = await fetchCurrentUser({
      _silent: true,
      headers: { Authorization: `Bearer ${token}` },
    });

    return {
      token,
      refreshToken,
      user,
    };
  } catch (error) {
    return rejectWithValue(error instanceof Error ? error.message : "刷新令牌失败");
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
