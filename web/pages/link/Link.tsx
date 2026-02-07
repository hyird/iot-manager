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

/** 连接状态配置 */
const connStatusConfig: Record<string, { label: string; color: string }> = {
  stopped: { label: "已停止", color: "default" },
  listening: { label: "监听中", color: "processing" },
  connected: { label: "已连接", color: "success" },
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
  status: Link.Status;
}

const LinkPage = () => {
  const [keyword, setKeyword] = useState("");
  const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
  const [modalVisible, setModalVisible] = useState(false);
  const [editing, setEditing] = useState<Link.Item | null>(null);
  const [form] = Form.useForm<LinkFormValues>();
  const { modal } = App.useApp();

  // 权限检查
  const canQuery = usePermission("iot:link:query");
  const canAdd = usePermission("iot:link:add");
  const canEdit = usePermission("iot:link:edit");
  const canDelete = usePermission("iot:link:delete");

  // 立即搜索
  const doSearch = (value: string) => {
    setKeyword(value);
    setPagination((prev) => ({ ...prev, page: 1 }));
  };

  // 防抖搜索（用于输入时）
  const { run: debouncedSearch } = useDebounceFn(doSearch, 300);

  // ========== 使用 Service Hooks ==========

  // 服务器公网 IP
  const { data: publicIpData } = usePublicIp({ enabled: canQuery });

  // 链路枚举值（模式和协议列表）
  const { data: linkEnums } = useLinkEnums({ enabled: canQuery });

  // 链路列表（WS 连接时禁用轮询，断开时回退到 10s 轮询）
  const { connected: wsConnected } = useWsStatus();
  const { data: linkPage, isLoading: loadingLinks } = useLinkList(
    { page: pagination.page, pageSize: pagination.pageSize, keyword: keyword || undefined },
    { enabled: canQuery, refetchInterval: wsConnected ? false : 10000 }
  );

  // 保存 mutation
  const saveMutation = useLinkSave();

  // 删除 mutation
  const deleteMutation = useLinkDelete();

  // ========== 事件处理 ==========

  const openCreateModal = () => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({ status: "enabled", mode: "TCP Client", protocol: "SL651", ip: "" });
    setModalVisible(true);
  };

  /** 根据模式获取可用的协议列表 */
  const getProtocolsByMode = (mode: Link.Mode): Link.Protocol[] => {
    if (mode === "TCP Server") {
      // TCP Server: SL651 或 Modbus RTU（串口透传）
      return ["SL651", "Modbus RTU"];
    } else {
      return ["SL651", "Modbus TCP", "Modbus RTU"];
    }
  };

  /** mode 变化时处理 IP 字段和协议选择 */
  const handleModeChange = (mode: Link.Mode) => {
    if (mode === "TCP Server") {
      form.setFieldValue("ip", "0.0.0.0");
    } else {
      // 切换到 Client 模式时，如果当前是 0.0.0.0 则清空
      const currentIp = form.getFieldValue("ip");
      if (currentIp === "0.0.0.0") {
        form.setFieldValue("ip", "");
      }
    }

    // 检查当前协议是否在新模式的可用列表中
    const currentProtocol = form.getFieldValue("protocol") as Link.Protocol;
    const availableProtocols = getProtocolsByMode(mode);
    if (currentProtocol && !availableProtocols.includes(currentProtocol)) {
      // Modbus TCP 在 Server 模式不可用，自动切换为 Modbus RTU
      if (currentProtocol === "Modbus TCP") {
        form.setFieldValue("protocol", "Modbus RTU");
      }
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
    saveMutation.mutate(values as Link.CreateDto & { id?: number }, {
      onSuccess: () => {
        setModalVisible(false);
        setEditing(null);
        form.resetFields();
      },
    });
  };

  const handleTableChange = (paginationConfig: TablePaginationConfig) => {
    setPagination({
      page: paginationConfig.current || 1,
      pageSize: paginationConfig.pageSize || 10,
    });
  };

  // ========== 渲染 ==========

  // 无查询权限时显示提示
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
    { title: "IP地址", dataIndex: "ip" },
    { title: "端口", dataIndex: "port" },
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

        // Server 模式显示客户端数量和 IP 列表
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

        return <Tag color={config.color}>{config.label}</Tag>;
      },
    },
    { title: "创建时间", dataIndex: "created_at" },
    {
      title: "操作",
      key: "actions",
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
          form.resetFields();
        }}
        onOk={() => form.submit()}
        confirmLoading={saveMutation.isPending}
        destroyOnHidden
        width={480}
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

          <Form.Item label="模式" name="mode" rules={[{ required: true, message: "请选择模式" }]}>
            <Select onChange={handleModeChange} disabled={!!editing}>
              {linkEnums?.modes.map((mode) => (
                <Select.Option key={mode} value={mode}>
                  {mode}
                </Select.Option>
              ))}
            </Select>
          </Form.Item>

          <Form.Item noStyle dependencies={["mode"]}>
            {({ getFieldValue }) => {
              const mode = getFieldValue("mode") as Link.Mode;
              const protocols = getProtocolsByMode(mode || "TCP Client");
              return (
                <Form.Item
                  label="协议"
                  name="protocol"
                  rules={[{ required: true, message: "请选择协议" }]}
                  extra={
                    mode === "TCP Server"
                      ? "Server 模式下 Modbus 仅支持 RTU（串口透传）"
                      : undefined
                  }
                >
                  <Select disabled={!!editing}>
                    {protocols.map((protocol) => (
                      <Select.Option key={protocol} value={protocol}>
                        {protocol}
                      </Select.Option>
                    ))}
                  </Select>
                </Form.Item>
              );
            }}
          </Form.Item>

          <Form.Item noStyle dependencies={["mode"]}>
            {({ getFieldValue }) => {
              const isServer = getFieldValue("mode") === "TCP Server";
              return (
                <Form.Item
                  label="IP地址"
                  name="ip"
                  rules={[
                    { required: true, message: "请输入IP地址" },
                    {
                      pattern: /^(\d{1,3}\.){3}\d{1,3}$/,
                      message: "请输入正确的IP地址格式",
                    },
                  ]}
                  extra={isServer ? "Server 模式固定监听 0.0.0.0" : undefined}
                >
                  <Input
                    placeholder={isServer ? "0.0.0.0" : "如: 192.168.1.100"}
                    disabled={isServer}
                  />
                </Form.Item>
              );
            }}
          </Form.Item>

          <Form.Item
            label="端口"
            name="port"
            rules={[
              { required: true, message: "请输入端口" },
              { type: "number", min: 1, max: 65535, message: "端口范围 1-65535" },
            ]}
          >
            <InputNumber placeholder="如: 8080" className="!w-full" min={1} max={65535} />
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
