#!/usr/bin/env python3
"""从 best_rot_raw640.onnx 生成 480 输入版本（重建网格常量 + 改尺寸）。

已验证的生成逻辑:
  grid  = 对每个 stride 做 meshgrid(ij) 展平, 顺序 P3(80²) → P4(40²) → P5(20²)
  中心坐标 = grid + 0.5 ; stride 表 = 按层填充
  480 时: P3(60²) + P4(30²) + P5(15²) = 4725
"""
import numpy as np
import onnx
from onnx import helper, numpy_helper as nh

SRC = '/home/oooa/Milk-V-Duo/models/best_rot_raw640.onnx'
DST = '/home/oooa/Milk-V-Duo/models/best_rot_raw_fixed.onnx'
NEW_SZ, OLD_N, NEW_N = 480, 8400, 4725
SIZES480 = [(8, 60), (16, 30), (32, 15)]

m = onnx.load(SRC)


def gen_grid(sizes):
    gs = []
    for s, n in sizes:
        y, x = np.meshgrid(np.arange(n), np.arange(n), indexing='ij')
        gs.append(np.stack((x, y), 2).reshape(-1, 2))
    return np.concatenate(gs, 0)          # (N,2)


def gen_stride(sizes):
    return np.concatenate([np.full(n * n, s) for s, n in sizes]).astype(np.float32)


g = gen_grid(SIZES480)                     # (4725,2)
st = gen_stride(SIZES480)                  # (4725,)
print(f'目标网格: {g.shape}  stride: {st.shape}')


def set_const(name, arr):
    for n in m.graph.node:
        if n.op_type == 'Constant' and n.name == name:
            for a in n.attribute:
                if a.name == 'value':
                    a.t.CopyFrom(nh.from_array(arr.astype(np.float32), name=a.t.name))
                    return True
    return False


# --- 1. 重建 5 个网格/步长常量 ---
r = []
r.append(('Constant_264', set_const('Constant_264', g.T[None, :, :] + 0.5)))
r.append(('Constant_266', set_const('Constant_266', g.T[None, :, :] + 0.5)))
r.append(('Constant_284', set_const('Constant_284', g.T)))
r.append(('Constant_272', set_const('Constant_272', st[None, :])))
r.append(('Constant_286', set_const('Constant_286', st[None, :])))
for nm, ok in r:
    print(f'  {nm}: {"OK" if ok else "失败"}')

# --- 2. 输入/输出尺寸 ---
for i in m.graph.input:
    if i.name == 'images':
        d = i.type.tensor_type.shape.dim
        d[2].dim_value = NEW_SZ
        d[3].dim_value = NEW_SZ
for o in m.graph.output:
    d = o.type.tensor_type.shape.dim
    if len(d) == 3 and d[2].dim_value == OLD_N:
        d[2].dim_value = NEW_N

# --- 3. 硬编码 anchors 的 Reshape shape 常量 ---
for idx, init in enumerate(m.graph.initializer):
    arr = nh.to_array(init)
    if arr.dtype.kind in 'iu' and OLD_N in arr.flatten():
        new = arr.copy()
        new[new == OLD_N] = NEW_N
        m.graph.initializer[idx].CopyFrom(nh.from_array(new, name=init.name))
        print(f'  init {init.name}: {arr.tolist()} → {new.tolist()}')

# --- 4. 补 Slice steps（TPU-MLIR v1.3.228 坑） ---
used = {i.name for i in m.graph.initializer}
used |= {o for n in m.graph.node for o in n.output}
used |= {i for n in m.graph.node for i in n.input}
fixed = 0
for n in m.graph.node:
    if n.op_type != 'Slice' or len(n.input) != 4:
        continue
    axes_name = n.input[3]
    ln = 1
    for init in m.graph.initializer:
        if init.name == axes_name:
            ln = int(nh.to_array(init).size)
            break
    sn = f'{n.name}_steps_const'
    k = 0
    while sn in used:
        k += 1
        sn = f'{n.name}_steps_const_{k}'
    used.add(sn)
    m.graph.initializer.append(nh.from_array(np.ones(ln, dtype=np.int64), name=sn))
    n.input.append(sn)
    fixed += 1
print(f'补 Slice steps: {fixed}')

onnx.checker.check_model(m)
onnx.save_model(m, DST)
print(f'已保存 {DST}')

# --- 5. 验证: 对比 640 与 480 的语义输出 ---
import onnxruntime as ort
from PIL import Image

img = Image.open('/home/oooa/gesture_captured/gesture_data/5/img_001.jpg').convert('RGB')


def prep(im, sz):
    w0, h0 = im.size
    sc = min(sz / w0, sz / h0)
    nw, nh = int(w0 * sc + 0.5), int(h0 * sc + 0.5)
    rz = im.resize((nw, nh), Image.BILINEAR)
    c = Image.new('RGB', (sz, sz), (114, 114, 114))
    c.paste(rz, ((sz - nw) // 2, (sz - nh) // 2))
    return np.transpose(np.asarray(c, dtype=np.float32) / 255.0, (2, 0, 1))[None, ...]


def top1(path, sz):
    s = ort.InferenceSession(path, providers=['CPUExecutionProvider'])
    names = [o.name for o in s.get_outputs()]
    x = prep(img, sz)
    o = dict(zip(names, s.run(None, {'images': x})))
    det = o['440'][0]
    scores = 1.0 / (1.0 + np.exp(-np.clip(det[64:71, :], -50, 50)))
    ba = int(np.argmax(scores.max(axis=0)))
    bc = int(np.argmax(scores[:, ba]))
    return names, o, float(scores.max()), bc, ba


print('\n=== 语义验证: 同一张图 (训练图 c5=张开) ===')
for path, sz, tag in [(SRC, 640, '640 原始'), (DST, 480, '480 新建')]:
    names, o, bsc, bc, ba = top1(path, sz)
    print(f'{tag}: 输出={sorted(names)}')
    print(f'   det_raw={o["440"].shape} kpt_raw={o["372"].shape}')
    print(f'   top1 类别={bc} 置信={bsc:.4f} anchor={ba}')
