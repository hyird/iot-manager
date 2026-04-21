import { DeleteOutlined } from "@ant-design/icons";
import type { ColumnsType } from "antd/es/table";
import { App, Button, Form, Input, Modal, Popconfirm, Select, Space, Table, Tag, Typography } from "antd";
import { useMemo } from "react";
import { useDeviceShareDelete, useDeviceShareSave, useDeviceShares } from "@/services";
import type { Device } from "@/types";

interface ShareFormValues {
  username: string;
  permission: Device.SharePermission;
}

interface DeviceShareModalProps {
  open: boolean;
  device: Device.RealTimeData | null;
  onClose: () => void;
}

const DeviceShareModal = ({ open, device, onClose }: DeviceShareModalProps) => {
  const { message } = App.useApp();
  const [form] = Form.useForm<ShareFormValues>();
  const deviceId = device?.id ?? 0;

  const { data: shares = [], isLoading } = useDeviceShares(deviceId, {
    enabled: open && deviceId > 0,
  });
  const saveMutation = useDeviceShareSave();
  const deleteMutation = useDeviceShareDelete();

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
          value === "control" ? <Tag color="orange">控制</Tag> : <Tag color="blue">查看</Tag>,
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
    const username = values.username.trim();
    if (!username) {
      message.warning("请输入分享用户名");
      return;
    }

    saveMutation.mutate(
      {
        deviceId,
        payload: {
          username,
          permission: values.permission,
        },
      },
      {
        onSuccess: () => {
          form.setFieldsValue({ username: "", permission: values.permission });
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
          仅支持给用户授予设备的查看或控制权限。被分享用户不能编辑、删除或再次分享该设备。
        </Typography.Text>

        <Form<ShareFormValues>
          form={form}
          layout="inline"
          initialValues={{ permission: "view" }}
          className="w-full"
        >
          <Form.Item
            name="username"
            rules={[{ required: true, message: "请输入用户名" }]}
            className="flex-1 min-w-[280px]"
          >
            <Input placeholder="输入要分享的用户名" maxLength={50} />
          </Form.Item>
          <Form.Item name="permission" className="min-w-[120px]">
            <Select
              options={[
                { label: "查看", value: "view" },
                { label: "控制", value: "control" },
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
