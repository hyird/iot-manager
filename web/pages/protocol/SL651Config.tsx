/**
 * SL651 协议配置页面
 * 参考原项目 iot-frontend/src/views/SL651/TypeConfig
 * 布局：左侧设备类型列表 + 右侧功能码/要素配置
 */

import { DownloadOutlined, UploadOutlined } from "@ant-design/icons";
import {
  Button,
  Card,
  Empty,
  Flex,
  Popconfirm,
  Result,
  Skeleton,
  Space,
  Table,
  Tag,
  Tooltip,
  Tree,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useMemo, useRef, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission, useProtocolImportExport } from "@/hooks";
import { useProtocolConfigDelete, useProtocolConfigList, useProtocolConfigSave } from "@/services";
import type { SL651 } from "@/types";
import {
  DeviceTypeModal,
  type DeviceTypeModalRef,
  DictConfigModal,
  type DictConfigModalRef,
  ElementModal,
  type ElementModalRef,
  FuncModal,
  type FuncModalRef,
  PresetValueModal,
  type PresetValueModalRef,
  ResponseElementsModal,
  type ResponseElementsModalRef,
} from "./sl651";

/** 带要素的功能码 */
interface FuncWithElements extends SL651.Func {
  elements: SL651.Element[];
  responseElements?: SL651.Element[];
}

