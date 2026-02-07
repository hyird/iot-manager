/**
 * 首页 QueryKeys
 */

export const homeKeys = {
  all: ["home"] as const,
  stats: () => ["home", "stats"] as const,
  systemInfo: () => ["home", "system"] as const,
  monitor: () => ["home", "monitor"] as const,
};
