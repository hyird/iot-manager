# S7 报文速查

> 完整版请看 `docs/s7-protocol-reference.md`。

## 1. 发送顺序

1. TCP 连 `102`
2. 发 `COTP CR`
3. 收 `COTP CC`
4. 发 `Setup Communication`
5. 收协商应答
6. 发 `Read Var / Write Var`
7. 解析返回

## 2. 最外层固定格式

数据阶段外层固定：

```text
03 00 LL LL 02 F0 80 ...
```

- `03 00 LL LL`：TPKT
- `02 F0 80`：COTP DT

## 3. 建连报文

### TSAP = 4D57 / 4D57

```text
03 00 00 16 11 E0 00 00 00 01 00 C0 01 0A C1 02 4D 57 C2 02 4D 57
```

### Rack0 Slot1 常见写法

本地 `0100`，远端 `0101`：

```text
03 00 00 16 11 E0 00 00 00 01 00 C0 01 0A C1 02 01 00 C2 02 01 01
```

## 4. PDU 协商

```text
03 00 00 19 02 F0 80 32 01 00 00 00 00 00 08 00 00 F0 00 00 01 00 01 01 E0
```

关键值：

- `32`：S7 协议
- `01`：request
- `F0`：Setup Communication
- `01 E0`：请求 PDU 长度 480

## 5. 读报文

## 5.1 读取 `PE0` 1 字节

```text
03 00 00 1F 02 F0 80 32 01 00 00 01 00 00 0E 00 00 04 01 12 0A 10 02 00 01 00 00 81 00 00 00
```

## 5.2 读取 `DB1.DBB100..103`

```text
03 00 00 1F 02 F0 80 32 01 00 00 03 00 00 0E 00 00 04 01 12 0A 10 02 00 04 00 01 84 00 03 20
```

关键规则：

- 读功能码：`04`
- `area=84`：DB
- `dbNumber=0001`
- `address=100 * 8 = 000320`

## 6. 写报文

### 写 `PA0 = 01`

```text
03 00 00 24 02 F0 80 32 01 00 00 02 00 00 0E 00 05 05 01 12 0A 10 02 00 01 00 00 82 00 00 00 00 04 00 08 01
```

关键规则：

- 写功能码：`05`
- `area=82`：PA
- `transportSize=04`
- `payloadLen=0008`：8 bit
- 数据：`01`

## 7. 响应怎么解析

先看 S7 头：

- `6..7`：parameter length
- `8..9`：data length
- `10..11`：错误码

如果 `10..11 != 0000`，先按失败处理。

### 读响应

数据区格式：

```text
returnCode transportSize payloadLenHi payloadLenLo data...
```

成功标志：

```text
returnCode = FF
```

### 写响应

写响应最重要的是：

```text
returnCode = FF
```

## 8. 常用常量

### area

- `PE = 81`
- `PA = 82`
- `MK = 83`
- `DB = 84`
- `CT = 1C`
- `TM = 1D`

### wordLen

- `BIT = 01`
- `BYTE = 02`
- `WORD = 04`
- `INT = 05`
- `DWORD = 06`
- `DINT = 07`
- `REAL = 08`

## 9. 最重要的公式

### 地址

按字节访问：

```text
address = byteOffset * 8
```

### 远端 TSAP

```text
remoteTSAP = (connectionType << 8) + rack * 0x20 + slot
```

## 10. 最实用的记忆版

1. 先 `CR/CC`
2. 再 `Setup Communication`
3. 数据阶段外层固定 `03 00 ... 02 F0 80`
4. 读用 `04`
5. 写用 `05`
6. 地址常常是字节偏移乘 `8`
7. 成功通常看 `10..11 == 0000` 且 `returnCode == FF`

