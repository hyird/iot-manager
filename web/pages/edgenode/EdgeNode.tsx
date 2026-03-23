import {
  CodeOutlined,
  DeleteOutlined,
  EditOutlined,
  FullscreenExitOutlined,
  FullscreenOutlined,
  HistoryOutlined,
  SyncOutlined,
} from "@ant-design/icons";
import {
  Button,
  Empty,
  Flex,
  Form,
  Input,
  InputNumber,
  Modal,
  Popconfirm,
  Result,
  Select,
  Space,
  Tag,
  Tooltip,
} from "antd";
import { lazy, Suspense, useMemo, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import {
  useAgentCreate,
  useAgentDelete,
  useAgentEndpointCreate,
  useAgentEndpointDelete,
  useAgentEndpointUpdate,
  useAgentEvents,
  useAgentList,
  useAgentNetworkConfig,
  useAgentResync,
  useAgentUpdate,
} from "@/services";
import type { Agent } from "@/types";

const EdgeNodeTerminal = lazy(() => import("@/components/EdgeNodeTerminal"));
const { Search } = Input;

const configStatusConfig: Record<string, { label: string; color: string }> = {
  idle: { label: "空闲", color: "default" },
  pending: { label: "待应用", color: "processing" },
  applied: { label: "已应用", color: "success" },
  failed: { label: "失败", color: "error" },
};

const eventLevelConfig: Record<string, { label: string; color: string }> = {
  info: { label: "信息", color: "blue" },
  success: { label: "成功", color: "success" },
  warning: { label: "告警", color: "warning" },
  error: { label: "错误", color: "error" },
};

function formatConfigVersion(value?: number) {
  return value && value > 0 ? String(value) : "-";
}

function getManagedDeviceCount(agent: Agent.Item) {
  return agent.managed_device_count ?? agent.runtime?.managedDeviceCount ?? 0;
}

function getInterfaces(agent?: Agent.Item | null) {
  return agent?.interfaces ?? agent?.capabilities?.interfaces ?? [];
}

function getExpectedEndpoints(agent: Agent.Item) {
  return agent.expected_endpoints ?? [];
}

function getRecentEvents(agent: Agent.Item) {
  return agent.recent_events ?? [];
}

function formatInterfaceAddress(item: Agent.Interface) {
  if (!item.ip) return "未配置IP";
  return item.prefix_length && item.prefix_length > 0
    ? `${item.ip}/${item.prefix_length}`
    : item.ip;
}

function formatEndpointDesc(ep: Agent.Endpoint) {
  if (ep.transport === "serial") {
    return [ep.channel, ep.baud_rate && `${ep.baud_rate} bps`].filter(Boolean).join(" · ");
  }
  const addr = [ep.ip, ep.port].filter(Boolean).join(":");
  return [ep.mode, addr].filter(Boolean).join(" · ");
}

function SummaryCard({
  title,
  value,
  tone,
}: {
  title: string;
  value: number;
  tone: "neutral" | "success" | "warning" | "danger";
}) {
  const toneClassName =
    tone === "success"
      ? "border-emerald-200 bg-emerald-50/70"
      : tone === "warning"
        ? "border-amber-200 bg-amber-50/70"
        : tone === "danger"
          ? "border-rose-200 bg-rose-50/70"
          : "border-slate-200 bg-slate-50/70";

  return (
    <div className={`rounded-2xl border p-4 ${toneClassName}`}>
      <div className="text-xs font-medium uppercase tracking-[0.2em] text-slate-500">{title}</div>
      <div className="mt-3 text-3xl font-semibold leading-none text-slate-900">{value}</div>
    </div>
  );
}

const methodLabel: Record<string, { text: string; color: string }> = {
  dhcp: { text: "DHCP", color: "processing" },
  static: { text: "Static", color: "warning" },
};

function NetworkInterfaceList({
  agent,
  interfaces,
  canEdit,
  onEditInterface,
  onCreateBridge,
}: {
  agent: Agent.Item;
  interfaces: Agent.Interface[];
  canEdit: boolean;
  onEditInterface: (iface: Agent.Interface) => void;
  onCreateBridge: () => void;
}) {
  const [showDown, setShowDown] = useState(false);
  const upInterfaces = useMemo(() => interfaces.filter((i) => i.up), [interfaces]);
  const downInterfaces = useMemo(() => interfaces.filter((i) => !i.up), [interfaces]);
  const networkBackend = agent.capabilities?.network_backend;

  const renderItem = (item: Agent.Interface) => {
    const isBridge = !!item.bridge_ports;
    const method = methodLabel[item.method || ""];
    return (
      <div key={`${agent.id}-${item.name}`}>
        <div className="group flex items-center justify-between rounded-lg border border-slate-100 px-3 py-1.5 text-sm">
          <span className="flex items-center gap-1.5 text-slate-700">
            {isBridge && (
              <Tag className="m-0" color="purple" variant="filled">
                Bridge
              </Tag>
            )}
            {item.display_name || item.name}
          </span>
          <span className="flex items-center gap-2">
            {method && (
              <Tag className="m-0" color={method.color} variant="filled">
                {method.text}
              </Tag>
            )}
            <span className="font-mono text-xs text-slate-500">{formatInterfaceAddress(item)}</span>
            <Tag className="m-0" color={item.up ? "success" : "default"}>
              {item.up ? "UP" : "DOWN"}
            </Tag>
            {canEdit && agent.is_online && (
              <Button
                type="link"
                size="small"
                className="hidden px-0 text-xs group-hover:inline-flex"
                onClick={() => onEditInterface(item)}
              >
                配置
              </Button>
            )}
          </span>
        </div>
        {item.bridge_ports && item.bridge_ports.length > 0 && (
          <div className="ml-4 mt-1 flex flex-wrap gap-1">
            {item.bridge_ports.map((port) => (
              <Tag key={port} className="m-0" color="default">
                {port}
              </Tag>
            ))}
          </div>
        )}
      </div>
    );
  };

  return (
    <div className="mt-4">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <span className="text-xs font-medium uppercase tracking-[0.16em] text-slate-500">
            网络接口
          </span>
          {networkBackend && <span className="text-[10px] text-slate-400">{networkBackend}</span>}
        </div>
        {canEdit && agent.is_online && (
          <Button type="link" size="small" className="px-0 text-xs" onClick={onCreateBridge}>
            + 创建桥接
          </Button>
        )}
      </div>
      {interfaces.length > 0 ? (
        <div className="mt-2 space-y-1.5">
          {upInterfaces.map(renderItem)}
          {downInterfaces.length > 0 && (
            <>
              <button
                type="button"
                className="w-full rounded-lg border border-dashed border-slate-200 px-3 py-1 text-xs text-slate-400 transition-colors hover:border-slate-300 hover:text-slate-500"
                onClick={() => setShowDown((v) => !v)}
              >
                {showDown ? "收起" : `展开 ${downInterfaces.length} 个未启用接口`}
              </button>
              {showDown && downInterfaces.map(renderItem)}
            </>
          )}
        </div>
      ) : (
        <div className="mt-2 text-xs text-slate-400">
          {agent.is_online ? "等待上报..." : "离线"}
        </div>
      )}
    </div>
  );
}

function AgentCard({
  agent,
  canEdit,
  onResync,
  onDelete,
  onEdit,
  onEditInterface,
  onCreateBridge,
  onViewEvents,
  onAddEndpoint,
  onEditEndpoint,
  onDeleteEndpoint,
  onOpenTerminal,
  resyncLoading,
  deleteLoading,
}: {
  agent: Agent.Item;
  canEdit: boolean;
  onResync: () => void;
  onDelete: () => void;
  onEdit: () => void;
  onEditInterface: (iface: Agent.Interface) => void;
  onCreateBridge: () => void;
  onViewEvents: () => void;
  onAddEndpoint: () => void;
  onEditEndpoint: (ep: Agent.Endpoint) => void;
  onDeleteEndpoint: (ep: Agent.Endpoint) => void;
  onOpenTerminal: () => void;
  resyncLoading: boolean;
  deleteLoading: boolean;
}) {
  const configStatus = configStatusConfig[agent.config_status || "idle"] || configStatusConfig.idle;
  const interfaces = getInterfaces(agent);
  const expectedEndpoints = getExpectedEndpoints(agent);
  const recentEvents = getRecentEvents(agent);
  const managedDevices = getManagedDeviceCount(agent);

  return (
    <div className="rounded-2xl border border-slate-200 bg-white p-5 transition-shadow hover:shadow-md">
      {/* Header */}
      <div className="flex items-start justify-between gap-3">
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <div className="truncate text-base font-semibold text-slate-900">{agent.name}</div>
            <Tag color={agent.is_online ? "success" : "default"}>
              {agent.is_online ? "在线" : "离线"}
            </Tag>
          </div>
          <div className="mt-1 flex items-center gap-3 text-xs text-slate-500">
            <span>{agent.code}</span>
            {agent.version && <span>v{agent.version}</span>}
          </div>
        </div>
        <Tag color={configStatus.color}>{configStatus.label}</Tag>
      </div>

      {/* Network interfaces */}
      <NetworkInterfaceList
        agent={agent}
        interfaces={interfaces}
        canEdit={canEdit}
        onEditInterface={onEditInterface}
        onCreateBridge={onCreateBridge}
      />

      {/* Endpoints management */}
      <div className="mt-4">
        <div className="flex items-center justify-between">
          <div className="text-xs font-medium uppercase tracking-[0.16em] text-slate-500">
            接入端点
          </div>
          {canEdit && (
            <Button type="link" size="small" className="px-0 text-xs" onClick={onAddEndpoint}>
              + 新增
            </Button>
          )}
        </div>
        {expectedEndpoints.length > 0 ? (
          <div className="mt-2 space-y-1.5">
            {expectedEndpoints.map((ep) => (
              <div
                key={ep.id}
                className="group flex items-center justify-between rounded-lg border border-slate-100 px-3 py-1.5 text-sm"
              >
                <span className="min-w-0 flex-1 truncate text-slate-700">
                  <Tag className="m-0 mr-1.5" color={ep.transport === "serial" ? "orange" : "blue"}>
                    {ep.transport === "serial" ? "串口" : "以太网"}
                  </Tag>
                  <span className="font-medium">{ep.name}</span>
                  <span className="ml-1.5 text-slate-400">{formatEndpointDesc(ep)}</span>
                </span>
                <span className="flex shrink-0 items-center gap-2">
                  <span className="text-xs text-slate-400">{ep.device_count || 0} 设备</span>
                  {canEdit && (
                    <span className="hidden gap-1 group-hover:flex">
                      <Button
                        type="link"
                        size="small"
                        className="px-0 text-xs"
                        onClick={() => onEditEndpoint(ep)}
                      >
                        编辑
                      </Button>
                      <Popconfirm
                        title="确认删除端点？"
                        description={
                          ep.device_count
                            ? `该端点关联 ${ep.device_count} 台设备，请先解除关联`
                            : undefined
                        }
                        onConfirm={() => onDeleteEndpoint(ep)}
                        disabled={(ep.device_count || 0) > 0}
                      >
                        <Button
                          type="link"
                          size="small"
                          danger
                          className="px-0 text-xs"
                          disabled={(ep.device_count || 0) > 0}
                        >
                          删除
                        </Button>
                      </Popconfirm>
                    </span>
                  )}
                </span>
              </div>
            ))}
          </div>
        ) : (
          <div className="mt-2 text-xs text-slate-400">暂无端点</div>
        )}
      </div>

      {/* Status bar */}
      <div className="mt-4 flex items-center gap-4 text-xs text-slate-500">
        <span>配置版本: {formatConfigVersion(agent.applied_config_version)}</span>
        <span>托管设备: {managedDevices}</span>
        {agent.config_error && (
          <Tooltip title={agent.config_error}>
            <Tag color="error" className="m-0 cursor-help">
              配置错误
            </Tag>
          </Tooltip>
        )}
        {expectedEndpoints.length > 0 && <span>端点: {expectedEndpoints.length}</span>}
      </div>

      {/* Recent events preview */}
      {recentEvents.length > 0 && (
        <div className="mt-3 rounded-lg border border-slate-100 bg-slate-50/50 px-3 py-2">
          <div className="flex items-center justify-between">
            <span className="text-xs text-slate-500">最近事件</span>
            <Button type="link" size="small" className="px-0 text-xs" onClick={onViewEvents}>
              查看全部
            </Button>
          </div>
          <div className="mt-1 truncate text-xs text-slate-600">{recentEvents[0].message}</div>
        </div>
      )}

      {/* Actions */}
      {canEdit && (
        <Flex gap={4} className="mt-4 border-t border-slate-100 pt-3">
          <Tooltip title="编辑">
            <Button type="text" size="small" icon={<EditOutlined />} onClick={onEdit} />
          </Tooltip>
          <Tooltip title="重新同步">
            <Button
              type="text"
              size="small"
              icon={<SyncOutlined />}
              disabled={!agent.is_online}
              loading={resyncLoading}
              onClick={onResync}
            />
          </Tooltip>
          <Tooltip title="终端">
            <Button
              type="text"
              size="small"
              icon={<CodeOutlined />}
              disabled={!agent.is_online}
              onClick={onOpenTerminal}
            />
          </Tooltip>
          <Tooltip title="事件记录">
            <Button type="text" size="small" icon={<HistoryOutlined />} onClick={onViewEvents} />
          </Tooltip>
          <Popconfirm
            title="确认删除"
            description={`确定删除 Agent「${agent.name}」？此操作不可恢复。`}
            onConfirm={onDelete}
            disabled={agent.is_online}
          >
            <Tooltip title="删除">
              <Button
                type="text"
                size="small"
                danger
                icon={<DeleteOutlined />}
                disabled={agent.is_online}
                loading={deleteLoading}
              />
            </Tooltip>
          </Popconfirm>
        </Flex>
      )}
    </div>
  );
}

function EndpointFormModal({
  open,
  editEndpoint,
  onClose,
  onSubmit,
  loading,
}: {
  open: boolean;
  editEndpoint: Agent.Endpoint | null;
  onClose: () => void;
  onSubmit: (values: Agent.EndpointCreate) => void;
  loading: boolean;
}) {
  const [form] = Form.useForm();
  const isEdit = !!editEndpoint;
  const transport = Form.useWatch("transport", form) as Agent.Transport | undefined;
  const protocol = Form.useWatch("protocol", form) as string | undefined;
  const mode = Form.useWatch("mode", form) as string | undefined;

  // 协议选项：串口只有 Modbus，以太网有 SL651 和 Modbus
  const protocolOptions = useMemo(() => {
    if (transport === "serial") {
      return [{ label: "Modbus", value: "Modbus" }];
    }
    return [
      { label: "SL651", value: "SL651" },
      { label: "Modbus", value: "Modbus" },
    ];
  }, [transport]);

  // 模式选项：SL651 只有 TCP Server，Modbus 只有 TCP Client
  const modeOptions = useMemo(() => {
    if (protocol === "SL651") {
      return [{ label: "TCP Server (监听)", value: "TCP Server" }];
    }
    return [{ label: "TCP Client (主动连接)", value: "TCP Client" }];
  }, [protocol]);

  // Server 模式下 IP 固定 0.0.0.0
  const isServerMode = mode === "TCP Server";

  // 传输方式变更时级联更新协议和模式
  const handleTransportChange = (value: string) => {
    if (value === "serial") {
      form.setFieldsValue({ protocol: "Modbus", mode: undefined });
    } else {
      form.setFieldsValue({ protocol: "SL651", mode: "TCP Server", ip: "0.0.0.0" });
    }
  };

  // 协议变更时级联更新模式
  const handleProtocolChange = (value: string) => {
    if (value === "SL651") {
      form.setFieldsValue({ mode: "TCP Server", ip: "0.0.0.0" });
    } else {
      form.setFieldsValue({ mode: "TCP Client", ip: "" });
    }
  };

  const handleOpen = () => {
    if (editEndpoint) {
      form.setFieldsValue({
        name: editEndpoint.name,
        transport: editEndpoint.transport,
        mode: editEndpoint.mode,
        protocol: editEndpoint.protocol,
        ip: editEndpoint.ip,
        port: editEndpoint.port,
        channel: editEndpoint.channel,
        baud_rate: editEndpoint.baud_rate,
      });
    } else {
      form.resetFields();
      form.setFieldsValue({
        transport: "ethernet",
        protocol: "SL651",
        mode: "TCP Server",
        ip: "0.0.0.0",
      });
    }
  };

  const handleSubmit = async () => {
    const values = await form.validateFields();
    // Server 模式强制 IP 为 0.0.0.0
    if (values.mode === "TCP Server") {
      values.ip = "0.0.0.0";
    }
    onSubmit(values);
  };

  return (
    <Modal
      title={isEdit ? "编辑端点" : "新增接入端点"}
      open={open}
      onCancel={onClose}
      onOk={handleSubmit}
      confirmLoading={loading}
      afterOpenChange={(visible) => visible && handleOpen()}
      destroyOnHidden
    >
      <Form form={form} layout="vertical" className="mt-4">
        <Form.Item
          name="name"
          label="端点名称"
          rules={[{ required: true, message: "请输入端点名称" }]}
        >
          <Input placeholder="例如: SL651 TCP Server :6001" />
        </Form.Item>
        <Form.Item name="transport" label="传输方式" rules={[{ required: true }]}>
          <Select
            options={[
              { label: "以太网 (TCP)", value: "ethernet" },
              { label: "串口 (Serial)", value: "serial" },
            ]}
            disabled={isEdit}
            onChange={handleTransportChange}
          />
        </Form.Item>
        <Form.Item name="protocol" label="协议" rules={[{ required: true }]}>
          <Select options={protocolOptions} onChange={handleProtocolChange} />
        </Form.Item>
        {transport === "ethernet" && (
          <>
            <Form.Item name="mode" label="模式" rules={[{ required: true, message: "请选择模式" }]}>
              <Select options={modeOptions} />
            </Form.Item>
            <Form.Item name="ip" label="IP 地址" rules={[{ required: true, message: "请输入 IP" }]}>
              <Input
                placeholder={isServerMode ? "0.0.0.0" : "192.168.1.100"}
                disabled={isServerMode}
              />
            </Form.Item>
            <Form.Item name="port" label="端口" rules={[{ required: true, message: "请输入端口" }]}>
              <InputNumber min={1} max={65535} className="w-full" placeholder="6001" />
            </Form.Item>
          </>
        )}
        {transport === "serial" && (
          <>
            <Form.Item
              name="channel"
              label="串口通道"
              rules={[{ required: true, message: "请输入串口通道" }]}
            >
              <Input placeholder="/dev/ttyS0" />
            </Form.Item>
            <Form.Item name="baud_rate" label="波特率">
              <Select
                allowClear
                placeholder="选择波特率"
                options={[
                  { label: "9600", value: 9600 },
                  { label: "19200", value: 19200 },
                  { label: "38400", value: 38400 },
                  { label: "57600", value: 57600 },
                  { label: "115200", value: 115200 },
                ]}
              />
            </Form.Item>
          </>
        )}
      </Form>
    </Modal>
  );
}

function StaticIpFields() {
  const form = Form.useFormInstance();
  const mode = Form.useWatch("mode", form) as string | undefined;
  if (mode !== "static") return null;

  return (
    <>
      <div className="mt-3 grid grid-cols-2 gap-3">
        <Form.Item
          name="ip"
          label="IP 地址"
          className="mb-0"
          rules={[{ required: true, message: "请输入 IP" }]}
        >
          <Input placeholder="192.168.1.100" />
        </Form.Item>
        <Form.Item
          name="prefix_length"
          label="子网前缀"
          className="mb-0"
          rules={[{ required: true, message: "请输入前缀长度" }]}
        >
          <InputNumber min={1} max={32} className="w-full" placeholder="24" />
        </Form.Item>
      </div>
      <Form.Item name="gateway" label="默认网关" className="mb-0 mt-3">
        <Input placeholder="192.168.1.1（可选）" />
      </Form.Item>
    </>
  );
}

function NetworkConfigModal({
  open,
  agent,
  iface,
  onClose,
  onSubmit,
  loading,
}: {
  open: boolean;
  agent: Agent.Item | null;
  iface: Agent.Interface | null;
  onClose: () => void;
  onSubmit: (data: Agent.NetworkConfigItem[]) => void;
  loading: boolean;
}) {
  const [form] = Form.useForm();
  const isBridge = !!iface?.bridge_ports;
  const allInterfaces = getInterfaces(agent);
  // 可选的桥接端口：所有 UP 的物理接口（排除桥接口自身和已被桥接的端口）
  const availablePorts = useMemo(() => {
    if (!agent) return [];
    return allInterfaces
      .filter((i) => i.up && !i.bridge_ports && i.name !== iface?.name)
      .map((i) => ({ label: i.display_name || i.name, value: i.name }));
  }, [agent, allInterfaces, iface]);

  const handleOpen = () => {
    if (!iface) return;
    // 直接从系统当前状态预填（method/ip/prefix_length/gateway 来自 capabilities 实时上报）
    const mode = iface.method === "static" ? "static" : "dhcp";
    form.setFieldsValue({
      mode,
      ip: iface.ip || "",
      prefix_length: iface.prefix_length ?? 24,
      gateway: iface.gateway || "",
      bridge_ports: iface.bridge_ports || [],
    });
  };

  const handleSubmit = async () => {
    if (!iface) return;
    const v = await form.validateFields();
    const item: Agent.NetworkConfigItem = {
      name: iface.name,
      mode: v.mode,
      ...(isBridge ? { type: "bridge" as const } : {}),
      ...(isBridge && v.bridge_ports?.length ? { bridge_ports: v.bridge_ports } : {}),
      ...(v.mode === "static"
        ? {
            ip: v.ip,
            prefix_length: v.prefix_length ?? 24,
            ...(v.gateway ? { gateway: v.gateway } : {}),
          }
        : {}),
    };
    onSubmit([item]);
  };

  const networkBackend = agent?.capabilities?.network_backend;

  return (
    <Modal
      title={`网络配置 — ${iface?.display_name || iface?.name || ""}`}
      open={open}
      onCancel={onClose}
      onOk={handleSubmit}
      confirmLoading={loading}
      afterOpenChange={(visible) => visible && handleOpen()}
      destroyOnHidden
      width={480}
    >
      {!iface ? (
        <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="未选择接口" />
      ) : (
        <Form form={form} layout="vertical" className="mt-4">
          <div className="mb-3 flex items-center gap-2">
            <span className="font-medium text-slate-700">{iface.display_name || iface.name}</span>
            {isBridge && (
              <Tag className="m-0" color="purple">
                Bridge
              </Tag>
            )}
            <Tag className="m-0" color={iface.up ? "success" : "default"}>
              {iface.up ? "UP" : "DOWN"}
            </Tag>
            <span className="font-mono text-xs text-slate-400">
              {formatInterfaceAddress(iface)}
            </span>
          </div>
          {networkBackend && (
            <div className="mb-3 text-xs text-slate-400">
              网络管理工具: <span className="font-medium text-slate-500">{networkBackend}</span>
            </div>
          )}

          {/* 桥接口的成员端口编辑 */}
          {isBridge && (
            <Form.Item name="bridge_ports" label="桥接成员端口" className="mb-3">
              <Select
                mode="multiple"
                placeholder="选择成员端口"
                options={[
                  // 当前已有的端口也要可选
                  ...(iface.bridge_ports || []).map((p) => ({
                    label: p,
                    value: p,
                  })),
                  ...availablePorts.filter((o) => !(iface.bridge_ports || []).includes(o.value)),
                ]}
              />
            </Form.Item>
          )}

          <Form.Item name="mode" label="地址模式" className="mb-0">
            <Select
              options={[
                { label: "DHCP（自动获取）", value: "dhcp" },
                { label: "静态 IP", value: "static" },
              ]}
            />
          </Form.Item>
          <StaticIpFields />
        </Form>
      )}
    </Modal>
  );
}

/** 创建桥接 Modal */
function CreateBridgeModal({
  open,
  agent,
  onClose,
  onSubmit,
  loading,
}: {
  open: boolean;
  agent: Agent.Item | null;
  onClose: () => void;
  onSubmit: (data: Agent.NetworkConfigItem[]) => void;
  loading: boolean;
}) {
  const [form] = Form.useForm();
  const allInterfaces = agent ? getInterfaces(agent) : [];
  const availablePorts = useMemo(
    () =>
      allInterfaces
        .filter((i) => i.up && !i.bridge_ports)
        .map((i) => ({ label: i.display_name || i.name, value: i.name })),
    [allInterfaces]
  );

  const handleOpen = () => {
    form.resetFields();
    form.setFieldsValue({ name: "br0", mode: "dhcp", bridge_ports: [] });
  };

  const handleSubmit = async () => {
    const v = await form.validateFields();
    const item: Agent.NetworkConfigItem = {
      name: v.name,
      mode: v.mode,
      type: "bridge",
      bridge_ports: v.bridge_ports || [],
      ...(v.mode === "static"
        ? {
            ip: v.ip,
            prefix_length: v.prefix_length ?? 24,
            ...(v.gateway ? { gateway: v.gateway } : {}),
          }
        : {}),
    };
    onSubmit([item]);
  };

  const networkBackend = agent?.capabilities?.network_backend;

  return (
    <Modal
      title="创建桥接"
      open={open}
      onCancel={onClose}
      onOk={handleSubmit}
      confirmLoading={loading}
      afterOpenChange={(visible) => visible && handleOpen()}
      destroyOnHidden
      width={480}
    >
      <Form form={form} layout="vertical" className="mt-4">
        {networkBackend && (
          <div className="mb-3 text-xs text-slate-400">
            网络管理工具: <span className="font-medium text-slate-500">{networkBackend}</span>
          </div>
        )}
        <Form.Item
          name="name"
          label="桥接口名称"
          rules={[
            { required: true, message: "请输入桥接口名称" },
            { pattern: /^[a-zA-Z][a-zA-Z0-9._-]*$/, message: "名称格式不合法" },
          ]}
        >
          <Input placeholder="br0" />
        </Form.Item>
        <Form.Item
          name="bridge_ports"
          label="成员端口"
          rules={[{ required: true, type: "array", min: 1, message: "至少选择一个端口" }]}
        >
          <Select mode="multiple" placeholder="选择成员端口" options={availablePorts} />
        </Form.Item>
        <Form.Item name="mode" label="地址模式" className="mb-0">
          <Select
            options={[
              { label: "DHCP（自动获取）", value: "dhcp" },
              { label: "静态 IP", value: "static" },
            ]}
          />
        </Form.Item>
        <StaticIpFields />
      </Form>
    </Modal>
  );
}

function AgentFormModal({
  open,
  editAgent,
  onClose,
  onCreate,
  onUpdate,
  createLoading,
  updateLoading,
}: {
  open: boolean;
  editAgent: Agent.Item | null;
  onClose: () => void;
  onCreate: (data: Agent.CreateInput) => void;
  onUpdate: (id: number, data: Agent.UpdateInput) => void;
  createLoading: boolean;
  updateLoading: boolean;
}) {
  const [form] = Form.useForm();
  const isEdit = !!editAgent;

  const handleOpen = () => {
    if (editAgent) {
      form.setFieldsValue({ name: editAgent.name });
    } else {
      form.resetFields();
    }
  };

  const handleSubmit = async () => {
    const values = await form.validateFields();
    if (isEdit) {
      onUpdate(editAgent!.id, {
        name: values.name || undefined,
      });
    } else {
      onCreate({
        code: values.code,
        name: values.name,
      });
    }
  };

  return (
    <Modal
      title={isEdit ? "编辑边缘节点" : "新增边缘节点"}
      open={open}
      onCancel={onClose}
      onOk={handleSubmit}
      confirmLoading={createLoading || updateLoading}
      afterOpenChange={(visible) => visible && handleOpen()}
      destroyOnHidden
    >
      <Form form={form} layout="vertical" className="mt-4">
        <Form.Item
          name="code"
          label="Agent 编码 (SN)"
          rules={[{ required: !isEdit, message: "请输入 Agent 编码" }]}
          tooltip="设备硬件唯一标识（SN码），创建后不可修改"
        >
          <Input placeholder="例如: agent-arm-001" disabled={isEdit} />
        </Form.Item>
        <Form.Item name="name" label="名称" rules={[{ required: true, message: "请输入名称" }]}>
          <Input placeholder="例如: ARM采集节点001" />
        </Form.Item>
      </Form>
    </Modal>
  );
}

export default function EdgeNodePage() {
  const [keyword, setKeyword] = useState("");
  const [issueFilter, setIssueFilter] = useState<"all" | "attention" | "failed" | "offline">("all");
  const [formOpen, setFormOpen] = useState(false);
  const [editAgent, setEditAgent] = useState<Agent.Item | null>(null);
  const [eventViewerAgent, setEventViewerAgent] = useState<Agent.Item | null>(null);
  const [networkConfigTarget, setNetworkConfigTarget] = useState<{
    agent: Agent.Item;
    iface: Agent.Interface;
  } | null>(null);
  const [createBridgeAgent, setCreateBridgeAgent] = useState<Agent.Item | null>(null);
  const [endpointModal, setEndpointModal] = useState<{
    open: boolean;
    agentId: number;
    editEndpoint: Agent.Endpoint | null;
  }>({ open: false, agentId: 0, editEndpoint: null });
  const [terminalAgent, setTerminalAgent] = useState<Agent.Item | null>(null);
  const [terminalFullscreen, setTerminalFullscreen] = useState(false);
  const canQuery = usePermission("iot:link:query");
  const canEdit = usePermission("iot:link:edit");
  const {
    data: agents = [],
    isLoading,
    isFetching,
    refetch,
  } = useAgentList({
    enabled: canQuery,
    refetchInterval: 5000,
  });
  const { data: eventViewerEvents = [], isLoading: loadingEventViewer } = useAgentEvents(
    eventViewerAgent?.id,
    { hours: 24, limit: 200 },
    { enabled: !!eventViewerAgent }
  );
  const createMutation = useAgentCreate();
  const updateMutation = useAgentUpdate();
  const resyncMutation = useAgentResync();
  const deleteMutation = useAgentDelete();
  const networkConfigMutation = useAgentNetworkConfig();
  const endpointCreateMutation = useAgentEndpointCreate();
  const endpointUpdateMutation = useAgentEndpointUpdate();
  const endpointDeleteMutation = useAgentEndpointDelete();

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询边缘节点的权限，请联系管理员" />
      </PageContainer>
    );
  }

  const normalizedKeyword = keyword.trim().toLowerCase();
  const filteredAgents = agents.filter((agent) => {
    const haystacks = [
      agent.name,
      agent.code,
      agent.version,
      agent.runtime?.hostname,
      ...getInterfaces(agent).flatMap((item) => [item.name, item.display_name, item.ip]),
    ];

    const matchedKeyword = !normalizedKeyword
      ? true
      : haystacks.some((value) => value?.toLowerCase().includes(normalizedKeyword));
    if (!matchedKeyword) return false;

    switch (issueFilter) {
      case "attention":
        return (
          !agent.is_online || agent.config_status === "failed" || agent.config_status === "pending"
        );
      case "failed":
        return agent.config_status === "failed";
      case "offline":
        return !agent.is_online;
      default:
        return true;
    }
  });

  const summary = {
    total: agents.length,
    online: agents.filter((item) => item.is_online).length,
    pending: agents.filter((item) => item.config_status === "pending").length,
    failed: agents.filter((item) => item.config_status === "failed").length,
    endpoints: agents.reduce((sum, item) => sum + (item.expected_endpoints?.length ?? 0), 0),
    managedDevices: agents.reduce((sum, item) => sum + getManagedDeviceCount(item), 0),
  };

  const handleCreate = (data: Agent.CreateInput) => {
    createMutation.mutate(data, {
      onSuccess: () => {
        setFormOpen(false);
        setEditAgent(null);
      },
    });
  };

  const handleUpdate = (id: number, data: Agent.UpdateInput) => {
    updateMutation.mutate(
      { id, data },
      {
        onSuccess: () => {
          setFormOpen(false);
          setEditAgent(null);
        },
      }
    );
  };

  const handleEndpointSubmit = (values: Agent.EndpointCreate) => {
    const { agentId, editEndpoint } = endpointModal;
    if (editEndpoint) {
      endpointUpdateMutation.mutate(
        { id: editEndpoint.id, data: values },
        {
          onSuccess: () => setEndpointModal({ open: false, agentId: 0, editEndpoint: null }),
        }
      );
    } else {
      endpointCreateMutation.mutate(
        { agentId, data: values },
        {
          onSuccess: () => setEndpointModal({ open: false, agentId: 0, editEndpoint: null }),
        }
      );
    }
  };

  return (
    <PageContainer
      header={
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="m-0 text-base font-medium">边缘节点</h3>
            <div className="mt-1 text-sm text-slate-500">
              管理边缘节点，查看在线状态、网口能力和配置同步
            </div>
          </div>
          <Space wrap>
            <Select
              className="w-[160px]"
              value={issueFilter}
              onChange={setIssueFilter}
              options={[
                { label: "全部节点", value: "all" },
                { label: "只看异常", value: "attention" },
                { label: "只看失败", value: "failed" },
                { label: "只看离线", value: "offline" },
              ]}
            />
            <Search
              allowClear
              className="w-[240px]"
              placeholder="名称 / 编码 / 网口 IP"
              value={keyword}
              onChange={(event) => setKeyword(event.target.value)}
            />
            <Button onClick={() => refetch()} loading={isFetching}>
              刷新
            </Button>
            {canEdit && (
              <Button
                type="primary"
                onClick={() => {
                  setEditAgent(null);
                  setFormOpen(true);
                }}
              >
                + 新增边缘节点
              </Button>
            )}
          </Space>
        </div>
      }
    >
      {/* Summary cards */}
      <div className="grid grid-cols-2 gap-4 md:grid-cols-3 xl:grid-cols-6">
        <SummaryCard title="边缘节点总数" value={summary.total} tone="neutral" />
        <SummaryCard title="在线节点" value={summary.online} tone="success" />
        <SummaryCard title="托管设备" value={summary.managedDevices} tone="neutral" />
        <SummaryCard title="待应用配置" value={summary.pending} tone="warning" />
        <SummaryCard title="配置失败" value={summary.failed} tone="danger" />
        <SummaryCard title="接入端点" value={summary.endpoints} tone="neutral" />
      </div>

      {/* Agent card grid */}
      {isLoading ? (
        <div className="mt-6 text-center text-slate-400">加载中...</div>
      ) : filteredAgents.length === 0 ? (
        <div className="mt-6">
          <Empty
            image={Empty.PRESENTED_IMAGE_SIMPLE}
            description={keyword ? "没有匹配的边缘节点" : "暂无边缘节点"}
          />
        </div>
      ) : (
        <div className="mt-4 grid grid-cols-1 gap-4 md:grid-cols-2 xl:grid-cols-3">
          {filteredAgents.map((agent) => (
            <AgentCard
              key={agent.id}
              agent={agent}
              canEdit={canEdit}
              onResync={() => resyncMutation.mutate(agent.id)}
              onDelete={() => deleteMutation.mutate(agent.id)}
              onEdit={() => {
                setEditAgent(agent);
                setFormOpen(true);
              }}
              onEditInterface={(iface) => setNetworkConfigTarget({ agent, iface })}
              onCreateBridge={() => setCreateBridgeAgent(agent)}
              onViewEvents={() => setEventViewerAgent(agent)}
              onAddEndpoint={() =>
                setEndpointModal({ open: true, agentId: agent.id, editEndpoint: null })
              }
              onEditEndpoint={(ep) =>
                setEndpointModal({ open: true, agentId: agent.id, editEndpoint: ep })
              }
              onDeleteEndpoint={(ep) => endpointDeleteMutation.mutate(ep.id)}
              onOpenTerminal={() => setTerminalAgent(agent)}
              resyncLoading={resyncMutation.isPending && resyncMutation.variables === agent.id}
              deleteLoading={deleteMutation.isPending && deleteMutation.variables === agent.id}
            />
          ))}
        </div>
      )}

      {/* Create / Edit modal */}
      <AgentFormModal
        open={formOpen}
        editAgent={editAgent}
        onClose={() => {
          setFormOpen(false);
          setEditAgent(null);
        }}
        onCreate={handleCreate}
        onUpdate={handleUpdate}
        createLoading={createMutation.isPending}
        updateLoading={updateMutation.isPending}
      />

      {/* Event viewer modal */}
      <Modal
        title={eventViewerAgent ? `${eventViewerAgent.name} 最近 24 小时事件` : "最近 24 小时事件"}
        open={!!eventViewerAgent}
        onCancel={() => setEventViewerAgent(null)}
        footer={null}
        width={760}
      >
        <div className="space-y-3">
          {eventViewerEvents.length > 0 ? (
            eventViewerEvents.map((event, index) => {
              const eventLevel = eventLevelConfig[event.level || "info"] || eventLevelConfig.info;
              return (
                <div
                  key={`${eventViewerAgent?.id || 0}-${event.ts}-${event.type}-${index}`}
                  className="rounded-xl border border-slate-200 bg-white px-4 py-3"
                >
                  <div className="flex items-center justify-between gap-3">
                    <Space size={[8, 8]} wrap>
                      <Tag color={eventLevel.color}>{eventLevel.label}</Tag>
                      <span className="text-xs text-slate-500">{event.type}</span>
                    </Space>
                    <div className="text-xs text-slate-400">{event.ts || "-"}</div>
                  </div>
                  <div className="mt-2 text-sm text-slate-700">{event.message}</div>
                  {event.detail ? (
                    <div className="mt-2 flex flex-wrap gap-2 text-xs text-slate-500">
                      {event.detail.configVersion ? (
                        <span>配置版本: {event.detail.configVersion}</span>
                      ) : null}
                      {event.detail.managedDeviceCount ? (
                        <span>托管设备数: {event.detail.managedDeviceCount}</span>
                      ) : null}
                      {event.detail.version ? <span>版本: {event.detail.version}</span> : null}
                      {event.detail.code ? <span>编码: {event.detail.code}</span> : null}
                    </div>
                  ) : null}
                  {event.detail?.error ? (
                    <div className="mt-2 text-xs text-rose-600">{event.detail.error}</div>
                  ) : null}
                </div>
              );
            })
          ) : (
            <Empty
              image={Empty.PRESENTED_IMAGE_SIMPLE}
              description={loadingEventViewer ? "正在加载事件..." : "最近 24 小时暂无事件"}
            />
          )}
        </div>
      </Modal>

      {/* Network config modal */}
      <NetworkConfigModal
        open={!!networkConfigTarget}
        agent={networkConfigTarget?.agent ?? null}
        iface={networkConfigTarget?.iface ?? null}
        onClose={() => setNetworkConfigTarget(null)}
        onSubmit={(data) => {
          if (networkConfigTarget) {
            networkConfigMutation.mutate(
              { id: networkConfigTarget.agent.id, data },
              {
                onSuccess: () => setNetworkConfigTarget(null),
              }
            );
          }
        }}
        loading={networkConfigMutation.isPending}
      />

      {/* Create bridge modal */}
      <CreateBridgeModal
        open={!!createBridgeAgent}
        agent={createBridgeAgent}
        onClose={() => setCreateBridgeAgent(null)}
        onSubmit={(data) => {
          if (createBridgeAgent) {
            networkConfigMutation.mutate(
              { id: createBridgeAgent.id, data },
              {
                onSuccess: () => setCreateBridgeAgent(null),
              }
            );
          }
        }}
        loading={networkConfigMutation.isPending}
      />

      {/* Endpoint form modal */}
      <EndpointFormModal
        open={endpointModal.open}
        editEndpoint={endpointModal.editEndpoint}
        onClose={() => setEndpointModal({ open: false, agentId: 0, editEndpoint: null })}
        onSubmit={handleEndpointSubmit}
        loading={endpointCreateMutation.isPending || endpointUpdateMutation.isPending}
      />

      {/* Terminal modal */}
      <Modal
        title={
          <div className="flex items-center gap-2">
            <span>{terminalAgent ? `终端 - ${terminalAgent.name}` : "终端"}</span>
            <Button
              type="text"
              size="small"
              icon={terminalFullscreen ? <FullscreenExitOutlined /> : <FullscreenOutlined />}
              onClick={() => setTerminalFullscreen((f) => !f)}
            />
          </div>
        }
        open={!!terminalAgent}
        onCancel={() => {
          setTerminalAgent(null);
          setTerminalFullscreen(false);
        }}
        footer={null}
        width={terminalFullscreen ? "100vw" : 960}
        destroyOnHidden
        styles={{
          body: {
            padding: 0,
            background: "#1e1e2e",
            height: terminalFullscreen ? "calc(100vh - 55px)" : 520,
          },
        }}
        style={terminalFullscreen ? { top: 0, maxWidth: "100vw", paddingBottom: 0 } : undefined}
      >
        {terminalAgent && (
          <Suspense fallback={null}>
            <EdgeNodeTerminal agentId={terminalAgent.id} visible={!!terminalAgent} />
          </Suspense>
        )}
      </Modal>
    </PageContainer>
  );
}
