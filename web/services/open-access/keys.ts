/**
 * 开放接入 Query Keys
 */

import type { OpenAccess } from "@/types";

export const openAccessKeys = {
  all: ["open-access"] as const,
  accessKeys: () => [...openAccessKeys.all, "access-keys"] as const,
  webhooks: (params?: OpenAccess.WebhookQuery) =>
    [...openAccessKeys.all, "webhooks", params] as const,
  logs: (params?: OpenAccess.AccessLogQuery) => [...openAccessKeys.all, "logs", params] as const,
};
