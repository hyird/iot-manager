# S7 报文发送与解析手册

> 本文只讲报文。
>
> 目标只有两个：
>
> 1. 你知道每一步应该发什么
> 2. 你知道收到之后应该怎么拆、怎么判定成功或失败

## 1. 最小通讯流程

最常见的 S7 TCP 读写流程只有 4 步：

1. TCP 连接 PLC 的 `102` 端口
2. 发送 `COTP CR` 建连请求，接收 `COTP CC`
3. 发送 `S7 Setup Communication`，接收协商应答
4. 循环发送：
   - `Read Var`
   - `Write Var`

如果你只是手工发包，顺序固定就是：

```text
TCP connect
-> COTP CR
<- COTP CC
-> S7 Setup Communication
<- S7 Setup Communication Response
-> S7 Read Var / Write Var
<- S7 Ack Data
```

## 2. 报文三层结构

每个数据包都分成三层：

1. `TPKT`
2. `COTP`
3. `S7Comm`

## 2.1 TPKT

固定 4 字节：

```text
03 00 LL LL
```

| 偏移 | 长度 | 含义 |
| --- | --- | --- |
| `0` | 1 | version，固定 `03` |
| `1` | 1 | 保留，固定 `00` |
| `2..3` | 2 | 整帧长度，大端 |

注意：

- `LL LL` 包含 TPKT 自己的 4 字节
- 所以完整帧长度不是 payload 长度，而是“整个包”的长度

### 2.1.1 长度公式

连接阶段：

```text
tpktLength = 4 + cotpConnectionLength
```

数据阶段：

```text
tpktLength = 4 + 3 + s7PayloadLength
```

也就是：

```text
tpktLength = 7 + s7PayloadLength
```

## 2.2 COTP

S7 TCP 里常见三种 COTP：

| 类型 | 代码 | 用途 |
| --- | --- | --- |
| `CR` | `E0` | 建连请求 |
| `CC` | `D0` | 建连应答 |
| `DR` | `80` | 断开 |

数据阶段固定是 `DT`：

```text
02 F0 80
```

| 偏移 | 长度 | 含义 |
| --- | --- | --- |
| `0` | 1 | 长度，固定 `02` |
| `1` | 1 | `DT`，固定 `F0` |
| `2` | 1 | EOT 标记，常见 `80` |

### 2.2.1 `EOT`

`80` 表示这是最后一段。

如果一个 S7 payload 被拆成多段发送，你要做的是：

1. 按 TPKT 长度一帧一帧收
2. 只取每帧 `TPKT + COTP DT` 之后的 S7 数据
3. 直到遇到 `EOT=80`
4. 把所有 S7 数据拼起来再解析

## 2.3 S7Comm 头

S7Comm 头固定 12 字节：

```text
32 rosctr 00 00 ref0 ref1 parLenHi parLenLo dataLenHi dataLenLo errHi errLo
```

| S7 偏移 | 长度 | 含义 |
| --- | --- | --- |
| `0` | 1 | Protocol ID，固定 `32` |
| `1` | 1 | ROSCTR |
| `2..3` | 2 | 保留，通常 `0000` |
| `4..5` | 2 | PDU Reference |
| `6..7` | 2 | Parameter Length，大端 |
| `8..9` | 2 | Data Length，大端 |
| `10..11` | 2 | Error Class/Code 或组合错误码 |

常见 `ROSCTR`：

| 值 | 含义 |
| --- | --- |
| `01` | Request |
| `03` | Ack Data |

### 2.3.1 解析时一定要先看这 3 个字段

收到 S7 响应后，先做三件事：

1. 看 `protocol id == 32`
2. 看 `ROSCTR` 是否是响应类型，通常是 `03`
3. 看 `10..11` 是否为 `0000`

如果 `10..11 != 0000`，说明 PLC 已经在 S7 头层面拒绝了请求。

## 3. 建连阶段报文

## 3.1 COTP `CR` 建连请求

### 3.1.1 通用格式

