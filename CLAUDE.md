# 项目开发规范

## C++ 后端规范

### Lambda 捕获
- **禁止隐式捕获**：不允许使用 `[&]` 或 `[=]`
- **必须显式捕获**：明确列出需要捕获的变量，如 `[&var1, &var2]`

```cpp
// 正确
auto fn = [&setClauses, &params](const std::string& f) { ... };

// 错误
auto fn = [&](const std::string& f) { ... };
```

### 指针管理
- **禁止裸指针**：不允许使用 `new`/`delete` 管理内存
- **使用智能指针**：优先使用 `std::unique_ptr`、`std::shared_ptr`
- **使用容器**：使用 `std::vector`、`std::array` 替代动态数组
- **C API 例外**：与 C 库（如 OpenSSL）交互时，使用带自定义删除器的智能指针封装

```cpp
// 正确
std::vector<char> buffer(size);
std::unique_ptr<BIO, decltype(&BIO_free_all)> bio(BIO_new(...), BIO_free_all);

// 错误
char* buffer = new char[size];
BIO* bio = BIO_new(...);
```

### 异常参数
- 未使用的异常参数省略变量名或注释

```cpp
// 正确
catch (const std::exception&) { ... }
catch (const std::exception& /*e*/) { ... }

// 错误 (会产生警告)
catch (const std::exception& e) { ... }  // e 未使用
```

### 协程与异步
- 所有数据库操作必须使用协程 (`Task<>`, `co_await`)
- 使用 `execSqlCoro` 而非回调式 API
- Controller 方法返回 `Task<HttpResponsePtr>`

```cpp
// 正确
Task<HttpResponsePtr> list(HttpRequestPtr req) {
    auto result = co_await dbClient->execSqlCoro("SELECT ...");
    co_return Response::ok(data);
}

// 错误 - 使用回调
void list(HttpRequestPtr req, Callback callback) {
    dbClient->execSqlAsync("SELECT ...", [callback](auto result) { ... });
}
```

### 数据库客户端
- 使用 `DatabaseService` 或 `AppDbConfig::useFast()` 获取数据库客户端
- 禁止直接调用 `app().getDbClient()` 或 `app().getFastDbClient()`

```cpp
// 正确
auto dbClient = AppDbConfig::useFast()
    ? app().getFastDbClient("default")
    : app().getDbClient("default");

// 或使用 DatabaseService
DatabaseService dbService;
auto client = dbService.getClient();

// 错误
auto dbClient = app().getDbClient("default");
```

### PostgreSQL 字段命名规范
- **数据库表字段使用小写下划线命名**：如 `created_at`, `deleted_at`, `password_hash`, `parent_id`
- **SQL 语句中字段名不需要加引号**：PostgreSQL 自动转为小写，与表定义一致
- **C++ 代码中访问字段使用小写下划线**：保持与数据库字段名一致

```cpp
// 正确 - SQL 中的字段名使用小写下划线
std::string sql = R"(
    SELECT id, username, password_hash, created_at
    FROM sys_user
    WHERE deleted_at IS NULL AND id = ?
)";

// 正确 - JOIN 中的字段也使用小写下划线
std::string sql = R"(
    SELECT u.id, r.name
    FROM sys_user u
    INNER JOIN sys_user_role ur ON u.id = ur.user_id
    INNER JOIN sys_role r ON ur.role_id = r.id
    WHERE u.deleted_at IS NULL
)";

// 正确 - ORDER BY 中的字段使用小写下划线
std::string sql = R"(
    SELECT * FROM sys_menu
    WHERE status = 'enabled'
    ORDER BY sort_order ASC, id ASC
)";

// 正确 - C++ 变量名使用小写下划线访问数据库字段
int userId = row["id"].as<int>();
std::string hash = row["password_hash"].as<std::string>();

// 错误 - 使用驼峰命名会导致字段找不到
std::string sql = "SELECT deletedAt FROM sys_user";  // ❌ 字段不存在

// 错误 - C++ 中使用驼峰访问字段
std::string hash = row["passwordHash"].as<std::string>();  // ❌ 字段不存在
```

**常用字段名对照表**：
- `created_at` / `updated_at` / `deleted_at` - 时间戳字段
- `password_hash` - 密码哈希
- `parent_id` / `department_id` / `leader_id` - 关联 ID
- `user_id` / `role_id` / `menu_id` - 关联表字段
- `permission_code` - 权限代码
- `is_default` - 布尔字段
- `sort_order` - 排序字段（order 是 PostgreSQL 保留字，改用 sort_order）

### 响应格式
- 使用 `Response` 工具类返回统一格式
- 成功响应 code = 0，错误响应 code > 0

```cpp
co_return Response::ok(data);           // 成功
co_return Response::created("创建成功");  // 201
co_return Response::badRequest("参数错误"); // 400
co_return Response::notFound("未找到");    // 404
```

### 权限检查
- Controller 方法内使用 `PermissionChecker::checkPermission`
- 权限码格式: `module:resource:action`

