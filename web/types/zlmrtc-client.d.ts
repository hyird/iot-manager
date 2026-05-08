declare module "zlmrtc-client" {
  export interface EndpointOptions {
    element?: HTMLVideoElement | null;
    debug?: boolean;
    zlmsdpUrl: string;
    simulcast?: boolean;
    useCamera?: boolean;
    audioEnable?: boolean;
    videoEnable?: boolean;
    recvOnly?: boolean;
    resolution?: { w: number; h: number };
    usedatachannel?: boolean;
    videoId?: string;
    audioId?: string;
  }

  export class Endpoint {
    constructor(options: EndpointOptions);
    on(event: string, handler: (data: unknown) => void): boolean;
    off(event: string, handler: (data: unknown) => void): boolean;
    offAll(): void;
    close(): void;
    readonly remoteStream: MediaStream | null;
    readonly localStream: MediaStream | null;
  }

  export const Events: {
    WEBRTC_NOT_SUPPORT: string;
    WEBRTC_ICE_CANDIDATE_ERROR: string;
    WEBRTC_OFFER_ANSWER_EXCHANGE_FAILED: string;
    WEBRTC_ON_REMOTE_STREAMS: string;
    WEBRTC_ON_LOCAL_STREAM: string;
    WEBRTC_ON_CONNECTION_STATE_CHANGE: string;
    WEBRTC_ON_DATA_CHANNEL_OPEN: string;
    WEBRTC_ON_DATA_CHANNEL_CLOSE: string;
    WEBRTC_ON_DATA_CHANNEL_ERR: string;
    WEBRTC_ON_DATA_CHANNEL_MSG: string;
    CAPTURE_STREAM_FAILED: string;
  };
}