```text
03 00 00 16
11 E0
00 00
00 01
00
C0 01 tpduSize
C1 02 localTsapHi localTsapLo
C2 02 remoteTsapHi remoteTsapLo
```

完整字段表：

| 全包偏移 | 字节 | 含义 |
| --- | --- | --- |
| `0` | `03` | TPKT version |
| `1` | `00` | 保留 |
| `2..3` | `0016` | 整帧长度 22 |
| `4` | `11` | COTP 参数长度 |
| `5` | `E0` | `CR` |
| `6..7` | `0000` | destination ref |
| `8..9` | `0001` | source ref |
| `10` | `00` | class |
| `11` | `C0` | TPDU Size 参数类型 |
| `12` | `01` | TPDU Size 参数长度 |
| `13` | `0A` | TPDU Size = 1024 |
| `14` | `C1` | Source TSAP 参数类型 |
| `15` | `02` | Source TSAP 长度 |
| `16..17` | `xxxx` | local TSAP |
| `18` | `C2` | Destination TSAP 参数类型 |
| `19` | `02` | Destination TSAP 长度 |
| `20..21` | `yyyy` | remote TSAP |

### 3.1.2 TSAP 模式示例

本地 TSAP 和远端 TSAP 都用 `4D57`：

```text
03 00 00 16 11 E0 00 00 00 01 00 C0 01 0A C1 02 4D 57 C2 02 4D 57
```

### 3.1.3 Rack/Slot 模式示例

如果你想连：

- `connectionType = PG`
- `rack = 0`
- `slot = 1`

常见做法：

- `localTSAP = 0100`
- `remoteTSAP = 0101`

建连请求：

```text
03 00 00 16 11 E0 00 00 00 01 00 C0 01 0A C1 02 01 00 C2 02 01 01
```

### 3.1.4 `remote TSAP` 计算公式

如果你手里只有 `rack / slot`，常见公式：

```text
remoteTSAP = (connectionType << 8) + rack * 0x20 + slot
```

常见 `connectionType`：

| 类型 | 值 |
| --- | --- |
| `PG` | `01` |
| `OP` | `02` |
| `S7_BASIC` | `03` |

常见结果：

| 参数 | remote TSAP |
| --- | --- |
| `PG, 0, 1` | `0101` |
| `PG, 0, 2` | `0102` |
| `PG, 0, 3` | `0103` |
| `OP, 0, 1` | `0201` |

## 3.2 COTP `CC` 建连应答

收到建连应答后，最少检查以下内容：

| 全包偏移 | 检查项 |
| --- | --- |
| `0` | 必须是 `03` |
| `5` | 必须是 `D0`，表示 `CC` |
| `2..3` | 长度必须合理，至少能覆盖整个 COTP 头 |

通常你不需要对 `CC` 的每个字段做深入业务解析。

最关键的是确认：

```text
frame[5] == D0
```

## 3.3 S7 `Setup Communication` 请求

### 3.3.1 完整帧

```text
03 00 00 19 02 F0 80 32 01 00 00 00 00 00 08 00 00 F0 00 00 01 00 01 01 E0
```

其中：

- `03 00 00 19`：TPKT
- `02 F0 80`：COTP DT
- 后面 18 字节是 S7 payload

### 3.3.2 S7 payload 按字节拆解

```text
32 01 00 00 00 00 00 08 00 00 F0 00 00 01 00 01 01 E0
```

| S7 偏移 | 字节 | 含义 |
| --- | --- | --- |
| `0` | `32` | Protocol ID |
| `1` | `01` | Request |
| `2..3` | `0000` | 保留 |
| `4..5` | `0000` | PDU Reference |
| `6..7` | `0008` | Parameter Length = 8 |
| `8..9` | `0000` | Data Length = 0 |
| `10..11` | `0000` | 错误码 = 0 |
| `12` | `F0` | Function = Setup Communication |
| `13` | `00` | 保留 |
| `14..15` | `0001` | Max AmQ Caller |
| `16..17` | `0001` | Max AmQ Callee |
| `18..19` | `01E0` | 请求 PDU 长度 = 480 |

## 3.4 `Setup Communication` 应答

