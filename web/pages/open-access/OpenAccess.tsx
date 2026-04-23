import {
  DeleteOutlined,
  PlusOutlined,
  QuestionCircleOutlined,
  ReloadOutlined,
} from "@ant-design/icons";
import {
  Alert,
  App,
  Button,
  Card,
  DatePicker,
  Descriptions,
  Drawer,
  Form,
  Input,
  InputNumber,
  Modal,
  Popover,
  Result,
  Select,
  Space,
  Switch,
  Table,
  Tabs,
  Tag,
  Tooltip,
  Typography,
} from "antd";
import type { ColumnsType, TablePaginationConfig } from "antd/es/table";
import dayjs, { type Dayjs } from "dayjs";
import { useMemo, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { StatusTag } from "@/components/StatusTag";
import { usePermission } from "@/hooks";
import {
  useAccessKeyDelete,
  useAccessKeyRotate,
  useAccessKeySave,
  useDeviceOptions,
  useOpenAccessKeys,
  useOpenAccessLogs,
  useOpenWebhooks,
  useWebhookDelete,
  useWebhookSave,
} from "@/services";
import type { Device, OpenAccess } from "@/types";

const { Search, TextArea } = Input;

const TAB_WEBHOOK = "webhook";
const TAB_LOG = "access-log";

const EVENT_TYPE_OPTIONS: Array<{ label: string; value: OpenAccess.EventType; color: string }> = [
  { label: "数据上报", value: "device.data.reported", color: "blue" },
  { label: "图片上报", value: "device.image.reported", color: "cyan" },
  { label: "命令下发", value: "device.command.dispatched", color: "orange" },
  { label: "命令应答", value: "device.command.responded", color: "geekblue" },
  { label: "告警触发", value: "device.alert.triggered", color: "red" },
  { label: "告警恢复", value: "device.alert.resolved", color: "green" },
];

const STATUS_OPTIONS: Array<{ label: string; value: OpenAccess.Status }> = [
  { label: "启用", value: "enabled" },
  { label: "禁用", value: "disabled" },
];

const LOG_DIRECTION_OPTIONS: Array<{ label: string; value: OpenAccess.LogDirection }> = [
  { label: "主动调用", value: "pull" },
  { label: "Webhook 推送", value: "push" },
];

const LOG_STATUS_OPTIONS: Array<{ label: string; value: OpenAccess.LogStatus }> = [
  { label: "成功", value: "success" },
  { label: "失败", value: "failed" },
];

const LOG_ACTION_OPTIONS = [
  { label: "设备列表", value: "device-list" },
  { label: "实时查询", value: "realtime" },
  { label: "历史查询", value: "history" },
  { label: "控制下发", value: "command" },
  { label: "告警查询", value: "alert" },
  { label: "Webhook 推送", value: "webhook" },
];

const AUTH_HEADER_EXAMPLE = [
  "X-Access-Key: <你的AccessKey>",
  "Authorization: AccessKey <你的AccessKey>",
].join("\n");

const DEVICE_LIST_QUERY_EXAMPLE = [
  'curl -X GET "https://your-host/open-api/device/list?page=1&pageSize=50" \\',
  '  -H "X-Access-Key: <你的AccessKey>"',
  "",
  "# 建议先获取设备列表，后续统一使用返回的 device.id",
].join("\n");

const REALTIME_QUERY_EXAMPLE = [
  'curl -X GET "https://your-host/open-api/device/realtime?deviceId=123&page=1&pageSize=20" \\',
  '  -H "X-Access-Key: <你的AccessKey>"',
  "",
  "# code 参数仅保留兼容旧调用方，推荐统一使用 deviceId",
  'curl -X GET "https://your-host/open-api/device/realtime?code=ST001" \\',
  '  -H "Authorization: AccessKey <你的AccessKey>"',
].join("\n");

const HISTORY_QUERY_EXAMPLE = [
  'curl -X GET "https://your-host/open-api/device/history?deviceId=123&dataType=ELEMENT&startTime=2026-03-09T00:00:00Z&endTime=2026-03-09T23:59:59Z" \\',
  '  -H "X-Access-Key: <你的AccessKey>"',
  "",
  "# 历史查询必须提供 deviceId（或兼容传 code），并且必须带 startTime / endTime",
  "# dataType 必填：ELEMENT（要素数据）或 IMAGE（图片数据）",
].join("\n");

const COMMAND_QUERY_EXAMPLE = [
  'curl -X POST "https://your-host/open-api/device/command" \\',
  '  -H "Content-Type: application/json" \\',
  '  -H "X-Access-Key: <你的AccessKey>" \\',
  "  -d '{",
  '    "deviceId": 123,',
  '    "elements": [',
  '      { "elementId": "001", "value": "1" }',
  "    ]",
  "  }'",
].join("\n");

const ALERT_QUERY_EXAMPLE = [
  'curl -X GET "https://your-host/open-api/device/alert?deviceId=123&status=active&severity=critical&page=1&pageSize=20" \\',
  '  -H "X-Access-Key: <你的AccessKey>"',
  "",
  "# 可按 deviceId（或兼容传 code）、status、severity、ruleId 过滤",
].join("\n");

const WEBHOOK_HEADER_EXAMPLE = [
  "X-IOT-Event: device.data.reported",
  "X-IOT-Timestamp: 2026-03-09T08:00:00Z",
  "X-IOT-Signature: sha256=<hmac>",
  "",
  "# 接收方返回 2xx 视为成功，否则平台会记录为失败",
].join("\n");

const WEBHOOK_PAYLOAD_EXAMPLE = JSON.stringify(
  {
    event: "device.data.reported",
    timestamp: "2026-03-09T08:00:00Z",
    reportTime: "2026-03-09T07:59:58Z",
    deliveryId: "2d4c7f14-8d79-4d39-8db7-2fe8b1d2a201",
    webhookId: 12,
    accessKeyId: 3,
    accessKeyName: "第三方数据平台",
    device: {
      id: 123,
      name: "泵站一号",
      deviceCode: "ST001",
      linkId: 8,
      protocol: "SL651",
    },
    data: {
      elements: [{ name: "水位", value: 12.34, unit: "m" }],
    },
  },
  null,
  2
);

interface AccessKeyFormValues {
  id?: number;
  name: string;
  status: OpenAccess.Status;
  allowRealtime: boolean;
  allowHistory: boolean;
  allowCommand: boolean;
  allowAlert: boolean;
  deviceIds: number[];
  expiresAt?: Dayjs | null;
  remark?: string;
}

interface WebhookHeaderFormItem {
  key: string;
  value: string;
}

interface WebhookFormValues {
  id?: number;
  accessKeyId: number;
  name: string;
  url: string;
  status: OpenAccess.Status;
  eventTypes: OpenAccess.EventType[];
  timeoutSeconds: number;
  secret?: string;
  headers?: WebhookHeaderFormItem[];
}

interface UsageCodeBlockProps {
  title: string;
  code: string;
  description: string;
}

interface JsonPayloadCardProps {
  title: string;
  content?: string;
  emptyText?: string;
}

const formatDateTime = (value?: string | null) => {
  if (!value) return "--";
  const parsed = dayjs(value);
  return parsed.isValid() ? parsed.format("YYYY-MM-DD HH:mm:ss") : value;
};

const toHeaderList = (
  headers: Record<string, string | number | boolean>
): WebhookHeaderFormItem[] =>
  Object.entries(headers).map(([key, value]) => ({
    key,
    value: String(value),
  }));

const toHeaderMap = (headers?: WebhookHeaderFormItem[]): Record<string, string> =>
  Object.fromEntries(
    (headers ?? [])
      .map((item) => ({
        key: item.key.trim(),
        value: item.value.trim(),
      }))
      .filter((item) => item.key.length > 0)
      .map((item) => [item.key, item.value])
  );

const renderEventTypes = (eventTypes: OpenAccess.EventType[]) => (
  <Space size={[4, 4]} wrap>
    {eventTypes.map((eventType) => {
      const option = EVENT_TYPE_OPTIONS.find((item) => item.value === eventType);
      return (
        <Tag key={eventType} color={option?.color ?? "default"}>
          {option?.label ?? eventType}
        </Tag>
      );
    })}
  </Space>
);

const getEventTypeMeta = (eventType?: string | null) =>
  EVENT_TYPE_OPTIONS.find((item) => item.value === eventType);

const formatPayload = (payload?: Record<string, unknown>) => {
  if (!payload || Object.keys(payload).length === 0) return "";
  return JSON.stringify(payload, null, 2);
};

function UsageCodeBlock({ title, code, description }: UsageCodeBlockProps) {
  return (
    <div className="rounded-xl border border-slate-200 bg-slate-50 p-4">
      <div className="mb-2 text-sm font-medium text-slate-800">{title}</div>
      <div className="mb-3 text-xs leading-5 text-slate-500">{description}</div>
      <Typography.Paragraph
        copyable={{ text: code, tooltips: ["复制示例", "已复制"] }}
        className="!mb-0 whitespace-pre-wrap rounded-lg bg-slate-950 p-3 font-mono text-xs !text-slate-100"
      >
        {code}
      </Typography.Paragraph>
    </div>
  );
}

function JsonPayloadCard({ title, content, emptyText = "暂无内容" }: JsonPayloadCardProps) {
  return (
    <div className="rounded-xl border border-slate-200 bg-slate-50 p-4">
      <div className="mb-3 text-sm font-medium text-slate-800">{title}</div>
      {content ? (
        <Typography.Paragraph
          copyable={{ text: content, tooltips: ["复制内容", "已复制"] }}
          className="!mb-0 max-h-[360px] overflow-auto whitespace-pre-wrap rounded-lg bg-slate-950 p-3 font-mono text-xs !text-slate-100"
        >
          {content}
        </Typography.Paragraph>
      ) : (
        <div className="rounded-lg bg-white px-3 py-6 text-center text-sm text-slate-400">
          {emptyText}
        </div>
      )}
    </div>
  );
}

function AccessKeyFormModal({
  open,
  editing,
  loading,
  deviceOptions,
  deviceOptionsLoading,
  deviceOptionsError,
  deviceOptionsUnavailable,
  onCancel,
  onFinish,
}: {
  open: boolean;
  editing: OpenAccess.AccessKeyItem | null;
  loading: boolean;
  deviceOptions: Device.Option[];
  deviceOptionsLoading: boolean;
  deviceOptionsError: boolean;
  deviceOptionsUnavailable: boolean;
  onCancel: () => void;
  onFinish: (values: OpenAccess.AccessKeyPayload & { id?: number }) => void;
}) {
  const [form] = Form.useForm<AccessKeyFormValues>();
  const { message } = App.useApp();

  const handleOpenChange = (isOpen: boolean) => {
    if (!isOpen) return;

    if (editing) {
      form.setFieldsValue({
        id: editing.id,
        name: editing.name,
        status: editing.status,
        allowRealtime: editing.allowRealtime,
        allowHistory: editing.allowHistory,
        allowCommand: editing.allowCommand,
        allowAlert: editing.allowAlert,
        deviceIds: editing.deviceIds,
        expiresAt: editing.expiresAt ? dayjs(editing.expiresAt) : null,
        remark: editing.remark ?? undefined,
      });
      return;
    }

    form.resetFields();
    form.setFieldsValue({
      status: "enabled",
      allowRealtime: true,
      allowHistory: true,
      allowCommand: false,
      allowAlert: false,
      deviceIds: [],
      expiresAt: null,
    });
  };

  return (
    <Modal
      open={open}
      title={editing ? "编辑主动调用配置" : "新建主动调用配置"}
      onCancel={() => {
        onCancel();
        form.resetFields();
      }}
      onOk={() => form.submit()}
      confirmLoading={loading}
      destroyOnHidden
      width={640}
      afterOpenChange={handleOpenChange}
    >
      <Form<AccessKeyFormValues>
        form={form}
        layout="vertical"
        onFinish={(values) => {
          if (
            !values.allowRealtime &&
            !values.allowHistory &&
            !values.allowCommand &&
            !values.allowAlert
          ) {
            message.error("实时、历史、控制、告警权限不能同时关闭");
            return;
          }

          onFinish({
            id: values.id,
            name: values.name.trim(),
            status: values.status,
            allowRealtime: values.allowRealtime,
            allowHistory: values.allowHistory,
            allowCommand: values.allowCommand,
            allowAlert: values.allowAlert,
            deviceIds: values.deviceIds,
            expiresAt: values.expiresAt ? values.expiresAt.toISOString() : null,
            remark: values.remark?.trim() || null,
          });
        }}
      >
        {(deviceOptionsUnavailable || deviceOptionsError) && (
          <Alert
            type="warning"
            showIcon
            className="mb-4"
            message={deviceOptionsUnavailable ? "缺少设备查询权限" : "设备列表加载失败"}
            description={
              deviceOptionsUnavailable ? (
                <>
                  当前账号没有 `iot:device:query` 权限。
                  <br />
                  可以继续查看或编辑其他字段，但无法在这里选择具体设备。
                </>
              ) : (
                <>
                  配置设备范围需要读取设备选项。
                  <br />
                  请检查设备接口或稍后重试。
                </>
              )
            }
          />
        )}

        <Form.Item name="id" hidden>
          <Input />
        </Form.Item>

        <Form.Item
          label="配置名称"
          name="name"
          rules={[{ required: true, message: "请输入配置名称" }]}
        >
          <Input maxLength={64} placeholder="例如：第三方数据平台" />
        </Form.Item>

        <Form.Item
          label="可访问设备"
          name="deviceIds"
          rules={[{ type: "array", required: true, min: 1, message: "请至少选择一个设备" }]}
          extra="该设备范围同时用于主动查询和 webhook 推送"
        >
          <Select
            mode="multiple"
            allowClear
            disabled={deviceOptionsUnavailable}
            showSearch
            loading={deviceOptionsLoading}
            placeholder={deviceOptionsUnavailable ? "当前账号无设备查询权限" : "选择允许访问的设备"}
            optionFilterProp="label"
            options={deviceOptions.map((device) => ({
              label: `${device.name} (${device.device_code || `ID:${device.id}`})`,
              value: device.id,
            }))}
          />
        </Form.Item>

        <div className="grid grid-cols-1 gap-4 md:grid-cols-2 xl:grid-cols-4">
          <Form.Item label="实时查询" name="allowRealtime" valuePropName="checked">
            <Switch checkedChildren="允许" unCheckedChildren="关闭" />
          </Form.Item>
          <Form.Item label="历史查询" name="allowHistory" valuePropName="checked">
            <Switch checkedChildren="允许" unCheckedChildren="关闭" />
          </Form.Item>
          <Form.Item label="控制下发" name="allowCommand" valuePropName="checked">
            <Switch checkedChildren="允许" unCheckedChildren="关闭" />
          </Form.Item>
          <Form.Item label="告警查询" name="allowAlert" valuePropName="checked">
            <Switch checkedChildren="允许" unCheckedChildren="关闭" />
          </Form.Item>
        </div>

        <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
          <Form.Item label="状态" name="status" rules={[{ required: true, message: "请选择状态" }]}>
            <Select options={STATUS_OPTIONS} />
          </Form.Item>
          <Form.Item label="过期时间" name="expiresAt">
            <DatePicker showTime allowClear className="!w-full" placeholder="为空表示不过期" />
          </Form.Item>
        </div>

        <Form.Item label="备注" name="remark">
          <TextArea rows={3} maxLength={200} placeholder="记录用途、对接方、联系人等信息" />
        </Form.Item>
      </Form>
    </Modal>
  );
}

function WebhookFormModal({
  open,
  editing,
  loading,
  accessKeys,
  onCancel,
  onFinish,
}: {
  open: boolean;
  editing: OpenAccess.WebhookItem | null;
  loading: boolean;
  accessKeys: OpenAccess.AccessKeyItem[];
  onCancel: () => void;
  onFinish: (
    values:
      | (OpenAccess.WebhookPayload & { id?: number })
      | (Partial<OpenAccess.WebhookPayload> & { id?: number })
  ) => void;
}) {
  const [form] = Form.useForm<WebhookFormValues>();
  const selectedAccessKeyId = Form.useWatch("accessKeyId", form);
  const selectedAccessKey = accessKeys.find((item) => item.id === selectedAccessKeyId);

  const handleOpenChange = (isOpen: boolean) => {
    if (!isOpen) return;

    if (editing) {
      form.setFieldsValue({
        id: editing.id,
        accessKeyId: editing.accessKeyId,
        name: editing.name,
        url: editing.url,
        status: editing.status,
        eventTypes: editing.eventTypes,
        timeoutSeconds: editing.timeoutSeconds,
        secret: "",
        headers: toHeaderList(editing.headers),
      });
      return;
    }

    form.resetFields();
    form.setFieldsValue({
      accessKeyId: accessKeys.length === 1 ? accessKeys[0]?.id : undefined,
      status: "enabled",
      timeoutSeconds: 5,
      eventTypes: ["device.data.reported"],
      headers: [],
    });
  };

  return (
    <Modal
      open={open}
      title={editing ? "编辑 Webhook" : "新建 Webhook"}
      onCancel={() => {
        onCancel();
        form.resetFields();
      }}
      onOk={() => form.submit()}
      confirmLoading={loading}
      destroyOnHidden
      width={760}
      afterOpenChange={handleOpenChange}
    >
      <Form<WebhookFormValues>
        form={form}
        layout="vertical"
        onFinish={(values) => {
          const payload: Partial<OpenAccess.WebhookPayload> & { id?: number } = {
            id: values.id,
            accessKeyId: values.accessKeyId,
            name: values.name.trim(),
            url: values.url.trim(),
            status: values.status,
            timeoutSeconds: values.timeoutSeconds,
            eventTypes: values.eventTypes,
            headers: toHeaderMap(values.headers),
          };

          const secret = values.secret?.trim();
          if (secret) {
            payload.secret = secret;
          } else if (!values.id) {
            payload.secret = "";
          }

          onFinish(payload);
        }}
      >
        <Alert
          type="info"
          showIcon
          className="mb-4"
          message="设备范围说明"
          description={
            <>
              Webhook 不单独配置设备。
              <br />
              推送范围完全继承所绑定调用配置的设备列表。
            </>
          }
        />

        <Form.Item name="id" hidden>
          <Input />
        </Form.Item>

        <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
          <Form.Item
            label="绑定调用配置"
            name="accessKeyId"
            rules={[{ required: true, message: "请选择调用配置" }]}
            extra={
              selectedAccessKey
                ? `当前继承 ${selectedAccessKey.deviceIds.length} 台设备`
                : "请选择一个已配置设备范围的调用配置"
            }
          >
            <Select
              showSearch
              placeholder="选择调用配置"
              optionFilterProp="label"
              options={accessKeys.map((item) => ({
                label: `${item.name} (${item.accessKeyPrefix})`,
                value: item.id,
                disabled: item.deviceIds.length === 0,
              }))}
            />
          </Form.Item>

          <Form.Item label="状态" name="status" rules={[{ required: true, message: "请选择状态" }]}>
            <Select options={STATUS_OPTIONS} />
          </Form.Item>
        </div>

        <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
          <Form.Item
            label="名称"
            name="name"
            rules={[{ required: true, message: "请输入 Webhook 名称" }]}
          >
            <Input maxLength={64} placeholder="例如：运营平台回调" />
          </Form.Item>
          <Form.Item
            label="超时秒数"
            name="timeoutSeconds"
            rules={[{ required: true, message: "请输入超时秒数" }]}
          >
            <InputNumber min={1} max={30} className="!w-full" placeholder="1-30 秒" />
          </Form.Item>
        </div>

        <Form.Item
          label="回调地址"
          name="url"
          rules={[
            { required: true, message: "请输入回调地址" },
            { type: "url", message: "请输入合法的 URL" },
          ]}
        >
          <Input placeholder="https://example.com/webhook" />
        </Form.Item>

        <Form.Item
          label="订阅事件"
          name="eventTypes"
          rules={[{ type: "array", required: true, min: 1, message: "请至少选择一个事件" }]}
        >
          <Select
            mode="multiple"
            options={EVENT_TYPE_OPTIONS.map((item) => ({
              label: item.label,
              value: item.value,
            }))}
            placeholder="选择需要推送的事件"
          />
        </Form.Item>

        <Form.Item
          label="签名密钥"
          name="secret"
          extra={editing ? "留空表示保持原密钥不变" : "可选，用于生成 X-IOT-Signature"}
        >
          <Input.Password placeholder="不填则不启用签名密钥" />
        </Form.Item>

        <Form.List name="headers">
          {(fields, { add, remove }) => (
            <div className="rounded-lg border border-dashed border-slate-300 p-4">
              <div className="mb-3 flex items-center justify-between gap-2">
                <div>
                  <div className="font-medium text-slate-800">自定义 Header</div>
                  <div className="text-xs text-slate-500">用于透传租户标识、来源系统等额外信息</div>
                </div>
                <Button type="dashed" icon={<PlusOutlined />} onClick={() => add()}>
                  添加 Header
                </Button>
              </div>

              {fields.length === 0 && (
                <div className="rounded-md bg-slate-50 px-3 py-2 text-sm text-slate-500">
                  当前没有额外 Header
                </div>
              )}

              {fields.map((field) => (
                <div key={field.key} className="mb-3 flex items-start gap-2 last:mb-0">
                  <Form.Item
                    className="mb-0 flex-1"
                    name={[field.name, "key"]}
                    rules={[{ required: true, message: "请输入 Header 名称" }]}
                  >
                    <Input placeholder="X-Tenant-Id" />
                  </Form.Item>
                  <Form.Item
                    className="mb-0 flex-1"
                    name={[field.name, "value"]}
                    rules={[{ required: true, message: "请输入 Header 值" }]}
                  >
                    <Input placeholder="tenant-a" />
                  </Form.Item>
                  <Button
                    danger
                    type="text"
                    icon={<DeleteOutlined />}
                    onClick={() => remove(field.name)}
                  />
                </div>
              ))}
            </div>
          )}
        </Form.List>
      </Form>
    </Modal>
  );
}

export default function OpenAccessPage() {
  const { modal } = App.useApp();
  const [activeTab, setActiveTab] = useState(TAB_WEBHOOK);
  const [webhookKeyword, setWebhookKeyword] = useState("");
  const [logKeyword, setLogKeyword] = useState("");
  const [logQuery, setLogQuery] = useState<OpenAccess.AccessLogQuery>({
    page: 1,
    pageSize: 20,
  });
  const [accessKeyModalOpen, setAccessKeyModalOpen] = useState(false);
  const [webhookModalOpen, setWebhookModalOpen] = useState(false);
  const [usageModalOpen, setUsageModalOpen] = useState(false);
  const [editingAccessKey, setEditingAccessKey] = useState<OpenAccess.AccessKeyItem | null>(null);
  const [editingWebhook, setEditingWebhook] = useState<OpenAccess.WebhookItem | null>(null);
  const [selectedAccessLog, setSelectedAccessLog] = useState<OpenAccess.AccessLogItem | null>(null);

  const canQuery = usePermission("iot:open-access:query");
  const canAdd = usePermission("iot:open-access:add");
  const canEdit = usePermission("iot:open-access:edit");
  const canDelete = usePermission("iot:open-access:delete");
  const canViewDevices = usePermission("iot:device:query");

  const { data: accessKeys = [], isLoading: loadingAccessKeys } = useOpenAccessKeys({
    enabled: canQuery,
  });
  const { data: webhooks = [], isLoading: loadingWebhooks } = useOpenWebhooks(undefined, {
    enabled: canQuery,
  });
  const { data: accessLogPage, isLoading: loadingAccessLogs } = useOpenAccessLogs(logQuery, {
    enabled: canQuery,
  });
  const {
    data: deviceOptionsData,
    isLoading: loadingDeviceOptions,
    isError: deviceOptionsError,
  } = useDeviceOptions({
    enabled: canQuery && canViewDevices,
  });

  const accessKeySaveMutation = useAccessKeySave();
  const accessKeyRotateMutation = useAccessKeyRotate();
  const accessKeyDeleteMutation = useAccessKeyDelete();
  const webhookSaveMutation = useWebhookSave();
  const webhookDeleteMutation = useWebhookDelete();

  const deviceOptions = deviceOptionsData?.list ?? [];
  const accessLogs = accessLogPage?.list ?? [];
  const deviceOptionMap = useMemo(
    () => new Map(deviceOptions.map((device) => [device.id, device])),
    [deviceOptions]
  );
  const accessKeyEnabledCount = accessKeys.filter((item) => item.status === "enabled").length;
  const webhookEnabledCount = webhooks.filter((item) => item.status === "enabled").length;
  const coveredDeviceCount = new Set(accessKeys.flatMap((item) => item.deviceIds)).size;
  const accessLogTotal = accessLogPage?.total ?? 0;

  const resolveDeviceLabel = (deviceId: number) => {
    const device = deviceOptionMap.get(deviceId);
    if (!device) return `ID:${deviceId}`;
    return `${device.name}${device.device_code ? ` (${device.device_code})` : ""}`;
  };

  const renderDeviceScope = (deviceIds: number[]) => {
    if (deviceIds.length === 0) return "--";

    const labels = deviceIds.map((deviceId) => resolveDeviceLabel(deviceId));
    const visibleLabels = labels.slice(0, 2);
    const hiddenLabels = labels.slice(2);

    return (
      <div className="max-w-[300px]">
        <div className="mb-1 text-xs text-slate-500">{labels.length} 台设备</div>
        <Space size={[4, 4]} wrap>
          {visibleLabels.map((label) => (
            <Tag key={label}>{label}</Tag>
          ))}
          {hiddenLabels.length > 0 && (
            <Popover
              trigger="hover"
              title="设备明细"
              content={
                <div className="max-w-[360px]">
                  <Space size={[4, 4]} wrap>
                    {labels.map((label) => (
                      <Tag key={label}>{label}</Tag>
                    ))}
                  </Space>
                </div>
              }
            >
              <Tag color="processing">+{hiddenLabels.length}</Tag>
            </Popover>
          )}
        </Space>
      </div>
    );
  };

  const filteredWebhooks = (() => {
    const keyword = webhookKeyword.trim().toLowerCase();
    if (!keyword) return webhooks;

    return webhooks.filter((item) =>
      [
        item.name,
        item.url,
        item.accessKeyName,
        item.lastError ?? "",
        item.deviceIds.map((deviceId) => resolveDeviceLabel(deviceId)).join(" "),
      ]
        .join(" ")
        .toLowerCase()
        .includes(keyword)
    );
  })();

  const filteredAccessLogs = (() => {
    const keyword = logKeyword.trim().toLowerCase();
    if (!keyword) return accessLogs;

    return accessLogs.filter((item) =>
      [
        item.accessKeyName ?? "",
        item.webhookName ?? "",
        item.action,
        item.eventType ?? "",
        item.target ?? "",
        item.deviceCode ?? "",
        item.message ?? "",
        item.deviceId ? resolveDeviceLabel(item.deviceId) : "",
      ]
        .join(" ")
        .toLowerCase()
        .includes(keyword)
    );
  })();

  const getLogActionLabel = (action: string) =>
    LOG_ACTION_OPTIONS.find((item) => item.value === action)?.label ?? action;

  const updateLogQuery = (patch: Partial<OpenAccess.AccessLogQuery>) => {
    setLogQuery((previous) => ({
      ...previous,
      ...patch,
      page: 1,
    }));
  };

  const handleAccessLogTableChange = (pagination: TablePaginationConfig) => {
    setLogQuery((previous) => ({
      ...previous,
      page: pagination.current ?? previous.page ?? 1,
      pageSize: pagination.pageSize ?? previous.pageSize ?? 20,
    }));
  };

  const renderAccessKeyPermissions = (record: OpenAccess.AccessKeyItem) => {
    const permissions = [
      record.allowRealtime ? { label: "实时", color: "blue" } : null,
      record.allowHistory ? { label: "历史", color: "gold" } : null,
      record.allowCommand ? { label: "控制", color: "purple" } : null,
      record.allowAlert ? { label: "告警", color: "red" } : null,
    ].filter((item): item is { label: string; color: string } => item !== null);

    if (permissions.length === 0) {
      return <Tag>无</Tag>;
    }

    return (
      <Space size={[4, 4]} wrap>
        {permissions.map((item) => (
          <Tag key={item.label} color={item.color}>
            {item.label}
          </Tag>
        ))}
      </Space>
    );
  };

  const renderLogPayload = (title: string, payload: Record<string, unknown>) => {
    const content = formatPayload(payload);
    if (!content) return null;

    return (
      <Popover
        trigger="click"
        title={title}
        content={
          <Typography.Paragraph
            copyable={{ text: content, tooltips: ["复制内容", "已复制"] }}
            className="!mb-0 max-h-[360px] max-w-[520px] overflow-auto whitespace-pre-wrap rounded-lg bg-slate-950 p-3 font-mono text-xs !text-slate-100"
          >
            {content}
          </Typography.Paragraph>
        }
      >
        <Button size="small" type="link" className="!px-0">
          {title}
        </Button>
      </Popover>
    );
  };

  const selectedLogRequestPayload = formatPayload(selectedAccessLog?.requestPayload);
  const selectedLogResponsePayload = formatPayload(selectedAccessLog?.responsePayload);
  const selectedLogEventMeta = getEventTypeMeta(selectedAccessLog?.eventType);

  const showAccessKeySecret = (result: OpenAccess.AccessKeyCreateResult, title: string) => {
    modal.info({
      title,
      width: 640,
      okText: "我已保存",
      content: (
        <div className="space-y-3">
          <Alert
            type="warning"
            showIcon
            message="该认证 Key 只会展示这一次"
            description={
              <>
                请立即复制保存。
                <br />
                后续页面只保留前缀，无法再次查看完整明文。
              </>
            }
          />
          <div className="rounded-lg bg-slate-50 p-3">
            <div className="mb-2 text-sm text-slate-500">完整认证 Key</div>
            <Typography.Paragraph
              copyable={{ text: result.accessKey }}
              className="!mb-0 break-all font-mono text-sm"
            >
              {result.accessKey}
            </Typography.Paragraph>
          </div>
        </div>
      ),
    });
  };

  const openCreateAccessKeyModal = () => {
    setEditingAccessKey(null);
    setAccessKeyModalOpen(true);
  };

  const openEditAccessKeyModal = (record: OpenAccess.AccessKeyItem) => {
    setEditingAccessKey(record);
    setAccessKeyModalOpen(true);
  };

  const openCreateWebhookModal = () => {
    setEditingWebhook(null);
    setWebhookModalOpen(true);
  };

  const openEditWebhookModal = (record: OpenAccess.WebhookItem) => {
    setEditingWebhook(record);
    setWebhookModalOpen(true);
  };

  const onRotateAccessKey = (record: OpenAccess.AccessKeyItem) => {
    modal.confirm({
      title: `确认轮换「${record.name}」的认证 Key 吗？`,
      content: "旧 key 会立即失效，第三方系统需要同步更新新 key。",
      okText: "确认轮换",
      onOk: async () => {
        const result = await accessKeyRotateMutation.mutateAsync(record.id);
        showAccessKeySecret(result, "认证 Key 已轮换");
      },
    });
  };

  const onDeleteAccessKey = (record: OpenAccess.AccessKeyItem) => {
    modal.confirm({
      title: `确认删除「${record.name}」吗？`,
      content: (
        <>
          删除后该调用配置的数据查询、设备控制、告警查询与 webhook 推送都会失效。
          <br />
          此操作不可撤销。
        </>
      ),
      okText: "确定删除",
      okButtonProps: { danger: true },
      onOk: () => accessKeyDeleteMutation.mutateAsync(record.id),
    });
  };

  const onDeleteWebhook = (record: OpenAccess.WebhookItem) => {
    modal.confirm({
      title: `确认删除「${record.name}」吗？`,
      content: "删除后将停止该回调地址的事件推送，此操作不可撤销。",
      okText: "确定删除",
      okButtonProps: { danger: true },
      onOk: () => webhookDeleteMutation.mutateAsync(record.id),
    });
  };

  if (!canQuery) {
    return (
      <PageContainer>
        <Result
          status="403"
          title="无权限"
          subTitle={"您没有开放接入查询权限，请联系管理员分配 iot:open-access:query"}
        />
      </PageContainer>
    );
  }

  const accessKeyColumns: ColumnsType<OpenAccess.AccessKeyItem> = [
    {
      title: "名称",
      dataIndex: "name",
      width: 180,
    },
    {
      title: "认证 Key 前缀",
      dataIndex: "accessKeyPrefix",
      width: 160,
      render: (value: string) => (
        <Typography.Text code className="font-mono text-xs">
          {value}
        </Typography.Text>
      ),
    },
    {
      title: "状态",
      dataIndex: "status",
      width: 90,
      render: (value: OpenAccess.Status) => <StatusTag status={value} />,
    },
    {
      title: "访问权限",
      key: "permissions",
      width: 240,
      render: (_, record) => renderAccessKeyPermissions(record),
    },
    {
      title: "设备范围",
      key: "deviceScope",
      width: 320,
      render: (_, record) => renderDeviceScope(record.deviceIds),
    },
    {
      title: "Webhook 数",
      dataIndex: "webhookCount",
      width: 110,
    },
    {
      title: "过期时间",
      dataIndex: "expiresAt",
      width: 180,
      render: (value: string | null | undefined) => formatDateTime(value),
    },
    {
      title: "最近使用",
      key: "lastUsed",
      width: 220,
      render: (_, record) =>
        record.lastUsedAt ? (
          <div className="leading-5">
            <div>{formatDateTime(record.lastUsedAt)}</div>
            <div className="text-xs text-slate-500">{record.lastUsedIp || "--"}</div>
          </div>
        ) : (
          "--"
        ),
    },
    {
      title: "备注",
      dataIndex: "remark",
      ellipsis: true,
      render: (value: string | null | undefined) =>
        value ? (
          <Tooltip title={value}>
            <span className="inline-block max-w-[220px] truncate">{value}</span>
          </Tooltip>
        ) : (
          "--"
        ),
    },
    {
      title: "操作",
      key: "actions",
      width: 220,
      fixed: "right",
      render: (_, record) => (
        <Space wrap>
          {canEdit && (
            <Button type="link" onClick={() => openEditAccessKeyModal(record)}>
              编辑
            </Button>
          )}
          {canEdit && (
            <Button type="link" icon={<ReloadOutlined />} onClick={() => onRotateAccessKey(record)}>
              轮换
            </Button>
          )}
          {canDelete && (
            <Button type="link" danger onClick={() => onDeleteAccessKey(record)}>
              删除
            </Button>
          )}
        </Space>
      ),
    },
  ];

  const webhookColumns: ColumnsType<OpenAccess.WebhookItem> = [
    {
      title: "名称",
      dataIndex: "name",
      width: 180,
    },
    {
      title: "绑定调用配置",
      dataIndex: "accessKeyName",
      width: 180,
    },
    {
      title: "回调地址",
      dataIndex: "url",
      render: (value: string) => (
        <Tooltip title={value}>
          <span className="inline-block max-w-[260px] truncate">{value}</span>
        </Tooltip>
      ),
    },
    {
      title: "事件",
      dataIndex: "eventTypes",
      width: 220,
      render: (value: OpenAccess.EventType[]) => renderEventTypes(value),
    },
    {
      title: "继承设备",
      key: "deviceScope",
      width: 320,
      render: (_, record) => renderDeviceScope(record.deviceIds),
    },
    {
      title: "超时",
      dataIndex: "timeoutSeconds",
      width: 90,
      render: (value: number) => `${value}s`,
    },
    {
      title: "状态",
      dataIndex: "status",
      width: 90,
      render: (value: OpenAccess.Status) => <StatusTag status={value} />,
    },
    {
      title: "最近触发",
      dataIndex: "lastTriggeredAt",
      width: 180,
      render: (value: string | null | undefined) => formatDateTime(value),
    },
    {
      title: "最近结果",
      key: "lastResult",
      width: 220,
      render: (_, record) => {
        if (record.lastFailureAt) {
          return (
            <Tooltip title={record.lastError || "推送失败"}>
              <div className="leading-5">
                <Tag color="error" className="!mr-0">
                  失败
                </Tag>
                <div className="text-xs text-slate-500">
                  {formatDateTime(record.lastFailureAt)}
                  {record.lastHttpStatus ? ` / ${record.lastHttpStatus}` : ""}
                </div>
              </div>
            </Tooltip>
          );
        }

        if (record.lastSuccessAt) {
          return (
            <div className="leading-5">
              <Tag color="success" className="!mr-0">
                成功
              </Tag>
              <div className="text-xs text-slate-500">{formatDateTime(record.lastSuccessAt)}</div>
            </div>
          );
        }

        return "--";
      },
    },
    {
      title: "签名",
      key: "secret",
      width: 90,
      render: (_, record) =>
        record.hasSecret ? <Tag color="purple">已配置</Tag> : <Tag>未配置</Tag>,
    },
    {
      title: "操作",
      key: "actions",
      width: 160,
      fixed: "right",
      render: (_, record) => (
        <Space wrap>
          {canEdit && (
            <Button type="link" onClick={() => openEditWebhookModal(record)}>
              编辑
            </Button>
          )}
          {canDelete && (
            <Button type="link" danger onClick={() => onDeleteWebhook(record)}>
              删除
            </Button>
          )}
        </Space>
      ),
    },
  ];

  const accessLogColumns: ColumnsType<OpenAccess.AccessLogItem> = [
    {
      title: "时间",
      dataIndex: "createdAt",
      width: 170,
      render: (value: string) => formatDateTime(value),
    },
    {
      title: "调用配置",
      key: "accessKey",
      width: 220,
      render: (_, record) => (
        <div className="leading-5">
          <div>{record.accessKeyName ?? "--"}</div>
          <div className="text-xs text-slate-500">
            {record.webhookName ? `Webhook：${record.webhookName}` : "--"}
          </div>
        </div>
      ),
    },
    {
      title: "来源",
      dataIndex: "direction",
      width: 110,
      render: (value: OpenAccess.LogDirection) => (
        <Tag color={value === "pull" ? "blue" : "purple"}>
          {value === "pull" ? "主动调用" : "Webhook 推送"}
        </Tag>
      ),
    },
    {
      title: "动作 / 事件",
      key: "action",
      width: 220,
      render: (_, record) => {
        const eventMeta = getEventTypeMeta(record.eventType);

        return (
          <div className="leading-5">
            <div>{getLogActionLabel(record.action)}</div>
            <div className="text-xs text-slate-500">
              {record.eventType ? (
                <Tag color={eventMeta?.color ?? "default"} className="!mr-0">
                  {eventMeta?.label ?? record.eventType}
                </Tag>
              ) : (
                "--"
              )}
            </div>
          </div>
        );
      },
    },
    {
      title: "设备",
      key: "device",
      width: 220,
      render: (_, record) => {
        if (!record.deviceId && !record.deviceCode) return "--";

        return (
          <div className="leading-5">
            <div>{record.deviceId ? resolveDeviceLabel(record.deviceId) : record.deviceCode}</div>
            <div className="text-xs text-slate-500">
              {record.deviceCode ? `编码：${record.deviceCode}` : `ID：${record.deviceId}`}
            </div>
          </div>
        );
      },
    },
    {
      title: "目标",
      key: "target",
      width: 240,
      render: (_, record) => {
        const method = record.httpMethod?.toUpperCase();
        const target = record.target ?? "--";

        return (
          <Tooltip title={target}>
            <div className="leading-5">
              <div className="truncate">{target}</div>
              <div className="text-xs text-slate-500">{method || "--"}</div>
            </div>
          </Tooltip>
        );
      },
    },
    {
      title: "结果",
      key: "status",
      width: 130,
      render: (_, record) => (
        <div className="leading-5">
          <Tag color={record.status === "success" ? "success" : "error"} className="!mr-0">
            {record.status === "success" ? "成功" : "失败"}
          </Tag>
          <div className="text-xs text-slate-500">HTTP {record.httpStatus ?? "--"}</div>
        </div>
      ),
    },
    {
      title: "摘要",
      dataIndex: "message",
      width: 220,
      render: (value: string | null | undefined) =>
        value ? (
          <Tooltip title={value}>
            <span className="inline-block max-w-[200px] truncate">{value}</span>
          </Tooltip>
        ) : (
          "--"
        ),
    },
    {
      title: "载荷",
      key: "payload",
      width: 150,
      render: (_, record) => {
        const requestButton = renderLogPayload("请求体", record.requestPayload);
        const responseButton = renderLogPayload("响应体", record.responsePayload);

        if (!requestButton && !responseButton) return "--";

        return (
          <Space size={8} wrap>
            {requestButton}
            {responseButton}
          </Space>
        );
      },
    },
    {
      title: "操作",
      key: "actions",
      width: 90,
      fixed: "right",
      render: (_, record) => (
        <Button type="link" onClick={() => setSelectedAccessLog(record)}>
          详情
        </Button>
      ),
    },
  ];

  return (
    <PageContainer
      header={
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <h3 className="m-0 text-base font-medium">开放接入</h3>
            {activeTab === TAB_WEBHOOK && (
              <Tag color="processing">设备范围继承调用配置，不单独配置</Tag>
            )}
          </div>
          <Space wrap>
            <Search
              allowClear
              value={activeTab === TAB_WEBHOOK ? webhookKeyword : logKeyword}
              placeholder={
                activeTab === TAB_WEBHOOK
                  ? "搜索名称 / 回调地址 / 调用配置 / 设备 / 错误信息"
                  : "搜索调用配置 / 设备 / 目标 / 事件 / 摘要"
              }
              onChange={(event) => {
                const value = event.target.value;
                if (activeTab === TAB_WEBHOOK) {
                  setWebhookKeyword(value);
                  return;
                }
                setLogKeyword(value);
              }}
              onSearch={(value) => {
                if (activeTab === TAB_WEBHOOK) {
                  setWebhookKeyword(value);
                  return;
                }
                setLogKeyword(value);
              }}
              className="w-full sm:w-[320px]"
            />
            <Button icon={<QuestionCircleOutlined />} onClick={() => setUsageModalOpen(true)}>
              使用说明
            </Button>
            {canAdd && (
              <Button type="primary" icon={<PlusOutlined />} onClick={openCreateAccessKeyModal}>
                新建调用配置
              </Button>
            )}
            {activeTab === TAB_WEBHOOK && canAdd && (
              <Button
                type="default"
                icon={<PlusOutlined />}
                disabled={accessKeys.length === 0}
                onClick={openCreateWebhookModal}
              >
                新建 Webhook
              </Button>
            )}
          </Space>
        </div>
      }
    >
      <div className="mb-4 grid gap-4 md:grid-cols-2 xl:grid-cols-4">
        <Card size="small" className="border-slate-200">
          <div className="text-xs uppercase tracking-[0.2em] text-slate-400">调用配置</div>
          <div className="mt-2 text-3xl font-semibold text-slate-900">{accessKeys.length}</div>
          <div className="mt-2 text-sm text-slate-500">
            启用 {accessKeyEnabledCount} / 总数 {accessKeys.length}
          </div>
        </Card>
        <Card size="small" className="border-slate-200">
          <div className="text-xs uppercase tracking-[0.2em] text-slate-400">Webhook</div>
          <div className="mt-2 text-3xl font-semibold text-slate-900">{webhooks.length}</div>
          <div className="mt-2 text-sm text-slate-500">
            启用 {webhookEnabledCount} / 总数 {webhooks.length}
          </div>
        </Card>
        <Card size="small" className="border-slate-200">
          <div className="text-xs uppercase tracking-[0.2em] text-slate-400">设备覆盖</div>
          <div className="mt-2 text-3xl font-semibold text-slate-900">{coveredDeviceCount}</div>
          <div className="mt-2 text-sm text-slate-500">
            当前所有调用配置共覆盖 {coveredDeviceCount} 台设备
          </div>
        </Card>
        <Card size="small" className="border-slate-200">
          <div className="text-xs uppercase tracking-[0.2em] text-slate-400">调用记录</div>
          <div className="mt-2 text-3xl font-semibold text-slate-900">{accessLogTotal}</div>
          <div className="mt-2 text-sm text-slate-500">
            包含主动查询、控制下发、告警查询和 Webhook 推送结果
          </div>
        </Card>
      </div>

      {!canViewDevices && (
        <Alert
          type="warning"
          showIcon
          className="mb-4"
          message="当前账号没有设备查询权限"
          description="列表里的设备范围会退化为设备 ID 显示；如果要看到具体设备名称，请补充 iot:device:query 权限。"
        />
      )}

      <div className="mb-4 space-y-4">
        <Alert
          type="info"
          showIcon
          message="调用配置说明"
          description="支持创建多个 AccessKey；每个调用配置可单独配置设备范围、查询权限、控制权限和告警权限，Webhook 再绑定到对应配置。"
        />
        <Table<OpenAccess.AccessKeyItem>
          rowKey="id"
          columns={accessKeyColumns}
          dataSource={accessKeys}
          loading={loadingAccessKeys}
          pagination={false}
          scroll={{ x: 1450 }}
          locale={{
            emptyText: canAdd ? "暂无调用配置，请点击右上角“新建调用配置”" : "暂无调用配置",
          }}
        />
      </div>

      <div className="rounded-xl border border-slate-200 bg-white">
        <Tabs
          activeKey={activeTab}
          onChange={setActiveTab}
          className="px-4 pt-3"
          items={[
            {
              key: TAB_WEBHOOK,
              label: `Webhook (${webhooks.length})`,
              children: (
                <div className="space-y-4">
                  <Alert
                    type="info"
                    showIcon
                    message="推送规则"
                    description={
                      <>
                        Webhook 按事件类型分发，设备范围继承绑定调用配置。
                        <br />
                        若还没有调用配置，请先创建调用配置再配置 Webhook。
                      </>
                    }
                  />
                  <Table<OpenAccess.WebhookItem>
                    rowKey="id"
                    columns={webhookColumns}
                    dataSource={filteredWebhooks}
                    loading={loadingWebhooks}
                    pagination={false}
                    scroll={{ x: 1650 }}
                  />
                </div>
              ),
            },
            {
              key: TAB_LOG,
              label: `调用记录 (${accessLogTotal})`,
              children: (
                <div className="space-y-4">
                  <Alert
                    type="info"
                    showIcon
                    message="记录说明"
                    description={
                      <>
                        `pull` 表示第三方主动调用实时查询、历史查询、控制下发或告警查询。
                        <br />
                        `push` 表示平台向第三方 Webhook 地址推送事件后的交付结果。
                      </>
                    }
                  />

                  <div className="flex flex-wrap gap-3">
                    <Select
                      allowClear
                      value={logQuery.accessKeyId}
                      placeholder="筛选调用配置"
                      className="min-w-[180px]"
                      options={accessKeys.map((item) => ({
                        label: item.name,
                        value: item.id,
                      }))}
                      onChange={(value) => updateLogQuery({ accessKeyId: value })}
                    />
                    <Select
                      allowClear
                      value={logQuery.direction}
                      placeholder="筛选来源"
                      className="min-w-[160px]"
                      options={LOG_DIRECTION_OPTIONS}
                      onChange={(value) =>
                        updateLogQuery({ direction: value as OpenAccess.LogDirection | undefined })
                      }
                    />
                    <Select
                      allowClear
                      value={logQuery.action}
                      placeholder="筛选动作"
                      className="min-w-[160px]"
                      options={LOG_ACTION_OPTIONS}
                      onChange={(value) => updateLogQuery({ action: value as string | undefined })}
                    />
                    <Select
                      allowClear
                      value={logQuery.status}
                      placeholder="筛选结果"
                      className="min-w-[160px]"
                      options={LOG_STATUS_OPTIONS}
                      onChange={(value) =>
                        updateLogQuery({ status: value as OpenAccess.LogStatus | undefined })
                      }
                    />
                    <Button
                      onClick={() => {
                        setLogKeyword("");
                        setLogQuery({
                          page: 1,
                          pageSize: logQuery.pageSize ?? 20,
                        });
                      }}
                    >
                      重置筛选
                    </Button>
                  </div>

                  <Table<OpenAccess.AccessLogItem>
                    rowKey="id"
                    columns={accessLogColumns}
                    dataSource={filteredAccessLogs}
                    loading={loadingAccessLogs}
                    onChange={handleAccessLogTableChange}
                    pagination={{
                      current: accessLogPage?.page ?? logQuery.page ?? 1,
                      pageSize: accessLogPage?.pageSize ?? logQuery.pageSize ?? 20,
                      total: accessLogTotal,
                      showSizeChanger: true,
                    }}
                    scroll={{ x: 1780 }}
                  />
                </div>
              ),
            },
          ]}
        />
      </div>

      <AccessKeyFormModal
        open={accessKeyModalOpen}
        editing={editingAccessKey}
        loading={accessKeySaveMutation.isPending}
        deviceOptions={deviceOptions}
        deviceOptionsLoading={loadingDeviceOptions}
        deviceOptionsError={deviceOptionsError}
        deviceOptionsUnavailable={!canViewDevices}
        onCancel={() => {
          setAccessKeyModalOpen(false);
          setEditingAccessKey(null);
        }}
        onFinish={(values) => {
          accessKeySaveMutation.mutate(values, {
            onSuccess: (result) => {
              setAccessKeyModalOpen(false);
              setEditingAccessKey(null);
              if (result && "accessKey" in result) {
                showAccessKeySecret(result, "认证 Key 已创建");
              }
            },
          });
        }}
      />

      <WebhookFormModal
        open={webhookModalOpen}
        editing={editingWebhook}
        loading={webhookSaveMutation.isPending}
        accessKeys={accessKeys}
        onCancel={() => {
          setWebhookModalOpen(false);
          setEditingWebhook(null);
        }}
        onFinish={(values) => {
          webhookSaveMutation.mutate(values, {
            onSuccess: () => {
              setWebhookModalOpen(false);
              setEditingWebhook(null);
            },
          });
        }}
      />

      <Modal
        open={usageModalOpen}
        title="开放接入使用说明"
        footer={null}
        width={980}
        destroyOnHidden
        onCancel={() => setUsageModalOpen(false)}
      >
        <Tabs
          size="small"
          items={[
            {
              key: "usage-call",
              label: "数据查询与控制",
              children: (
                <div className="space-y-4">
                  <Alert
                    type="info"
                    showIcon
                    message="主动调用说明"
                    description={
                      <>
                        一个调用配置可统一管理实时查询、历史查询、设备控制下发和告警查询。
                        <br />
                        建议先调用设备列表接口拿到授权范围内的 `deviceId`，后续查询和控制统一传
                        `deviceId`；`code` 仅保留兼容。实时查询与告警查询不传设备时会返回当前
                        AccessKey 授权范围内的数据。
                      </>
                    }
                  />
                  <div className="mx-auto max-w-4xl space-y-4">
                    <UsageCodeBlock
                      title="1. 鉴权方式"
                      description="当前仅支持通过请求头传递 AccessKey，避免密钥暴露在 URL 与代理日志中。"
                      code={AUTH_HEADER_EXAMPLE}
                    />
                    <UsageCodeBlock
                      title="2. 设备列表"
                      description="先拿当前 AccessKey 可访问设备的 id、名称、协议、状态等摘要，后续统一用 deviceId。"
                      code={DEVICE_LIST_QUERY_EXAMPLE}
                    />
                    <UsageCodeBlock
                      title="3. 实时数据查询"
                      description="推荐按设备 ID 查询，也支持分页；code 仅保留兼容旧调用方。"
                      code={REALTIME_QUERY_EXAMPLE}
                    />
                    <UsageCodeBlock
                      title="4. 历史数据查询"
                      description="必须指定 dataType（ELEMENT 或 IMAGE）和时间范围，推荐统一传 deviceId。"
                      code={HISTORY_QUERY_EXAMPLE}
                    />
                    <UsageCodeBlock
                      title="5. 控制下发"
                      description="当前为 POST JSON 请求，推荐指定 deviceId，并传入 elements。"
                      code={COMMAND_QUERY_EXAMPLE}
                    />
                    <UsageCodeBlock
                      title="6. 告警查询"
                      description="支持按设备、状态、严重级别、规则等条件筛选；不传设备时返回授权范围内的告警。"
                      code={ALERT_QUERY_EXAMPLE}
                    />
                  </div>
                </div>
              ),
            },
            {
              key: "usage-webhook",
              label: "Webhook 推送",
              children: (
                <div className="space-y-4">
                  <Alert
                    type="info"
                    showIcon
                    message="推送说明"
                    description={
                      <>
                        Webhook 只按事件类型过滤，设备范围继承绑定的调用配置。
                        <br />
                        事件包括：{renderEventTypes(EVENT_TYPE_OPTIONS.map((item) => item.value))}
                      </>
                    }
                  />
                  <div className="mx-auto max-w-4xl space-y-4">
                    <UsageCodeBlock
                      title="1. 接收方需要关注的请求头"
                      description="配置了 secret 时才会带 X-IOT-Signature，用于验签。"
                      code={WEBHOOK_HEADER_EXAMPLE}
                    />
                    <UsageCodeBlock
                      title="2. Webhook 推送体示例"
                      description="命令下发、命令应答事件会额外包含 command 对象；data 字段内容随协议解析结果变化。"
                      code={WEBHOOK_PAYLOAD_EXAMPLE}
                    />
                  </div>
                </div>
              ),
            },
          ]}
        />
      </Modal>

      <Drawer
        title="调用记录详情"
        open={selectedAccessLog !== null}
        width={880}
        destroyOnHidden
        onClose={() => setSelectedAccessLog(null)}
      >
        {selectedAccessLog && (
          <div className="space-y-4">
            <Descriptions
              bordered
              size="small"
              column={2}
              items={[
                {
                  key: "createdAt",
                  label: "时间",
                  children: formatDateTime(selectedAccessLog.createdAt),
                },
                {
                  key: "status",
                  label: "结果",
                  children: (
                    <Space size={8} wrap>
                      <Tag color={selectedAccessLog.status === "success" ? "success" : "error"}>
                        {selectedAccessLog.status === "success" ? "成功" : "失败"}
                      </Tag>
                      <span>HTTP {selectedAccessLog.httpStatus ?? "--"}</span>
                    </Space>
                  ),
                },
                {
                  key: "accessKey",
                  label: "调用配置",
                  children: selectedAccessLog.accessKeyName ?? "--",
                },
                {
                  key: "webhook",
                  label: "Webhook",
                  children: selectedAccessLog.webhookName ?? "--",
                },
                {
                  key: "direction",
                  label: "来源",
                  children: (
                    <Tag color={selectedAccessLog.direction === "pull" ? "blue" : "purple"}>
                      {selectedAccessLog.direction === "pull" ? "主动调用" : "Webhook 推送"}
                    </Tag>
                  ),
                },
                {
                  key: "action",
                  label: "动作",
                  children: getLogActionLabel(selectedAccessLog.action),
                },
                {
                  key: "eventType",
                  label: "事件",
                  children: selectedAccessLog.eventType ? (
                    <Tag color={selectedLogEventMeta?.color ?? "default"}>
                      {selectedLogEventMeta?.label ?? selectedAccessLog.eventType}
                    </Tag>
                  ) : (
                    "--"
                  ),
                },
                {
                  key: "httpMethod",
                  label: "HTTP 方法",
                  children: selectedAccessLog.httpMethod?.toUpperCase() ?? "--",
                },
                {
                  key: "target",
                  label: "目标",
                  span: 2,
                  children: selectedAccessLog.target ?? "--",
                },
                {
                  key: "device",
                  label: "设备",
                  children: selectedAccessLog.deviceId
                    ? resolveDeviceLabel(selectedAccessLog.deviceId)
                    : (selectedAccessLog.deviceCode ?? "--"),
                },
                {
                  key: "deviceCode",
                  label: "设备编码",
                  children: selectedAccessLog.deviceCode ?? "--",
                },
                {
                  key: "requestIp",
                  label: "请求 IP",
                  children: selectedAccessLog.requestIp ?? "--",
                },
                {
                  key: "message",
                  label: "摘要",
                  children: selectedAccessLog.message ?? "--",
                },
              ]}
            />

            <div className="grid gap-4 xl:grid-cols-2">
              <JsonPayloadCard
                title="请求体"
                content={selectedLogRequestPayload}
                emptyText="当前记录没有请求体"
              />
              <JsonPayloadCard
                title="响应体"
                content={selectedLogResponsePayload}
                emptyText="当前记录没有响应体"
              />
            </div>
          </div>
        )}
      </Drawer>
    </PageContainer>
  );
}
