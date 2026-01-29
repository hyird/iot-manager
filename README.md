# IoT Manager

基于 Drogon 框架的管理平台。

## 功能特性

### 系统管理
- **用户管理**：用户 CRUD、角色分配
- **角色管理**：角色权限配置
- **菜单管理**：动态菜单配置
- **部门管理**：组织架构管理

### 技术特性
- C++20 协程 + Drogon 框架
- Redis 缓存 + MySQL 数据库
- JWT 认证 + RBAC 权限控制
- React 19 + Ant Design 6 前端

## 快速开始

### 环境要求
- C++20 编译器 (Clang 17+ / GCC 13+ / MSVC 2022+)
- CMake 3.20+
- vcpkg
- Node.js 18+ / Bun
- MySQL 8.0+
- Redis 6.0+ (可选)

### 安装依赖

```bash
# 安装前端依赖
bun install

# 配置 CMake (vcpkg 自动安装后端依赖)
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

### 配置数据库

1. 创建数据库：
```sql
CREATE DATABASE iot_manager CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
```

2. 修改 `config/config.json` 中的数据库连接信息

### 编译运行

```bash
# 编译
cmake --build build

# 运行
./build/release/iot-manager
```

### 访问系统

- 前端地址：http://localhost:3000
- 默认账号：admin / admin123

## 项目结构

```
iot-manager/
├── server/                 # 后端代码
│   ├── common/            # 公共模块
│   │   ├── cache/         # 缓存管理
│   │   ├── database/      # 数据库服务
│   │   ├── filters/       # 过滤器
│   │   └── utils/         # 工具类
│   └── modules/           # 业务模块
│       ├── home/          # 首页
│       └── system/        # 系统管理
├── web/                    # 前端代码
│   ├── components/        # 公共组件
│   ├── pages/             # 页面组件
│   ├── services/          # API 服务
│   └── store/             # 状态管理
├── config/                 # 配置文件
└── database/              # 数据库脚本
```

## License

MIT
