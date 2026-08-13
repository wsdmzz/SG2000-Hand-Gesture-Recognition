#!/usr/bin/env python3
"""
jpg_to_handdet_input.py - 把真实推流 JPEG 帧转成 yolov8n_det_hand 的 int8 NCHW 输入
用于验证真实摄像头画面能否被手部检测检出
"""
import sys
import numpy as np
import cv2

def main():
    if len(sys.argv) < 2:
        print("usage: jpg_to_handdet_input.py in.jpg [out.bin]")
        sys.exit(1)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "handdet_prep.bin"
    target_w, target_h = 640, 384

    img = cv2.imread(src)                 # BGR
    if img is None:
        print("cannot read", src); sys.exit(1)
    print(f"input {src}: {img.shape[1]}x{img.shape[0]}")
    # 等比例缩放到 640x384 (模型输入), letterbox 保持
    h, w = img.shape[:2]
    scale = min(target_w / w, target_h / h)
    nw, nh = int(w * scale), int(h * scale)
    resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.zeros((target_h, target_w, 3), dtype=np.uint8)
    x0, y0 = (target_w - nw)//2, (target_h - nh)//2
    canvas[y0:y0+nh, x0:x0+nw] = resized
    # BGR -> RGB
    rgb = canvas[:, :, ::-1].copy()
    # 归一化 x/127.5-1, * qscale -> int8 NCHW
    norm = (rgb.astype(np.float32) / 127.5) - 1.0
    qscale = 127.702
    i8 = np.clip(np.round(norm * qscale), -128, 127).astype(np.int8)
    nchw = i8.transpose(2, 0, 1).reshape(-1)
    nchw.tofile(out)
    print(f"wrote {out}: {nchw.shape} bytes [1,3,{target_h},{target_w}] int8, qscale={qscale}")
    print(f"  int8 min={i8.min()} max={i8.max()} mean={i8.mean():.1f}")

if __name__ == "__main__":
    main()