### 3.4.1 典型成功应答

下面是一个典型的成功响应模板：

```text
03 00 00 1B 02 F0 80 32 03 00 00 00 00 00 08 00 00 00 00 F0 00 00 01 00 01 01 E0
```

### 3.4.2 你应该怎样解析

步骤固定：

1. 检查 `TPKT version = 03`
2. 检查 `COTP DT = 02 F0`
3. 进入 S7 payload
4. 检查：
   - `payload[0] == 32`
   - `payload[1] == 03`
5. 检查：

```text
payload[10..11] == 0000
```

6. 读取：

```text
parLen = payload[6..7]
```

7. 如果 `parLen < 8`，判失败
8. 读取：

```text
negotiatedPduLength = payload[18..19]
```

### 3.4.3 为什么要拿 PDU 长度

因为后续读写如果超过单次 PDU 能承载的数据，你就要自己分包。

保守做法：

- 始终按 PLC 返回的协商值做上限
- 不要继续用本地想当然的固定值

## 4. `Read Var` 请求

## 4.1 单 item 通用格式

为了简单，本文只讲单 item 的 `Read Var`。

S7 payload：

```text
32 01 00 00 ref0 ref1 00 0E 00 00
04 01
12 0A 10 wordLen amountHi amountLo dbHi dbLo area addrHi addrMid addrLo
```

### 4.1.1 头部 12 字节

| S7 偏移 | 长度 | 含义 |
| --- | --- | --- |
| `0` | 1 | `32` |
| `1` | 1 | `01`，request |
| `2..3` | 2 | 保留 |
| `4..5` | 2 | PDU Reference |
| `6..7` | 2 | Parameter Length = `000E` |
| `8..9` | 2 | Data Length = `0000` |
| `10..11` | 2 | 错误码，request 中通常 `0000` |

### 4.1.2 参数区 14 字节

| S7 偏移 | 长度 | 含义 |
| --- | --- | --- |
| `12` | 1 | Function = `04` |
| `13` | 1 | Item Count = `01` |
| `14` | 1 | Var Spec = `12` |
| `15` | 1 | Item Length = `0A` |
| `16` | 1 | Syntax ID = `10` |
| `17` | 1 | `wordLen` |
| `18..19` | 2 | `amount` |
| `20..21` | 2 | `dbNumber` |
| `22` | 1 | `area` |
| `23..25` | 3 | `address` |

## 4.2 常用 area 代码

| 区域 | 代码 |
| --- | --- |
| 输入区 `PE / I` | `81` |
| 输出区 `PA / Q` | `82` |
| M 区 `MK / M` | `83` |
| DB 区 `DB` | `84` |
| 计数器 `CT` | `1C` |
| 定时器 `TM` | `1D` |

## 4.3 常用 `wordLen`

| 类型 | 代码 |
| --- | --- |
| `BIT` | `01` |
| `BYTE` | `02` |
| `CHAR` | `03` |
| `WORD` | `04` |
| `INT` | `05` |
| `DWORD` | `06` |
| `DINT` | `07` |
| `REAL` | `08` |
| `COUNTER` | `1C` |
| `TIMER` | `1D` |

## 4.4 地址怎么算

最重要的一条：

### 4.4.1 按字节访问

如果你读的是：

- `DBB`
- `DBW`
- `DBD`
- `MB`
- `IB`
- `QB`

这类按字节偏移寻址的区域，地址字段都写：

```text
address = byteOffset * 8
```

例子：

| 目标 | 偏移 | 地址字段 |
| --- | --- | --- |
| `DB1.DBB100` | `100` | `000320` |
| `MB10` | `10` | `000050` |
| `QB0` | `0` | `000000` |

### 4.4.2 按位访问

如果你真的直接做 bit 读，地址字段通常写 bit 地址本身。

例如：

- `Q0.3`
- bit 地址 = `0 * 8 + 3 = 3`
- 地址字段 = `000003`

### 4.4.3 `Counter / Timer`

通常直接写索引：

- `C5` -> `000005`
- `T2` -> `000002`

