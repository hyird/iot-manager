import { PlusOutlined } from "@ant-design/icons";
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
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
  TreeSelect,
  type TreeSelectProps,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useMemo, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { StatusTag } from "@/components/StatusTag";
import { useDebounceFn, usePermission } from "@/hooks";
import { departmentApi, departmentQueryKeys, userApi, userQueryKeys } from "@/services";
import type { Department, User } from "@/types";

const { Search } = Input;

interface DepartmentFormValues {
  id?: number;
  name: string;
  code?: string;
  parent_id?: number | null;
  sort_order?: number;
  leader_id?: number | null;
  status: Department.Status;
}

function filterDepartmentTree(tree: Department.TreeItem[], keyword: string): Department.TreeItem[] {
  const kw = keyword.trim().toLowerCase();
  if (!kw) return tree;

  const filter = (nodes: Department.TreeItem[]): Department.TreeItem[] => {
    const result: Department.TreeItem[] = [];
    nodes.forEach((n) => {
      const selfMatch =
        n.name.toLowerCase().includes(kw) || (n.code || "").toLowerCase().includes(kw);
      const children = n.children ? filter(n.children) : [];
      if (selfMatch || children.length > 0) {
        const node: Department.TreeItem = { ...n };
        if (children.length > 0) {
          node.children = children;
        } else {
          delete node.children;
        }
        result.push(node);
      }
    });
    return result;
  };
  return filter(tree);
}

