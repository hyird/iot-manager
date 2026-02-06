/**
 * SL651 协议配置 - 共享类型和常量
 */

import type { useProtocolConfigSave } from "@/services";
import type { SL651 } from "@/types";

/** 编码类型列表 */
export const EncodeList: SL651.EncodeType[] = ["BCD", "TIME_YYMMDDHHMMSS", "JPEG", "DICT", "HEX"];

/** 生成唯一 ID */
export const generateId = () => crypto.randomUUID();

/** SaveMutation 类型（避免每个 Modal 重复定义） */
export type SaveMutation = ReturnType<typeof useProtocolConfigSave>;

/** 表单中的条件数据（可能不完整） */
export interface FormCondition {
  bitIndex?: string;
  bitValue?: string;
}

/** 表单中的映射项数据（可能不完整） */
export interface FormMapItem {
  key?: string;
  label?: string;
  value?: string;
  dependsOn?: {
    operator?: "AND" | "OR";
    conditions?: FormCondition[];
  };
}
