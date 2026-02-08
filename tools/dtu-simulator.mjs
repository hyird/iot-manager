#!/usr/bin/env node
/**
 * DTU Modbus 客户端模拟器
 *
 * 模拟多个 DTU 设备通过 TCP 连接到 IoT Manager 服务端，
 * 发送注册包、心跳包，并响应 Modbus TCP/RTU 读写请求。
 *
 * 用法：node dtu-simulator.mjs [host] [port] [mode]
 * 默认：127.0.0.1 9003 TCP
 *
 * mode: TCP | RTU (Modbus 帧模式)
 */

import net from "node:net";

// ============================================================
// 配置
// ============================================================

const SERVER_HOST = process.argv[2] || "127.0.0.1";
const SERVER_PORT = parseInt(process.argv[3] || "9003", 10);
const FRAME_MODE = (process.argv[4] || "TCP").toUpperCase(); // TCP | RTU
const HEARTBEAT_INTERVAL = 30_000;
const RECONNECT_DELAY = 5_000;
const REGISTRATION_DELAY = 500;

// DTU 设备定义（来自数据库 device 表）
const DTU_DEVICES = [
  { id: 3, name: "阀门控制柜-1", registration: "860410067048389", slaveId: 1 },
  { id: 4, name: "阀门控制柜-2", registration: "860410067041475", slaveId: 1 },
  { id: 5, name: "阀门控制柜-3", registration: "860410067033662", slaveId: 1 },
  { id: 6, name: "阀门控制柜-4", registration: "860410067032961", slaveId: 1 },
  { id: 7, name: "阀门控制柜-5", registration: "860410067031963", slaveId: 1 },
];

const HEARTBEAT_CONTENT = "HELLO";

// 寄存器初始值（来自 protocol_config id=3）
const INITIAL_COILS = {
  1024: false, // 远程开
  1025: false, // 远程关
  1026: false, // 远程停
};

const INITIAL_DISCRETE_INPUTS = {
  1024: false, // 阀门开输出
  1025: true, // 阀门关输出
  1026: false, // 热保护
  1027: false, // 阀门故障
  1028: true, // 手自动模式 (自动)
  1029: false, // 闸门开到位
  1030: true, // 阀门关到位
};

// ============================================================
// Modbus 协议常量
// ============================================================

const FC = {
  READ_COILS: 0x01,
  READ_DISCRETE_INPUTS: 0x02,
  READ_HOLDING_REGISTERS: 0x03,
  READ_INPUT_REGISTERS: 0x04,
  WRITE_SINGLE_COIL: 0x05,
  WRITE_SINGLE_REGISTER: 0x06,
  WRITE_MULTIPLE_COILS: 0x0f,
  WRITE_MULTIPLE_REGISTERS: 0x10,
};

const FC_NAMES = {
  [FC.READ_COILS]: "ReadCoils",
  [FC.READ_DISCRETE_INPUTS]: "ReadDiscreteInputs",
  [FC.READ_HOLDING_REGISTERS]: "ReadHoldingRegisters",
  [FC.READ_INPUT_REGISTERS]: "ReadInputRegisters",
  [FC.WRITE_SINGLE_COIL]: "WriteSingleCoil",
  [FC.WRITE_SINGLE_REGISTER]: "WriteSingleRegister",
  [FC.WRITE_MULTIPLE_COILS]: "WriteMultipleCoils",
  [FC.WRITE_MULTIPLE_REGISTERS]: "WriteMultipleRegisters",
};

const EXCEPTION = {
  ILLEGAL_FUNCTION: 0x01,
  ILLEGAL_DATA_ADDRESS: 0x02,
  ILLEGAL_DATA_VALUE: 0x03,
  SLAVE_DEVICE_FAILURE: 0x04,
};

// MBAP Header 长度 (TransactionID + ProtocolID + Length = 6 bytes)
const MBAP_HEADER_LEN = 6;

// ============================================================
// CRC16 (Modbus RTU)
// ============================================================

