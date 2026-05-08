import { createQueryKeys } from "../common";

export const gb28181Keys = {
  ...createQueryKeys("gb28181"),
  health: () => ["gb28181", "health"] as const,
  sipConfig: () => ["gb28181", "sipConfig"] as const,
  devices: () => ["gb28181", "devices"] as const,
  streams: () => ["gb28181", "streams"] as const,
};
