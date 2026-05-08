import type { GB28181 } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as api from "./api";
import { gb28181Keys } from "./keys";

export function useGb28181MockRegister() {
  return useMutationWithFeedback({
    mutationFn: api.mockRegister,
    successMessage: "模拟注册已写入",
    invalidateKeys: [gb28181Keys.devices()],
  });
}

export function useGb28181CatalogQuery() {
  return useMutationWithFeedback({
    mutationFn: api.queryCatalog,
    successMessage: "目录查询已发送",
    invalidateKeys: [gb28181Keys.devices()],
  });
}

export function useGb28181PreviewStart() {
  return useMutationWithFeedback<GB28181.PreviewStartResult, GB28181.StartPreviewPayload>({
    mutationFn: api.startPreview,
    successMessage: "预览已发起",
    invalidateKeys: [gb28181Keys.streams()],
  });
}

export function useGb28181PreviewStop() {
  return useMutationWithFeedback<GB28181.PreviewStopResult, GB28181.StopPreviewPayload>({
    mutationFn: api.stopPreview,
    successMessage: "会话已停止",
    invalidateKeys: [gb28181Keys.streams()],
  });
}

export function useGb28181Ptz() {
  return useMutationWithFeedback<GB28181.CommandResult, GB28181.PtzPayload>({
    mutationFn: api.sendPtz,
    successMessage: "云台指令已发送",
  });
}

export function useGb28181RecordQuery() {
  return useMutationWithFeedback<GB28181.CommandResult, GB28181.RecordQueryPayload>({
    mutationFn: api.queryRecords,
    successMessage: "录像查询已发送",
    invalidateKeys: [gb28181Keys.devices()],
  });
}

export function useGb28181PlaybackStart() {
  return useMutationWithFeedback<GB28181.PreviewStartResult, GB28181.RecordQueryPayload>({
    mutationFn: api.startPlayback,
    successMessage: "回放已发起",
    invalidateKeys: [gb28181Keys.streams()],
  });
}
