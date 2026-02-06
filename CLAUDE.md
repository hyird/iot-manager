# IoT Manager - Claude Code 项目指南

## 语言要求

- 使用中文回答
- 使用中文思考
- Compact 总结也要使用中文

## 项目概况

IoT 设备管理平台：C++20 后端 (Drogon) + React 19 前端 (Vite + Bun)。
支持 SL651/Modbus 协议，TCP 链路管理，设备数据采集与监控。

## 构建与验证

### 前端

```bash
cd /d/WorkSpace/iot-manager && bun install    # 安装依赖
cd /d/WorkSpace/iot-manager && bun run build  # TypeScript 检查 + Vite 构建
cd /d/WorkSpace/iot-manager && bun run check  # Biome lint + format 检查并修复
```

### 后端

**不要**通过 Agent 编译后端。后端需要 vcpkg + CMake 环境，仅在 CI 或本地手动编译。

### CI

GitHub Actions 矩阵构建：Ubuntu + Windows × Debug + Release，编译器为 LLVM/Clang 18。

## 项目结构

```
server/                     # C++20 后端
├── main.cpp
├── pch.hpp                 # 预编译头
├── common/                 # 公共基础设施
│   ├── domain/            # Aggregate, DomainEvent, EventBus
│   ├── database/          # DatabaseService, RedisService, TransactionGuard
│   ├── cache/             # AuthCache, DeviceCache, ResourceVersion
│   ├── filters/           # AuthFilter, PermissionFilter, RequestLogFilter
│   ├── network/           # TcpLinkManager
│   ├── protocol/          # ProtocolDispatcher, SL651 协议
│   └── utils/             # AppException, Response, ValidatorHelper, JwtUtils
└── modules/               # 业务模块 (DDD 聚合根)
    ├── system/            # Auth, User, Role, Menu, Department
    ├── device/            # Device 聚合根, CommandRepository
    ├── link/              # Link 聚合根, LinkEventHandlers
    ├── protocol/          # ProtocolConfig
    └── home/              # 首页统计

web/                        # React 19 前端
├── main.tsx
├── components/            # 共享 UI 组件
├── config/                # 页面/权限注册中心 (registry.ts)
├── hooks/                 # usePermission, useDynamicRoutes, useInitAuth
├── layouts/               # AdminLayout
├── pages/                 # 页面模块 (device, link, protocol, system)
├── providers/             # TanStack Query 配置
├── routes/                # 动态路由生成
├── services/              # API 层 (api.ts, keys.ts, queries.ts, mutations.ts)
├── store/                 # Redux (authSlice, tabsSlice)
├── types/                 # TypeScript 类型定义
├── utils/                 # 工具函数
└── styles/                # Tailwind + Ant Design 样式
```

## 架构设计

### 后端 - DDD 聚合根模式

**分层职责：**
- **Controller**: 参数校验 + 权限检查 + ETag，不含业务逻辑
- **Service**: 业务流程协调（可选，简单 CRUD 直接用聚合根）
- **Domain (Aggregate)**: 核心业务逻辑，流式 API + 声明式约束

**聚合根流式 API：**
```cpp
co_await User::of(id)
    .require(User::notBuiltinAdmin)
    .require(User::notSelf(currentUserId))
    .update(data)
    .withRoles(roleIds)
    .save();  // 自动：约束检查 → 事务 → 持久化 → 发布事件
```

**事件总线解耦：**
- `EventBus::instance().publish(event)` 发布领域事件
- `LinkEventHandlers::registerAll()` 订阅事件，解耦 TcpLinkManager
- EventBus 内置副作用：自动清缓存、递增 ResourceVersion

**CommandRepository：** DB 写操作从 ProtocolDispatcher 提取到 CommandRepository，避免循环依赖。

### 前端 - 状态管理分层

- **Redux Toolkit**: 仅管理客户端状态（auth token、标签页），通过 redux-persist 持久化
- **TanStack Query**: 管理所有服务端状态，useQuery/useMutation hooks 封装
- **权限**: `useCurrentUser` query → `usePermission` hook 派生，不用额外 Redux slice

**Service 模块标准结构：**
```
services/MODULE/
├── api.ts          # ENDPOINTS 常量 + 请求函数
├── keys.ts         # QueryKey 工厂 (createQueryKeys)
├── queries.ts      # useQuery hooks
├── mutations.ts    # useMutation hooks (useMutationWithFeedback)
└── index.ts        # 统一导出
```

**页面注册：** 在 `pages/index.ts` 调用 `registerPage()` 注册组件、权限、懒加载。

## 后端编码规范 (C++)

### 命名

| 类型 | 规范 | 示例 |
|------|------|------|
| 类/枚举/类型 | PascalCase | `UserController`, `ValidationException` |
| 公有方法 | camelCase | `execute()`, `isValid()`, `getMessage()` |
| 私有方法 | camelCase（无前缀） | `persistCreate()`, `markDirty()` |
| 成员变量 | 后缀下划线 | `id_`, `username_`, `loaded_` |
| 局部变量 | camelCase | `auto result`, `std::string finalSql` |
| 常量 | UPPER_SNAKE_CASE | `CACHE_TTL_USER_SESSION`, `PASSWORD_MIN_LENGTH` |
| 文件名 | Module.Type.hpp | `User.Controller.hpp`, `Link.Service.hpp` |

