#!/usr/bin/env python3
"""判定实验: 「固定 ROI」vs「自适应 ROI」在模拟远距离下的表现。

方法: 把训练图(手占画面 47%)缩放到不同比例并居中贴回 640x384 画布,
      模拟用户后退导致手变小。对比两种裁剪策略的 bsc:
  A. 固定 ROI 280x280 (当前实现)
  B. 自适应 ROI: 边长 = 手实际尺寸 / 0.93 (让手始终占满 ROI 的 ~93%)

结论用于回答: 该改 640 输入, 还是重新采集远距离数据重训。
"""
import numpy as np
from PIL import Image
import onnxruntime as ort

SZ, PAD, QSCALE = 480, 114, 127.0
OUTDIR = '/home/oooa/Milk-V-Duo'
CANVAS = (640, 384)

sess = ort.InferenceSession(f'{OUTDIR}/models/best_rot_raw_fixed.onnx',
                            providers=['CPUExecutionProvider'])
names = [o.name for o in sess.get_outputs()]


def run(im):
    w0, h0 = im.size
    sc = min(SZ / w0, SZ / h0)
    nw, nh = int(w0 * sc + 0.5), int(h0 * sc + 0.5)
    rz = im.resize((nw, nh), Image.BILINEAR)
    c = Image.new('RGB', (SZ, SZ), (PAD, PAD, PAD))
    c.paste(rz, ((SZ - nw) // 2, (SZ - nh) // 2))
    v = np.asarray(c, dtype=np.float32) / 255.0
    nchw = np.transpose(v, (2, 0, 1))[None, ...]
    q = np.rint(nchw * QSCALE).clip(-128, 127).astype(np.int8)
    o = dict(zip(names, sess.run(None, {'images': q.astype(np.float32) / QSCALE})))
    det = o['440'][0]
    s = 1.0 / (1.0 + np.exp(-np.clip(det[64:71, :], -50, 50)))
    ba = int(np.argmax(s.max(axis=0)))
    bc = int(np.argmax(s[:, ba]))
    return float(s.max()), bc


src = Image.open('/home/oooa/gesture_captured/gesture_data/5/img_001.jpg').convert('RGB')
HAND_PX = 272      # 训练图里手的 bbox 边长(检测模型实测)
FIXED_ROI = 280    # 当前实现的固定 ROI

print(f'基准图: {src.size}, 手约 {HAND_PX}px (占画面长边 {HAND_PX/640*100:.0f}%)')
print(f'画布: {CANVAS}, 固定 ROI={FIXED_ROI}\n')
print(f'{"缩放":>6} {"手像素":>7} | {"A.固定ROI bsc":>13} {"类":>3} | '
      f'{"B.自适应ROI":>12} {"边长":>5} {"bsc":>8} {"类":>3}')

for scale in [1.0, 0.8, 0.65, 0.5, 0.4, 0.3, 0.22]:
    w, h = int(640 * scale), int(384 * scale)
    small = src.resize((w, h), Image.BILINEAR)
    canvas = Image.new('RGB', CANVAS, (114, 114, 114))
    canvas.paste(small, ((CANVAS[0] - w) // 2, (CANVAS[1] - h) // 2))
    hand = HAND_PX * scale

    # A: 固定 ROI
    cx, cy = CANVAS[0] // 2, CANVAS[1] // 2
    x1, y1 = max(0, cx - FIXED_ROI // 2), max(0, cy - FIXED_ROI // 2)
    x2, y2 = min(CANVAS[0], x1 + FIXED_ROI), min(CANVAS[1], y1 + FIXED_ROI)
    bsc_a, c_a = run(canvas.crop((x1, y1, x2, y2)))

    # B: 自适应 ROI (手占满 ~93%)
    side = int(hand / 0.93)
    side = max(64, min(640, side))
    x1b, y1b = max(0, cx - side // 2), max(0, cy - side // 2)
    x2b, y2b = min(CANVAS[0], x1b + side), min(CANVAS[1], y1b + side)
    bsc_b, c_b = run(canvas.crop((x1b, y1b, x2b, y2b)))

    print(f'{scale:>5.2f}x {hand:>6.0f} | {bsc_a:>13.4f} {c_a:>3} | '
          f'{" ":>12} {side:>5} {bsc_b:>8.4f} {c_b:>3}')

print()
print('判读:')
print('  A 列若随缩放快速衰减 → 固定 ROI 无法适应距离变化')
print('  B 列若在各缩放都保持高值 → 自适应 ROI 可解决远距离问题')
print('  → 若 B 成立, 则不必改 640 输入, 也说明"重训"不是当前唯一出路')