// 预计算 CRC16 查找表，多项式 0xA001（标准 Modbus RTU）
const CRC_TABLE = new Uint16Array(256);
for (let i = 0; i < 256; i++) {
  let crc = i;
  for (let j = 0; j < 8; j++) {
    crc = crc & 1 ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
  }
  CRC_TABLE[i] = crc;
}

function crc16(buf, offset = 0, length = buf.length - offset) {
  let crc = 0xffff;
  const end = offset + length;
  for (let i = offset; i < end; i++) {
    crc = (crc >>> 8) ^ CRC_TABLE[(crc ^ buf[i]) & 0xff];
  }
  return crc;
}

function appendCrc(buf) {
  const crc = crc16(buf, 0, buf.length - 2);
  buf[buf.length - 2] = crc & 0xff; // CRC Low
  buf[buf.length - 1] = (crc >>> 8) & 0xff; // CRC High
  return buf;
}

function verifyCrc(buf, len) {
  const crcCalc = crc16(buf, 0, len - 2);
  const crcRecv = buf[len - 2] | (buf[len - 1] << 8);
  return crcCalc === crcRecv;
}

// ============================================================
// Modbus TCP 帧编解码器
// ============================================================

const ModbusTcp = {
  /**
   * 从缓冲区尝试解析一帧请求
   * @returns {{ frame: ParsedRequest, consumed: number } | null}
   */
  parse(buf) {
    if (buf.length < MBAP_HEADER_LEN + 2) return null; // 最少 8 字节 (MBAP + UnitID + FC)

    const protocolId = buf.readUInt16BE(2);
    if (protocolId !== 0) return { frame: null, consumed: 1 }; // 对齐跳过

    const pduLength = buf.readUInt16BE(4);
    if (pduLength < 2 || pduLength > 254) return { frame: null, consumed: 1 };

    const totalLen = MBAP_HEADER_LEN + pduLength;
    if (buf.length < totalLen) return null; // 数据不足

    const transactionId = buf.readUInt16BE(0);
    const unitId = buf[6];
    const funcCode = buf[7];

    const pdu = { transactionId, unitId, funcCode };

    // 根据功能码解析 PDU 数据
    switch (funcCode) {
      case FC.READ_COILS:
      case FC.READ_DISCRETE_INPUTS:
      case FC.READ_HOLDING_REGISTERS:
      case FC.READ_INPUT_REGISTERS:
        // 读请求: [FC][StartAddr(2)][Quantity(2)] = 5 bytes PDU
        if (pduLength < 6) return { frame: null, consumed: totalLen };
        pdu.startAddress = buf.readUInt16BE(8);
        pdu.quantity = buf.readUInt16BE(10);
        break;

      case FC.WRITE_SINGLE_COIL:
      case FC.WRITE_SINGLE_REGISTER:
        // 写单个: [FC][Addr(2)][Value(2)] = 5 bytes PDU
        if (pduLength < 6) return { frame: null, consumed: totalLen };
        pdu.address = buf.readUInt16BE(8);
        pdu.value = buf.readUInt16BE(10);
        break;

      case FC.WRITE_MULTIPLE_COILS:
      case FC.WRITE_MULTIPLE_REGISTERS:
        // 写多个: [FC][StartAddr(2)][Quantity(2)][ByteCount(1)][Data...]
        if (pduLength < 7) return { frame: null, consumed: totalLen };
        pdu.startAddress = buf.readUInt16BE(8);
        pdu.quantity = buf.readUInt16BE(10);
        pdu.byteCount = buf[12];
        pdu.data = Buffer.from(buf.subarray(13, 13 + pdu.byteCount));
        break;

      default:
        break;
    }

    return { frame: pdu, consumed: totalLen };
  },

  /** 构建读响应 (FC01-04) 的比特类型 */
  buildReadBitsResponse(req, dataBytes) {
    const byteCount = dataBytes.length;
    const len = 1 + 1 + 1 + byteCount; // UnitID + FC + ByteCount + Data
    const resp = Buffer.alloc(MBAP_HEADER_LEN + len);
    resp.writeUInt16BE(req.transactionId, 0);
    resp.writeUInt16BE(0, 2); // ProtocolID
    resp.writeUInt16BE(len, 4);
    resp[6] = req.unitId;
    resp[7] = req.funcCode;
    resp[8] = byteCount;
    dataBytes.copy(resp, 9);
    return resp;
  },

  /** 构建读响应 (FC03/04) 的寄存器类型 */
  buildReadRegistersResponse(req, dataBytes) {
    const byteCount = dataBytes.length;
    const len = 1 + 1 + 1 + byteCount;
    const resp = Buffer.alloc(MBAP_HEADER_LEN + len);
    resp.writeUInt16BE(req.transactionId, 0);
    resp.writeUInt16BE(0, 2);
    resp.writeUInt16BE(len, 4);
    resp[6] = req.unitId;
    resp[7] = req.funcCode;
    resp[8] = byteCount;
    dataBytes.copy(resp, 9);
    return resp;
  },

  /** 构建写单个响应 (FC05/06) - 回显请求 */
  buildWriteSingleResponse(req) {
    const resp = Buffer.alloc(12);
    resp.writeUInt16BE(req.transactionId, 0);
    resp.writeUInt16BE(0, 2);
    resp.writeUInt16BE(6, 4); // UnitID(1) + FC(1) + Addr(2) + Value(2)
    resp[6] = req.unitId;
    resp[7] = req.funcCode;
    resp.writeUInt16BE(req.address, 8);
    resp.writeUInt16BE(req.value, 10);
    return resp;
  },

  /** 构建写多个响应 (FC0F/10) */
  buildWriteMultipleResponse(req) {
    const resp = Buffer.alloc(12);
    resp.writeUInt16BE(req.transactionId, 0);
    resp.writeUInt16BE(0, 2);
    resp.writeUInt16BE(6, 4); // UnitID(1) + FC(1) + StartAddr(2) + Quantity(2)
    resp[6] = req.unitId;
    resp[7] = req.funcCode;
    resp.writeUInt16BE(req.startAddress, 8);
    resp.writeUInt16BE(req.quantity, 10);
    return resp;
  },

  /** 构建异常响应 */
  buildExceptionResponse(req, exceptionCode) {
    const resp = Buffer.alloc(9);
    resp.writeUInt16BE(req.transactionId, 0);
    resp.writeUInt16BE(0, 2);
    resp.writeUInt16BE(3, 4); // UnitID(1) + FC|0x80(1) + ExCode(1)
    resp[6] = req.unitId;
    resp[7] = req.funcCode | 0x80;
    resp[8] = exceptionCode;
    return resp;
  },
};