## 4.5 `amount` 怎么填

`amount` 不是永远填“元素个数”，而是和 `wordLen` 有关系。

最常用的填法：

| 读法 | `wordLen` | `amount` |
| --- | --- | --- |
| 读 1 字节 | `02` | `0001` |
| 读 2 字节 | `02` | `0002` |
| 读 4 字节 | `02` | `0004` |
| 读 1 bit | `01` | `0001` |
| 读 1 个 counter | `1C` | `0001` |
| 读 1 个 timer | `1D` | `0001` |

如果你采用“先按字节读，再本地解码”的策略：

- `BOOL` 也可以直接读 1 字节
- `INT16` 直接读 2 字节
- `FLOAT` 直接读 4 字节

## 4.6 示例 1：读取 `PE0` 1 字节

完整帧：

```text
03 00 00 1F
02 F0 80
32 01 00 00 01 00 00 0E 00 00
04 01 12 0A 10 02 00 01 00 00 81 00 00 00
```

拼成一行：

```text
03 00 00 1F 02 F0 80 32 01 00 00 01 00 00 0E 00 00 04 01 12 0A 10 02 00 01 00 00 81 00 00 00
```

如何读这条报文：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `Function` | `04` | Read Var |
| `wordLen` | `02` | BYTE |
| `amount` | `0001` | 读 1 字节 |
| `dbNumber` | `0000` | 非 DB 区 |
| `area` | `81` | PE |
| `address` | `000000` | 偏移 0 |

## 4.7 示例 2：读取 `DB1.DBB100..103`

完整帧：

```text
03 00 00 1F
02 F0 80
32 01 00 00 03 00 00 0E 00 00
04 01 12 0A 10 02 00 04 00 01 84 00 03 20
```

拼成一行：

```text
03 00 00 1F 02 F0 80 32 01 00 00 03 00 00 0E 00 00 04 01 12 0A 10 02 00 04 00 01 84 00 03 20
```

如何读这条报文：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `wordLen` | `02` | BYTE |
| `amount` | `0004` | 读 4 字节 |
| `dbNumber` | `0001` | DB1 |
| `area` | `84` | DB |
| `address` | `000320` | 100 * 8 |

## 4.8 示例 3：读取 `Q0.3`，采用直接 bit 读

S7 payload 核心字段：

```text
04 01 12 0A 10 01 00 01 00 00 82 00 00 03
```

解释：

| 字段 | 值 |
| --- | --- |
| `wordLen` | `01`，BIT |
| `amount` | `0001` |
| `area` | `82`，PA |
| `address` | `000003` |

如果你不想直接处理 bit 读，也可以更简单：

1. 先读 `QB0`
2. 再在本地取 bit3

## 5. `Read Var` 响应

## 5.1 通用格式

单 item 成功响应通常长这样：

```text
32 03 00 00 ref0 ref1 parLenHi parLenLo dataLenHi dataLenLo 00 00
04 01
FF transportSize payloadLenHi payloadLenLo data...
```

字段解释：

### 5.1.1 S7 头

| S7 偏移 | 长度 | 含义 |
| --- | --- | --- |
| `0` | 1 | `32` |
| `1` | 1 | `03`，Ack Data |
| `4..5` | 2 | PDU Reference |
| `6..7` | 2 | Parameter Length |
| `8..9` | 2 | Data Length |
| `10..11` | 2 | 头级错误码 |

### 5.1.2 参数区

对于单 item `Read Var` 响应，常见是：

```text
04 01
```

也就是：

- `04`：Read Var
- `01`：1 个 item

### 5.1.3 数据区

格式固定：

```text
returnCode transportSize payloadLenHi payloadLenLo payload...
```

## 5.2 `returnCode`

最常见：

| 值 | 含义 |
| --- | --- |
| `FF` | 成功 |
| 其他值 | item 级失败 |

如果 `returnCode != FF`，就不要再当成功数据解析。

## 5.3 `payloadLen` 的单位

这是最容易搞错的点之一。

常见规则：