```cpp
int userId = req->attributes()->get<int>("userId");
co_await PermissionChecker::checkPermission(userId, {"system:user:query"});
```

### 代码组织
- Controller 只负责参数校验和调用 Service
- Service 负责业务逻辑和数据库操作
- 文件命名: `Name.Controller.hpp`, `Name.Service.hpp`

### 工具类与模块化
- **main.cpp 职责**：仅负责应用启动和组装，不包含业务逻辑或工具函数
- **工具函数位置**：所有通用工具函数放在 `common/utils/` 目录
- **Filter 使用**：中间件逻辑使用 Drogon Filter，放在 `common/filters/`
- **单一职责**：每个工具类只负责一个明确的功能

```cpp
// 正确 - 工具函数在工具类中
#include "common/utils/ETagGenerator.hpp"
std::string etag = ETagGenerator::generate(content);

// 错误 - main.cpp 中定义工具函数
std::string generateETag(const std::string& content) { ... }
```

### 日志管理
- 使用 `LoggerManager` 统一管理日志
- 支持文件日志和控制台输出
- 日志格式自动美化，移除冗余信息

```cpp
// 初始化日志系统
LoggerManager::initialize("./logs/server.log");
LoggerManager::setLogLevel("DEBUG");
LoggerManager::setConsoleOutput(true);
```

### 配置管理
- 使用 `ConfigManager` 加载和管理配置
- 自动查找多个配置文件路径
- 提供类型安全的配置访问方法

```cpp
// 加载配置
ConfigManager::load();

// 获取配置项
std::string logLevel = ConfigManager::getLogLevel();
bool consoleLog = ConfigManager::isConsoleLogEnabled();
```

## TypeScript 前端规范

### React Hooks
- `useMemo` / `useCallback` 依赖项必须稳定，避免每次渲染创建新引用
- 不在组件/Hook 内部定义 class，提取到组件外部
- 使用 `ahooks` 提供的 Hooks 简化常见操作
- 避免 Maximum update depth exceeded 错误：
  - 内联对象/数组会导致无限渲染，使用 useMemo 缓存
  - Navigate 的 state 属性应使用 useMemo
  - motion 组件的 style 属性提取为常量

```typescript
// 错误 - 每次渲染创建新对象引用
<Navigate to="/login" state={{ from: location }} />
<motion.div style={{ width: "100%" }} />

// 正确 - 使用 useMemo 缓存
const redirectState = useMemo(
  () => ({ from: { pathname: location.pathname } }),
  [location.pathname]
);
<Navigate to="/login" state={redirectState} />

// 正确 - 提取为常量
const containerStyle = { width: "100%" };
<motion.div style={containerStyle} />
```

### 状态管理
- 全局状态使用 Redux Toolkit（`@reduxjs/toolkit`）
- 服务端状态使用 TanStack Query
- 组件局部状态使用 useState

```typescript
// Redux Toolkit slice
const authSlice = createSlice({
  name: "auth",
  initialState,
  reducers: {
    setAuth: (state, action) => { ... },
    clearAuth: (state) => { ... },
  },
});

// 使用 hooks
const { token, user } = useAuth();
const { setPermissions } = usePermissionStore();

// TanStack Query
const { data, isLoading } = useUserList({ page });
```

### API 请求与 Service 层
- 使用 `web/services/common/request.ts` 封装的 axios 实例
- Service 层放在 `web/services/` 目录，按模块组织
- 每个模块包含: `api.ts`（端点）、`keys.ts`（Query Keys）、`queries.ts`（useQuery hooks）、`mutations.ts`（useMutation hooks）

```typescript
// 使用 Service hooks（推荐）
import { useUserList, useUserSave } from "@/services";

const { data, isLoading } = useUserList({ page, pageSize });
const saveMutation = useUserSave();

// 直接使用 API（特殊场景）
import { userApi } from "@/services";
const users = await userApi.getList({ page });

// 错误 - 直接使用 axios
import axios from "axios";
axios.get("/api/users");
```

### 类型定义
- 所有 API 响应和请求参数必须有类型定义
- 类型文件放在 `web/types/` 目录
- 使用 `interface` 而非 `type` 定义对象类型

### 代码格式
- 使用双引号
- 语句结尾有分号
- 使用 Prettier 格式化: `bun run format`

### 组件规范
- 页面组件放在 `web/pages/` 目录
- 公共组件放在 `web/components/` 目录
- 使用函数组件 + Hooks，禁止 Class 组件
- 页面组件必须在 `web/pages/index.ts` 中注册到组件注册中心

```typescript
// web/pages/index.ts
import { registerComponent } from "@/config/registry";

// 注册页面组件
registerComponent("UserPage", () => import("./system/User"));
registerComponent("RolePage", () => import("./system/Role"));
```

### 懒加载与代码分割
- 所有页面组件使用 `React.lazy()` 懒加载
- 懒加载组件通过统一注册中心管理，支持缓存
- 路由级别自动代码分割，按需加载
- 移除不必要的 Suspense fallback，动画本身提供视觉反馈

