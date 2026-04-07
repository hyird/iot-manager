import { createQueryKeys } from "../common";

const deviceGroupBaseKeys = createQueryKeys("deviceGroup");

export const deviceGroupKeys = {
  ...deviceGroupBaseKeys,
  tree: () => [...deviceGroupBaseKeys.all, "tree"] as const,
  treeWithCount: () => [...deviceGroupBaseKeys.all, "treeCount"] as const,
};
