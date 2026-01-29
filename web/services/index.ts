/**
 * Service 层统一导出
 *
 * 架构说明:
 * - api.ts:       API 端点定义 + 请求函数
 * - keys.ts:      React Query keys（类型安全）
 * - queries.ts:   useQuery hooks
 * - mutations.ts: useMutation hooks
 * - types.ts:     模块特有类型（可选）
 * - index.ts:     模块导出
 */

// Auth
export * from "./auth";

// Home
export * from "./home";

// User
export * from "./user";

// Role
export * from "./role";

// Department
export * from "./department";

// Menu
export * from "./menu";

// Link
export * from "./link";

// Protocol
export * from "./protocol";

// Device
export * from "./device";
