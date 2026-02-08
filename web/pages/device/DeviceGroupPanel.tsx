import {
  ApartmentOutlined,
  DeleteOutlined,
  DownOutlined,
  EditOutlined,
  PlusOutlined,
} from "@ant-design/icons";
import { App, Button, Dropdown, Flex, Popover, Space, Spin, Tree } from "antd";
import type { DataNode, TreeProps } from "antd/es/tree";
import { useMemo, useState } from "react";
import { useDeviceGroupDelete, useDeviceGroupSave, useDeviceGroupTreeWithCount } from "@/services";
import type { DeviceGroup } from "@/types";
import DeviceGroupFormModal from "./DeviceGroupFormModal";

interface DeviceGroupPanelProps {
  selectedGroupId: number | null;
  onSelect: (groupId: number | null) => void;
  canManageGroup: boolean;
  ungroupedCount: number;
}

type TreeKey = string | number;

const DeviceGroupPanel = ({
  selectedGroupId,
  onSelect,
  canManageGroup,
  ungroupedCount,
}: DeviceGroupPanelProps) => {
  const { modal } = App.useApp();
  const [popoverOpen, setPopoverOpen] = useState(false);
  const [formModalVisible, setFormModalVisible] = useState(false);
  const [editingGroup, setEditingGroup] = useState<DeviceGroup.TreeItem | null>(null);
  const [parentIdForCreate, setParentIdForCreate] = useState<number | null>(null);

  const { data: treeData = [], isLoading } = useDeviceGroupTreeWithCount();
  const saveMutation = useDeviceGroupSave();
  const deleteMutation = useDeviceGroupDelete();

  const antTreeData = useMemo(() => {
    const convert = (nodes: DeviceGroup.TreeItem[]): DataNode[] =>
      nodes.map((node) => ({
        key: node.id,
        title: `${node.name} (${node.deviceCount ?? 0})`,
        children: node.children?.length ? convert(node.children) : undefined,
      }));

    return [
      { key: "all", title: "全部设备", isLeaf: true } as DataNode,
      ...(ungroupedCount > 0
        ? [{ key: "ungrouped", title: `未分组 (${ungroupedCount})`, isLeaf: true } as DataNode]
        : []),
      ...convert(treeData),
    ];
  }, [treeData, ungroupedCount]);

  const selectedKeys = useMemo<TreeKey[]>(() => {
    if (selectedGroupId === null) return ["all"];
    if (selectedGroupId === 0) return ["ungrouped"];
    return [selectedGroupId];
  }, [selectedGroupId]);

  // 当前选中的分组名称（用于按钮显示）
  const selectedLabel = useMemo(() => {
    if (selectedGroupId === null) return "全部设备";
    if (selectedGroupId === 0) return "未分组";
    const find = (nodes: DeviceGroup.TreeItem[]): string | null => {
      for (const n of nodes) {
        if (n.id === selectedGroupId) return n.name;
        if (n.children) {
          const found = find(n.children);
          if (found) return found;
        }
      }
      return null;
    };
    return find(treeData) ?? "全部设备";
  }, [selectedGroupId, treeData]);

  const handleSelect: TreeProps["onSelect"] = (keys) => {
    if (!keys.length) return;
    const key = keys[0];
    if (key === "all") onSelect(null);
    else if (key === "ungrouped") onSelect(0);
    else onSelect(Number(key));
    setPopoverOpen(false);
  };

  const findGroup = (id: number, nodes: DeviceGroup.TreeItem[]): DeviceGroup.TreeItem | null => {
    for (const n of nodes) {
      if (n.id === id) return n;
      if (n.children) {
        const found = findGroup(id, n.children);
        if (found) return found;
      }
    }
    return null;
  };

  const handleAddChild = (parentId: number) => {
    setEditingGroup(null);
    setParentIdForCreate(parentId);
    setFormModalVisible(true);
  };

  const handleEdit = (id: number) => {
    const group = findGroup(id, treeData);
    if (group) {
      setEditingGroup(group);
      setParentIdForCreate(null);
      setFormModalVisible(true);
    }
  };

  const handleDelete = (id: number) => {
    const group = findGroup(id, treeData);
    if (!group) return;
    modal.confirm({
      title: `确认删除分组「${group.name}」？`,
      content: "删除后该分组下的子分组和设备不会被删除，但需要先移除或转移。",
      okText: "确定删除",
      okButtonProps: { danger: true },
      onOk: () => deleteMutation.mutate(id),
    });
  };

  const contextMenuItems = (nodeKey: TreeKey) => {
    if (nodeKey === "all" || nodeKey === "ungrouped") return [];
    const items = [];
    if (canManageGroup) {
      items.push(
        { key: "addChild", label: "新增子分组", icon: <PlusOutlined /> },
        { key: "edit", label: "编辑", icon: <EditOutlined /> },
        { key: "delete", label: "删除", icon: <DeleteOutlined />, danger: true },
      );
    }
    return items;
  };

  const treeContent = (
    <div className="w-56">
      {canManageGroup && (
        <Flex justify="flex-end" className="mb-1.5">
          <Button
            type="text"
            size="small"
            icon={<PlusOutlined />}
            onClick={() => {
              setEditingGroup(null);
              setParentIdForCreate(null);
              setFormModalVisible(true);
            }}
          >
            新建分组
          </Button>
        </Flex>
      )}
      {isLoading ? (
        <div className="py-6 text-center">
          <Spin size="small" />
        </div>
      ) : (
        <Tree
          treeData={antTreeData}
          selectedKeys={selectedKeys}
          onSelect={handleSelect}
          defaultExpandAll
          blockNode
          titleRender={(node) => {
            const items = contextMenuItems(node.key as TreeKey);
            if (!items.length) return <span>{node.title as string}</span>;
            return (
              <Dropdown
                menu={{
                  items,
                  onClick: ({ key }) => {
                    const id = Number(node.key);
                    if (key === "addChild") handleAddChild(id);
                    else if (key === "edit") handleEdit(id);
                    else if (key === "delete") handleDelete(id);
                  },
                }}
                trigger={["contextMenu"]}
              >
                <span className="block">{node.title as string}</span>
              </Dropdown>
            );
          }}
        />
      )}
    </div>
  );

  return (
    <>
      <Popover
        content={treeContent}
        trigger="click"
        open={popoverOpen}
        onOpenChange={setPopoverOpen}
        placement="bottomLeft"
      >
        <Button icon={<ApartmentOutlined />}>
          <Space size={4}>
            {selectedLabel}
            <DownOutlined className="!text-[10px] text-gray-400" />
          </Space>
        </Button>
      </Popover>

      <DeviceGroupFormModal
        open={formModalVisible}
        editing={editingGroup}
        parentId={parentIdForCreate}
        treeData={treeData}
        loading={saveMutation.isPending}
        onCancel={() => {
          setFormModalVisible(false);
          setEditingGroup(null);
        }}
        onFinish={(values) =>
          saveMutation.mutate(values, {
            onSuccess: () => {
              setFormModalVisible(false);
              setEditingGroup(null);
            },
          })
        }
      />
    </>
  );
};

export default DeviceGroupPanel;
