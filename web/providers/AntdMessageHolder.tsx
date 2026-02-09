/**
 * 将 Ant Design App 上下文中的 message 实例暴露给非组件代码（如 Axios 拦截器）
 * 必须在 <App> 内部渲染
 */

import { App } from "antd";
import type { MessageInstance } from "antd/es/message/interface";

let messageInstance: MessageInstance;

/** 获取全局 message 实例（供 Axios 拦截器等非组件代码使用） */
export function getAntdMessage(): MessageInstance {
  return messageInstance;
}

export function AntdMessageHolder() {
  const { message } = App.useApp();
  messageInstance = message;
  return null;
}