| `transportSize` | `payloadLen` 含义 |
| --- | --- |
| `03` BIT | 直接按字节数或 1 bit 容器处理 |
| `07` REAL | 直接按字节数 |
| `09` OCTET STRING | 直接按字节数 |
| `04` BYTE/WORD family | 常按 bit 数给出 |
| `05` INT family | 常按 bit 数给出 |

换算时，最常见经验规则是：

- `03 / 07 / 09`：直接拿来当字节数
- 其他常见值：除以 `8`

### 5.3.1 常见例子

| `transportSize` | `payloadLen` | 实际数据 |
| --- | --- | --- |
| `04` | `0008` | 1 字节 |
| `04` | `0010` | 2 字节 |
| `04` | `0020` | 4 字节 |
| `09` | `0004` | 4 字节 |

## 5.4 示例 1：读取 `PE0` 返回 `01`

完整帧：

```text
03 00 00 1A 02 F0 80 32 03 00 00 01 00 00 02 00 05 00 00 04 01 FF 04 00 08 01
```

解析：

| 字段 | 值 |
| --- | --- |
| `ROSCTR` | `03` |
| `parLen` | `0002` |
| `dataLen` | `0005` |
| 参数区 | `04 01` |
| `returnCode` | `FF` |
| `transportSize` | `04` |
| `payloadLen` | `0008` |
| 数据 | `01` |

如果你读的是 `I0.0`：

```text
value = (0x01 >> 0) & 0x01 = 1
```

如果你读的是 `I0.3`：

```text
value = (0x01 >> 3) & 0x01 = 0
```

## 5.5 示例 2：读取 `DB1.DBB100..103` 返回 `11 22 33 44`

完整帧：

```text
03 00 00 1D 02 F0 80 32 03 00 00 03 00 00 02 00 08 00 00 04 01 FF 04 00 20 11 22 33 44
```

解析：

| 字段 | 值 |
| --- | --- |
| `parLen` | `0002` |
| `dataLen` | `0008` |
| 参数区 | `04 01` |
| `returnCode` | `FF` |
| `transportSize` | `04` |
| `payloadLen` | `0020` |
| 数据 | `11 22 33 44` |

如何本地解释：

- `UINT32`：`0x11223344`
- `INT32`：按补码解释
- `FLOAT`：按 IEEE754 解释
- `STRING(4)`：按 ASCII 解释

## 5.6 `Read Var` 响应解析算法

拿到完整 S7 payload 后，按下面顺序走：

```text
1. 长度至少 >= 12
2. payload[0] == 32
3. payload[1] 是响应类型，常见 03
4. headerError = payload[10..11]
5. 如果 headerError != 0000，直接失败
6. parLen = payload[6..7]
7. dataLen = payload[8..9]
8. dataOffset = 12 + parLen
9. returnCode = payload[dataOffset]
10. 如果 returnCode != FF，失败
11. transportSize = payload[dataOffset + 1]
12. payloadLen = payload[dataOffset + 2..3]
13. 根据 transportSize 算真实字节数
14. 从 dataOffset + 4 取出真实数据
```

## 6. `Write Var` 请求

## 6.1 单 item 通用格式

S7 payload：

```text
32 01 00 00 ref0 ref1 00 0E dataLenHi dataLenLo
05 01
12 0A 10 wordLen amountHi amountLo dbHi dbLo area addrHi addrMid addrLo
00 transportSize payloadLenHi payloadLenLo payload...
```

### 6.1.1 与 `Read Var` 的主要差别

只有两个核心差异：

1. 功能码从 `04` 变成 `05`
2. 后面多了数据区

## 6.2 数据区结构

```text
00 transportSize payloadLenHi payloadLenLo payload...
```

| 相对数据区偏移 | 含义 |
| --- | --- |
| `0` | 保留，常见 `00` |
| `1` | `transportSize` |
| `2..3` | `payloadLen` |
| `4..` | 实际数据 |

## 6.3 常用 `transportSize`

| 类型 | 值 |
| --- | --- |
| `BIT` | `03` |
| `BYTE/WORD family` | `04` |
| `INT family` | `05` |
| `REAL` | `07` |
| `OCTET STRING` | `09` |