const SystemDepartmentPage = () => {
  const [keyword, setKeyword] = useState("");
  const [modalVisible, setModalVisible] = useState(false);
  const [editing, setEditing] = useState<Department.TreeItem | null>(null);
  const [form] = Form.useForm<DepartmentFormValues>();
  const queryClient = useQueryClient();
  const { message, modal } = App.useApp();

  // 权限检查
  const canQuery = usePermission("system:dept:query");
  const canAdd = usePermission("system:dept:add");
  const canEdit = usePermission("system:dept:edit");
  const canDelete = usePermission("system:dept:delete");

  // 立即搜索
  const doSearch = (value: string) => setKeyword(value);

  // 防抖搜索（用于输入时）
  const { run: debouncedSearch } = useDebounceFn(doSearch, 300);

  // 部门树 - 需要查询权限
  const { data: rawDepartmentTree = [], isLoading } = useQuery({
    queryKey: departmentQueryKeys.tree(),
    queryFn: () => departmentApi.getTree(),
    enabled: canQuery,
  });

  // 用户列表 - 用于负责人选择
  const { data: usersData } = useQuery({
    queryKey: userQueryKeys.lists(),
    queryFn: () => userApi.getList({}),
    enabled: canQuery,
  });
  const userList = useMemo(() => usersData?.list || [], [usersData?.list]);

  // 缓存用户选择器选项，避免每次渲染重新创建
  const userSelectOptions = useMemo(
    () => userList.map((u) => ({ value: u.id, label: u.nickname || u.username })),
    [userList]
  );
  const userMap = useMemo(() => {
    const map = new Map<number, User.Item>();
    for (const u of userList) {
      map.set(u.id, u);
    }
    return map;
  }, [userList]);

  const departmentTree = useMemo(
    () => filterDepartmentTree(rawDepartmentTree, keyword),
    [rawDepartmentTree, keyword]
  );

  const getParentTreeData = (excludeId?: number): TreeSelectProps["treeData"] => {
    const loop = (nodes: Department.TreeItem[]): TreeSelectProps["treeData"] =>
      nodes
        .filter((n) => n.id !== excludeId)
        .map((n) => ({
          title: n.name,
          value: n.id,
          children: n.children ? loop(n.children) : undefined,
        }));
    return loop(rawDepartmentTree);
  };

  const saveMutation = useMutation({
    mutationFn: async (values: DepartmentFormValues) => {
      if (values.id) {
        const payload: Department.UpdateDto = {
          name: values.name,
          code: values.code,
          parent_id: values.parent_id === undefined ? null : values.parent_id,
          sort_order: values.sort_order,
          leader_id: values.leader_id,
          status: values.status,
        };
        await departmentApi.update(values.id, payload);
      } else {
        const payload: Department.CreateDto = {
          name: values.name,
          code: values.code,
          parent_id: values.parent_id === undefined ? null : values.parent_id,
          sort_order: values.sort_order,
          leader_id: values.leader_id,
          status: values.status,
        };
        await departmentApi.create(payload);
      }
    },
    onSuccess: () => {
      message.success("保存成功");
      setModalVisible(false);
      setEditing(null);
      form.resetFields();
      queryClient.invalidateQueries({ queryKey: departmentQueryKeys.all });
    },
  });

  const deleteMutation = useMutation({
    mutationFn: departmentApi.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: departmentQueryKeys.all });
    },
  });

  const openCreateModal = () => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({
      status: "enabled",
      sort_order: 0,
      parent_id: null,
    });
    setModalVisible(true);
  };

  const openCreateChildModal = (parent: Department.TreeItem) => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({
      status: "enabled",
      sort_order: 0,
      parent_id: parent.id,
    });
    setModalVisible(true);
  };

  const openEditModal = (record: Department.TreeItem) => {
    setEditing(record);
    form.setFieldsValue({
      id: record.id,
      name: record.name,
      code: record.code,
      parent_id: record.parent_id ?? null,
      sort_order: record.sort_order,
      leader_id: record.leader_id ?? null,
      status: record.status,
    });
    setModalVisible(true);
  };

  const onDelete = (record: Department.TreeItem) => {
    modal.confirm({
      title: `确认删除部门「${record.name}」吗？`,
      content: "若存在子部门或用户，请先处理后再删除。",
      onOk: () => deleteMutation.mutate(record.id),
    });
  };

  const onFinish = (values: DepartmentFormValues) => {
    saveMutation.mutate(values);
  };

  // 无查询权限时显示提示
  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询部门列表的权限，请联系管理员" />
      </PageContainer>
    );
  }

  const columns: ColumnsType<Department.TreeItem> = [
    {
      title: "部门名称",
      dataIndex: "name",
    },
    {
      title: "部门编码",
      dataIndex: "code",
      render: (v) => v || "-",
    },
    {
      title: "负责人",
      dataIndex: "leader_id",
      render: (v: number | null) =>
        v ? userMap.get(v)?.nickname || userMap.get(v)?.username || "-" : "-",
    },
    {
      title: "联系电话",
      dataIndex: "leader_id",
      key: "phone",
      render: (v: number | null) => (v ? userMap.get(v)?.phone || "-" : "-"),
    },
    {
      title: "邮箱",
      dataIndex: "leader_id",
      key: "email",
      render: (v: number | null) => (v ? userMap.get(v)?.email || "-" : "-"),
    },
    {
      title: "排序",
      dataIndex: "sort_order",
    },
    {
      title: "状态",
      dataIndex: "status",
      render: (v: Department.Status) => <StatusTag status={v} />,
    },
    {
      title: "操作",
      key: "actions",
      width: 200,
      fixed: "right" as const,
      render: (_, record) => (
        <Space>
          {canAdd && (
            <Button
              type="link"
              size="small"
              icon={<PlusOutlined />}
              onClick={() => openCreateChildModal(record)}
            >
              新增
            </Button>
          )}
          {canEdit && (
            <Button type="link" size="small" onClick={() => openEditModal(record)}>
              编辑
            </Button>
          )}
          {canDelete && (
            <Button type="link" size="small" danger onClick={() => onDelete(record)}>
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
          <h3 className="text-base font-medium m-0">部门管理</h3>
          <Space wrap>
            <Search
              allowClear
              placeholder="部门名称 / 编码"
              onChange={(e) => debouncedSearch(e.target.value)}
              onSearch={doSearch}
              className="w-60"
            />
            {canAdd && (
              <Button type="primary" onClick={openCreateModal}>
                新建部门
              </Button>
            )}
          </Space>
        </div>
      }
    >
      <Table<Department.TreeItem>
        rowKey="id"
        columns={columns}
        dataSource={departmentTree}
        loading={isLoading}
        pagination={false}
        size="middle"
        expandable={{
          defaultExpandAllRows: true,
          rowExpandable: (record) => Array.isArray(record.children) && record.children.length > 0,
        }}
        scroll={{ x: "max-content" }}
        sticky
      />

      <Modal
        open={modalVisible}
        title={editing ? "编辑部门" : "新建部门"}
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
        <Form<DepartmentFormValues> form={form} layout="vertical" onFinish={onFinish}>
          <Form.Item name="id" hidden>
            <Input />
          </Form.Item>

          <Form.Item
            label="部门名称"
            name="name"
            rules={[{ required: true, message: "请输入部门名称" }]}
          >
            <Input placeholder="请输入部门名称" />
          </Form.Item>

          <Form.Item label="部门编码" name="code">
            <Input placeholder="请输入部门编码（可选）" />
          </Form.Item>

          <Form.Item label="上级部门" name="parent_id">
            <TreeSelect
              allowClear
              treeData={getParentTreeData(editing?.id)}
              placeholder="不选则为顶级部门"
              treeDefaultExpandAll
            />
          </Form.Item>

          <Form.Item label="负责人" name="leader_id">
            <Select
              allowClear
              showSearch
              placeholder="请选择负责人"
              optionFilterProp="label"
              options={userSelectOptions}
            />
          </Form.Item>

          <Form.Item label="排序" name="sort_order">
            <InputNumber className="!w-full" placeholder="数值越小越靠前" />
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

export default SystemDepartmentPage;
