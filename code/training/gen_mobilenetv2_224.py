#!/usr/bin/env python3
"""生成官方教程同款 224x224 MobileNetV2 ONNX，对照验证工具链"""
import tensorflow as tf
import tf2onnx
import onnx
import os

OUT = "/tmp/mobilenetv2_224.onnx"

# 官方教程结构: MobileNetV2 不含顶层 + GAP + Dense(1000类,官方用ImageNet)
# 这里用 7 类(与手势一致) + 224x224(官方金标准尺寸)
base = tf.keras.applications.MobileNetV2(
    input_shape=(224, 224, 3), include_top=False, weights=None, pooling='avg')
inp = tf.keras.Input(shape=(224, 224, 3), name="input")
x = base(inp)
x = tf.keras.layers.Dense(7, activation='softmax', name="output")(x)
model = tf.keras.Model(inp, x)
print("MobileNetV2 224x224 参数量:", model.count_params())

spec = (tf.TensorSpec((1, 224, 224, 3), tf.float32, name="input"),)
tf2onnx.convert.from_keras(model, input_signature=spec, output_path=OUT)
print("ONNX 导出:", OUT, os.path.getsize(OUT))
m = onnx.load(OUT)
print("输入:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
print("输出:", [o.name for o in m.graph.output])
