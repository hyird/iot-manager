import {
  App,
  Button,
  Form,
  Input,
  InputNumber,
  Modal,
  Result,
  Select,
  Space,
  Table,
  Tag,
  Tooltip,
} from "antd";
import type { ColumnsType, TablePaginationConfig } from "antd/es/table";
import { useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { StatusTag } from "@/components/StatusTag";
import { useDebounceFn, usePermission } from "@/hooks";
import { useWsStatus } from "@/providers";
import { useLinkDelete, useLinkEnums, useLinkList, useLinkSave, usePublicIp } from "@/services";
import type { Link } from "@/types";
import { DATE_TIME_COLUMN_WIDTH, formatDateTime } from "@/utils";

/** 连接状态配置 */
const connStatusConfig: Record<string, { label: string; color: string }> = {
  stopped: { label: "已停止", color: "default" },
  listening: { label: "监听中", color: "processing" },
  connected: { label: "已连接", color: "success" },
  partial: { label: "部分连接", color: "warning" },
  connecting: { label: "连接中", color: "warning" },
  error: { label: "错误", color: "error" },
};

const { Search } = Input;

interface LinkFormValues {
  id?: number;
  name: string;
  mode: Link.Mode;
  protocol: Link.Protocol;
  ip: string;
  port: number;
  targets: Link.Target[];
  status: Link.Status;
}

const createTarget = (): Link.Target => ({
  id: `target-${Date.now()}-${Math.random().toString(16).slice(2)}`,
  name: "目标1",
  ip: "",
  port: 502,
  status: "enabled",
});

const LinkPage = () => {
  const [keyword, setKeyword] = useState("");
  const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
  const [modalVisible, setModalVisible] = useState(false);
  const [editing, setEditing] = useState<Link.Item | null>(null);
  const [form] = Form.useForm<LinkFormValues>();
  const { modal } = App.useApp();

  const canQuery = usePermission("iot:link:query");
  const canAdd = usePermission("iot:link:add");
  const canEdit = usePermission("iot:link:edit");
  const canDelete = usePermission("iot:link:delete");

  const doSearch = (value: string) => {
    setKeyword(value);
    setPagination((prev) => ({ ...prev, page: 1 }));
  };

  const { run: debouncedSearch } = useDebounceFn(doSearch, 300);

  const { data: publicIpData } = usePublicIp({ enabled: canQuery });
  const { data: linkEnums } = useLinkEnums({ enabled: canQuery });

  const { connected: wsConnected } = useWsStatus();
  const { data: linkPage, isLoading: loadingLinks } = useLinkList(
    { page: pagination.page, pageSize: pagination.pageSize, keyword: keyword || undefined },
    { enabled: canQuery, refetchInterval: wsConnected ? false : 10000 }
  );

  const saveMutation = useLinkSave();
  const deleteMutation = useLinkDelete();

  const openCreateModal = () => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({
      status: "enabled",
      mode: "TCP Client",
      protocol: "SL651",
      ip: "",
      port: 0,
      targets: [createTarget()],
    });
    setModalVisible(true);
  };

  const handleModeChange = (mode: Link.Mode) => {
    if (mode === "TCP Server") {
      form.setFieldsValue({ ip: "0.0.0.0", targets: [] });
    } else {
      form.setFieldsValue({ ip: "", port: 0, targets: [createTarget()] });
    }
  };

  const openEditModal = (record: Link.Item) => {
    setEditing(record);
    form.setFieldsValue({
      id: record.id,
      name: record.name,
      mode: record.mode,
      protocol: record.protocol,
      ip: record.mode === "TCP Server" ? "0.0.0.0" : record.ip,
      port: record.port,
      targets: record.targets || [],
      status: record.status,
    });
    setModalVisible(true);
  };

  const onDelete = (record: Link.Item) => {
    modal.confirm({
      title: `确认删除链路「${record.name}」吗？`,
      content: "删除后该链路下的所有设备将无法通信。此操作不可撤销。",
      okText: "确定删除",
      okButtonProps: { danger: true },
      onOk: () => deleteMutation.mutate(record.id),
    });
  };

  const onFinish = (values: LinkFormValues) => {
    const payload: Link.CreateDto & { id?: number } = {
      id: values.id,
      name: values.name,
      mode: values.mode,
      protocol: values.protocol,
      ip: values.mode === "TCP Server" ? values.ip : "",
      port: values.mode === "TCP Server" ? values.port : 0,
      targets: values.mode === "TCP Client" ? values.targets : [],
      status: values.status,
    };

    saveMutation.mutate(payload, {
      onSuccess: () => {
        setModalVisible(false);
        setEditing(null);
      },
    });
  };

  const handleTableChange = (paginationConfig: TablePaginationConfig) => {
    setPagination({
      page: paginationConfig.current || 1,
      pageSize: paginationConfig.pageSize || 10,
    });
  };

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询链路列表的权限，请联系管理员" />
      </PageContainer>
    );
  }

  const columns: ColumnsType<Link.Item> = [
    { title: "链路名称", dataIndex: "name" },
    { title: "模式", dataIndex: "mode" },
    { title: "协议", dataIndex: "protocol" },
    {
      title: "监听 / 目标地址",
      key: "endpoint",
      render: (_, record) =>
        record.mode === "TCP Server" ? (
          `${record.ip}:${record.port}`
        ) : (
          <Tooltip
            title={(record.targets || []).map((target) => (
              <div key={target.id}>
                {target.name}: {target.ip}:{target.port}
              </div>
            ))}
          >
            <Tag color="blue">{record.targets?.length || 0} 个目标</Tag>
          </Tooltip>
        ),
    },
    {
      title: "启用",
      dataIndex: "status",
      render: (v: Link.Status) => <StatusTag status={v} />,
    },
    {
      title: "连接状态",
      key: "conn_status",
      render: (_, record) => {
        const status = record.conn_status || "stopped";
        const config = connStatusConfig[status] || connStatusConfig.stopped;

        if (record.mode === "TCP Server" && status === "listening") {
          const count = record.client_count || 0;
          const clients = record.clients || [];

          const clientTag = (
            <Tag color="blue" className={count > 0 ? "cursor-pointer" : "cursor-default"}>
              {count} 客户端
            </Tag>
          );

          return (
            <Space size={4}>
              <Tag color={config.color}>{config.label}</Tag>
              {count > 0 ? (
                <Tooltip
                  title={
                    <div>
                      <div className="mb-1 font-medium">已连接客户端：</div>
                      {clients.map((ip, idx) => (
                        <div key={idx}>{ip}</div>
                      ))}
                    </div>
                  }
                >
                  {clientTag}
                </Tooltip>
              ) : (
                clientTag
              )}
            </Space>
          );
        }

        if (record.mode === "TCP Client") {
          const enabledTargets = (record.targets || []).filter(
            (target) => target.status === "enabled"
          );
          const connectedTargets = enabledTargets.filter(
            (target) => target.conn_status === "connected"
          ).length;
          return (
            <Space size={4}>
              <Tag color={config.color}>{config.label}</Tag>
              <Tag color="blue">
                {connectedTargets}/{enabledTargets.length} 目标
              </Tag>
            </Space>
          );
        }

        return <Tag color={config.color}>{config.label}</Tag>;
      },
    },
    {
      title: "创建时间",
      dataIndex: "created_at",
      width: DATE_TIME_COLUMN_WIDTH,
      render: (value?: string) => formatDateTime(value),
    },
    {
      title: "操作",
      key: "actions",
      className: "table-action-cell",
      width: 150,
      fixed: "right" as const,
      render: (_, record) => (
        <Space>
          {canEdit && (
            <Button type="link" onClick={() => openEditModal(record)}>
              编辑
            </Button>
          )}
          {canDelete && (
            <Button type="link" danger onClick={() => onDelete(record)}>
              删除
            </Button>
          )}
        </Space>
      ),
    },
  ];

  const protocolOptions = (_mode: Link.Mode | undefined) => {
    const protocols = linkEnums?.protocols || ["SL651", "Modbus", "Modbus TCP", "Modbus RTU", "S7"];
    return protocols;
  };

  return (
    <PageContainer
      header={
        <div className="flex items-center justify-between flex-wrap gap-2">
          <Space wrap>
            <h3 className="text-base font-medium m-0">链路管理</h3>
            {publicIpData?.ip && <Tag color="blue">公网IP: {publicIpData.ip}</Tag>}
          </Space>
          <Space wrap>
            <Search
              allowClear
              placeholder="链路名称 / IP地址"
              onChange={(e) => debouncedSearch(e.target.value)}
              onSearch={doSearch}
              className="w-60"
            />
            {canAdd && (
              <Button type="primary" onClick={openCreateModal}>
                新建链路
              </Button>
            )}
          </Space>
        </div>
      }
    >
      <Table<Link.Item>
        rowKey="id"
        columns={columns}
        dataSource={linkPage?.list || []}
        loading={loadingLinks}
        pagination={{
          current: pagination.page,
          pageSize: pagination.pageSize,
          total: linkPage?.total || 0,
          showSizeChanger: true,
          showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
        }}
        onChange={handleTableChange}
        size="middle"
        scroll={{ x: "max-content" }}
        sticky
      />

      <Modal
        open={modalVisible}
        title={editing ? "编辑链路" : "新建链路"}
        onCancel={() => {
          setModalVisible(false);
          setEditing(null);
        }}
        onOk={() => form.submit()}
        confirmLoading={saveMutation.isPending}
        afterOpenChange={(open) => {
          if (!open) form.resetFields();
        }}
        destroyOnHidden
        width={760}
      >
        <Form<LinkFormValues> form={form} layout="vertical" onFinish={onFinish}>
          <Form.Item name="id" hidden>
            <Input />
          </Form.Item>

          <Form.Item
            label="链路名称"
            name="name"
            rules={[{ required: true, message: "请输入链路名称" }]}
          >
            <Input placeholder="链路名称" />
          </Form.Item>

          <Form.Item
            label="模式"
            name="mode"
            rules={[{ required: true, message: "请选择模式" }]}
            extra={editing ? "链路创建后模式不可修改" : undefined}
          >
            <Select onChange={handleModeChange} disabled={!!editing}>
              {linkEnums?.modes.map((mode) => (
                <Select.Option key={mode} value={mode}>
                  {mode}
                </Select.Option>
              ))}
            </Select>
          </Form.Item>

          <Form.Item label="协议" extra={editing ? "链路创建后协议不可修改" : undefined}>
            <Form.Item noStyle dependencies={["mode"]}>
              {({ getFieldValue }) => {
                const mode = getFieldValue("mode") as Link.Mode | undefined;
                return (
                  <Form.Item
                    name="protocol"
                    rules={[{ required: true, message: "请选择协议" }]}
                    noStyle
                  >
                    <Select disabled={!!editing}>
                      {protocolOptions(mode).map((protocol) => (
                        <Select.Option key={protocol} value={protocol}>
                          {protocol}
                        </Select.Option>
                      ))}
                    </Select>
                  </Form.Item>
                );
              }}
            </Form.Item>
          </Form.Item>

          <Form.Item noStyle shouldUpdate={(prev, next) => prev.mode !== next.mode}>
            {({ getFieldValue }) =>
              getFieldValue("mode") === "TCP Server" ? (
                <div className="grid grid-cols-2 gap-3">
                  <Form.Item
                    label="监听IP"
                    name="ip"
                    rules={[
                      { required: true, message: "请输入监听IP" },
                      {
                        pattern: /^(\d{1,3}\.){3}\d{1,3}$/,
                        message: "请输入正确的IPv4地址格式",
                      },
                    ]}
                  >
                    <Input placeholder="0.0.0.0" disabled />
                  </Form.Item>
                  <Form.Item
                    label="监听端口"
                    name="port"
                    rules={[
                      { required: true, message: "请输入监听端口" },
                      { type: "number", min: 1, max: 65535, message: "端口范围 1-65535" },
                    ]}
                  >
                    <InputNumber placeholder="如: 8080" className="!w-full" min={1} max={65535} />
                  </Form.Item>
                </div>
              ) : (
                <Form.List
                  name="targets"
                  rules={[
                    {
                      validator: async (_, targets) => {
                        if (!targets?.length) throw new Error("至少配置一个目标地址");
                      },
                    },
                  ]}
                >
                  {(fields, { add, remove }, { errors }) => (
                    <div>
                      <div className="mb-2 flex items-center justify-between">
                        <span>目标地址</span>
                        <Button
                          type="dashed"
                          onClick={() =>
                            add({ ...createTarget(), name: `目标${fields.length + 1}` })
                          }
                        >
                          添加目标
                        </Button>
                      </div>
                      {fields.map((field) => (
                        <div
                          key={field.key}
                          className="mb-3 grid grid-cols-[1fr_1.25fr_110px_100px_auto] items-start gap-2 rounded-lg border border-gray-200 p-3"
                        >
                          <Form.Item name={[field.name, "id"]} hidden>
                            <Input />
                          </Form.Item>
                          <Form.Item
                            name={[field.name, "name"]}
                            rules={[{ required: true, message: "请输入名称" }]}
                          >
                            <Input placeholder="目标名称" />
                          </Form.Item>
                          <Form.Item
                            name={[field.name, "ip"]}
                            rules={[
                              { required: true, message: "请输入目标IP" },
                              {
                                pattern: /^(\d{1,3}\.){3}\d{1,3}$/,
                                message: "IPv4格式错误",
                              },
                            ]}
                          >
                            <Input placeholder="192.168.1.100" />
                          </Form.Item>
                          <Form.Item
                            name={[field.name, "port"]}
                            rules={[
                              { required: true, message: "请输入端口" },
                              { type: "number", min: 1, max: 65535, message: "1-65535" },
                            ]}
                          >
                            <InputNumber className="!w-full" min={1} max={65535} />
                          </Form.Item>
                          <Form.Item name={[field.name, "status"]}>
                            <Select>
                              <Select.Option value="enabled">启用</Select.Option>
                              <Select.Option value="disabled">禁用</Select.Option>
                            </Select>
                          </Form.Item>
                          <Button danger type="text" onClick={() => remove(field.name)}>
                            删除
                          </Button>
                        </div>
                      ))}
                      <Form.ErrorList errors={errors} />
                    </div>
                  )}
                </Form.List>
              )
            }
          </Form.Item>

          <Form.Item label="状态" name="status" rules={[{ required: true }]}>
            <Select>
              <Select.Option value="enabled">启用</Select.Option>
              <Select.Option value="disabled">禁用</Select.Option>
            </Select>
          </Form.Item>
        </Form>
      </Modal>
    </PageContainer>
  );
};

export default LinkPage;
