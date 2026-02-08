export const deviceGroupKeys = {
  all: ["deviceGroup"] as const,
  tree: () => [...deviceGroupKeys.all, "tree"] as const,
  treeWithCount: () => [...deviceGroupKeys.all, "treeCount"] as const,
  details: () => [...deviceGroupKeys.all, "detail"] as const,
  detail: (id: number) => [...deviceGroupKeys.details(), id] as const,
};
