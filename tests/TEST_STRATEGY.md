# 数据库迁移系统测试策略

## 测试金字塔

```
        /  E2E  \        <- 7 个场景，需要真实 PostgreSQL
       /----------\
      / 集成测试   \      <- 15 个测试，需要真实 PostgreSQL
     /--------------\
    /   单元测试      \   <- 70+ 个测试，无任何外部依赖
   /------------------\
```

## 文件结构

```
tests/
├── unit/migration/
│   ├── MigrationVersionParserTest.cpp   # 版本解析（40 个测试）
│   ├── MigrationFileLoaderTest.cpp      # 文件加载（22 个测试）
│   ├── MigrationStateManagerTest.cpp    # 状态管理（24 个测试）
│   ├── MigrationEngineTest.cpp          # 执行引擎（20 个测试）
│   └── ToParameterizedTest.cpp          # SQL 参数化（12 个测试）
├── integration/migration/
│   └── MigrationIntegrationTest.cpp     # 真实 DB 集成（12 个测试）
├── e2e/migration/
│   └── MigrationE2ETest.cpp             # 端到端场景（7 个场景）
├── helpers/
│   ├── TestDatabaseHelper.hpp           # 数据库辅助工具
│   └── MockDbClient.hpp                 # 数据库 Mock
├── fixtures/migrations/
│   └── V001__baseline.up.sql            # 基线迁移 fixture
├── CMakeLists.txt                       # 测试构建配置
└── run_tests.sh                         # 测试运行脚本
```

## TDD 红-绿-重构 循环

### 阶段一（红灯）：当前状态
所有测试编译通过但运行失败，因为 `server/migration/` 下的实现文件尚未创建。

### 阶段二（绿灯）：按顺序实现
1. `MigrationVersionParser.cpp`    - 最简单，纯字符串解析
2. `MigrationFileLoader.cpp`       - 文件系统 + SHA-256
3. `MigrationStateManager.cpp`     - SQL 操作 + Advisory Lock
4. `MigrationEngine.cpp`           - 协调以上三者

### 阶段三（重构）
- 提取公共 SQL builder
- 优化 checksum 缓存
- 添加迁移执行耗时统计

## 覆盖率目标

| 组件 | 目标覆盖率 |
|------|-----------|
| MigrationVersionParser | 95%+ |
| MigrationFileLoader    | 90%+ |
| MigrationStateManager  | 85%+ |
| MigrationEngine        | 85%+ |
| 整体                   | 80%+ |

## 本地运行

```bash
# 仅运行单元测试（无需数据库）
./tests/run_tests.sh unit

# 运行所有测试（需要 PostgreSQL）
export TEST_DB_HOST=localhost
export TEST_DB_PASSWORD=yourpassword
./tests/run_tests.sh all

# 生成覆盖率报告（仅 Linux/GCC）
./tests/run_tests.sh all --coverage
```

## CI 配置要点

- 单元测试：每次 PR 必跑，无需数据库服务
- 集成测试：每次 PR 必跑，通过 `services: postgres` 提供数据库
- E2E 测试：合并到 main 前必跑，timeout=120s
- 覆盖率门槛：80%，低于时阻断合并