// ============================================================
// Modbus RTU 帧编解码器 (RTU over TCP)
// ============================================================

const ModbusRtu = {
  /**
   * 从缓冲区尝试解析一帧 RTU 请求
   *
   * RTU 帧无显式长度字段，需要根据功能码推导帧长度：
   *   读请求 (FC01-04): SlaveID(1) + FC(1) + StartAddr(2) + Quantity(2) + CRC(2) = 8
   *   写单个 (FC05/06): SlaveID(1) + FC(1) + Addr(2) + Value(2) + CRC(2) = 8
   *   写多个 (FC0F/10): SlaveID(1) + FC(1) + StartAddr(2) + Qty(2) + ByteCount(1) + Data(N) + CRC(2)
   */
  parse(buf) {
    if (buf.length < 4) return null; // 最小帧: SlaveID + FC + CRC(2)

    const slaveId = buf[0];
    const funcCode = buf[1];

    let frameLen;

    switch (funcCode) {
      case FC.READ_COILS:
      case FC.READ_DISCRETE_INPUTS:
      case FC.READ_HOLDING_REGISTERS:
      case FC.READ_INPUT_REGISTERS:
      case FC.WRITE_SINGLE_COIL:
      case FC.WRITE_SINGLE_REGISTER:
        frameLen = 8; // 固定 8 字节
        break;

      case FC.WRITE_MULTIPLE_COILS:
      case FC.WRITE_MULTIPLE_REGISTERS:
        if (buf.length < 7) return null; // 需要读到 ByteCount
        frameLen = 7 + buf[6] + 2; // header(7) + data(N) + CRC(2)
        break;

      default:
        // 未知功能码，跳过 1 字节
        return { frame: null, consumed: 1 };
    }

    if (buf.length < frameLen) return null; // 数据不足

    // CRC 校验
    if (!verifyCrc(buf, frameLen)) {
      return { frame: null, consumed: 1 }; // CRC 错误，跳 1 字节重新对齐
    }

    const pdu = { transactionId: 0, unitId: slaveId, funcCode };

    switch (funcCode) {
      case FC.READ_COILS:
      case FC.READ_DISCRETE_INPUTS:
      case FC.READ_HOLDING_REGISTERS:
      case FC.READ_INPUT_REGISTERS:
        pdu.startAddress = buf.readUInt16BE(2);
        pdu.quantity = buf.readUInt16BE(4);
        break;

      case FC.WRITE_SINGLE_COIL:
      case FC.WRITE_SINGLE_REGISTER:
        pdu.address = buf.readUInt16BE(2);
        pdu.value = buf.readUInt16BE(4);
        break;

      case FC.WRITE_MULTIPLE_COILS:
      case FC.WRITE_MULTIPLE_REGISTERS:
        pdu.startAddress = buf.readUInt16BE(2);
        pdu.quantity = buf.readUInt16BE(4);
        pdu.byteCount = buf[6];
        pdu.data = Buffer.from(buf.subarray(7, 7 + pdu.byteCount));
        break;
    }

    return { frame: pdu, consumed: frameLen };
  },

  /** 构建读比特响应 (FC01/02) */
  buildReadBitsResponse(req, dataBytes) {
    // SlaveID(1) + FC(1) + ByteCount(1) + Data(N) + CRC(2)
    const len = 3 + dataBytes.length + 2;
    const resp = Buffer.alloc(len);
    resp[0] = req.unitId;
    resp[1] = req.funcCode;
    resp[2] = dataBytes.length;
    dataBytes.copy(resp, 3);
    return appendCrc(resp);
  },

  /** 构建读寄存器响应 (FC03/04) */
  buildReadRegistersResponse(req, dataBytes) {
    const len = 3 + dataBytes.length + 2;
    const resp = Buffer.alloc(len);
    resp[0] = req.unitId;
    resp[1] = req.funcCode;
    resp[2] = dataBytes.length;
    dataBytes.copy(resp, 3);
    return appendCrc(resp);
  },

  /** 构建写单个响应 (FC05/06) - 回显 */
  buildWriteSingleResponse(req) {
    // SlaveID(1) + FC(1) + Addr(2) + Value(2) + CRC(2) = 8
    const resp = Buffer.alloc(8);
    resp[0] = req.unitId;
    resp[1] = req.funcCode;
    resp.writeUInt16BE(req.address, 2);
    resp.writeUInt16BE(req.value, 4);
    return appendCrc(resp);
  },

  /** 构建写多个响应 (FC0F/10) */
  buildWriteMultipleResponse(req) {
    // SlaveID(1) + FC(1) + StartAddr(2) + Quantity(2) + CRC(2) = 8
    const resp = Buffer.alloc(8);
    resp[0] = req.unitId;
    resp[1] = req.funcCode;
    resp.writeUInt16BE(req.startAddress, 2);
    resp.writeUInt16BE(req.quantity, 4);
    return appendCrc(resp);
  },

  /** 构建异常响应 */
  buildExceptionResponse(req, exceptionCode) {
    // SlaveID(1) + FC|0x80(1) + ExCode(1) + CRC(2) = 5
    const resp = Buffer.alloc(5);
    resp[0] = req.unitId;
    resp[1] = req.funcCode | 0x80;
    resp[2] = exceptionCode;
    return appendCrc(resp);
  },
};

