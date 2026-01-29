import { useState, useMemo } from "react";
import {
  Button,
  Form,
  Input,
  Modal,
  Select,
  Space,
  Table,
  Tag,
  TreeSelect,
  App,
  Result,
} from "antd";
import type { ColumnsType, TablePaginationConfig } from "antd/es/table";
import { useDebounceFn } from "ahooks";
import type { User, Role } from "@/types";
import { usePermission } from "@/hooks";
import { StatusTag } from "@/components/StatusTag";
import { PageContainer } from "@/components/PageContainer";
import {
  useUserList,
  useUserSave,
  useUserDelete,
  useRoleOptions,
  useDepartmentTreeSelect,
} from "@/services";

const { Search } = Input;

interface UserFormValues {
  id?: number;
  username: string;
  password?: string;
  nickname?: string;
  phone?: string;
  email?: string;
  department_id?: number | null;
  status: User.Status;
  roleIds?: number[];
}

const SystemUserPage = () => {
  const [keyword, setKeyword] = useState("");
  const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
  const [modalVisible, setModalVisible] = useState(false);
  const [editing, setEditing] = useState<User.Item | null>(null);
  const [form] = Form.useForm<UserFormValues>();
  const { modal } = App.useApp();

  // 权限检查
  const canQuery = usePermission("system:user:query");
  const canAdd = usePermission("system:user:add");
  const canEdit = usePermission("system:user:edit");
  const canDelete = usePermission("system:user:delete");

  // 立即搜索
  const doSearch = (value: string) => {
    setKeyword(value);
    setPagination((prev) => ({ ...prev, page: 1 }));
  };

  // 防抖搜索（用于输入时）
  const { run: debouncedSearch } = useDebounceFn(doSearch, { wait: 300 });

  // ========== 使用 Service Hooks ==========

  // 用户列表
  const { data: userPage, isLoading: loadingUsers } = useUserList(
    { page: pagination.page, pageSize: pagination.pageSize, keyword: keyword || undefined },
    { enabled: canQuery }
  );

  // 部门树（带 TreeSelect 数据转换）
  const { treeData: departmentTreeData, departmentMap } = useDepartmentTreeSelect("enabled");

  // 角色选项
  const { data: roleOptions = [] } = useRoleOptions({ enabled: canAdd || canEdit });

  // 缓存角色选择器选项，避免每次渲染重新创建
  const roleSelectOptions = useMemo(
    () => roleOptions.map((r: Role.Option) => ({ label: r.name, value: r.id })),
    [roleOptions]
  );

  // 保存 mutation
  const saveMutation = useUserSave();

  // 删除 mutation
  const deleteMutation = useUserDelete();

  // ========== 事件处理 ==========

  const openCreateModal = () => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({ status: "enabled" });
    setModalVisible(true);
  };

  const openEditModal = (record: User.Item) => {
    setEditing(record);
    form.setFieldsValue({
      id: record.id,
      username: record.username,
      nickname: record.nickname ?? undefined,
      phone: record.phone ?? undefined,
      email: record.email ?? undefined,
      department_id: record.department_id ?? undefined,
      status: record.status,
      roleIds: record.roles.map((r) => r.id),
    });
    setModalVisible(true);
  };

  const onDelete = (record: User.Item) => {
    modal.confirm({
      title: `确认删除用户「${record.username}」吗？`,
      onOk: () => deleteMutation.mutate(record.id),
    });
  };

  const onFinish = (values: UserFormValues) => {
    saveMutation.mutate(values as User.CreateDto & { id?: number }, {
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
        <Result status="403" title="无权限" subTitle="您没有查询用户列表的权限，请联系管理员" />
      </PageContainer>
    );
  }

  const columns: ColumnsType<User.Item> = [
    { title: "用户名", dataIndex: "username", width: 120 },
    { title: "昵称", dataIndex: "nickname", width: 120 },
    { title: "手机号", dataIndex: "phone", width: 140 },
    { title: "邮箱", dataIndex: "email", width: 180 },
    {
      title: "部门",
      dataIndex: "department_id",
      width: 120,
      render: (id: number | null) => (id ? departmentMap.get(id)?.name || "-" : "-"),
    },
    {
      title: "角色",
      dataIndex: "roles",
      width: 200,
      render: (roles: User.Item["roles"]) =>
        roles.map((r) => (
          <Tag key={r.id} color="blue">
            {r.name}
          </Tag>
        )),
    },
    {
      title: "状态",
      dataIndex: "status",
      width: 80,
      render: (v: User.Status) => <StatusTag status={v} />,
    },
    {
      title: "操作",
      key: "actions",
      width: 150,
      render: (_, record) => {
        const isBuiltinAdmin = record.username === "admin";
        return (
          <Space>
            {canEdit && (
              <Button type="link" onClick={() => openEditModal(record)} disabled={isBuiltinAdmin}>
                编辑
              </Button>
            )}
            {canDelete && (
              <Button type="link" danger onClick={() => onDelete(record)} disabled={isBuiltinAdmin}>
                删除
              </Button>
            )}
          </Space>
        );
      },
    },
  ];

  return (
    <PageContainer
      header={
        <div className="flex items-center justify-between">
          <h3 className="text-base font-medium m-0">用户管理</h3>
          <Space>
            <Search
              allowClear
              placeholder="用户名 / 昵称 / 手机 / 邮箱"
              onChange={(e) => debouncedSearch(e.target.value)}
              onSearch={doSearch}
              style={{ width: 280 }}
            />
            {canAdd && (
              <Button type="primary" onClick={openCreateModal}>
                新建用户
              </Button>
            )}
          </Space>
        </div>
      }
    >
      <Table<User.Item>
        rowKey="id"
        columns={columns}
        dataSource={userPage?.list || []}
        loading={loadingUsers}
        pagination={{
          current: pagination.page,
          pageSize: pagination.pageSize,
          total: userPage?.total || 0,
          showSizeChanger: true,
          showTotal: (total) => `共 ${total} 条`,
        }}
        onChange={handleTableChange}
        size="middle"
        scroll={{ x: 1200 }}
        sticky
      />

      <Modal
        open={modalVisible}
        title={editing ? "编辑用户" : "新建用户"}
        onCancel={() => {
          setModalVisible(false);
          setEditing(null);
          form.resetFields();
        }}
        onOk={() => form.submit()}
        confirmLoading={saveMutation.isPending}
        destroyOnHidden
        width={560}
      >
        <Form<UserFormValues> form={form} layout="vertical" onFinish={onFinish}>
          <Form.Item name="id" hidden>
            <Input />
          </Form.Item>

          <Form.Item
            label="用户名"
            name="username"
            rules={[{ required: true, message: "请输入用户名" }]}
          >
            <Input placeholder="登录用户名" disabled={!!editing} />
          </Form.Item>

          <Form.Item
            label="密码"
            name="password"
            rules={editing ? [] : [{ required: true, message: "请输入密码" }]}
          >
            <Input.Password placeholder={editing ? "留空则不修改密码" : "请输入密码"} />
          </Form.Item>

          <Form.Item label="昵称" name="nickname">
            <Input placeholder="用户昵称" />
          </Form.Item>

          <Form.Item
            label="手机号"
            name="phone"
            rules={[
              {
                pattern: /^1[3-9]\d{9}$/,
                message: "请输入正确的手机号",
              },
            ]}
          >
            <Input placeholder="11位手机号" maxLength={11} />
          </Form.Item>

          <Form.Item
            label="邮箱"
            name="email"
            rules={[{ type: "email", message: "请输入正确的邮箱" }]}
          >
            <Input placeholder="电子邮箱" />
          </Form.Item>

          <Form.Item label="部门" name="department_id">
            <TreeSelect
              allowClear
              treeData={departmentTreeData}
              placeholder="选择所属部门"
              treeDefaultExpandAll
            />
          </Form.Item>
          <Form.Item
            label="角色"
            name="roleIds"
            rules={[{ required: true, message: "请选择至少一个角色" }]}
          >
            <Select
              mode="multiple"
              allowClear={false}
              placeholder="选择角色"
              disabled={editing?.username === "admin"}
              options={roleSelectOptions}
            />
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

export default SystemUserPage;
