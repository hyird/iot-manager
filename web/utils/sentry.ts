import * as Sentry from "@sentry/react";

/**
 * 初始化 Sentry 错误监控
 */
export function initSentry() {
  const sentryDsn = import.meta.env.VITE_SENTRY_DSN;

  if (!sentryDsn) {
    // 未配置 DSN 时静默返回，不需要在开发环境输出警告
    return;
  }

  Sentry.init({
    dsn: sentryDsn,
    environment: import.meta.env.MODE,
    integrations: [
      // 浏览器跟踪集成
      Sentry.browserTracingIntegration(),
      // 重放集成（记录用户操作，方便复现错误）
      Sentry.replayIntegration({
        maskAllText: true,
        blockAllMedia: true,
      }),
    ],

    // 性能监控采样率
    tracesSampleRate: import.meta.env.PROD ? 0.1 : 1.0,

    // 会话重放采样率
    replaysSessionSampleRate: 0.1, // 10% 的正常会话
    replaysOnErrorSampleRate: 1.0, // 100% 的错误会话

    // 过滤敏感信息
    beforeSend(event, hint) {
      // 移除敏感的请求头
      if (event.request?.headers) {
        delete event.request.headers.authorization;
        delete event.request.headers.cookie;
      }

      // 在开发环境打印错误到控制台
      if (import.meta.env.DEV) {
        console.error("[Sentry]", hint.originalException || hint.syntheticException);
      }

      return event;
    },
  });

  if (import.meta.env.DEV) {
    // eslint-disable-next-line no-console
    console.log("[Sentry] Initialized successfully");
  }
}

/**
 * 设置用户上下文
 */
export function setUser(user: { id: number; username: string; email?: string | null }) {
  Sentry.setUser({
    id: user.id.toString(),
    username: user.username,
    email: user.email || undefined,
  });
}

/**
 * 清除用户上下文
 */
export function clearUser() {
  Sentry.setUser(null);
}
