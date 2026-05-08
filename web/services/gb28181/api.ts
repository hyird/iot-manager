import type { GB28181 } from "@/types";
import { request } from "../common";

const BASE = "/api/gb28181";

const pathPart = (value: string) => encodeURIComponent(value);

export function stopPreviewKeepalive(sessionId: string, token?: string | null) {
  const headers = new Headers();
  if (token) {
    headers.set("Authorization", `Bearer ${token}`);
  }

  void fetch(`${BASE}/previews/${pathPart(sessionId)}/stop`, {
    method: "POST",
    headers,
    credentials: "same-origin",
    keepalive: true,
  }).catch(() => undefined);
}

export function getHealth() {
  return request.get<GB28181.Health>(`${BASE}/health`, { _silent: true });
}

export function getSipConfig() {
  return request.get<GB28181.SipConfig>(`${BASE}/config/sip`);
}

export function getDevices() {
  return request.get<GB28181.Items<GB28181.Device>>(`${BASE}/devices`);
}

export function getStreams() {
  return request.get<GB28181.Items<GB28181.StreamStatus>>(`${BASE}/streams`);
}

export function mockRegister(deviceId: string) {
  return request.post<GB28181.MockRegisterResult>(`${BASE}/devices/mock-register`, undefined, {
    params: { device_id: deviceId },
  });
}

export function queryCatalog(deviceId: string) {
  return request.post<GB28181.CommandResult>(`${BASE}/devices/${pathPart(deviceId)}/catalog/query`);
}

export function startPreview(payload: GB28181.StartPreviewPayload) {
  return request.post<GB28181.PreviewStartResult>(
    `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/preview/start`
  );
}

export function stopPreview(payload: GB28181.StopPreviewPayload) {
  return request.post<GB28181.PreviewStopResult>(
    `${BASE}/previews/${pathPart(payload.sessionId)}/stop`
  );
}

export function sendPtz(payload: GB28181.PtzPayload) {
  return request.post<GB28181.CommandResult>(
    `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/ptz/${payload.action}`,
    undefined,
    { params: { speed: payload.speed } }
  );
}

export function queryRecords(payload: GB28181.RecordQueryPayload) {
  return request.post<GB28181.CommandResult>(
    `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/records/query`,
    undefined,
    {
      params: {
        start_time: payload.startTime,
        end_time: payload.endTime,
      },
    }
  );
}

export function startPlayback(payload: GB28181.RecordQueryPayload) {
  return request.post<GB28181.PreviewStartResult>(
    `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/playback/start`,
    undefined,
    {
      params: {
        start_time: payload.startTime,
        end_time: payload.endTime,
      },
    }
  );
}
