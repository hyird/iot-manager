/**
 * 设备管理模块导出
 */

export * as deviceApi from "./api";
export { deviceKeys } from "./keys";
export { useDeviceList, useDeviceDetail, useDeviceOptions, useDeviceHistoryData } from "./queries";
export { useDeviceSave, useDeviceDelete, useDeviceCommand } from "./mutations";