// ============================================================
// Modbus 设备寄存器存储
// ============================================================

class ModbusRegisters {
  constructor() {
    this.coils = new Map(Object.entries(INITIAL_COILS).map(([k, v]) => [+k, v]));
    this.discreteInputs = new Map(
      Object.entries(INITIAL_DISCRETE_INPUTS).map(([k, v]) => [+k, v]),
    );
    this.holdingRegisters = new Map(); // 16-bit 寄存器
    this.inputRegisters = new Map();
  }

  // ---- 比特读取 (FC01/02) ----

  readBits(store, startAddr, quantity) {
    const byteCount = Math.ceil(quantity / 8);
    const data = Buffer.alloc(byteCount, 0);
    for (let i = 0; i < quantity; i++) {
      if (store.get(startAddr + i)) {
        data[Math.floor(i / 8)] |= 1 << (i % 8);
      }
    }
    return data;
  }

  readCoils(startAddr, quantity) {
    return this.readBits(this.coils, startAddr, quantity);
  }

  readDiscreteInputs(startAddr, quantity) {
    return this.readBits(this.discreteInputs, startAddr, quantity);
  }

  // ---- 寄存器读取 (FC03/04) ----

  readRegisters(store, startAddr, quantity) {
    const data = Buffer.alloc(quantity * 2, 0);
    for (let i = 0; i < quantity; i++) {
      const val = store.get(startAddr + i) ?? 0;
      data.writeUInt16BE(val & 0xffff, i * 2);
    }
    return data;
  }

