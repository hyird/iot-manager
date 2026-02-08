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
  Switch,
  Table,
  Tag,
  TreeSelect,
  type TreeSelectProps,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useEffect, useMemo, useState } from "react";
import DynamicIcon from "@/components/DynamicIcon";
import { PageContainer } from "@/components/PageContainer";
import { StatusTag } from "@/components/StatusTag";
import { getPageConfig, getRegisteredPages, getRegisteredPermissions } from "@/config/registry";
import { useDebounceFn, usePermission } from "@/hooks";
import { authKeys, menuApi, menuQueryKeys } from "@/services";
import type { Menu } from "@/types";
import { MenuTypeMap } from "@/types/constants";
import { filterMenuTree, flattenTree, getPathSegment } from "@/utils";

const { Search } = Input;

interface MenuFormValues {
  id?: number;
  name: string;
  pathSegment?: string;
  component?: string;
  icon?: string;
  parent_id?: number | null;
  sort_order?: number;
  type: Menu.Type;
  status: Menu.Status;
  permission_code?: string;
}

function typeTag(type: Menu.Type) {
  const config = MenuTypeMap[type];
  return <Tag color={config.color}>{config.text}</Tag>;
}

// 过滤掉按钮类型的树节点
function filterOutButtons(nodes: Menu.TreeItem[]): Menu.TreeItem[] {
  return nodes
    .filter((node) => node.type !== "button")
    .map((node) => {
      const filteredChildren = node.children ? filterOutButtons(node.children) : undefined;
      return {
        ...node,
        children: filteredChildren && filteredChildren.length > 0 ? filteredChildren : undefined,
      };
    });
}

