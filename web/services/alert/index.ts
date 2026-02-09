/**
 * 告警管理模块导出
 */

export * as alertApi from "./api";
export { alertKeys } from "./keys";
export {
  useAlertAcknowledge,
  useAlertApplyTemplate,
  useAlertBatchAcknowledge,
  useAlertRuleBatchDelete,
  useAlertRuleDelete,
  useAlertRuleSave,
  useAlertTemplateDelete,
  useAlertTemplateSave,
} from "./mutations";
export {
  useAlertGroupedRecords,
  useAlertRecordList,
  useAlertRuleList,
  useAlertStats,
  useAlertTemplateList,
} from "./queries";