  readHoldingRegisters(startAddr, quantity) {
    return this.readRegisters(this.holdingRegisters, startAddr, quantity);
  }

  readInputRegisters(startAddr, quantity) {
    return this.readRegisters(this.inputRegisters, startAddr, quantity);
  }

  // ---- 写操作 ----

  writeSingleCoil(addr, value) {
    const boolVal = value === 0xff00;
    this.coils.set(addr, boolVal);
    return boolVal;
  }

  writeSingleRegister(addr, value) {
    this.holdingRegisters.set(addr, value & 0xffff);
  }

  writeMultipleCoils(startAddr, quantity, data) {
    for (let i = 0; i < quantity; i++) {
      const byteIdx = Math.floor(i / 8);
      const bitIdx = i % 8;
      this.coils.set(startAddr + i, ((data[byteIdx] >> bitIdx) & 1) === 1);
    }
  }

  writeMultipleRegisters(startAddr, quantity, data) {
    for (let i = 0; i < quantity; i++) {
      this.holdingRegisters.set(startAddr + i, data.readUInt16BE(i * 2));
    }
  }

  // ---- 直接设置离散输入（模拟用） ----

  setDiscreteInput(addr, value) {
    this.discreteInputs.set(addr, value);
  }

  getCoil(addr) {
    return this.coils.get(addr) ?? false;
  }
}

// ============================================================
// 颜色输出
// ============================================================

const C = {
  reset: "\x1b[0m",
  red: "\x1b[31m",
  green: "\x1b[32m",
  yellow: "\x1b[33m",
  blue: "\x1b[34m",
  magenta: "\x1b[35m",
  cyan: "\x1b[36m",
  gray: "\x1b[90m",
  bold: "\x1b[1m",
};

const PALETTE = [C.green, C.cyan, C.magenta, C.yellow, C.blue];

function ts() {
  return new Date().toLocaleTimeString("zh-CN", { hour12: false });
}

function hex(buf) {
  return Array.from(buf)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join(" ");
}

function fcName(fc) {
  return FC_NAMES[fc] || `0x${fc.toString(16).padStart(2, "0")}`;
}

// ============================================================
// DTU 模拟客户端
// ============================================================

class DtuClient {
  constructor(device, colorIdx) {
    this.device = device;
    this.color = PALETTE[colorIdx % PALETTE.length];
    this.tag = device.name;
    this.codec = FRAME_MODE === "RTU" ? ModbusRtu : ModbusTcp;
    this.registers = new ModbusRegisters();
    this.registrationPrefix = Buffer.from(device.registration, "ascii");

    this.socket = null;
    this.heartbeatTimer = null;
    this.connected = false;
    this.reconnecting = false;
    this.rxBuffer = Buffer.alloc(0);
    this.stats = { rx: 0, tx: 0, errors: 0 };
  }