## 6.4 `payloadLen` 怎么填

常见写法：

| 数据 | `transportSize` | `payloadLen` |
| --- | --- | --- |
| 1 字节 | `04` | `0008` |
| 2 字节 | `04` | `0010` |
| 4 字节 | `04` | `0020` |
| 4 字节原始串 | `09` | `0004` |
| 1 bit 容器 | `03` | `0001` |

## 6.5 示例 1：写 `PA0 = 01`

完整帧：

```text
03 00 00 24 02 F0 80 32 01 00 00 02 00 00 0E 00 05 05 01 12 0A 10 02 00 01 00 00 82 00 00 00 00 04 00 08 01
```

拆解：

| 字段 | 值 |
| --- | --- |
| `Function` | `05` |
| `wordLen` | `02`，BYTE |
| `amount` | `0001` |
| `area` | `82`，PA |
| `address` | `000000` |
| `transportSize` | `04` |
| `payloadLen` | `0008` |
| `payload` | `01` |

## 6.6 示例 2：写 `DB10.DBW2 = 123`

`123` 转成大端 2 字节：

```text
00 7B
```

完整 S7 payload：

```text
32 01 00 00 04 00 00 0E 00 06 05 01 12 0A 10 02 00 02 00 0A 84 00 00 10 00 04 00 10 00 7B
```

解释：

| 字段 | 值 |
| --- | --- |
| `Function` | `05` |
| `wordLen` | `02` |
| `amount` | `0002` |
| `dbNumber` | `000A` |
| `area` | `84` |
| `address` | `000010` |
| `transportSize` | `04` |
| `payloadLen` | `0010` |
| `payload` | `00 7B` |

如果加上 TPKT + COTP，完整帧是：

```text
03 00 00 25 02 F0 80 32 01 00 00 04 00 00 0E 00 06 05 01 12 0A 10 02 00 02 00 0A 84 00 00 10 00 04 00 10 00 7B
```

## 6.7 示例 3：写 bit，推荐做法是整字节读改写

最稳的步骤：

1. 先读目标字节
2. 在本地改位
3. 再按字节写回

比如写 `Q0.3 = 1`：

### 第一步，先读 `QB0`

如果返回：

```text
01
```

### 第二步，改 bit3

```text
01 | 08 = 09
```

### 第三步，写 `QB0 = 09`

发：

```text
03 00 00 24 02 F0 80 32 01 00 00 05 00 00 0E 00 05 05 01 12 0A 10 02 00 01 00 00 82 00 00 00 00 04 00 08 09
```

这样不会误伤同一字节里的其他位。

## 7. `Write Var` 响应

## 7.1 通用格式

单 item 成功响应通常长这样：

```text
32 03 00 00 ref0 ref1 00 02 00 01 00 00
05 01
FF
```

也就是：

- 参数区一般是 `05 01`
- 数据区只有 1 个 `returnCode`

## 7.2 示例：写成功响应

完整帧：

```text
03 00 00 16 02 F0 80 32 03 00 00 02 00 00 02 00 01 00 00 05 01 FF
```

解析：

| 字段 | 值 |
| --- | --- |
| `ROSCTR` | `03` |
| `parLen` | `0002` |
| `dataLen` | `0001` |
| 参数区 | `05 01` |
| `returnCode` | `FF` |

## 7.3 `Write Var` 响应解析算法

拿到完整 S7 payload 后：

```text
1. payload 长度至少 >= 12
2. payload[0] == 32
3. payload[1] 是响应类型，通常 03
4. headerError = payload[10..11]
5. 如果 headerError != 0000，失败
6. parLen = payload[6..7]
7. dataOffset = 12 + parLen
8. returnCode = payload[dataOffset]
9. 如果 returnCode == FF，成功；否则失败
```

## 8. 直接收包时的解析顺序

如果你自己写 socket 解析器，建议固定按下面顺序：

## 8.1 先收 TPKT 头

先收 4 字节：

```text
03 00 LL LL
```

校验：

