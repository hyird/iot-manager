import { App, Button, Input, Result, Select, Space, Table, Tabs, Tag } from "antd";
import type { ColumnsType, TablePaginationConfig } from "antd/es/table";
import { useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { StatusTag } from "@/components/StatusTag";
import { useDebounceFn, usePermission } from "@/hooks";
import {
  useAlertAcknowledge,
  useAlertBatchAcknowledge,
  useAlertRecordList,
  useAlertRuleDelete,
  useAlertRuleList,
  useAlertRuleSave,
} from "@/services";
import type { Alert } from "@/types";
import { AlertRuleFormModal } from "./AlertRuleFormModal";

const { Search } = Input;

const SEVERITY_OPTIONS = [
  { label: "全部级别", value: "" },
  { label: "严重", value: "critical" },
  { label: "警告", value: "warning" },
  { label: "信息", value: "info" },
];

const STATUS_OPTIONS = [
  { label: "全部状态", value: "" },
  { label: "活跃", value: "active" },
  { label: "已确认", value: "acknowledged" },
  { label: "已恢复", value: "resolved" },
];

const SEVERITY_COLORS: Record<string, string> = {
  critical: "red",
  warning: "orange",
  info: "blue",
};

const STATUS_COLORS: Record<string, string> = {
  active: "error",
  acknowledged: "warning",
  resolved: "success",
};

const STATUS_LABELS: Record<string, string> = {
  active: "活跃",
  acknowledged: "已确认",
  resolved: "已恢复",
};

// ==================== 告警规则 Tab ====================

function RuleTab() {
  const [keyword, setKeyword] = useState("");
  const [severity, setSeverity] = useState("");
  const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
  const [modalVisible, setModalVisible] = useState(false);
  const [editing, setEditing] = useState<Alert.RuleItem | null>(null);
  const { modal } = App.useApp();

  const canAdd = usePermission("iot:alert:add");
  const canEdit = usePermission("iot:alert:edit");
  const canDelete = usePermission("iot:alert:delete");

  const doSearch = (value: string) => {
    setKeyword(value);
    setPagination((prev) => ({ ...prev, page: 1 }));
  };

  const { run: debouncedSearch } = useDebounceFn(doSearch, 300);

  const { data: rulePage, isLoading } = useAlertRuleList({
    page: pagination.page,
    pageSize: pagination.pageSize,
    keyword: keyword || undefined,
    severity: severity || undefined,
  });

  const saveMutation = useAlertRuleSave();
  const deleteMutation = useAlertRuleDelete();

  const openCreateModal = () => {
    setEditing(null);
    setModalVisible(true);
  };

  const openEditModal = (record: Alert.RuleItem) => {
    setEditing(record);
    setModalVisible(true);
  };

  const onDelete = (record: Alert.RuleItem) => {
    modal.confirm({
      title: `确认删除规则「${record.name}」吗？`,
      content: "删除后该规则将停止告警检测。此操作不可撤销。",
      okText: "确定删除",
      okButtonProps: { danger: true },
      onOk: () => deleteMutation.mutate(record.id),
    });
  };

  const handleTableChange = (paginationConfig: TablePaginationConfig) => {
    setPagination({
      page: paginationConfig.current || 1,
      pageSize: paginationConfig.pageSize || 10,
    });
  };

  const columns: ColumnsType<Alert.RuleItem> = [
    { title: "规则名称", dataIndex: "name", ellipsis: true },
    { title: "关联设备", dataIndex: "device_name", ellipsis: true },
    {
      title: "严重级别",
      dataIndex: "severity",
      width: 100,
      render: (v: Alert.Severity) => (
        <Tag color={SEVERITY_COLORS[v]}>
          {v === "critical" ? "严重" : v === "warning" ? "警告" : "信息"}
        </Tag>
      ),
    },
    {
      title: "条件数",
      dataIndex: "conditions",
      width: 80,
      render: (conditions: Alert.Condition[]) => conditions?.length ?? 0,
    },
    {
      title: "逻辑",
      dataIndex: "logic",
      width: 80,
      render: (v: string) => <Tag>{v === "and" ? "全部满足" : "任一满足"}</Tag>,
    },
    {
      title: "冷却时间",
      dataIndex: "silence_duration",
      width: 100,
      render: (v: number) => `${v}秒`,
    },
    {
      title: "状态",
      dataIndex: "status",
      width: 80,
      render: (v: "enabled" | "disabled") => <StatusTag status={v} />,
    },
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
    <>
      <div className="flex items-center justify-between flex-wrap gap-2 mb-4">
        <Space wrap>
          <Search
            allowClear
            placeholder="规则名称 / 备注"
            onChange={(e) => debouncedSearch(e.target.value)}
            onSearch={doSearch}
            className="w-[220px]"
          />
          <Select
            value={severity}
            onChange={(v) => {
              setSeverity(v);
              setPagination((prev) => ({ ...prev, page: 1 }));
            }}
            options={SEVERITY_OPTIONS}
            className="w-[120px]"
          />
        </Space>
        {canAdd && (
          <Button type="primary" onClick={openCreateModal}>
            新建规则
          </Button>
        )}
      </div>

      <Table<Alert.RuleItem>
        rowKey="id"
        columns={columns}
        dataSource={rulePage?.list || []}
        loading={isLoading}
        pagination={{
          current: pagination.page,
          pageSize: pagination.pageSize,
          total: rulePage?.total || 0,
          showSizeChanger: true,
          showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
        }}
        onChange={handleTableChange}
        size="middle"
        scroll={{ x: "max-content" }}
        sticky
      />

      <AlertRuleFormModal
        open={modalVisible}
        editing={editing}
        saveMutation={saveMutation}
        onClose={() => {
          setModalVisible(false);
          setEditing(null);
        }}
      />
    </>
  );
}

// ==================== 告警记录 Tab ====================

function RecordTab() {
  const [severity, setSeverity] = useState("");
  const [status, setStatus] = useState("");
  const [pagination, setPagination] = useState({ page: 1, pageSize: 20 });
  const [selectedRowKeys, setSelectedRowKeys] = useState<number[]>([]);
  const { modal } = App.useApp();

  const canAck = usePermission("iot:alert:ack");

  const { data: recordPage, isLoading } = useAlertRecordList({
    page: pagination.page,
    pageSize: pagination.pageSize,
    severity: severity || undefined,
    status: status || undefined,
  });

  const ackMutation = useAlertAcknowledge();
  const batchAckMutation = useAlertBatchAcknowledge();

  const onAcknowledge = (record: Alert.RecordItem) => {
    modal.confirm({
      title: "确认告警",
      content: `确认告警「${record.message}」？`,
      okText: "确认",
      onOk: () => ackMutation.mutate(record.id),
    });
  };

  const onBatchAcknowledge = () => {
    if (selectedRowKeys.length === 0) return;
    modal.confirm({
      title: "批量确认",
      content: `确认选中的 ${selectedRowKeys.length} 条告警？`,
      okText: "确认",
      onOk: () =>
        batchAckMutation.mutate(selectedRowKeys, {
          onSuccess: () => setSelectedRowKeys([]),
        }),
    });
  };

  const handleTableChange = (paginationConfig: TablePaginationConfig) => {
    setPagination({
      page: paginationConfig.current || 1,
      pageSize: paginationConfig.pageSize || 20,
    });
  };

  const columns: ColumnsType<Alert.RecordItem> = [
    { title: "设备", dataIndex: "device_name", ellipsis: true, width: 120 },
    {
      title: "级别",
      dataIndex: "severity",
      width: 80,
      render: (v: Alert.Severity) => (
        <Tag color={SEVERITY_COLORS[v]}>
          {v === "critical" ? "严重" : v === "warning" ? "警告" : "信息"}
        </Tag>
      ),
    },
    { title: "告警消息", dataIndex: "message", ellipsis: true },
    {
      title: "触发时间",
      dataIndex: "triggered_at",
      width: 170,
      render: (v: string) => (v ? new Date(v).toLocaleString("zh-CN") : "-"),
    },
    {
      title: "状态",
      dataIndex: "status",
      width: 90,
      render: (v: Alert.RecordStatus) => (
        <Tag color={STATUS_COLORS[v]}>{STATUS_LABELS[v] || v}</Tag>
      ),
    },
    {
      title: "确认时间",
      dataIndex: "acknowledged_at",
      width: 170,
      render: (v: string | undefined) => (v ? new Date(v).toLocaleString("zh-CN") : "-"),
    },
    {
      title: "操作",
      key: "actions",
      width: 80,
      fixed: "right" as const,
      render: (_, record) =>
        canAck && record.status === "active" ? (
          <Button type="link" onClick={() => onAcknowledge(record)}>
            确认
          </Button>
        ) : null,
    },
  ];

  return (
    <>
      <div className="flex items-center justify-between flex-wrap gap-2 mb-4">
        <Space wrap>
          <Select
            value={severity}
            onChange={(v) => {
              setSeverity(v);
              setPagination((prev) => ({ ...prev, page: 1 }));
            }}
            options={SEVERITY_OPTIONS}
            className="w-[120px]"
          />
          <Select
            value={status}
            onChange={(v) => {
              setStatus(v);
              setPagination((prev) => ({ ...prev, page: 1 }));
            }}
            options={STATUS_OPTIONS}
            className="w-[120px]"
          />
        </Space>
        {canAck && selectedRowKeys.length > 0 && (
          <Button type="primary" onClick={onBatchAcknowledge} loading={batchAckMutation.isPending}>
            批量确认 ({selectedRowKeys.length})
          </Button>
        )}
      </div>

      <Table<Alert.RecordItem>
        rowKey="id"
        columns={columns}
        dataSource={recordPage?.list || []}
        loading={isLoading}
        rowSelection={
          canAck
            ? {
                selectedRowKeys,
                onChange: (keys) => setSelectedRowKeys(keys as number[]),
                getCheckboxProps: (record) => ({
                  disabled: record.status !== "active",
                }),
              }
            : undefined
        }
        pagination={{
          current: pagination.page,
          pageSize: pagination.pageSize,
          total: recordPage?.total || 0,
          showSizeChanger: true,
          showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
        }}
        onChange={handleTableChange}
        size="middle"
        scroll={{ x: "max-content" }}
        sticky
      />
    </>
  );
}

// ==================== 主页面 ====================

const AlertPage = () => {
  const canQuery = usePermission("iot:alert:query");

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询告警的权限，请联系管理员" />
      </PageContainer>
    );
  }

  return (
    <PageContainer>
      <Tabs
        defaultActiveKey="rule"
        items={[
          { key: "rule", label: "告警规则", children: <RuleTab /> },
          { key: "record", label: "告警记录", children: <RecordTab /> },
        ]}
      />
    </PageContainer>
  );
};

export default AlertPage;