  start() {
    this.connect();
  }

  connect() {
    if (this.reconnecting) return;
    this.reconnecting = false;
    this.rxBuffer = Buffer.alloc(0);

    this.log(`连接 ${SERVER_HOST}:${SERVER_PORT} ...`);
    this.socket = new net.Socket();
    this.socket.setNoDelay(true);

    this.socket.connect(SERVER_PORT, SERVER_HOST, () => {
      this.connected = true;
      this.log("TCP 连接建立");
      setTimeout(() => this.sendRegistration(), REGISTRATION_DELAY);
    });

    this.socket.on("data", (data) => this.onData(data));
    this.socket.on("close", () => this.onClose());
    this.socket.on("error", (err) => this.onError(err));
  }

  scheduleReconnect() {
    if (this.reconnecting) return;
    this.reconnecting = true;
    this.connected = false;
    this.stopHeartbeat();
    this.log(`${C.yellow}${RECONNECT_DELAY / 1000}s 后重连${C.reset}`);
    setTimeout(() => {
      this.reconnecting = false;
      this.connect();
    }, RECONNECT_DELAY);
  }

  onClose() {
    this.connected = false;
    this.stopHeartbeat();
    this.log(`${C.red}连接关闭${C.reset}`);
    this.scheduleReconnect();
  }

  onError(err) {
    if (err.code === "ECONNREFUSED" || err.code === "ECONNRESET") {
      this.log(`${C.red}${err.code}${C.reset}`);
    } else {
      this.log(`${C.red}错误: ${err.message}${C.reset}`);
    }
    this.stats.errors++;
  }

  // ---- 注册 & 心跳 ----

  sendRegistration() {
    if (!this.connected) return;
    const buf = Buffer.from(this.device.registration, "ascii");
    this.send(buf);
    this.log(`注册 → "${this.device.registration}" [${hex(buf)}]`);
    this.startHeartbeat();
  }

