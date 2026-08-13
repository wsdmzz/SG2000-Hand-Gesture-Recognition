#!/usr/bin/env python3
"""
inject_normalize.py - 在 ONNX 输入后注入归一化节点 x/255。
训练/验证脚本精确使用 pixel/255.0 -> [0,1]（见 train.py / verify_gesture_model.py）。
TDL 对 FP32 输入模型是 0-255 直通(不做归一化)，把归一化编进模型后 TDL 直通即正确。
注入: input -> Mul(1/255) -> 原首个算子。y = x * (1/255)
此版本修正前版错误用 (x-128)/127.5，改为正确的 x/255。
"""
import onnx
from onnx import helper, TensorProto
import sys

SRC = '/home/oooa/gesture_160_tflite_v2/gesture_cls_sg2000_gap.onnx'
DST = '/home/oooa/gesture_160_tflite_v2/gesture_cls_sg2000_gap_norm255.onnx'
SCALE = 1.0 / 255.0  # ≈0.0039216

m = onnx.load(SRC)
g = m.graph

in_name = g.input[0].name
first_node = None
for n in g.node:
    if in_name in n.input:
        first_node = n
        break
assert first_node is not None, "输入未被任何节点消费?"

mul_name = in_name + '_norm255_mul'
scale_init = helper.make_tensor('norm255_scale_cst', TensorProto.FLOAT, [1], [SCALE])
mul_node = helper.make_node('Mul', [in_name, 'norm255_scale_cst'], [mul_name], name='Normalize255_Mul')

first_node.input[:] = [mul_name if x == in_name else x for x in first_node.input]

insert_idx = g.node.index(first_node)
g.node.insert(insert_idx, mul_node)
g.initializer.append(scale_init)

onnx.save(m, DST)
print(f"已注入 x/255 归一化: {SRC} -> {DST}")
print(f"  Mul(scale={SCALE:.7f}), 即 y=x/255")
print(f"  首个算子 {first_node.op_type} 的输入已从 {in_name} 改为 {mul_name}")