1. `version == 03`
2. `LL LL >= 4`

## 8.2 再收剩余字节

根据 `LL LL` 再收：

```text
remaining = totalLength - 4
```

## 8.3 判断是不是 COTP DT

如果是数据帧，通常有：

```text
02 F0 xx
```

你要做的是：

1. 取出第 7 字节之后的 S7 payload
2. 如果 `EOT` 没置位，继续收下一帧
3. 拼完整 payload 再做 S7 解析

## 8.4 再解析 S7

进入 S7 payload 后：

1. 看 `protocol id`
2. 看 `ROSCTR`
3. 看 `parLen`
4. 看 `dataLen`
5. 看 `header error`
6. 再进参数区和数据区

## 9. 多帧重组

如果一条响应被拆成多个 TPKT/COTP DT：

1. 每次都按 `TPKT length` 读满一帧
2. 检查 `frame[4..6]`
3. 取 `frame[7..end]` 拼到缓冲区
4. 直到：

```text
frame[6] & 0x80 == 0x80
```

5. 最后把缓冲区当作完整 S7 payload 解析

## 10. 失败判断

## 10.1 头级失败

如果 S7 头里的：

```text
payload[10..11] != 0000
```

直接按失败处理。

这类错误通常代表：

- 地址非法
- 功能不支持
- PDU 超限
- 权限或口令问题

## 10.2 item 级失败

如果头级成功，但数据区：

```text
returnCode != FF
```

也要按失败处理。

## 10.3 常见 PLC 错误码

常见错误：

| 错误码 | 含义 |
| --- | --- |
| `0005` | Address out of range |
| `0006` | Invalid transport size |
| `0007` | Write data size mismatch |
| `000A` | Item not available |
| `8104` | Function not available |
| `8500` | Data over PDU |
| `D209` | Item not available |
| `D241` | Need password |
| `D602` | Invalid password |
| `DC01` | Invalid value |

## 11. 常用数据解释

## 11.1 `BOOL`

如果你读的是一个字节 `b`，位号是 `n`：

```text
value = (b >> n) & 0x01
```

## 11.2 `UINT16`

大端：

```text
u16 = (b0 << 8) | b1
```

## 11.3 `UINT32`

大端：

```text
u32 = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
```

## 11.4 `FLOAT`

1. 先按大端拼成 32 位整数
2. 再按 IEEE754 单精度解释

## 11.5 `STRING`

如果你自己定义的是固定长度字符串：

1. 按固定字节数读取
2. 可以遇 `00` 截断
3. 剩余字节按 ASCII 或目标编码解释

## 12. 现成模板

## 12.1 建连模板

```text
03 00 00 16 11 E0 00 00 00 01 00 C0 01 0A C1 02 01 00 C2 02 01 01
```

## 12.2 协商模板

```text
03 00 00 19 02 F0 80 32 01 00 00 00 00 00 08 00 00 F0 00 00 01 00 01 01 E0
```

## 12.3 读 `DB1.DBB100..103`

```text
03 00 00 1F 02 F0 80 32 01 00 00 03 00 00 0E 00 00 04 01 12 0A 10 02 00 04 00 01 84 00 03 20
```

## 12.4 写 `QB0 = 01`

```text
03 00 00 24 02 F0 80 32 01 00 00 02 00 00 0E 00 05 05 01 12 0A 10 02 00 01 00 00 82 00 00 00 00 04 00 08 01
```

## 13. 最后只记这 12 条

1. 先 TCP 连 `102`
2. 再发 `CR`
3. 收 `CC`
4. 再发 `Setup Communication`
5. 收应答，拿 PDU 长度
6. 之后所有数据帧外层固定 `03 00 LL LL 02 F0 80`
7. `Read Var` 功能码是 `04`
8. `Write Var` 功能码是 `05`
9. 按字节访问时，地址字段通常是 `byteOffset * 8`
10. 读响应先看 S7 头的 `10..11`
11. 再看数据区 `returnCode`
12. 写 bit 最稳的方法永远是“读整字节 -> 改位 -> 写整字节”