  startHeartbeat() {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => {
      if (!this.connected) return;
      const buf = Buffer.from(HEARTBEAT_CONTENT, "ascii");
      this.send(buf);
      this.log(`${C.gray}心跳 → "${HEARTBEAT_CONTENT}"${C.reset}`);
    }, HEARTBEAT_INTERVAL);
  }

  stopHeartbeat() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }

  // ---- 数据收发 ----

  send(buf) {
    if (!this.connected || !this.socket || this.socket.destroyed) return;
    this.socket.write(buf);
    this.stats.tx++;
  }

  onData(data) {
    this.rxBuffer = Buffer.concat([this.rxBuffer, data]);
    this.processBuffer();
  }

  processBuffer() {
    while (this.rxBuffer.length > 0) {
      const result = this.codec.parse(this.rxBuffer);

      if (result === null) break; // 数据不足

      const { frame, consumed } = result;
      this.rxBuffer = this.rxBuffer.subarray(consumed);

      if (frame === null) continue; // 无效帧，已跳过

      this.stats.rx++;
      this.handleRequest(frame);
    }
  }

  // ---- 请求处理 ----

  handleRequest(req) {
    const txLabel =
      FRAME_MODE === "TCP" ? ` TxID=${req.transactionId}` : "";
    this.log(
      `← ${C.bold}${fcName(req.funcCode)}${C.reset}` +
        `${txLabel} Unit=${req.unitId}` +
        (req.startAddress !== undefined ? ` Addr=${req.startAddress} Qty=${req.quantity}` : "") +
        (req.address !== undefined ? ` Addr=${req.address} Val=0x${req.value.toString(16)}` : ""),
    );

    let response;

    switch (req.funcCode) {
      case FC.READ_COILS:
        response = this.onReadCoils(req);
        break;
      case FC.READ_DISCRETE_INPUTS:
        response = this.onReadDiscreteInputs(req);
        break;
      case FC.READ_HOLDING_REGISTERS:
        response = this.onReadHoldingRegisters(req);
        break;
      case FC.READ_INPUT_REGISTERS:
        response = this.onReadInputRegisters(req);
        break;
      case FC.WRITE_SINGLE_COIL:
        response = this.onWriteSingleCoil(req);
        break;
      case FC.WRITE_SINGLE_REGISTER:
        response = this.onWriteSingleRegister(req);
        break;
      case FC.WRITE_MULTIPLE_COILS:
        response = this.onWriteMultipleCoils(req);
        break;
      case FC.WRITE_MULTIPLE_REGISTERS:
        response = this.onWriteMultipleRegisters(req);
        break;
      default:
        this.log(`  ${C.red}不支持的功能码${C.reset}`);
        response = this.codec.buildExceptionResponse(req, EXCEPTION.ILLEGAL_FUNCTION);
        break;
    }

    if (response) {
      if (this.device.silent) {
        this.log(`  → ${C.red}[静默] 丢弃响应 ${response.length}B${C.reset}`);
        return;
      }
      // 注册码前缀 + Modbus 响应帧（服务端 RegistrationNormalizer 做 PrefixedPayload 匹配）
      const prefixed = Buffer.concat([this.registrationPrefix, response]);
      this.send(prefixed);
      this.log(`  → ${C.green}响应 ${response.length}B + 前缀 ${this.registrationPrefix.length}B${C.reset} [${hex(prefixed)}]`);
    }
  }

  // ---- FC01: Read Coils ----
  onReadCoils(req) {
    const data = this.registers.readCoils(req.startAddress, req.quantity);
    this.logBitStates("COIL", this.registers.coils, req.startAddress, req.quantity);
    return this.codec.buildReadBitsResponse(req, data);
  }

  // ---- FC02: Read Discrete Inputs ----
  onReadDiscreteInputs(req) {
    const data = this.registers.readDiscreteInputs(req.startAddress, req.quantity);
    this.logBitStates("DI", this.registers.discreteInputs, req.startAddress, req.quantity);
    return this.codec.buildReadBitsResponse(req, data);
  }

  // ---- FC03: Read Holding Registers ----
  onReadHoldingRegisters(req) {
    const data = this.registers.readHoldingRegisters(req.startAddress, req.quantity);
    return this.codec.buildReadRegistersResponse(req, data);
  }

  // ---- FC04: Read Input Registers ----
  onReadInputRegisters(req) {
    const data = this.registers.readInputRegisters(req.startAddress, req.quantity);
    return this.codec.buildReadRegistersResponse(req, data);
  }

  // ---- FC05: Write Single Coil ----
  onWriteSingleCoil(req) {
    if (req.value !== 0xff00 && req.value !== 0x0000) {
      this.log(`  ${C.red}非法线圈值: 0x${req.value.toString(16)}${C.reset}`);
      return this.codec.buildExceptionResponse(req, EXCEPTION.ILLEGAL_DATA_VALUE);
    }
    const boolVal = this.registers.writeSingleCoil(req.address, req.value);
    this.log(`  ${C.yellow}写线圈 [${req.address}] = ${boolVal ? "ON" : "OFF"}${C.reset}`);
    this.simulateValveAction(req.address, boolVal);
    return this.codec.buildWriteSingleResponse(req);
  }

  // ---- FC06: Write Single Register ----
  onWriteSingleRegister(req) {
    this.registers.writeSingleRegister(req.address, req.value);
    this.log(`  ${C.yellow}写寄存器 [${req.address}] = ${req.value}${C.reset}`);
    return this.codec.buildWriteSingleResponse(req);
  }

  // ---- FC0F: Write Multiple Coils ----
  onWriteMultipleCoils(req) {
    this.registers.writeMultipleCoils(req.startAddress, req.quantity, req.data);
    this.log(
      `  ${C.yellow}写多线圈 [${req.startAddress}..${req.startAddress + req.quantity - 1}]${C.reset}`,
    );
    // 检查每个线圈是否触发阀门动作
    for (let i = 0; i < req.quantity; i++) {
      const addr = req.startAddress + i;
      if (this.registers.getCoil(addr)) {
        this.simulateValveAction(addr, true);
      }
    }
    return this.codec.buildWriteMultipleResponse(req);
  }

  // ---- FC10: Write Multiple Registers ----
  onWriteMultipleRegisters(req) {
    this.registers.writeMultipleRegisters(req.startAddress, req.quantity, req.data);
    this.log(
      `  ${C.yellow}写多寄存器 [${req.startAddress}..${req.startAddress + req.quantity - 1}]${C.reset}`,
    );
    return this.codec.buildWriteMultipleResponse(req);
  }

  // ---- 阀门联动模拟 ----

  simulateValveAction(coilAddr, value) {
    if (!value) return;
    const reg = this.registers;

    if (coilAddr === 1024) {
      // 远程开
      this.log(`  ${C.magenta}[SIM] 阀门打开中...${C.reset}`);
      setTimeout(() => {
        reg.setDiscreteInput(1024, true); // 阀门开输出
        reg.setDiscreteInput(1025, false); // 阀门关输出
        reg.setDiscreteInput(1029, true); // 开到位
        reg.setDiscreteInput(1030, false); // 关到位
        reg.coils.set(1024, false); // 指令复位
        this.log(`  ${C.magenta}[SIM] 阀门已打开${C.reset}`);
      }, 2000);
    }

    if (coilAddr === 1025) {
      // 远程关
      this.log(`  ${C.magenta}[SIM] 阀门关闭中...${C.reset}`);
      setTimeout(() => {
        reg.setDiscreteInput(1024, false);
        reg.setDiscreteInput(1025, true);
        reg.setDiscreteInput(1029, false);
        reg.setDiscreteInput(1030, true);
        reg.coils.set(1025, false);
        this.log(`  ${C.magenta}[SIM] 阀门已关闭${C.reset}`);
      }, 2000);
    }

    if (coilAddr === 1026) {
      // 远程停
      this.log(`  ${C.magenta}[SIM] 阀门急停${C.reset}`);
      reg.setDiscreteInput(1024, false);
      reg.setDiscreteInput(1025, false);
      reg.coils.set(1026, false);
    }
  }

  // ---- 日志辅助 ----

  logBitStates(label, store, startAddr, quantity) {
    const parts = [];
    for (let i = 0; i < quantity; i++) {
      const addr = startAddr + i;
      parts.push(`${addr}=${store.get(addr) ? 1 : 0}`);
    }
    this.log(`  ${C.gray}${label}: ${parts.join(" ")}${C.reset}`);
  }

  log(msg) {
    console.log(`${C.gray}${ts()}${C.reset} ${this.color}[${this.tag}]${C.reset} ${msg}`);
  }
}

