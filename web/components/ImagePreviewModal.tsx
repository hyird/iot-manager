/**
 * 图片预览弹窗组件
 */

import { forwardRef, useImperativeHandle, useState } from "react";
import { Modal } from "antd";

type ImageMimeType = "image/png" | "image/jpeg" | "image/gif" | "image/webp";

export interface ImagePreviewModalRef {
  open: (base64: string, title?: string, mimeType?: ImageMimeType) => void;
}

interface ImagePreviewModalProps {
  onClose?: () => void;
}

const contentStyle: React.CSSProperties = {
  textAlign: "center",
  maxHeight: "70vh",
  overflow: "auto",
};

const imgStyle: React.CSSProperties = {
  maxWidth: "100%",
  maxHeight: "100%",
  display: "inline-block",
};

const DEFAULT_TITLE = "图片预览";

const ImagePreviewModal = forwardRef<ImagePreviewModalRef, ImagePreviewModalProps>(
  ({ onClose }, ref) => {
    const [open, setOpen] = useState(false);
    const [imgSrc, setImgSrc] = useState("");
    const [title, setTitle] = useState(DEFAULT_TITLE);

    useImperativeHandle(ref, () => ({
      open: (base64: string, t?: string, mimeType: ImageMimeType = "image/png") => {
        const src = base64.startsWith("data:image") ? base64 : `data:${mimeType};base64,${base64}`;
        setImgSrc(src);
        setTitle(t || DEFAULT_TITLE);
        setOpen(true);
      },
    }));

    const handleClose = () => {
      setOpen(false);
      setImgSrc("");
      onClose?.();
    };

    return (
      <Modal open={open} title={title} onCancel={handleClose} footer={null} width={800}>
        <div style={contentStyle}>
          <img src={imgSrc} alt="preview" style={imgStyle} />
        </div>
      </Modal>
    );
  }
);

ImagePreviewModal.displayName = "ImagePreviewModal";

export default ImagePreviewModal;
