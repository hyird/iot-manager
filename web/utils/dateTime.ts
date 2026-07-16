import dayjs from "dayjs";

export type DateTimeValue = string | number | Date | null | undefined;

export const DATE_TIME_DISPLAY_FORMAT = "YYYY-MM-DD HH:mm:ss";
export const DATE_DISPLAY_FORMAT = "YYYY-MM-DD";
export const TIME_DISPLAY_FORMAT = "HH:mm:ss";
export const DATE_TIME_COLUMN_WIDTH = 180;

const normalizeTimestamp = (value: DateTimeValue): DateTimeValue => {
  if (typeof value === "number" && Math.abs(value) < 100_000_000_000) {
    return value * 1000;
  }

  if (typeof value === "string" && /^-?\d{10}$/.test(value.trim())) {
    return Number(value) * 1000;
  }

  return value;
};

const formatValue = (value: DateTimeValue, format: string, fallback: string) => {
  if (value === null || value === undefined || value === "") return fallback;

  const parsed = dayjs(normalizeTimestamp(value));
  return parsed.isValid() ? parsed.format(format) : fallback;
};

/** 统一的前端日期时间展示，按浏览器本地时区输出固定长度。 */
export const formatDateTime = (value: DateTimeValue, fallback = "--") =>
  formatValue(value, DATE_TIME_DISPLAY_FORMAT, fallback);

/** 仅显示日期。 */
export const formatDate = (value: DateTimeValue, fallback = "--") =>
  formatValue(value, DATE_DISPLAY_FORMAT, fallback);

/** 仅显示时间。 */
export const formatTime = (value: DateTimeValue, fallback = "--") =>
  formatValue(value, TIME_DISPLAY_FORMAT, fallback);

/** 图表坐标轴使用的紧凑月日格式。 */
export const formatMonthDay = (value: DateTimeValue, fallback = "--") =>
  formatValue(value, "MM-DD", fallback);