### 头文件

- 使用 `#pragma once`
- 包含顺序：标准库 → Drogon/Trantor → 第三方库 → 项目头文件（空行分隔）
- 预编译头 `pch.hpp` 仅含稳定的标准库和框架头

### 异常处理

```
AppException (基类: code + message + HttpStatusCode)
├── NotFoundException      (1001, 404)
├── ValidationException    (1002, 400)
├── ForbiddenException     (1003, 403)
└── namespace AuthException (2xxx 系列)
```

- 错误码：1xxx 通用错误，2xxx 认证错误，5xxx 服务器错误
- 429 需显式传 `k429TooManyRequests` 参数
- 全局异常处理器 `RequestAdvices` 统一捕获并转换为 JSON 响应

### 响应格式

统一使用 `Response` 类：`ok()`, `page()`, `created()`, `updated()`, `deleted()`, `error()`。
JSON 格式：`{ code: 0, message: "...", data: ... }`

### 协程

- 所有异步操作使用 `Task<T>` + `co_await` / `co_return`
- Controller 方法签名：`Task<HttpResponsePtr> method(HttpRequestPtr req)`
- 聚合根的 `save()` 自动管理事务和事件发布

### 数据库

- 使用 `DatabaseService` 封装 Drogon ORM (`execSqlCoro`)
- 事务用 `TransactionGuard` (RAII，析构自动回滚)
- SQL 参数通过 `?` 占位符 + `buildSql()` 绑定（防注入）

## 前端编码规范 (TypeScript/React)

### Biome 格式化

- 缩进：2 空格，LF 行尾，行宽 100
- 双引号，总是分号，尾逗号 (ES5)，箭头函数总是括号
- `useConst: error`, `noUnusedVariables: error`, `noUnusedImports: error`
- `noExplicitAny: warn`（避免 any，用 unknown + 类型守卫）

### 命名

| 类型 | 规范 | 示例 |
|------|------|------|
| 组件/文件 | PascalCase | `DeviceCard.tsx`, `PageTabs.tsx` |
| Hook | camelCase + use 前缀 | `usePermission()`, `useDynamicRoutes()` |
| API 函数 | camelCase 动词 | `getDetail()`, `create()`, `update()` |
| 类型/接口 | PascalCase | `DeviceCardProps`, `PageResult<T>` |
| 常量 | UPPER_SNAKE_CASE | `ENDPOINTS`, `HOME_TAB` |
| 局部变量 | camelCase | `const handleTabChange` |

### 类型定义

- Props 和 API 响应用 `interface`
- 联合类型、字面量用 `type`
- 命名空间导出：`User.Item`, `Auth.LoginRequest`, `Device.CreateDto`
- 路径别名：`@/` 映射到 `web/`

### 组件模式

- 全部函数组件，不用 class 组件
- 权限检查：组件内调用 `usePermission("iot:device:edit")`
- 表单弹窗：独立为 `XxxFormModal.tsx`
- 页面容器：`<PageContainer>` 包裹

### API 调用

- Axios 实例 (`services/common/request.ts`) 自动附加 Bearer token
- 401 自动刷新 token（单队列），刷新失败清 auth 跳登录
- `useMutationWithFeedback` 统一处理成功提示 + 缓存失效

### UI 技术栈

- **Ant Design v6** + zhCN 本地化
- **Tailwind CSS v4** 实用工具类
- **Framer Motion** 页面过渡动画
- 不使用 CSS Modules，样式全在 Tailwind + Ant Design

## 技术栈速查

| 层 | 技术 |
|------|------|
| 后端框架 | Drogon (C++20 协程) |
| 数据库 | PostgreSQL |
| 缓存 | Redis (hiredis) |
| 协议 | SL651, Modbus (libmodbus) |
| 前端框架 | React 19 + TypeScript 5.9 |
| 构建工具 | Vite 7 + Bun |
| UI 库 | Ant Design 6 + Tailwind CSS 4 |
| 状态管理 | Redux Toolkit + TanStack Query 5 |
| 路由 | React Router 7 (Hash Router) |
| 代码质量 | Biome (lint + format) |
| 包管理 | vcpkg (后端) + Bun (前端) |
| 编译器 | LLVM/Clang 18 (Linux) + ClangCL (Windows) |

## 注意事项

- 前端修改后运行 `bun run build` 验证构建
- 删除 Redux slice 需同时清理 store/index、slices/index、hooks、middleware
- `initialData` 用函数形式 `() => store.getState()...` 避免订阅
- 含 JSX 的函数不适合提取到纯 utils
- 新增业务模块：后端创建 `modules/xxx/` (Controller + Service + domain/)，前端创建 `services/xxx/` + `pages/xxx/` 并在 `pages/index.ts` 注册
- `DatabaseInitializer.hpp` 只保留最终表结构（CREATE TABLE），不写迁移代码（ALTER/UPDATE/DROP），数据库变更由开发者手动重建
