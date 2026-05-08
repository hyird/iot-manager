import {
  ArrowDownOutlined,
  ArrowLeftOutlined,
  ArrowRightOutlined,
  ArrowUpOutlined,
  MinusOutlined,
  PauseCircleOutlined,
  PlayCircleOutlined,
  PlusOutlined,
  ReloadOutlined,
  SendOutlined,
  StopOutlined,
  VideoCameraOutlined,
} from "@ant-design/icons";
import {
  App,
  Badge,
  Button,
  Card,
  Descriptions,
  Input,
  Result,
  Select,
  Slider,
  Space,
  Statistic,
  Table,
  Tag,
  Tooltip,
  Typography,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import Hls from "hls.js";
import mpegts from "mpegts.js";
import { type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { Endpoint, Events } from "zlmrtc-client";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import {
  createJessibucaFlvPlayer,
  type JessibucaFlvPlayer,
} from "@/lib/gb28181/jessibucaFlvPlayer";
import {
  useGb28181CatalogQuery,
  useGb28181Devices,
  useGb28181Health,
  useGb28181PreviewStart,
  useGb28181PreviewStop,
  useGb28181Ptz,
} from "@/services";
import type { GB28181 } from "@/types";

const { Text, Title } = Typography;

const WEBRTC_FALLBACK_DELAY_MS = 8000;
const WEBRTC_CODEC_RETRY_MS = 400;
const WEBRTC_CODEC_RETRY_COUNT = 10;

const PTZ_ACTIONS: Array<{
  action: GB28181.PtzAction;
  title: string;
  icon: ReactNode;
  className?: string;
}> = [
  { action: "up", title: "上", icon: <ArrowUpOutlined />, className: "col-start-2" },
  { action: "left", title: "左", icon: <ArrowLeftOutlined />, className: "col-start-1" },
  { action: "stop", title: "停止", icon: <StopOutlined />, className: "col-start-2" },
  { action: "right", title: "右", icon: <ArrowRightOutlined />, className: "col-start-3" },
  { action: "down", title: "下", icon: <ArrowDownOutlined />, className: "col-start-2" },
];

const onlineTag = (online: boolean) => (
  <Badge status={online ? "processing" : "default"} text={online ? "在线" : "离线"} />
);

const displayText = (value?: string | number | null) => {
  if (value === undefined || value === null || value === "") return "--";
  return value;
};

const remoteEndpoint = (device?: GB28181.Device) => {
  if (!device) return "--";
  const ip = device.remote_ip || device.remote_address;
  const port = device.remote_port;
  if (!ip) return "--";
  return port ? `${ip}:${port}` : ip;
};

const registrationSourceTag = (source?: string) => {
  if (source === "mock") {
    return <Tag color="orange">模拟</Tag>;
  }
  return null;
};

const ptzCapabilityTag = (channel?: GB28181.Channel) => {
  if (!channel || channel.ptz_type === undefined || channel.ptz_type < 0) {
    return <Tag>云台未知</Tag>;
  }
  return channel.ptz_capable ? <Tag color="green">支持云台</Tag> : <Tag color="red">无云台</Tag>;
};

type PlaybackCandidate = {
  label: string;
  engine: "jessibuca" | "hls" | "mpegts";
  mediaType?: "flv" | "mpegts";
  url: string;
};

type PlayerResources = {
  hls?: Hls;
  mpegts?: ReturnType<typeof mpegts.createPlayer>;
  rtc?: Endpoint;
  jessibuca?: JessibucaFlvPlayer;
  fallbackTimer?: number;
};

const resetVideoElement = (video: HTMLVideoElement) => {
  video.pause();
  video.removeAttribute("src");
  video.srcObject = null;
  video.load();
};

const closePlayerResources = (resources: PlayerResources, video?: HTMLVideoElement | null) => {
  if (resources.fallbackTimer) {
    window.clearTimeout(resources.fallbackTimer);
    resources.fallbackTimer = undefined;
  }
  resources.hls?.destroy();
  resources.hls = undefined;
  resources.mpegts?.destroy();
  resources.mpegts = undefined;
  resources.rtc?.close();
  resources.rtc = undefined;
  resources.jessibuca?.close();
  resources.jessibuca = undefined;
  if (video) resetVideoElement(video);
};

const buildPlaybackCandidates = (urls: GB28181.PlayUrls): PlaybackCandidate[] => {
  const candidates: Array<PlaybackCandidate | null> = [
    urls.http_flv
      ? { label: "Jessibuca HTTP-FLV", engine: "jessibuca" as const, url: urls.http_flv }
      : null,
    urls.ws_flv
      ? { label: "Jessibuca WS-FLV", engine: "jessibuca" as const, url: urls.ws_flv }
      : null,
    urls.hls ? { label: "HLS", engine: "hls" as const, url: urls.hls } : null,
    urls.http_flv
      ? {
          label: "HTTP-FLV",
          engine: "mpegts" as const,
          mediaType: "flv" as const,
          url: urls.http_flv,
        }
      : null,
    urls.ws_flv
      ? { label: "WS-FLV", engine: "mpegts" as const, mediaType: "flv" as const, url: urls.ws_flv }
      : null,
    urls.http_ts
      ? {
          label: "HTTP-TS",
          engine: "mpegts" as const,
          mediaType: "mpegts" as const,
          url: urls.http_ts,
        }
      : null,
  ];
  return candidates.filter((candidate): candidate is PlaybackCandidate => Boolean(candidate?.url));
};

const AUXILIARY_VIDEO_CODECS = new Set(["RTX", "RED", "ULPFEC", "FLEXFEC-03"]);

type RtcEndpointWithPeerConnection = Endpoint & {
  pc?: RTCPeerConnection | null;
};

type RtcStatsRecord = {
  id?: string;
  type?: string;
  kind?: string;
  mediaType?: string;
  codecId?: string;
  mimeType?: string;
  payloadType?: number;
  packetsReceived?: number;
  framesDecoded?: number;
};

const normalizeVideoCodecName = (value?: string) => {
  if (!value) return undefined;
  let codec = value.trim();
  const mimeMatch = /^video\/([^;\s]+)/i.exec(codec);
  if (mimeMatch?.[1]) {
    codec = mimeMatch[1];
  }
  codec = codec.split(";")[0].split("/")[0].trim().toUpperCase();
  if (!codec || AUXILIARY_VIDEO_CODECS.has(codec)) return undefined;
  if (codec === "HEVC" || codec === "HVC1") return "H265";
  if (codec === "AVC" || codec === "AVC1") return "H264";
  return codec;
};

const parseVideoCodecFromSdp = (sdp?: string) => {
  if (!sdp) return undefined;
  const lines = sdp.split(/\r?\n/).map((line) => line.trim());
  const videoLineIndex = lines.findIndex((line) => line.startsWith("m=video "));
  if (videoLineIndex < 0) return undefined;

  const payloadTypes = lines[videoLineIndex].split(/\s+/).slice(3);
  const payloadCodecs = new Map<string, string>();
  for (let index = videoLineIndex + 1; index < lines.length; index += 1) {
    const line = lines[index];
    if (line.startsWith("m=")) break;
    const match = /^a=rtpmap:(\d+)\s+([^\s]+)/i.exec(line);
    const codec = normalizeVideoCodecName(match?.[2]);
    if (match?.[1] && codec) payloadCodecs.set(match[1], codec);
  }

  for (const payloadType of payloadTypes) {
    const codec = payloadCodecs.get(payloadType);
    if (codec) return codec;
  }
  return undefined;
};

const getEndpointPeerConnection = (endpoint: Endpoint) =>
  (endpoint as RtcEndpointWithPeerConnection).pc ?? null;

const findWebRtcVideoCodecFromStats = async (pc: RTCPeerConnection) => {
  try {
    const report = await pc.getStats();
    const codecsById = new Map<string, string>();
    const codecsByPayloadType = new Map<number, string>();

    report.forEach((value: RtcStatsRecord) => {
      if (value.type !== "codec") return;
      const codec = normalizeVideoCodecName(value.mimeType);
      if (!codec) return;
      if (value.id) codecsById.set(value.id, codec);
      if (typeof value.payloadType === "number") codecsByPayloadType.set(value.payloadType, codec);
    });

    let selectedCodecId: string | undefined;
    let selectedPayloadType: number | undefined;
    let selectedScore = -1;
    report.forEach((value: RtcStatsRecord) => {
      if (value.type !== "inbound-rtp") return;
      if (value.kind !== "video" && value.mediaType !== "video") return;
      const score = (value.framesDecoded ?? 0) + (value.packetsReceived ?? 0);
      if (score < selectedScore) return;
      selectedScore = score;
      selectedCodecId = value.codecId;
      selectedPayloadType = value.payloadType;
    });

    if (selectedCodecId) {
      const codec = codecsById.get(selectedCodecId);
      if (codec) return codec;
    }
    if (typeof selectedPayloadType === "number") {
      return codecsByPayloadType.get(selectedPayloadType);
    }
  } catch {
    return undefined;
  }

  return undefined;
};

const findWebRtcVideoCodec = async (endpoint: Endpoint) => {
  const pc = getEndpointPeerConnection(endpoint);
  if (!pc) return undefined;
  return (
    (await findWebRtcVideoCodecFromStats(pc)) ?? parseVideoCodecFromSdp(pc.remoteDescription?.sdp)
  );
};

const wait = (duration: number) =>
  new Promise<void>((resolve) => {
    window.setTimeout(resolve, duration);
  });

function Gb28181LivePlayer({ session }: { session: GB28181.PreviewStartResult }) {
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const resourcesRef = useRef<PlayerResources>({});
  const [status, setStatus] = useState("准备播放");
  const [activeProtocol, setActiveProtocol] = useState<string>();
  const [surface, setSurface] = useState<"video" | "canvas">("video");

  useEffect(() => {
    let disposed = false;
    let fallbackStarted = false;
    const resources = resourcesRef.current;
    const playUrls = session.play_urls;

    const playElement = async (label: string, playingStatus = "播放中") => {
      const video = videoRef.current;
      if (!video || disposed) return;
      setActiveProtocol(label);
      try {
        await video.play();
        if (!disposed) setStatus(playingStatus);
      } catch {
        if (!disposed) setStatus("已就绪");
      }
    };

    const updateWebRtcVideoCodec = async (endpoint: Endpoint) => {
      for (let attempt = 0; attempt < WEBRTC_CODEC_RETRY_COUNT; attempt += 1) {
        if (disposed || resources.rtc !== endpoint) return;
        const codec = await findWebRtcVideoCodec(endpoint);
        if (disposed || resources.rtc !== endpoint) return;
        if (codec) {
          setStatus(codec);
          return;
        }
        if (attempt < WEBRTC_CODEC_RETRY_COUNT - 1) {
          await wait(WEBRTC_CODEC_RETRY_MS);
        }
      }
      if (!disposed && resources.rtc === endpoint) {
        setStatus("未知");
      }
    };

    const playFallbackCandidate = (candidates: PlaybackCandidate[], index: number) => {
      const video = videoRef.current;
      if (!video || disposed) return;

      closePlayerResources(resources, video);
      const candidate = candidates[index];
      if (!candidate) {
        setActiveProtocol(undefined);
        setStatus("没有可用播放地址");
        return;
      }

      const playNext = () => {
        if (!disposed) playFallbackCandidate(candidates, index + 1);
      };

      if (candidate.engine === "jessibuca") {
        const canvas = canvasRef.current;
        if (!canvas || typeof VideoFrame === "undefined") {
          playNext();
          return;
        }

        setSurface("canvas");
        setActiveProtocol(candidate.label);
        setStatus("连接中");

        let firstFrameRendered = false;
        let player: JessibucaFlvPlayer | undefined;
        let closed = false;
        const firstFrameTimer = window.setTimeout(() => {
          if (!firstFrameRendered) playNext();
        }, WEBRTC_FALLBACK_DELAY_MS);

        resources.jessibuca = {
          close: () => {
            closed = true;
            window.clearTimeout(firstFrameTimer);
            player?.close();
            canvas.getContext("2d")?.clearRect(0, 0, canvas.width, canvas.height);
          },
        };

        void createJessibucaFlvPlayer({
          url: candidate.url,
          canvas,
          onFirstFrame: () => {
            if (disposed) return;
            firstFrameRendered = true;
            window.clearTimeout(firstFrameTimer);
            setStatus("播放中");
          },
          onError: () => {
            playNext();
          },
        })
          .then((runtime) => {
            player = runtime;
            if (closed || disposed) runtime.close();
          })
          .catch(() => {
            playNext();
          });
        return;
      }

      setSurface("video");
      if (candidate.engine === "hls") {
        setActiveProtocol(candidate.label);
        setStatus("连接中");
        if (Hls.isSupported()) {
          const hls = new Hls({
            backBufferLength: 30,
            lowLatencyMode: true,
          });
          resources.hls = hls;
          hls.attachMedia(video);
          hls.on(Hls.Events.MEDIA_ATTACHED, () => {
            hls.loadSource(candidate.url);
          });
          hls.on(Hls.Events.MANIFEST_PARSED, () => {
            void playElement(candidate.label);
          });
          hls.on(Hls.Events.ERROR, (_event, data) => {
            if (data.fatal) playNext();
          });
          return;
        }

        if (video.canPlayType("application/vnd.apple.mpegurl")) {
          video.src = candidate.url;
          video.addEventListener("loadedmetadata", () => void playElement(candidate.label), {
            once: true,
          });
          video.addEventListener("error", playNext, { once: true });
          return;
        }

        playNext();
        return;
      }

      if (mpegts.isSupported()) {
        setActiveProtocol(candidate.label);
        setStatus("连接中");
        const player = mpegts.createPlayer(
          {
            type: candidate.mediaType ?? "flv",
            isLive: true,
            url: candidate.url,
          },
          {
            enableStashBuffer: false,
            isLive: true,
            liveBufferLatencyChasing: true,
            liveBufferLatencyMaxLatency: 1.5,
            liveBufferLatencyMinRemain: 0.2,
          }
        );
        resources.mpegts = player;
        player.on(mpegts.Events.ERROR, playNext);
        player.attachMediaElement(video);
        player.load();
        void playElement(candidate.label);
        return;
      }

      playNext();
    };

    const startFallbackPlayback = () => {
      if (disposed || fallbackStarted) return;
      fallbackStarted = true;
      playFallbackCandidate(buildPlaybackCandidates(playUrls), 0);
    };

    const startWebRtcPlayback = () => {
      const video = videoRef.current;
      if (!video || !playUrls.webrtc || typeof RTCPeerConnection === "undefined") {
        return false;
      }

      closePlayerResources(resources, video);
      setActiveProtocol("WebRTC");
      setSurface("video");
      setStatus("连接中");

      const endpoint = new Endpoint({
        element: video,
        debug: false,
        zlmsdpUrl: playUrls.webrtc,
        simulcast: false,
        useCamera: false,
        audioEnable: true,
        videoEnable: true,
        recvOnly: true,
        resolution: { w: 0, h: 0 },
        usedatachannel: false,
      });
      resources.rtc = endpoint;

      endpoint.on(Events.WEBRTC_ON_REMOTE_STREAMS, () => {
        if (resources.fallbackTimer) {
          window.clearTimeout(resources.fallbackTimer);
          resources.fallbackTimer = undefined;
        }
        void playElement("WebRTC", "获取中").then(() => updateWebRtcVideoCodec(endpoint));
      });
      endpoint.on(Events.WEBRTC_NOT_SUPPORT, startFallbackPlayback);
      endpoint.on(Events.WEBRTC_ICE_CANDIDATE_ERROR, startFallbackPlayback);
      endpoint.on(Events.WEBRTC_OFFER_ANSWER_EXCHANGE_FAILED, startFallbackPlayback);
      endpoint.on(Events.WEBRTC_ON_CONNECTION_STATE_CHANGE, (state) => {
        if (["closed", "disconnected", "failed"].includes(String(state))) {
          startFallbackPlayback();
        }
      });

      resources.fallbackTimer = window.setTimeout(() => {
        if (video.readyState < video.HAVE_CURRENT_DATA) {
          startFallbackPlayback();
        }
      }, WEBRTC_FALLBACK_DELAY_MS);
      return true;
    };

    setStatus("准备播放");
    if (!startWebRtcPlayback()) {
      startFallbackPlayback();
    }

    return () => {
      disposed = true;
      closePlayerResources(resources, videoRef.current);
    };
  }, [session.play_urls]);

  return (
    <>
      <video
        ref={videoRef}
        controls
        autoPlay
        muted
        playsInline
        className={`h-full w-full bg-black ${surface === "video" ? "block" : "hidden"}`}
      />
      <canvas
        ref={canvasRef}
        className={`h-full w-full bg-black ${surface === "canvas" ? "block" : "hidden"}`}
      />
      <div className="absolute left-3 top-3 flex items-center gap-2 rounded bg-black/60 px-2 py-1 text-xs text-white">
        {activeProtocol && <span>{activeProtocol}</span>}
        <span>{status}</span>
      </div>
    </>
  );
}

export default function Gb28181Page() {
  const { message } = App.useApp();
  const canQuery = usePermission("iot:gb28181:query");
  const canControl = usePermission("iot:gb28181:control");

  const [keyword, setKeyword] = useState("");
  const [selectedDeviceId, setSelectedDeviceId] = useState<string>();
  const [selectedChannelId, setSelectedChannelId] = useState<string>();
  const [activeSession, setActiveSession] = useState<GB28181.PreviewStartResult | null>(null);
  const [ptzSpeed, setPtzSpeed] = useState(80);

  const healthQuery = useGb28181Health({ enabled: canQuery });
  const devicesQuery = useGb28181Devices({ enabled: canQuery });

  const catalogMutation = useGb28181CatalogQuery();
  const previewStartMutation = useGb28181PreviewStart();
  const previewStopMutation = useGb28181PreviewStop();
  const ptzMutation = useGb28181Ptz();

  const devices = devicesQuery.data?.items ?? [];

  const filteredDevices = useMemo(() => {
    const text = keyword.trim().toLowerCase();
    if (!text) return devices;
    return devices.filter((device) =>
      [
        device.id,
        device.name,
        device.manufacturer,
        device.remote_address,
        device.remote_ip,
        device.remote_port,
      ]
        .filter(Boolean)
        .some((item) => item.toLowerCase().includes(text))
    );
  }, [devices, keyword]);

  const selectedDevice = useMemo(
    () => devices.find((device) => device.id === selectedDeviceId) ?? devices[0],
    [devices, selectedDeviceId]
  );

  const channels = useMemo<GB28181.Channel[]>(() => {
    if (!selectedDevice) return [];
    if (selectedDevice.channels.length > 0) return selectedDevice.channels;
    return [
      {
        id: selectedDevice.id,
        name: selectedDevice.name || selectedDevice.id,
        manufacturer: selectedDevice.manufacturer,
        online: selectedDevice.online,
        ptz_type: -1,
        ptz_capable: false,
      },
    ];
  }, [selectedDevice]);

  const selectedChannel = useMemo(
    () => channels.find((channel) => channel.id === selectedChannelId) ?? channels[0],
    [channels, selectedChannelId]
  );

  const selectedChannelSupportsPtz =
    selectedChannel?.ptz_type === undefined ||
    selectedChannel.ptz_type < 0 ||
    selectedChannel.ptz_capable;
  const ptzDisabled =
    !canControl || !selectedDevice || !selectedChannel || !selectedChannelSupportsPtz;

  const stats = useMemo(() => {
    const onlineDevices = devices.filter((device) => device.online).length;
    const channelCount = devices.reduce((sum, device) => sum + device.channels.length, 0);
    const onlineChannels = devices.reduce(
      (sum, device) => sum + device.channels.filter((channel) => channel.online).length,
      0
    );
    return { onlineDevices, channelCount, onlineChannels };
  }, [devices]);

  const refreshAll = () => {
    healthQuery.refetch();
    devicesQuery.refetch();
  };

  const currentTarget = () => {
    if (!selectedDevice || !selectedChannel) {
      message.warning("请选择在线设备和通道");
      return null;
    }
    return { deviceId: selectedDevice.id, channelId: selectedChannel.id };
  };

  const handleQueryCatalog = () => {
    if (!selectedDevice) {
      message.warning("请选择设备");
      return;
    }
    catalogMutation.mutate(selectedDevice.id, {
      onSuccess: () => {
        window.setTimeout(() => devicesQuery.refetch(), 1200);
      },
    });
  };

  const handleStartPreview = () => {
    const target = currentTarget();
    if (!target) return;
    previewStartMutation.mutate(
      target,
      {
        onSuccess: (result) => {
          setActiveSession(result);
        },
      }
    );
  };

  const handleStopSession = () => {
    if (!activeSession) {
      message.warning("当前没有活动会话");
      return;
    }
    previewStopMutation.mutate(
      { sessionId: activeSession.session_id },
      {
        onSuccess: () => {
          setActiveSession(null);
        },
      }
    );
  };

  const handlePtz = (action: GB28181.PtzAction) => {
    const target = currentTarget();
    if (!target) return;
    ptzMutation.mutate({ ...target, action, speed: ptzSpeed });
  };

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有 GB28181 查询权限，请联系管理员。" />
      </PageContainer>
    );
  }

  const deviceColumns: ColumnsType<GB28181.Device> = [
    {
      title: "状态",
      dataIndex: "online",
      width: 78,
      render: (online: boolean) => onlineTag(online),
    },
    {
      title: "设备",
      dataIndex: "name",
      ellipsis: true,
      render: (name: string, record) => (
        <Space direction="vertical" size={2} className="min-w-0">
          <Space size={4}>
            <Text strong>{displayText(name)}</Text>
            {registrationSourceTag(record.registration_source)}
          </Space>
          <Text type="secondary" className="text-xs">
            {record.id}
          </Text>
          <Text type="secondary" className="text-xs">
            IP {remoteEndpoint(record)}
          </Text>
        </Space>
      ),
    },
    {
      title: "通道",
      width: 64,
      align: "right",
      render: (_, record) => record.channels.length,
    },
  ];

  const channelOptions = channels.map((channel) => ({
    label: `${channel.name || channel.id} (${channel.id})`,
    value: channel.id,
  }));

  const pageHeader = (
    <div className="flex flex-wrap items-center justify-between gap-3">
      <Space size={12} wrap>
        <Title level={4} className="!mb-0">
          GB28181
        </Title>
        <Tag color={healthQuery.isError ? "error" : "success"}>
          {healthQuery.isError ? "模块异常" : healthQuery.data?.status || "运行中"}
        </Tag>
      </Space>
      <Space wrap>
        <Input.Search
          allowClear
          placeholder="搜索设备、编号、厂商、地址"
          value={keyword}
          onChange={(event) => setKeyword(event.target.value)}
          className="w-[280px]"
        />
        <Button icon={<ReloadOutlined />} loading={devicesQuery.isFetching} onClick={refreshAll}>
          刷新
        </Button>
      </Space>
    </div>
  );

  return (
    <PageContainer header={pageHeader}>
      <div className="grid grid-cols-1 2xl:grid-cols-[420px_minmax(0,1fr)] gap-4">
        <div className="space-y-4 min-w-0">
          <Card
            size="small"
            title={
              <Space>
                <VideoCameraOutlined />
                设备列表
              </Space>
            }
            extra={
              <Tag color="blue">
                {stats.onlineDevices}/{devices.length}
              </Tag>
            }
          >
            <div className="grid grid-cols-2 gap-3 mb-3">
              <Statistic
                title="设备在线"
                value={stats.onlineDevices}
                suffix={`/ ${devices.length}`}
                valueStyle={{ fontSize: 20 }}
              />
              <Statistic
                title="通道在线"
                value={stats.onlineChannels}
                suffix={`/ ${stats.channelCount}`}
                valueStyle={{ fontSize: 20 }}
              />
            </div>
            <Table
              rowKey="id"
              size="small"
              columns={deviceColumns}
              dataSource={filteredDevices}
              loading={devicesQuery.isLoading}
              pagination={{ pageSize: 10, size: "small" }}
              onRow={(record) => ({
                onClick: () => {
                  setSelectedDeviceId(record.id);
                  setSelectedChannelId(undefined);
                },
                className:
                  record.id === selectedDevice?.id ? "cursor-pointer bg-blue-50" : "cursor-pointer",
              })}
            />
          </Card>
        </div>

        <div className="space-y-4 min-w-0">
          <Card
            size="small"
            title={
              <Space wrap>
                <PlayCircleOutlined />
                实时预览
                {selectedDevice && <Tag>{selectedDevice.name || selectedDevice.id}</Tag>}
                {ptzCapabilityTag(selectedChannel)}
                <Select
                  size="small"
                  value={selectedChannel?.id}
                  placeholder="选择通道"
                  options={channelOptions}
                  disabled={!selectedDevice}
                  className="min-w-[260px]"
                  onChange={setSelectedChannelId}
                />
              </Space>
            }
            extra={
              <Space wrap>
                <Button
                  icon={<SendOutlined />}
                  disabled={!selectedDevice || !canControl}
                  loading={catalogMutation.isPending}
                  onClick={handleQueryCatalog}
                >
                  目录查询
                </Button>
                <Button
                  type="primary"
                  icon={<PlayCircleOutlined />}
                  disabled={!canControl}
                  loading={previewStartMutation.isPending}
                  onClick={handleStartPreview}
                >
                  开始预览
                </Button>
                <Button
                  danger
                  icon={<PauseCircleOutlined />}
                  disabled={!activeSession || !canControl}
                  loading={previewStopMutation.isPending}
                  onClick={handleStopSession}
                >
                  停止
                </Button>
              </Space>
            }
          >
            <div className="grid grid-cols-1 xl:grid-cols-[minmax(0,1fr)_280px] gap-4 items-start">
              <div className="relative aspect-video bg-black rounded overflow-hidden">
                {activeSession ? (
                  <Gb28181LivePlayer key={activeSession.session_id} session={activeSession} />
                ) : (
                  <div className="h-full flex flex-col items-center justify-center text-white/70">
                    <VideoCameraOutlined className="text-5xl mb-3" />
                    <Text className="!text-white/80">
                      {selectedDevice
                        ? `${selectedDevice.name || selectedDevice.id} / ${selectedChannel?.name || selectedChannel?.id || "未选择通道"}`
                        : "请选择设备和通道"}
                    </Text>
                  </div>
                )}
              </div>

              <div className="space-y-4 xl:border-l xl:pl-4">
                <div className="flex items-center justify-between">
                  <Text strong>云台</Text>
                  <Text type="secondary" className="text-xs">
                    速度 {ptzSpeed}
                  </Text>
                </div>
                <div>
                  <Slider
                    min={1}
                    max={255}
                    value={ptzSpeed}
                    onChange={(value) => setPtzSpeed(Number(value))}
                  />
                </div>
                <div className="grid grid-cols-3 gap-2 w-[210px] mx-auto">
                  {PTZ_ACTIONS.map((item) => (
                    <Tooltip title={item.title} key={item.action}>
                      <Button
                        className={item.className}
                        icon={item.icon}
                        disabled={ptzDisabled}
                        loading={ptzMutation.isPending}
                        onClick={() => handlePtz(item.action)}
                      />
                    </Tooltip>
                  ))}
                </div>
                <Space.Compact block>
                  <Button
                    icon={<PlusOutlined />}
                    disabled={ptzDisabled}
                    loading={ptzMutation.isPending}
                    onClick={() => handlePtz("zoomin")}
                  >
                    变倍+
                  </Button>
                  <Button
                    icon={<MinusOutlined />}
                    disabled={ptzDisabled}
                    loading={ptzMutation.isPending}
                    onClick={() => handlePtz("zoomout")}
                  >
                    变倍-
                  </Button>
                </Space.Compact>
              </div>
            </div>
          </Card>

          <Card size="small" title="当前会话">
            <Descriptions size="small" column={{ xs: 1, lg: 2 }}>
              <Descriptions.Item label="设备">{displayText(selectedDevice?.id)}</Descriptions.Item>
              <Descriptions.Item label="通道">{displayText(selectedChannel?.id)}</Descriptions.Item>
              <Descriptions.Item label="云台">
                {ptzCapabilityTag(selectedChannel)}
              </Descriptions.Item>
              <Descriptions.Item label="设备 IP">
                {remoteEndpoint(selectedDevice)}
              </Descriptions.Item>
              <Descriptions.Item label="状态">
                {selectedDevice ? onlineTag(selectedDevice.online) : "--"}
              </Descriptions.Item>
              <Descriptions.Item label="会话">
                {displayText(activeSession?.session_id)}
              </Descriptions.Item>
              <Descriptions.Item label="RTP">
                {displayText(activeSession?.rtp_port)}
              </Descriptions.Item>
            </Descriptions>
          </Card>
        </div>
      </div>
    </PageContainer>
  );
}
