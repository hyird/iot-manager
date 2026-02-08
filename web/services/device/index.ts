/**
 * 设备管理模块导出
 */

export * as deviceApi from "./api";
export { deviceKeys } from "./keys";
export { useDeviceCommand, useDeviceDelete, useDeviceSave } from "./mutations";
export { useDeviceDetail, useDeviceHistoryData, useDeviceList, useDeviceOptions } from "./queries";
