import { DeleteOutlined } from "@ant-design/icons";
import type { ColumnsType } from "antd/es/table";
import { App, Button, Form, Modal, Popconfirm, Select, Space, Table, Tag, Typography } from "antd";
import { useMemo } from "react";
import {
  useDepartmentTree,
  useDeviceShareDelete,
  useDeviceShareSave,
  useDeviceShares,
  useUserList,
} from "@/services";
import type { Department, Device } from "@/types";

interface ShareFormValues {
  department_ids?: number[];
  user_ids?: number[];
  permission: Device.SharePermission;
}

interface DeviceShareModalProps {
  open: boolean;
  device: Device.RealTimeData | null;
  onClose: () => void;
}

interface OptionItem {
  label: string;
  value: number;
}

const flattenDepartmentOptions = (
  nodes: Department.TreeItem[],
  prefix = ""
): OptionItem[] => {
  const items: OptionItem[] = [];
  for (const node of nodes) {
    const label = prefix ? `${prefix} / ${node.name}` : node.name;
    items.push({ label, value: node.id });
    if (node.children?.length) {
      items.push(...flattenDepartmentOptions(node.children, label));
    }
  }
  return items;
};

const DeviceShareModal = ({ open, device, onClose }: DeviceShareModalProps) => {
  const { message } = App.useApp();
  const [form] = Form.useForm<ShareFormValues>();
  const deviceId = device?.id ?? 0;

  const { data: shares = [], isLoading } = useDeviceShares(deviceId, {
    enabled: open && deviceId > 0,
  });
  const { data: departmentTree = [] } = useDepartmentTree("enabled", {
    enabled: open,
  });
  const { data: userPage } = useUserList(
    { page: 1, pageSize: 2000, status: "enabled" },
    { enabled: open }
  );
  const saveMutation = useDeviceShareSave();
  const deleteMutation = useDeviceShareDelete();

  const departmentOptions = useMemo(
    () => flattenDepartmentOptions(departmentTree),
    [departmentTree]
  );
  const userOptions = useMemo(() => {
    const users = userPage?.list ?? [];
    return users.map((user) => ({
      label: user.nickname ? `${user.nickname} (${user.username})` : user.username,
      value: user.id,
    }));
  }, [userPage?.list]);

  const columns = useMemo<ColumnsType<Device.ShareItem>>(
    () => [
      {
        title: "类型",
        dataIndex: "target_type",
        width: 100,
        render: (value: Device.ShareItem["target_type"]) =>
          value === "department" ? <Tag color="purple">部门</Tag> : <Tag color="blue">用户</Tag>,
      },
      {
        title: "分享对象",
        key: "target",
        render: (_, record) => {
          if (record.target_type === "department") {
            return record.department_name || `(部门 #${record.target_id})`;
          }
          const username = record.username || `用户 #${record.target_id}`;
          return record.nickname ? `${record.nickname} (${username})` : username;
        },
      },
      {
        title: "权限",
        dataIndex: "permission",
        width: 120,
        render: (value: Device.SharePermission) =>
          value === "control" ? <Tag color="orange">读写</Tag> : <Tag color="blue">只读</Tag>,
      },
      {
        title: "更新时间",
        dataIndex: "updated_at",
        width: 180,
        render: (value?: string) => value || "-",
      },
      {
        title: "操作",
        key: "actions",
        width: 110,
        render: (_, record) => (
          <Popconfirm
            title={`取消${record.target_type === "department" ? "部门" : "用户"}「${
              record.target_type === "department"
                ? record.department_name || `#${record.target_id}`
                : record.nickname
                  ? `${record.nickname} (${record.username || `#${record.target_id}`})`
                  : record.username || `#${record.target_id}`
            }」的设备分享？`}
            okText="确认"
            cancelText="取消"
            onConfirm={() =>
              deleteMutation.mutate({
                deviceId,
                targetType: record.target_type,
                targetId: record.target_id,
              })
            }
          >
            <Button
              type="link"
              danger
              icon={<DeleteOutlined />}
              loading={deleteMutation.isPending}
            >
              移除
            </Button>
          </Popconfirm>
        ),
      },
    ],
    [deleteMutation, deviceId]
  );

  const handleSubmit = async () => {
    if (!deviceId) return;

    const values = await form.validateFields();
    const departmentIds = (values.department_ids ?? []).filter((id) => Number.isInteger(id) && id > 0);
    const userIds = (values.user_ids ?? []).filter((id) => Number.isInteger(id) && id > 0);

    if (departmentIds.length === 0 && userIds.length === 0) {
      message.warning("请至少选择一个部门或用户");
      return;
    }

    saveMutation.mutate(
      {
        deviceId,
        payload: {
          department_ids: departmentIds,
          user_ids: userIds,
          permission: values.permission,
        },
      },
      {
        onSuccess: () => {
          form.setFieldsValue({
            department_ids: [],
            user_ids: [],
            permission: values.permission,
          });
        },
      }
    );
  };

  return (
    <Modal
      open={open}
      title={device ? `设备分享：${device.name}` : "设备分享"}
      onCancel={onClose}
      footer={null}
      destroyOnHidden
      width={760}
    >
      <Space direction="vertical" size={16} className="w-full">
        <Typography.Text type="secondary">
          支持同时选择多个部门和多个用户进行混合分享。分享后仅可按权限进行查看/下发，不能编辑、删除或再次分享设备。
        </Typography.Text>
        {typeof userPage?.total === "number" &&
          userPage.total > (userPage.list?.length ?? 0) && (
            <Typography.Text type="secondary">
              当前下拉仅加载前 {userPage.list?.length ?? 0} 个启用用户，如需更多可先通过用户筛选再分享。
            </Typography.Text>
          )}

        <Form<ShareFormValues>
          form={form}
          layout="vertical"
          initialValues={{ department_ids: [], user_ids: [], permission: "view" }}
          className="w-full"
        >
          <Form.Item label="部门" name="department_ids" className="w-full">
            <Select
              mode="multiple"
              allowClear
              showSearch
              placeholder="选择部门（可多选）"
              options={departmentOptions}
              optionFilterProp="label"
              className="w-full"
            />
          </Form.Item>
          <Form.Item label="用户" name="user_ids" className="w-full">
            <Select
              mode="multiple"
              allowClear
              showSearch
              placeholder="选择用户（可多选）"
              options={userOptions}
              optionFilterProp="label"
              className="w-full"
            />
          </Form.Item>
          <Form.Item label="权限" name="permission" className="w-full">
            <Select
              options={[
                { label: "只读", value: "view" },
                { label: "读写", value: "control" },
              ]}
              className="w-full"
            />
          </Form.Item>
          <Form.Item className="w-full !mb-0">
            <Button type="primary" block onClick={handleSubmit} loading={saveMutation.isPending}>
              添加/更新
            </Button>
          </Form.Item>
        </Form>

        <Table<Device.ShareItem>
          rowKey={(record) => `${record.target_type}-${record.target_id}`}
          columns={columns}
          dataSource={shares}
          loading={isLoading}
          pagination={false}
          locale={{ emptyText: "当前设备暂无分享记录" }}
          size="middle"
        />
      </Space>
    </Modal>
  );
};

export default DeviceShareModal;
