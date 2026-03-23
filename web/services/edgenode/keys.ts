/**
 * EdgeNode QueryKeys
 */

import { createQueryKeys } from "../common";

export const agentKeys = createQueryKeys("agents");

export const agentQueryKeys = {
  ...agentKeys,
  list: () => [...agentKeys.all, "list"] as const,
  options: () => [...agentKeys.all, "options"] as const,
  events: (id: number, hours = 24, limit = 100) =>
    [...agentKeys.all, "events", id, hours, limit] as const,
  endpoints: (agentId: number) => [...agentKeys.all, "endpoints", agentId] as const,
};
