/**
 * 图片预览弹窗组件
 */

import React, { useImperativeHandle, useState, forwardRef } from "react";
import { Modal } from "antd";

export interface ImagePreviewModalRef {
  open: (base64: string, title?: string) => void;
}

interface ImagePreviewModalProps {
  onClose?: () => void;
}

const ImagePreviewModal = forwardRef<ImagePreviewModalRef, ImagePreviewModalProps>((props, ref) => {
  const { onClose } = props;
  const [open, setOpen] = useState(false);
  const [imgSrc, setImgSrc] = useState<string>("");
  const [title, setTitle] = useState<string>("图片预览");

  useImperativeHandle(ref, () => ({
    open: (base64: string, t?: string) => {
      const hasPrefix = base64.startsWith("data:image");
      const src = hasPrefix ? base64 : `data:image/png;base64,${base64}`;
      setImgSrc(src);
      setTitle(t || "图片预览");
      setOpen(true);
    },
  }));

  const handleCancel = () => {
    setOpen(false);
    onClose?.();
  };

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

  return (
    <Modal open={open} title={title} onCancel={handleCancel} footer={null} width={800}>
      {imgSrc ? (
        <div style={contentStyle}>
          <img src={imgSrc} alt="preview" style={imgStyle} />
        </div>
      ) : null}
    </Modal>
  );
});

ImagePreviewModal.displayName = "ImagePreviewModal";

export default ImagePreviewModal;
