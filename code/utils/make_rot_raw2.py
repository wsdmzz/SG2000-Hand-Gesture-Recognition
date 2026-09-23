#!/usr/bin/env python3
"""在 best_rot.onnx 里定位张量 440/372 (det_raw/kpt_raw), 提升为 graph output。"""
import numpy as np
import onnx
from onnx import helper

ROT = '/home/oooa/Milk-V-Duo/models/best_rot.onnx'
OUT = '/home/oooa/Milk-V-Duo/models/best_rot_raw640.onnx'

m = onnx.load(ROT)
prod = {}
for n in m.graph.node:
    for o in n.output:
        prod[o] = n

print('=== 定位目标张量 ===')
targets = []
for tname, want_ch in [('440', 71), ('372', 63)]:
    n = prod.get(tname)
    if n is None:
        print(f'  ✗ 张量 {tname} 不存在')
        continue
    print(f'  ✓ {tname} <- {n.op_type} "{n.name}" inputs={list(n.input)} outputs={list(n.output)}')
    targets.append((tname, n))

if len(targets) != 2:
    raise SystemExit('定位失败')

# 加为 graph output（类型用 best_debug 的信息）
shapes = {'440': [1, 71, 8400], '372': [1, 63, 8400]}
names = {'440': 'det_raw', '372': 'kpt_raw'}
existing = {o.name for o in m.graph.output}
for tname, n in targets:
    if tname in existing:
        print(f'  {tname} 已是输出')
        continue
    tb = helper.make_tensor_type_proto(onnx.TensorProto.FLOAT, shapes[tname])
    m.graph.output.append(helper.make_value_info(tname, tb))
    print(f'  已添加输出: {tname} (预期形状 {shapes[tname]})')

onnx.checker.check_model(m)
onnx.save_model(m, OUT)
print(f'\n已保存 {OUT}')

# 用 onnxruntime 验证形状
import onnxruntime as ort
sess = ort.InferenceSession(OUT, providers=['CPUExecutionProvider'])
x = np.random.rand(1, 3, 640, 640).astype(np.float32)
outs = sess.run(None, {'images': x})
print('\n=== onnxruntime 实测输出 ===')
for node, arr in zip(sess.get_outputs(), outs):
    print(f'  {node.name}: {arr.shape}  mean={arr.mean():.4f} max={arr.max():.4f}')