// ============================================================
// 启动
// ============================================================

console.log(`
${C.cyan}${C.bold}DTU Modbus 模拟器${C.reset}
${"─".repeat(48)}
  服务端:   ${C.green}${SERVER_HOST}:${SERVER_PORT}${C.reset}
  帧模式:   ${C.green}${FRAME_MODE}${C.reset}${FRAME_MODE === "RTU" ? " (CRC16)" : " (MBAP Header)"}
  心跳:     ${C.green}${HEARTBEAT_INTERVAL / 1000}s "${HEARTBEAT_CONTENT}"${C.reset}
  DTU 数量: ${C.green}${DTU_DEVICES.length}${C.reset}

  寄存器:
    COIL     (FC01/05/0F) 1024-1026  远程开/关/停
    DI       (FC02)       1024-1030  阀门状态 x7
    HR       (FC03/06/10) 动态
    IR       (FC04)       动态

  设备:
${DTU_DEVICES.map((d, i) => `    ${PALETTE[i % PALETTE.length]}${d.name}${C.reset}  ${d.registration}  slave=${d.slaveId}`).join("\n")}
${"─".repeat(48)}
`);

DTU_DEVICES.forEach((device, i) => {
  setTimeout(() => new DtuClient(device, i).start(), i * 1000);
});

process.on("SIGINT", () => {
  console.log(`\n${C.yellow}退出${C.reset}`);
  process.exit(0);
});
