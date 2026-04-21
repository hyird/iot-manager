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
        title: "用户名",
        dataIndex: "username",
        width: 180,
      },
      {
        title: "昵称",
        dataIndex: "nickname",
        render: (value?: string) => value || "-",
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
            title={`取消用户「${record.username}」的设备分享？`}
            okText="确认"
            cancelText="取消"
            onConfirm={() =>
              deleteMutation.mutate({
                deviceId,
                userId: record.user_id,
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
          layout="inline"
          initialValues={{ department_ids: [], user_ids: [], permission: "view" }}
          className="w-full"
        >
          <Form.Item name="department_ids" className="flex-1 min-w-[260px]">
            <Select
              mode="multiple"
              allowClear
              showSearch
              placeholder="选择部门（可多选）"
              options={departmentOptions}
              optionFilterProp="label"
            />
          </Form.Item>
          <Form.Item name="user_ids" className="flex-1 min-w-[320px]">
            <Select
              mode="multiple"
              allowClear
              showSearch
              placeholder="选择用户（可多选）"
              options={userOptions}
              optionFilterProp="label"
            />
          </Form.Item>
          <Form.Item name="permission" className="min-w-[120px]">
            <Select
              options={[
                { label: "只读", value: "view" },
                { label: "读写", value: "control" },
              ]}
            />
          </Form.Item>
          <Form.Item>
            <Button type="primary" onClick={handleSubmit} loading={saveMutation.isPending}>
              添加/更新
            </Button>
          </Form.Item>
        </Form>

        <Table<Device.ShareItem>
          rowKey={(record) => record.user_id}
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