```typescript
// 路由配置中的懒加载
const LoginPage = lazy(() => import("@/pages/system/Login"));
const AdminLayout = lazy(() => import("@/layouts/AdminLayout"));

// 动态路由的懒加载通过注册中心
function getLazyComponent(name: string) {
  if (!lazyCache.has(name)) {
    lazyCache.set(name, lazy(componentMap[name]));
  }
  return lazyCache.get(name)!;
}
```

### 路由配置
- 所有路由配置集中在 `web/routes/index.tsx`
- 使用 React Router 7 的 `createHashRouter`
- 动态路由基于用户权限生成，从 `useDynamicRoutes` Hook 获取
- 懒加载组件通过统一注册中心管理

```typescript
// 路由结构
createHashRouter([
  {
    element: <RootTransition />,  // 根级动画
    children: [
      { path: "/login", element: <LoginPage /> },
      {
        element: <AuthGuard />,  // 认证守卫
        children: [
          {
            path: "/",
            element: <AdminLayout />,
            children: [
              { path: "home", element: <HomePage /> },
              ...dynamicRoutes  // 基于权限的动态路由
            ],
          },
        ],
      },
    ],
  },
])
```

### 动画效果
- 使用 Framer Motion 12+ 实现流畅过渡动画
- 动画配置集中在 `web/utils/animations.ts`
- 避免每次渲染创建新对象引用，使用 `as const` 或提取到组件外部
- 两级动画系统：
  - **RootTransition**: 登录页 ↔ 后台应用的全页面动画（淡入淡出）
  - **PageTransition**: 后台内部页面切换的 outlet 区域动画（上滑）

```typescript
// 动画预设（web/utils/animations.ts）
export const fadeVariants: Variants = {
  initial: { opacity: 0 },
  animate: { opacity: 1 },
  exit: { opacity: 0 },
};

export const slideUpVariants: Variants = {
  initial: { opacity: 0, y: 20 },
  animate: { opacity: 1, y: 0 },
  exit: { opacity: 0, y: -20 },
};

export const pageTransition = {
  type: "tween" as const,
  ease: "easeInOut" as const,
  duration: 0.3,
};

// 使用示例
import { motion } from "framer-motion";
import { fadeVariants, pageTransition } from "@/utils/animations";

// 提取 style 对象到组件外部，避免重复创建引用
const containerStyle = { width: "100%", height: "100%" };

<motion.div
  variants={fadeVariants}
  transition={pageTransition}
  style={containerStyle}
>
  <YourComponent />
</motion.div>
```

### 响应式设计
- 使用 Ant Design Grid 断点系统
- 移动端自动切换为抽屉菜单
- 支持侧边栏折叠/展开

```typescript
import { Grid } from "antd";
const { useBreakpoint } = Grid;

const screens = useBreakpoint();
const isMobile = !screens.md; // 小于 md 断点为移动端
```

### 认证与权限
- **路由守卫**: 使用 `<AuthGuard />` 组件保护需要认证的路由
- **权限检查**: 使用 `usePermission` Hook 检查按钮级权限
- **自动重定向**: 未登录自动跳转登录页，登录后返回原页面
- **状态管理**: token/user 存储在 Redux，permissions 单独管理

```typescript
// AuthGuard 自动处理未登录重定向
<AuthGuard>
  <ProtectedRoutes />
</AuthGuard>

// 按钮级权限控制
const { hasPermission } = usePermission();
const canAdd = hasPermission("system:user:add");

<Button disabled={!canAdd}>添加用户</Button>

// 登录成功后自动导航
useEffect(() => {
  if (token && !mutation.isPending) {
    const from = location.state?.from?.pathname || "/home";
    navigate(from, { replace: true });
  }
}, [token, mutation.isPending]);
```

### 最佳实践
- **避免手动导航**: 认证相关的重定向交给 AuthGuard 处理
- **单一职责**: 组件只做一件事，复杂逻辑提取到 Hook 或 Service
- **类型安全**: 所有 API 调用都应有完整的类型定义
- **错误处理**: 使用 TanStack Query 的 `onError` 处理错误，显示用户友好提示
- **性能优化**: 使用 useMemo/useCallback 缓存计算结果和回调函数
- **代码简洁**: 移除未使用的组件、导入和代码，保持代码库整洁

## Agent 限制

### C++ 后端编译
- **禁止 Agent 执行 C++ 编译命令**：Agent 不允许运行 `cmake --build`、`make` 等 C++ 编译命令
- **用户负责后端编译测试**：C++ 后端的编译和测试工作由用户在本地环境执行
- **前端编译允许**：Agent 可以执行前端相关命令（如 `bun run build`、`bun run dev`）

## Git 提交规范

### Commit Message 格式
```
<type>: <subject>

<body>
```

### Type 类型
- `feat`: 新功能
- `fix`: Bug 修复
- `docs`: 文档更新
- `style`: 代码格式 (不影响功能)
- `refactor`: 重构
- `perf`: 性能优化
- `test`: 测试相关
- `chore`: 构建/工具变动