const SystemMenuPage = () => {
  const [keyword, setKeyword] = useState("");
  const [hideButtons, setHideButtons] = useState(true);
  const [modalVisible, setModalVisible] = useState(false);
  const [editing, setEditing] = useState<Menu.TreeItem | null>(null);
  const [form] = Form.useForm<MenuFormValues>();
  const queryClient = useQueryClient();
  const { message, modal } = App.useApp();

  // 新增权限弹窗状态
  const [permModalVisible, setPermModalVisible] = useState(false);
  const [permTargetPage, setPermTargetPage] = useState<Menu.TreeItem | null>(null);
  const [permSelectedCodes, setPermSelectedCodes] = useState<string[]>([]);

  // 权限检查
  const canQuery = usePermission("system:menu:query");
  const canAdd = usePermission("system:menu:add");
  const canEdit = usePermission("system:menu:edit");
  const canDelete = usePermission("system:menu:delete");

  // 立即搜索
  const doSearch = (value: string) => setKeyword(value);

  // 防抖搜索（用于输入时）
  const { run: debouncedSearch } = useDebounceFn(doSearch, 300);

  // 菜单树 - 需要查询权限
  const { data: rawMenuTree = [], isLoading } = useQuery({
    queryKey: menuQueryKeys.tree(),
    queryFn: () => menuApi.getTree(),
    enabled: canQuery,
  });

  // 扁平化 map（不含 children，用于快速查找父级类型等）
  const menuMap = useMemo(() => {
    const flattened = flattenTree(rawMenuTree);
    return Object.fromEntries(flattened.map((item) => [item.id, item]));
  }, [rawMenuTree]);

  // 完整节点 map（保留 children，用于新增权限时判断已存在的子权限）
  const fullMenuMap = useMemo(() => {
    const map: Record<number, Menu.TreeItem> = {};
    const traverse = (nodes: Menu.TreeItem[]) => {
      nodes.forEach((node) => {
        map[node.id] = node;
        if (node.children?.length) traverse(node.children);
      });
    };
    traverse(rawMenuTree);
    return map;
  }, [rawMenuTree]);
  const menuTree = useMemo(() => {
    let tree = filterMenuTree(rawMenuTree, keyword);
    if (hideButtons) {
      tree = filterOutButtons(tree);
    }
    return tree;
  }, [rawMenuTree, keyword, hideButtons]);

  const parentTreeData = useMemo((): TreeSelectProps["treeData"] => {
    const loop = (nodes: Menu.TreeItem[]): TreeSelectProps["treeData"] =>
      nodes
        .filter((n) => n.type !== "button")
        .map((n) => ({
          title: `${n.name} (${n.type})`,
          value: n.id,
          children: n.children ? loop(n.children) : undefined,
        }));
    return loop(rawMenuTree);
  }, [rawMenuTree]);

  const watchParentId = Form.useWatch("parent_id", form) as number | null | undefined;
  const watchType = Form.useWatch("type", form) as Menu.Type | undefined;

  const parentType: Menu.Type | undefined = useMemo(() => {
    if (!watchParentId) return undefined;
    return menuMap[watchParentId]?.type;
  }, [watchParentId, menuMap]);

  const availableTypes: Menu.Type[] = useMemo(() => {
    if (!watchParentId) {
      return ["menu", "page"];
    }
    if (parentType === "menu") {
      return ["menu", "page"];
    }
    if (parentType === "page") {
      return ["button"];
    }
    return ["menu", "page"];
  }, [watchParentId, parentType]);

  // 缓存页面组件选项，避免每次渲染重新创建
  const pageSelectOptions = useMemo(
    () =>
      getRegisteredPages().map((page) => ({
        label: `${page.name} (${page.component})`,
        value: page.component,
        description: page.description,
      })),
    []
  );

  // 获取父级页面的可用权限列表
  const availablePermissions = useMemo(() => {
    // 如果父级不是 page 类型,或者当前类型不是 button,返回所有权限
    if (parentType !== "page" || watchType !== "button") {
      return getRegisteredPermissions();
    }

    // 获取父级菜单项
    const parentMenu = watchParentId ? menuMap[watchParentId] : undefined;
    if (!parentMenu?.component) {
      return getRegisteredPermissions();
    }

    // 从注册中心获取该页面配置
    const pageConfig = getPageConfig(parentMenu.component);
    if (!pageConfig?.permissions) {
      return [];
    }

    // 返回该页面下的权限列表 (需要补全 module 和 resource 字段用于显示)
    return pageConfig.permissions.map((perm) => ({
      ...perm,
      module: pageConfig.module,
      resource: pageConfig.name,
    }));
  }, [parentType, watchType, watchParentId, menuMap]);

  // 缓存权限选择器选项，避免每次渲染重新创建
  const permissionSelectOptions = useMemo(
    () =>
      availablePermissions.map((perm) => ({
        label: `${perm.name} (${perm.code})`,
        value: perm.code,
        module: perm.module,
        resource: perm.resource,
        description: perm.description,
      })),
    [availablePermissions]
  );

  // 当可选类型改变时,如果当前类型不在可选项中,自动设置为第一个可选项
  useEffect(() => {
    if (watchType && !availableTypes.includes(watchType)) {
      form.setFieldValue("type", availableTypes[0]);
    }
  }, [availableTypes, watchType, form]);

  const syncAuthAfterMenuChange = () => {
    queryClient.invalidateQueries({ queryKey: authKeys.currentUser });
  };

  const saveMutation = useMutation({
    mutationFn: async (values: MenuFormValues) => {
      const parent =
        values.parent_id !== undefined && values.parent_id !== null
          ? menuMap[values.parent_id]
          : undefined;

      let finalPath: string | null = null;
      if (values.type === "menu" || values.type === "page") {
        const seg = (values.pathSegment || "").trim();
        if (seg) {
          const normalizedSeg = seg.startsWith("/") ? seg : `/${seg}`;
          const parentPath = (parent?.path || "").trim();
          if (parentPath) {
            finalPath = `${parentPath.replace(/\/$/, "")}${normalizedSeg}`;
          } else {
            finalPath = normalizedSeg;
          }
        } else {
          finalPath = null;
        }
      } else {
        finalPath = null;
      }

      if (values.id) {
        const payload: Menu.UpdateDto = {
          name: values.name,
          path: finalPath ?? undefined,
          component: values.component,
          icon: values.icon,
          parent_id: values.parent_id === undefined ? null : values.parent_id,
          sort_order: values.sort_order,
          type: values.type,
          status: values.status,
          permission_code: values.permission_code,
        };
        await menuApi.update(values.id, payload);
        return;
      }

      const payload: Menu.CreateDto = {
        name: values.name,
        path: finalPath ?? undefined,
        component: values.component,
        icon: values.icon,
        parent_id: values.parent_id === undefined ? null : values.parent_id,
        sort_order: values.sort_order,
        type: values.type,
        status: values.status,
        permission_code: values.permission_code,
      };
      await menuApi.create(payload);
    },
    onSuccess: async () => {
      message.success("保存成功");
      setModalVisible(false);
      setEditing(null);
      form.resetFields();
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
      await syncAuthAfterMenuChange();
    },
  });

  const deleteMutation = useMutation({
    mutationFn: (id: number) => menuApi.remove(id),
    onSuccess: async () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
      await syncAuthAfterMenuChange();
    },
  });

  // 批量创建权限 mutation
  const batchCreatePermMutation = useMutation({
    mutationFn: async ({ parentPage, codes }: { parentPage: Menu.TreeItem; codes: string[] }) => {
      const pageConfig = parentPage.component ? getPageConfig(parentPage.component) : undefined;
      if (!pageConfig?.permissions) return;

      // 获取已存在的权限码
      const existingCodes = new Set(
        (parentPage.children || [])
          .filter((c) => c.type === "button" && c.permission_code)
          .map((c) => c.permission_code!)
      );

      // 只创建不存在的权限
      const toCreate = codes.filter((code) => !existingCodes.has(code));
      if (toCreate.length === 0) {
        message.warning("所选权限已存在");
        return;
      }

      // 获取当前最大排序号
      const maxSort = Math.max(0, ...(parentPage.children || []).map((c) => c.sort_order || 0));

      for (let i = 0; i < toCreate.length; i++) {
        const code = toCreate[i];
        const perm = pageConfig.permissions.find((p) => p.code === code);
        if (!perm) continue;

        await menuApi.create({
          name: perm.name,
          parent_id: parentPage.id,
          type: "button",
          status: "enabled",
          permission_code: perm.code,
          sort_order: maxSort + i + 1,
        });
      }

      return toCreate.length;
    },
    onSuccess: async (count) => {
      if (count && count > 0) {
        message.success(`成功添加 ${count} 个权限`);
      }
      setPermModalVisible(false);
      setPermTargetPage(null);
      setPermSelectedCodes([]);
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
      await syncAuthAfterMenuChange();
    },
  });

  // 打开新增权限弹窗
  const openPermModal = (record: Menu.TreeItem) => {
    // 从完整数据中获取（包含 button children），避免隐藏按钮时丢失已存在权限判断
    const fullRecord = fullMenuMap[record.id] || record;
    setPermTargetPage(fullRecord);
    setPermSelectedCodes([]);
    setPermModalVisible(true);
  };

  // 获取目标页面的可用权限列表
  const targetPagePermissions = useMemo(() => {
    if (!permTargetPage?.component) return [];
    const pageConfig = getPageConfig(permTargetPage.component);
    if (!pageConfig?.permissions) return [];

    // 获取已存在的权限码
    const existingCodes = new Set(
      (permTargetPage.children || [])
        .filter((c) => c.type === "button" && c.permission_code)
        .map((c) => c.permission_code!)
    );

    return pageConfig.permissions.map((perm) => ({
      ...perm,
      exists: existingCodes.has(perm.code),
    }));
  }, [permTargetPage]);

  const openCreateModal = (parentId?: number | null, defaultType?: Menu.Type) => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({
      type: defaultType ?? "menu",
      status: "enabled",
      sort_order: 0,
      parent_id: parentId ?? null,
    });
    setModalVisible(true);
  };

  const openEditModal = (record: Menu.TreeItem) => {
    setEditing(record);
    const pathSegment =
      record.type === "menu" || record.type === "page" ? getPathSegment(record, menuMap) : "";
    form.setFieldsValue({
      id: record.id,
      name: record.name,
      pathSegment,
      component: record.component,
      icon: record.icon,
      parent_id: record.parent_id ?? null,
      sort_order: record.sort_order,
      type: record.type,
      status: record.status,
      permission_code: record.permission_code,
    });
    setModalVisible(true);
  };

  const onDelete = (record: Menu.TreeItem) => {
    modal.confirm({
      title: `确认删除「${record.name}」吗？`,
      content: "若存在子菜单，请先删除子菜单。",
      onOk: () => deleteMutation.mutate(record.id),
    });
  };

  const onFinish = (values: MenuFormValues) => {
    saveMutation.mutate(values);
  };

  // 无查询权限时显示提示
  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询菜单列表的权限，请联系管理员" />
      </PageContainer>
    );
  }

  const columns: ColumnsType<Menu.TreeItem> = [
    {
      title: "名称",
      dataIndex: "name",
    },
    {
      title: "类型",
      dataIndex: "type",
      render: (t: Menu.Type) => typeTag(t),
    },
    {
      title: "图标",
      dataIndex: "icon",
      render: (icon: string | undefined) =>
        icon ? (
          <Space>
            <DynamicIcon name={icon} />
            <span className="text-gray-400 text-xs">{icon}</span>
          </Space>
        ) : (
          "-"
        ),
    },
    {
      title: "完整路由",
      dataIndex: "path",
      render: (v: string | null, record) => (record.type === "button" ? "-" : v || "-"),
    },
    {
      title: "组件标识",
      dataIndex: "component",
      render: (v: string | undefined, record) => (record.type === "page" ? v || "-" : "-"),
    },
    {
      title: "权限标识",
      dataIndex: "permission_code",
      render: (v: string | undefined, record) =>
        record.type === "button" ? v || <span className="text-[#faad14]">未配置</span> : "-",
    },
    {
      title: "排序",
      dataIndex: "sort_order",
    },
    {
      title: "状态",
      dataIndex: "status",
      render: (v: Menu.Status) => <StatusTag status={v} />,
    },
    {
      title: "操作",
      key: "actions",
      width: 260,
      fixed: "right" as const,
      render: (_, record) => (
        <Space>
          {canEdit && (
            <Button type="link" onClick={() => openEditModal(record)}>
              编辑
            </Button>
          )}
          {canAdd && record.type === "menu" && (
            <Button type="link" onClick={() => openCreateModal(record.id, "page")}>
              添加页面
            </Button>
          )}
          {canAdd && record.type === "page" && record.component && (
            <Button type="link" onClick={() => openPermModal(record)}>
              新增权限
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
          <h3 className="text-base font-medium m-0">菜单管理</h3>
          <Space wrap>
            <Search
              allowClear
              placeholder="按名称 / 路由搜索（树状过滤）"
              onChange={(e) => debouncedSearch(e.target.value)}
              onSearch={doSearch}
              className="w-full sm:w-[280px]"
            />
            <Switch
              checked={!hideButtons}
              onChange={(checked) => setHideButtons(!checked)}
              checkedChildren="显示权限"
              unCheckedChildren="隐藏权限"
            />
            {canAdd && (
              <Button type="primary" onClick={() => openCreateModal()}>
                新建菜单
              </Button>
            )}
          </Space>
        </div>
      }
    >
      <Table<Menu.TreeItem>
        rowKey="id"
        columns={columns}
        dataSource={menuTree}
        loading={isLoading}
        pagination={false}
        size="middle"
        scroll={{ x: "max-content" }}
        expandable={{
          defaultExpandAllRows: true,
          rowExpandable: (record) => Array.isArray(record.children) && record.children.length > 0,
        }}
        sticky
      />

      <Modal
        open={modalVisible}
        title={editing ? "编辑菜单" : "新建菜单"}
        onCancel={() => {
          setModalVisible(false);
          setEditing(null);
          form.resetFields();
        }}
        onOk={() => form.submit()}
        confirmLoading={saveMutation.isPending}
        destroyOnHidden
        width={640}
      >
        <Form<MenuFormValues> form={form} layout="vertical" onFinish={onFinish}>
          <Form.Item name="id" hidden>
            <Input />
          </Form.Item>

          <Form.Item label="名称" name="name" rules={[{ required: true, message: "请输入名称" }]}>
            <Input placeholder="菜单名称 / 页面标题 / 按钮名称" />
          </Form.Item>

          <Form.Item label="父级菜单" name="parent_id">
            <TreeSelect
              allowClear
              treeData={parentTreeData}
              placeholder="不选则为顶级"
              treeDefaultExpandAll
            />
          </Form.Item>

          <Form.Item label="类型" name="type" rules={[{ required: true, message: "请选择类型" }]}>
            <Select>
              {availableTypes.map((t) => (
                <Select.Option key={t} value={t}>
                  {t === "menu" ? "菜单" : t === "page" ? "页面" : "按钮"}
                </Select.Option>
              ))}
            </Select>
          </Form.Item>

          {(watchType === "menu" || watchType === "page") && (
            <Form.Item
              label="路径片段"
              name="pathSegment"
              rules={
                watchType === "page"
                  ? [
                      {
                        required: true,
                        message: "页面必须配置路径片段",
                      },
                    ]
                  : []
              }
            >
              <Input placeholder="例如：system / user / menu（不必带父路径）" />
            </Form.Item>
          )}

          {watchType === "page" && (
            <Form.Item
              label="组件标识"
              name="component"
              rules={[
                {
                  required: true,
                  message: "页面必须配置组件标识",
                },
              ]}
            >
              <Select
                showSearch
                allowClear
                placeholder="选择页面组件"
                optionFilterProp="label"
                options={pageSelectOptions}
                optionRender={(option) => (
                  <div>
                    <div>{option.label}</div>
                    {option.data.description && (
                      <div className="text-xs text-gray-400">{option.data.description}</div>
                    )}
                  </div>
                )}
              />
            </Form.Item>
          )}

          {watchType === "button" && (
            <Form.Item
              label="权限标识"
              name="permission_code"
              rules={[
                {
                  required: true,
                  message: "按钮必须配置权限标识",
                },
              ]}
            >
              <Select
                showSearch
                allowClear
                placeholder="选择权限标识"
                filterOption={(input, option) =>
                  (option?.label ?? "").toLowerCase().includes(input.toLowerCase()) ||
                  (option?.value ?? "").toLowerCase().includes(input.toLowerCase())
                }
                options={permissionSelectOptions}
                optionRender={(option) => (
                  <div>
                    <div>
                      <Tag color="blue" className="!mr-1">
                        {option.data.module}
                      </Tag>
                      <Tag color="green" className="!mr-1">
                        {option.data.resource}
                      </Tag>
                      {option.data.label}
                    </div>
                    {option.data.description && (
                      <div className="text-xs text-gray-400 mt-0.5">{option.data.description}</div>
                    )}
                  </div>
                )}
              />
            </Form.Item>
          )}

          {(watchType === "menu" || watchType === "page") && (
            <Form.Item label="图标" name="icon">
              <Input placeholder="例如：SettingOutlined（仅页面显示图标）" />
            </Form.Item>
          )}

          <Form.Item label="排序" name="sort_order">
            <InputNumber className="!w-full" />
          </Form.Item>
          <Form.Item label="状态" name="status" rules={[{ required: true }]}>
            <Select>
              <Select.Option value="enabled">启用</Select.Option>
              <Select.Option value="disabled">禁用</Select.Option>
            </Select>
          </Form.Item>
        </Form>
      </Modal>

      {/* 新增权限弹窗 */}
      <Modal
        open={permModalVisible}
        title={`新增权限 - ${permTargetPage?.name || ""}`}
        onCancel={() => {
          setPermModalVisible(false);
          setPermTargetPage(null);
          setPermSelectedCodes([]);
        }}
        onOk={() => {
          if (permTargetPage && permSelectedCodes.length > 0) {
            batchCreatePermMutation.mutate({
              parentPage: permTargetPage,
              codes: permSelectedCodes,
            });
          }
        }}
        okButtonProps={{ disabled: permSelectedCodes.length === 0 }}
        confirmLoading={batchCreatePermMutation.isPending}
        destroyOnHidden
        width={500}
      >
        {targetPagePermissions.length === 0 ? (
          <div className="text-center text-gray-400 py-5">该页面未配置权限列表</div>
        ) : (
          <div>
            <div className="mb-3 text-gray-500">选择要添加的权限（已存在的权限无法重复添加）：</div>
            <Space direction="vertical" className="w-full">
              {targetPagePermissions.map((perm) => (
                <div
                  key={perm.code}
                  className={`flex items-center px-3 py-2 rounded-md ${perm.exists ? "bg-gray-100 cursor-not-allowed opacity-60" : "bg-gray-50 cursor-pointer"}`}
                  onClick={() => {
                    if (perm.exists) return;
                    setPermSelectedCodes((prev) =>
                      prev.includes(perm.code)
                        ? prev.filter((c) => c !== perm.code)
                        : [...prev, perm.code]
                    );
                  }}
                >
                  <input
                    type="checkbox"
                    checked={permSelectedCodes.includes(perm.code)}
                    disabled={perm.exists}
                    onChange={() => {}}
                    className="mr-3"
                  />
                  <div className="flex-1">
                    <div className="font-medium">
                      {perm.name}
                      {perm.exists && (
                        <Tag color="default" className="!ml-2">
                          已存在
                        </Tag>
                      )}
                    </div>
                    <div className="text-xs text-gray-400">{perm.code}</div>
                  </div>
                </div>
              ))}
            </Space>
          </div>
        )}
      </Modal>
    </PageContainer>
  );
};

export default SystemMenuPage;
