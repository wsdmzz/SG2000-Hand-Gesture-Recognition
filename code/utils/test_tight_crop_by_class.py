#!/usr/bin/env python3
"""关键实验: 训练图上对比「全图 letterbox」vs「手部紧裁剪 letterbox」的 bsc。

回答: 模型对 G2/G3/G4（部分伸展手形）在「手占满」条件下能否可靠检出？
  - 若紧裁剪后各类都高  → 模型能力OK, 问题在实时场景(手位/内容)
  - 若紧裁剪后 G2~G4 仍低 → 模型对该类手形本身不敏感(需重训)
"""
import glob
import numpy as np
from PIL import Image
import onnxruntime as ort

SZ, PAD, QSCALE = 480, 114, 127.0
GEST = '/home/oooa/gesture_captured/gesture_data'

sess = ort.InferenceSession('/home/oooa/Milk-V-Duo/models/best_480_raw_fixed.onnx',
                            providers=['CPUExecutionProvider'])
names = [o.name for o in sess.get_outputs()]


def run(im):
    w0, h0 = im.size
    sc = min(SZ / w0, SZ / h0)
    nw, nh = int(w0 * sc + 0.5), int(h0 * sc + 0.5)
    rz = im.resize((nw, nh), Image.BILINEAR)
    canvas = Image.new('RGB', (SZ, SZ), (PAD, PAD, PAD))
    canvas.paste(rz, ((SZ - nw) // 2, (SZ - nh) // 2))
    v = np.asarray(canvas, dtype=np.float32) / 255.0
    nchw = np.transpose(v, (2, 0, 1))[None, ...]
    q = np.rint(nchw * QSCALE).clip(-128, 127).astype(np.int8)
    o = dict(zip(names, sess.run(None, {'images': q.astype(np.float32) / QSCALE})))
    det = o['det_raw'][0]
    s = 1.0 / (1.0 + np.exp(-det[64:71, :]))
    ba = int(np.argmax(s.max(axis=0)))
    bc = int(np.argmax(s[:, ba]))
    return float(s.max()), bc


def skin_box(im, pad=6):
    a = np.asarray(im, dtype=np.int16)
    R, G, B = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    m = (R > 95) & (G > 40) & (B > 20) & (R > G) & (R > B) & ((R - B) > 15) & (np.abs(R - G) > 10)
    if m.sum() < 50:
        return None
    ys, xs = np.nonzero(m)
    x1, y1, x2, y2 = int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())
    cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
    side = max(x2 - x1, y2 - y1) + 2 * pad
    return (max(0, cx - side // 2), max(0, cy - side // 2),
            min(im.size[0], cx + side // 2), min(im.size[1], cy + side // 2))


CLS_NAME = {0: 'G0拳', 1: 'G1一', 2: 'G2二', 3: 'G3三', 4: 'G4四', 5: 'G5五', 6: 'G6赞'}
print(f'{"类别":>8} {"文件":>12} | {"全图 bsc":>9} {"类":>3} | {"紧裁剪 bsc":>11} {"类":>3} {"命中":>4}')
stat = {}
for cls in range(7):
    for f in sorted(glob.glob(f'{GEST}/{cls}/*.jpg'))[:3]:
        im = Image.open(f).convert('RGB')
        b1, c1 = run(im)
        box = skin_box(im)
        b2, c2 = (-1, -1)
        if box:
            crop = im.crop(box)
            # 裁成正方形(避免变形)
            side = max(crop.size)
            sq = Image.new('RGB', (side, side), (114, 114, 114))
            sq.paste(crop, ((side - crop.size[0]) // 2, (side - crop.size[1]) // 2))
            b2, c2 = run(sq)
        ok = '✓' if c2 == cls else '✗'
        nm = f.split('/')[-1]
        print(f'{CLS_NAME[cls]:>8} {nm:>12} | {b1:>9.4f} {c1:>3} | {b2:>11.4f} {c2:>3} {ok:>4}')
        stat.setdefault(cls, []).append((b1, b2))

print('\n=== 各类均值 ===')
print(f'{"类别":>8} {"全图均值":>10} {"紧裁剪均值":>12}')
for cls in range(7):
    if cls in stat:
        v1 = np.mean([x[0] for x in stat[cls]])
        v2 = np.mean([x[1] for x in stat[cls]])
        print(f'{CLS_NAME[cls]:>8} {v1:>10.4f} {v2:>12.4f}')
