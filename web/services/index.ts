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

// Alert
export * from "./alert";
// Auth
export * from "./auth";
// Department
export * from "./department";
// Device
export * from "./device";
// Device Group
export * from "./device-group";
// Home
export * from "./home";
// Link
export * from "./link";

// Menu
export * from "./menu";
// Protocol
export * from "./protocol";
// Role
export * from "./role";
// User
export * from "./user";
