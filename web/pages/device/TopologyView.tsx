import { Modal } from "antd";
import { TreeChart } from "echarts/charts";
import { TooltipComponent } from "echarts/components";
import * as echarts from "echarts/core";
import { CanvasRenderer } from "echarts/renderers";
import ReactEChartsCore from "echarts-for-react/lib/core";
import { useMemo } from "react";
import { useDeviceGroupTreeWithCount, useDeviceList } from "@/services";
import type { DeviceGroup } from "@/types";
import { isOnline } from "./utils";

echarts.use([TreeChart, TooltipComponent, CanvasRenderer]);

interface TopologyViewProps {
  open: boolean;
  onClose: () => void;
}

interface TreeNode {
  name: string;
  value?: string;
  itemStyle?: { color: string; borderColor: string };
  label?: { color: string };
  children?: TreeNode[];
}

const TopologyView = ({ open, onClose }: TopologyViewProps) => {
  const { data: groupTree = [] } = useDeviceGroupTreeWithCount({ enabled: open });
  const { data: deviceData } = useDeviceList({ enabled: open, pollingInterval: false });
  const deviceList = useMemo(() => deviceData?.list || [], [deviceData?.list]);

  const chartData = useMemo(() => {
    const buildGroupNode = (group: DeviceGroup.TreeItem): TreeNode => ({
      name: group.name,
      value: "分组",
      itemStyle: { color: "#e6f4ff", borderColor: "#1677ff" },
      label: { color: "#1677ff" },
      children: [
        ...(group.children || []).map(buildGroupNode),
        ...deviceList
          .filter((d) => d.group_id === group.id)
          .map((d) => {
            const online = isOnline(d.lastHeartbeatTime, d.reportTime, d.online_timeout);
            return {
              name: d.name,
              value: online ? "在线" : "离线",
              itemStyle: {
                color: online ? "#f6ffed" : "#fff2f0",
                borderColor: online ? "#52c41a" : "#ff4d4f",
              },
              label: { color: online ? "#389e0d" : "#cf1322" },
            };
          }),
      ],
    });

    const ungroupedDevices = deviceList
      .filter((d) => !d.group_id)
      .map((d) => {
        const online = isOnline(d.lastHeartbeatTime, d.reportTime, d.online_timeout);
        return {
          name: d.name,
          value: online ? "在线" : "离线",
          itemStyle: {
            color: online ? "#f6ffed" : "#fff2f0",
            borderColor: online ? "#52c41a" : "#ff4d4f",
          },
          label: { color: online ? "#389e0d" : "#cf1322" },
        };
      });

    const rootChildren: TreeNode[] = [...groupTree.map(buildGroupNode)];
    if (ungroupedDevices.length > 0) {
      rootChildren.push({
        name: "未分组",
        value: "分组",
        itemStyle: { color: "#fff7e6", borderColor: "#d48806" },
        label: { color: "#d48806" },
        children: ungroupedDevices,
      });
    }

    return {
      name: "设备拓扑",
      itemStyle: { color: "#f0f5ff", borderColor: "#2f54eb" },
      label: { color: "#2f54eb" },
      children: rootChildren,
    };
  }, [groupTree, deviceList]);

  const option = useMemo(
    () => ({
      tooltip: {
        trigger: "item" as const,
        formatter: (params: { data: TreeNode }) => {
          const { name, value } = params.data;
          return `${name}${value ? ` - ${value}` : ""}`;
        },
      },
      series: [
        {
          type: "tree" as const,
          data: [chartData],
          top: "5%",
          left: "15%",
          bottom: "5%",
          right: "15%",
          symbolSize: 10,
          orient: "TB" as const,
          label: {
            position: "top" as const,
            verticalAlign: "middle" as const,
            fontSize: 12,
          },
          leaves: {
            label: {
              position: "bottom" as const,
              verticalAlign: "middle" as const,
            },
          },
          expandAndCollapse: true,
          initialTreeDepth: 3,
          animationDuration: 400,
          animationDurationUpdate: 400,
        },
      ],
    }),
    [chartData]
  );

  return (
    <Modal
      open={open}
      title="设备拓扑"
      onCancel={onClose}
      footer={null}
      width="90vw"
      styles={{ body: { height: "70vh" } }}
      destroyOnClose
    >
      <ReactEChartsCore
        echarts={echarts}
        option={option}
        style={{ height: "100%", width: "100%" }}
        notMerge
      />
    </Modal>
  );
};

export default TopologyView;
