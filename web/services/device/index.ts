/**
 * 设备管理模块导出
 */

export * as deviceApi from "./api";
export { deviceKeys } from "./keys";
export {
  useDeviceCommand,
  useDeviceDelete,
  useDeviceSave,
  useDeviceShareDelete,
  useDeviceShareSave,
} from "./mutations";
export {
  useDeviceDetail,
  useDeviceHistoryData,
  useDeviceList,
  useDeviceOptions,
  useDeviceShares,
  useDeviceStatic,
} from "./queries";