const SL651ConfigPage = () => {
  // 权限检查
  const canQuery = usePermission("iot:protocol:query");
  const canAdd = usePermission("iot:protocol:add");
  const canEdit = usePermission("iot:protocol:edit");
  const canDelete = usePermission("iot:protocol:delete");
  const canImport = usePermission("iot:protocol:import");
  const canExport = usePermission("iot:protocol:export");

  // 设备类型列表查询
  const {
    data: configPage,
    isLoading: loadingTypes,
    refetch: refetchTypes,
  } = useProtocolConfigList({ protocol: "SL651" }, { enabled: canQuery });

  // 保存和删除 mutations
  const saveMutation = useProtocolConfigSave();
  const deleteMutation = useProtocolConfigDelete();

  // 导入导出
  const { exportConfigs, triggerImport, importing } = useProtocolImportExport("SL651");

  // 当前选中的设备类型 ID（用户手动选择）
  const [selectedTypeId, setSelectedTypeId] = useState<number>();

  // Modal refs
  const deviceTypeModalRef = useRef<DeviceTypeModalRef>(null);
  const funcModalRef = useRef<FuncModalRef>(null);
  const elementModalRef = useRef<ElementModalRef>(null);
  const presetValueModalRef = useRef<PresetValueModalRef>(null);
  const dictConfigModalRef = useRef<DictConfigModalRef>(null);
  const responseElementsModalRef = useRef<ResponseElementsModalRef>(null);

  // 设备类型列表（使用 useMemo 保持引用稳定）
  const types = useMemo(() => configPage?.list || [], [configPage?.list]);

  // 计算当前激活的类型 ID：优先用户选择，否则默认第一个
  const activeTypeId = useMemo(() => {
    if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
      return selectedTypeId;
    }
    return types.length > 0 ? types[0].id : undefined;
  }, [selectedTypeId, types]);

  // 功能码列表（派生状态，根据 activeTypeId 计算）
  const funcs = useMemo<FuncWithElements[]>(() => {
    if (!activeTypeId) return [];
    const type = types.find((t) => t.id === activeTypeId);
    if (!type) return [];
    const config = type.config as SL651.Config;
    return config?.funcs || [];
  }, [activeTypeId, types]);

  // 加载状态（与数据加载状态同步）
  const loadingFuncs = loadingTypes;

  // ========== 设备类型操作 ==========

  const handleDeleteDeviceType = async () => {
    if (!activeTypeId) return;
    // 删除前记录下一个可选中的类型
    const idx = types.findIndex((t) => t.id === activeTypeId);
    const nextType = types[idx + 1] ?? types[idx - 1];
    await deleteMutation.mutateAsync(activeTypeId);
    setSelectedTypeId(nextType?.id);
  };

  // ========== 功能码操作 ==========

  const handleDeleteFunc = async (funcId: string) => {
    if (!activeTypeId) return;
    const type = types.find((t) => t.id === activeTypeId);
    if (!type) return;

    const config = type.config as SL651.Config;
    const newConfig: SL651.Config = {
      funcs: config.funcs.filter((f) => f.id !== funcId),
    };

    await saveMutation.mutateAsync({
      id: activeTypeId,
      protocol: "SL651",
      config: newConfig,
    });
  };

  // ========== 要素操作 ==========

  const handleDeleteElement = async (funcId: string, eleId: string) => {
    if (!activeTypeId) return;
    const type = types.find((t) => t.id === activeTypeId);
    if (!type) return;

    const config = type.config as SL651.Config;
    const newConfig: SL651.Config = {
      funcs: config.funcs.map((f) =>
        f.id === funcId ? { ...f, elements: f.elements.filter((e) => e.id !== eleId) } : f
      ),
    };

    await saveMutation.mutateAsync({
      id: activeTypeId,
      protocol: "SL651",
      config: newConfig,
    });
  };

  // ========== 功能码表格列 ==========

  const funcColumns: ColumnsType<FuncWithElements> = [
    { title: "功能码", dataIndex: "funcCode", ellipsis: true },
    { title: "名称", dataIndex: "name", ellipsis: true },
    {
      title: "方向",
      render: (_, r) => (
        <Tag color={r.dir === "UP" ? "green" : "orange"}>{r.dir === "UP" ? "上行" : "下行"}</Tag>
      ),
    },
    {
      title: "要素数",
      render: (_, r) => r.elements?.length ?? 0,
    },
    {
      title: "应答要素",
      render: (_, r) =>
        r.dir === "DOWN" ? (
          <Tooltip title="下行指令应答报文的解析要素">
            <Tag color="cyan">{r.responseElements?.length ?? 0}个</Tag>
          </Tooltip>
        ) : (
          "-"
        ),
    },
    {
      title: "备注",
      dataIndex: "remark",
      ellipsis: true,
    },
    {
      title: "操作",
      width: 320,
      fixed: "right" as const,
      render: (_, r) => (
        <Space>
          {canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => funcModalRef.current?.open("edit", activeTypeId!, r)}
            >
              编辑
            </Button>
          )}
          {canDelete && (
            <Popconfirm title="确认删除？" onConfirm={() => handleDeleteFunc(r.id)}>
              <Button size="small" danger type="link">
                删除
              </Button>
            </Popconfirm>
          )}
          {canAdd && (
            <Button
              size="small"
              type="link"
              onClick={() => elementModalRef.current?.open("create", activeTypeId!, r.id)}
            >
              新增要素
            </Button>
          )}
          {r.dir === "DOWN" && canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => responseElementsModalRef.current?.open(activeTypeId!, r)}
            >
              应答要素
            </Button>
          )}
        </Space>
      ),
    },
  ];

  // ========== 要素表格列 ==========

  const elementColumns = (funcId: string, funcDir: SL651.Direction): ColumnsType<SL651.Element> => [
    { title: "要素名称", dataIndex: "name", ellipsis: true },
    { title: "引导符", dataIndex: "guideHex" },
    { title: "编码", dataIndex: "encode" },
    { title: "长度", dataIndex: "length" },
    { title: "小数位", dataIndex: "digits" },
    { title: "单位", dataIndex: "unit" },
    ...(funcDir === "DOWN"
      ? [
          {
            title: "预设值",
            dataIndex: "options",
            render: (options: SL651.Element["options"]) =>
              options?.length ? `${options.length}个` : "-",
          },
        ]
      : []),
    {
      title: "字典配置",
      dataIndex: "dictConfig",
      render: (dictConfig: SL651.Element["dictConfig"]) => {
        if (!dictConfig?.items?.length) return "-";
        const typeLabel = dictConfig.mapType === "VALUE" ? "值" : "位";
        return `${typeLabel}映射(${dictConfig.items.length}个)`;
      },
    },
    { title: "备注", dataIndex: "remark", ellipsis: true },
    {
      title: "操作",
      width: funcDir === "DOWN" ? 300 : 240,
      fixed: "right" as const,
      render: (_, r) => (
        <Space>
          {canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => elementModalRef.current?.open("edit", activeTypeId!, funcId, r)}
            >
              编辑
            </Button>
          )}
          {funcDir === "DOWN" && canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => presetValueModalRef.current?.open(activeTypeId!, funcId, r)}
            >
              预设值
            </Button>
          )}
          {r.encode === "DICT" && canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => dictConfigModalRef.current?.open(activeTypeId!, funcId, r)}
            >
              字典
            </Button>
          )}
          {canDelete && (
            <Popconfirm title="确认删除？" onConfirm={() => handleDeleteElement(funcId, r.id)}>
              <Button size="small" danger type="link">
                删除
              </Button>
            </Popconfirm>
          )}
        </Space>
      ),
    },
  ];

  // 权限检查
  if (!canQuery) {
    return (
      <PageContainer title="SL651配置">
        <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
      </PageContainer>
    );
  }

  return (
    <PageContainer title="SL651配置">
      <div className="h-full flex">
        {/* 左侧：设备类型列表 */}
        <div className="w-[360px] shrink-0 pr-3 h-full">
          <Card
            title="设备类型"
            className="h-full flex flex-col"
            styles={{ body: { flex: 1, overflow: "auto", padding: 16 } }}
            extra={
              <Space size={4}>
                {canAdd && (
                  <Button
                    size="small"
                    type="primary"
                    onClick={() => deviceTypeModalRef.current?.open("create")}
                  >
                    新增
                  </Button>
                )}
                {canEdit && (
                  <Button
                    size="small"
                    disabled={!activeTypeId}
                    onClick={() =>
                      deviceTypeModalRef.current?.open(
                        "edit",
                        types.find((t) => t.id === activeTypeId)
                      )
                    }
                  >
                    编辑
                  </Button>
                )}
                {canDelete && (
                  <Popconfirm
                    title="确认删除该设备类型？"
                    onConfirm={handleDeleteDeviceType}
                    disabled={!activeTypeId}
                  >
                    <Button size="small" danger disabled={!activeTypeId}>
                      删除
                    </Button>
                  </Popconfirm>
                )}
                {canExport && (
                  <Tooltip title="导出">
                    <Button
                      size="small"
                      icon={<DownloadOutlined />}
                      disabled={!types.length}
                      onClick={() => exportConfigs(types)}
                    />
                  </Tooltip>
                )}
                {canImport && (
                  <Tooltip title="导入">
                    <Button
                      size="small"
                      icon={<UploadOutlined />}
                      loading={importing}
                      onClick={triggerImport}
                    />
                  </Tooltip>
                )}
              </Space>
            }
          >
            {loadingTypes ? (
              <Skeleton active paragraph={{ rows: 6 }} />
            ) : types.length === 0 ? (
              <Empty description="暂无设备类型" />
            ) : (
              <Tree
                blockNode
                className="[&_.ant-tree-switcher]:hidden"
                selectedKeys={activeTypeId ? [String(activeTypeId)] : []}
                onSelect={(keys) => {
                  if (keys.length > 0) {
                    setSelectedTypeId(Number(keys[0]));
                  }
                }}
                treeData={types.map((t) => {
                  const config = t.config as SL651.Config;
                  const mode = config?.responseMode || "M1";
                  return {
                    key: String(t.id),
                    title: (
                      <Tooltip title={t.remark || "暂无备注"} placement="right">
                        <Flex justify="space-between" align="center" className="h-8 p-1">
                          <Space size={4}>
                            <span>{t.name}</span>
                            <Tag color="blue">{mode}</Tag>
                          </Space>
                          {t.enabled ? <Tag color="green">启用</Tag> : <Tag color="red">禁用</Tag>}
                        </Flex>
                      </Tooltip>
                    ),
                  };
                })}
              />
            )}
          </Card>
        </div>

        {/* 右侧：功能码配置 */}
        <div className="flex-1 min-w-0 h-full">
          <Card
            title={activeTypeId ? "功能码配置" : "请选择设备类型"}
            className="h-full flex flex-col"
            styles={{ body: { flex: 1, overflow: "auto", padding: 0 } }}
            extra={
              activeTypeId &&
              canAdd && (
                <Button
                  type="primary"
                  onClick={() => funcModalRef.current?.open("create", activeTypeId)}
                >
                  新增功能码
                </Button>
              )
            }
          >
            {!activeTypeId ? (
              <Empty description="未选择设备类型" />
            ) : (
              <div style={{ "--ant-table-header-border-radius": 0 } as React.CSSProperties}>
                <Table
                  dataSource={funcs}
                  rowKey="id"
                  pagination={false}
                  loading={loadingFuncs}
                  sticky
                  scroll={{ x: "max-content" }}
                  expandable={{
                    expandedRowRender: (record) => (
                      <Table
                        dataSource={record.elements}
                        columns={elementColumns(record.id, record.dir)}
                        rowKey="id"
                        pagination={false}
                        size="small"
                        scroll={{ x: "max-content" }}
                      />
                    ),
                  }}
                  columns={funcColumns}
                />
              </div>
            )}
          </Card>
        </div>
      </div>

      {/* 设备类型 Modal */}
      <DeviceTypeModal
        ref={deviceTypeModalRef}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 功能码 Modal */}
      <FuncModal
        ref={funcModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 要素 Modal */}
      <ElementModal
        ref={elementModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 预设值 Modal */}
      <PresetValueModal
        ref={presetValueModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 字典配置 Modal */}
      <DictConfigModal
        ref={dictConfigModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 应答要素 Modal */}
      <ResponseElementsModal
        ref={responseElementsModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />
    </PageContainer>
  );
};

export default SL651ConfigPage;
