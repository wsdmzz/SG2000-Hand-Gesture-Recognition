#!/usr/bin/env python3
"""
nv21_to_handdet_input.py - 把真实 NV21 摄像头帧转成 yolov8n_det_hand 的 int8 NCHW 输入
输入: NV21 YUV 帧 + 尺寸
输出: prep_int8.bin (与 synth_hand_prep.bin 同格式, 384x640 输入)
用于验证: 真实摄像头帧能否被手部检测模型检出
"""
import sys, math
import numpy as np

def nv21_to_rgb(yuv, w, h):
    """NV21(YYYY VU): Y 平面 w*h, 后 w*h/2 为 VU 交错"""
    Y = yuv[:w*h].reshape(h, w).astype(np.float32)
    uv_size = w*h//4
    uv = yuv[w*h:].astype(np.float32)
    u = uv[0::2].reshape(h//2, w//2)  # NV21: U 在奇数位
    v = uv[1::2].reshape(h//2, w//2)
    # 上采样 UV
    U = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)
    V = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)
    # YUV -> RGB (BT.601)
    C = Y - 16.0
    D = U - 128.0
    E = V - 128.0
    R = np.clip((298.082*C + 408.583*E) / 256.0, 0, 255)
    G = np.clip((298.082*C - 100.291*D - 208.120*E) / 256.0, 0, 255)
    B = np.clip((298.082*C + 516.412*D) / 256.0, 0, 255)
    rgb = np.stack([R, G, B], axis=-1).astype(np.uint8)  # H,W,3 BGR顺序被覆盖
    return rgb  # 返回 R,G,B 通道顺序的 H,W,3

def main():
    if len(sys.argv) < 2:
        print("usage: nv21_to_handdet_input.py in.nv21 [w] [h] [out.bin]")
        sys.exit(1)
    src = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 1280
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 720
    out = sys.argv[4] if len(sys.argv) > 4 else "handdet_prep.bin"
    target_w, target_h = 640, 384

    yuv = np.fromfile(src, dtype=np.uint8)
    print(f"read {len(yuv)} bytes from {src}")
    rgb = nv21_to_rgb(yuv, w, h)          # H,W,3 (R,G,B顺序)
    print(f"rgb shape={rgb.shape}")

    # resize 到 640x384 (模型输入)
    import cv2
    img = cv2.resize(rgb, (target_w, target_h), interpolation=cv2.INTER_LINEAR)
    # img 是 BGR->实际我们rgb顺序, 模型要 RGB; cv2读图是BGR,这里rgb数组是RGB顺序直接用
    # 模型输入 x/127.5-1 归一化后 * qscale(~127.7) -> int8
    norm = (img.astype(np.float32) / 127.5) - 1.0
    qscale = 127.702
    int8_val = np.clip(np.round(norm * qscale), -128, 127).astype(np.int8)
    # 转 NCHW: 模型输入 [1,3,384,640], 通道序 RGB
    nchw = int8_val.transpose(2, 0, 1)   # C,H,W (3,384,640)
    nchw_flat = nchw.reshape(-1)
    nchw_flat.tofile(out)
    print(f"wrote {out}: {nchw_flat.shape} bytes, dims [1,3,{target_h},{target_w}] int8")
    # 统计
    print(f"  int8 min={int8_val.min()} max={int8_val.max()} mean={int8_val.mean():.1f}")
    print(f"  非零比例={(np.abs(int8_val)>4).mean()*100:.1f}%")

if __name__ == "__main__":
    main()
