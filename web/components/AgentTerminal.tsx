/**
 * Agent WebSSH 终端组件
 *
 * 通过独立 WebSocket 连接与 Agent 的 PTY 通信。
 * 消息流: 前端 → /ws → Server → Agent → PTY
 */

import { inflate } from "pako";
import { useCallback, useEffect, useRef } from "react";

import { useAppSelector } from "@/store/hooks";
import { FitAddon } from "@xterm/addon-fit";
import { WebglAddon } from "@xterm/addon-webgl";
import { Terminal } from "@xterm/xterm";
import "@xterm/xterm/css/xterm.css";

interface AgentTerminalProps {
  agentId: number;
  visible: boolean;
}

export default function AgentTerminal({
  agentId,
  visible,
}: AgentTerminalProps) {
  const token = useAppSelector((s) => s.auth.token);
  const containerRef = useRef<HTMLDivElement>(null);
  const termRef = useRef<Terminal | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const openedRef = useRef(false);

  const cleanup = useCallback(() => {
    if (wsRef.current) {
      if (wsRef.current.readyState === WebSocket.OPEN) {
        wsRef.current.send(
          JSON.stringify({
            type: "shell:close",
            data: { agentId },
          }),
        );
      }
      wsRef.current.onclose = null;
      wsRef.current.close();
      wsRef.current = null;
    }
    if (termRef.current) {
      termRef.current.dispose();
      termRef.current = null;
    }
    fitRef.current = null;
    openedRef.current = false;
  }, [agentId]);

  useEffect(() => {
    if (!visible || !token || !containerRef.current || agentId <= 0) return;

    // 初始化 xterm
    const term = new Terminal({
      cursorBlink: true,
      fontSize: 14,
      fontFamily: "'JetBrains Mono', 'Fira Code', 'Cascadia Code', Menlo, Monaco, 'Courier New', monospace",
      scrollback: 5000,
      fastScrollSensitivity: 5,
      theme: {
        background: "#1e1e2e",
        foreground: "#cdd6f4",
        cursor: "#f5e0dc",
        selectionBackground: "#585b70",
      },
    });
    const fit = new FitAddon();
    term.loadAddon(fit);
    term.open(containerRef.current);

    // WebGL 渲染器 — 大幅提升渲染性能
    try {
      const webgl = new WebglAddon();
      webgl.onContextLoss(() => webgl.dispose());
      term.loadAddon(webgl);
    } catch {
      // WebGL 不可用时回退到 canvas 渲染
    }

    fit.fit();

    termRef.current = term;
    fitRef.current = fit;

    term.writeln("\x1b[33m连接到 Agent...\x1b[0m");

    // 建立独立 WebSocket
    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const url = `${protocol}//${window.location.host}/ws?token=${encodeURIComponent(token)}`;
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer"; // 二进制帧直接返回 ArrayBuffer
    wsRef.current = ws;

    const textDecoder = new TextDecoder();

    ws.onopen = () => {
      // 等收到 connected 确认后再发 shell:open
    };

    ws.onmessage = (event) => {
      // 二进制帧 = shell 输出数据（服务端零拷贝转发）
      if (event.data instanceof ArrayBuffer) {
        const bytes = new Uint8Array(event.data);
        if (bytes.length < 2) return;
        const flags = bytes[0];
        const payload = bytes.subarray(1);
        if (flags & 0x01) {
          // zlib 压缩数据 — 解压
          term.write(textDecoder.decode(inflate(payload)));
        } else {
          term.write(textDecoder.decode(payload));
        }
        return;
      }

      // 文本帧 = JSON 控制消息
      try {
        const msg = JSON.parse(event.data) as {
          type: string;
          data?: Record<string, unknown>;
        };

        if (msg.type === "connected") {
          const { cols, rows } = fit.proposeDimensions() ?? {
            cols: 80,
            rows: 24,
          };
          ws.send(
            JSON.stringify({
              type: "shell:open",
              data: { agentId, cols, rows },
            }),
          );
          return;
        }

        if (msg.type === "shell:opened") {
          if (msg.data?.success) {
            openedRef.current = true;
            term.writeln("\x1b[32m已连接\x1b[0m\r\n");
          } else {
            term.writeln(
              `\x1b[31m连接失败: ${msg.data?.error ?? "未知错误"}\x1b[0m`,
            );
          }
          return;
        }

        if (msg.type === "shell:data") {
          if (typeof msg.data?.data === "string") {
            term.write(msg.data.data);
          }
          return;
        }

        if (msg.type === "shell:closed") {
          const code = msg.data?.exitCode ?? -1;
          const reason = msg.data?.reason;
          openedRef.current = false;
          term.writeln(
            `\r\n\x1b[33m会话已结束${reason === "agent_offline" ? " (Agent 离线)" : ""} (code=${code})\x1b[0m`,
          );
          return;
        }
      } catch {
        // 忽略
      }
    };

    ws.onclose = () => {
      if (openedRef.current) {
        term.writeln("\r\n\x1b[31m连接已断开\x1b[0m");
        openedRef.current = false;
      }
    };

    // 终端输入 → shell:data
    const dataDisposable = term.onData((data) => {
      if (ws.readyState === WebSocket.OPEN && openedRef.current) {
        ws.send(
          JSON.stringify({
            type: "shell:data",
            data: { agentId, data },
          }),
        );
      }
    });

    // 窗口大小变化 → shell:resize（防抖 100ms）
    let resizeTimer: ReturnType<typeof setTimeout> | null = null;
    const resizeObserver = new ResizeObserver(() => {
      if (resizeTimer) clearTimeout(resizeTimer);
      resizeTimer = setTimeout(() => {
        if (!fitRef.current) return;
        fitRef.current.fit();
        if (ws.readyState === WebSocket.OPEN && openedRef.current) {
          const dims = fitRef.current.proposeDimensions();
          if (dims) {
            ws.send(
              JSON.stringify({
                type: "shell:resize",
                data: { agentId, cols: dims.cols, rows: dims.rows },
              }),
            );
          }
        }
      }, 100);
    });
    if (containerRef.current) {
      resizeObserver.observe(containerRef.current);
    }

    return () => {
      if (resizeTimer) clearTimeout(resizeTimer);
      dataDisposable.dispose();
      resizeObserver.disconnect();
      cleanup();
    };
  }, [visible, token, agentId, cleanup]);

  if (!visible) return null;

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%" }}>
      <div
        ref={containerRef}
        style={{
          flex: 1,
          minHeight: 0,
          padding: 4,
          background: "#1e1e2e",
          borderRadius: 6,
        }}
      />
    </div>
  );
}
