#!/usr/bin/env python3
"""预验证: MobileNetV2 架构能否稳定导出 ONNX (Keras 3 + tf2onnx)"""
import tensorflow as tf
import tf2onnx
import onnx
import numpy as np
import os

OUT = "/tmp/mobilenetv2_verify.onnx"

# 用官方 MobileNetV2 (不含顶层), 接一个分类头
print("构建 MobileNetV2 分类模型 (128x128x3 -> 7 类)...")
base = tf.keras.applications.MobileNetV2(
    input_shape=(128, 128, 3), include_top=False, weights=None, pooling='avg')
inp = tf.keras.Input(shape=(128, 128, 3), name="input")
x = base(inp)
x = tf.keras.layers.Dense(7, activation='softmax', name="output")(x)
model = tf.keras.Model(inp, x)
print("参数量:", model.count_params())

# 导出一个测试权重文件供后续转换
model.save("/tmp/mobilenetv2_verify.h5")

# 尝试导出 ONNX - 用 from_keras 直接导出
spec = (tf.TensorSpec((1, 128, 128, 3), tf.float32, name="input"),)
print("导出 ONNX (from_keras)...")
try:
    tf2onnx.convert.from_keras(model, input_signature=spec, output_path=OUT)
    print("ONNX 导出成功:", OUT, os.path.getsize(OUT))
    m = onnx.load(OUT)
    print("ONNX 输入:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
    print("ONNX 输出:", [o.name for o in m.graph.output])
    print("VERIFY_OK")
except Exception as e:
    print("DEBUG: from_keras 直接导出失败:", e)
    print("尝试用 Input 包装法...")
    inp2 = tf.keras.layers.Input(shape=(128, 128, 3), name="input")
    out2 = model(inp2)
    m2 = tf.keras.Model(inp2, out2)
    spec2 = (tf.TensorSpec((1, 128, 128, 3), tf.float32, name="input"),)
    tf2onnx.convert.from_keras(m2, input_signature=spec2, output_path=OUT)
    print("ONNX 导出成功(包装法):", OUT, os.path.getsize(OUT))
    m = onnx.load(OUT)
    print("ONNX 输入:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
    print("ONNX 输出:", [o.name for o in m.graph.output])
    print("VERIFY_OK")
