# 协议生命周期设计说明

本文档统一说明 `Modbus / S7 / SL651` 在服务端的协议运行时语义，避免把“配置存在”和“运行态可用”混为一谈。

## 统一术语

### Configured

设备或协议配置已存在于数据库中，且可以被适配器读取。

### Bound

协议已经把某个链路连接和具体运行对象建立了关联。

- Modbus: `linkId + registration` 聚合成 `dtuKey`，并与当前 session 绑定
- S7: 单个设备 runtime 与 bridge session 绑定
- SL651: 设备编码与连接映射建立后，解析器可以按配置路由

### RuntimeActivated

协议已经把当前绑定关系恢复到运行态，可以真正进行轮询、收发或解析。

## 标准入口

### `initializeAsync()`

进程启动或协议适配器首次加载时调用。

职责：

- 构建初始配置快照
- 清理旧缓存
- 预热必要的调度器或 runtime 容器

### `reloadAsync()`

配置或拓扑变化后调用。

职责：

- 刷新配置快照
- 清理失效的运行态缓存
- 按协议语义恢复当前运行态

### `onConnectionChanged(...)`

物理连接建立或断开时调用。

职责：

- 更新 session 状态
- 触发 bind / unbind
- 释放旧连接占用的 runtime 或路由

### `onDataReceived(...)`

链路收到原始报文时调用。

职责：

- 预处理报文
- 识别注册信息或协议头
- 交给协议 parser / session engine / scheduler 处理

### `onMaintenanceTick()`

周期性维护入口。

职责：

- 超时处理
- 轮询推进
- 清理悬挂状态

## 协议实现差异

### Modbus

- 采用 `DtuRegistry + DtuSessionManager + PollScheduler`
- `reloadAsync()` 会刷新 DTU 聚合结果，并重放当前已绑定 session
- 新增同一 DTU 下的设备，reload 后需要重新激活对应运行态

### S7

- 采用单设备 runtime + bridge session + PollScheduler
- `reloadAsync()` 直接重建 runtime 列表和轮询配置
- 运行态是否可用主要取决于 runtime 是否重建成功，而不是 DTU 聚合

### SL651

- 以设备配置缓存和报文解析为主
- `reloadAsync()` 主要清理缓存
- 不依赖 DTU 聚合或轮询激活模型

## 设计原则

1. `reloadAsync()` 只负责刷新配置和恢复运行态，不直接承载协议解析逻辑。
2. `onConnectionChanged()` 只处理连接生命周期，不负责业务解析。
3. `onDataReceived()` 只处理输入报文，不负责配置重建。
4. 共享逻辑放到基类 helper 或公共工具中，协议特有逻辑保留在各自适配器内。
5. 仅在明确的 TCP 发送失败场景下，才允许服务端主动断开连接；配置更新、session 重绑、注册变化、reload、维护任务等都不应主动踢掉 TCP server client。

