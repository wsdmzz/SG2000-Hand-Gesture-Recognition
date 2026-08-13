#!/usr/bin/env python3
"""将 best.h5 (Keras3) 导出为 ONNX (简化: 直接用 from_function 传入 spec)"""
import os
import shutil
import tensorflow as tf
import tf2onnx
import onnx
import numpy as np

BASE = "/home/oooa/gesture_160_tflite_v2"
MODEL_H5 = os.path.join(BASE, "best.h5")
SM_DIR = os.path.join(BASE, "sm_export")
OUT_ONNX = os.path.join(BASE, "best_gesture.onnx")

print("加载模型:", MODEL_H5)
model = tf.keras.models.load_model(MODEL_H5)
in_shape = list(model.inputs[0].shape)
print("模型输入:", in_shape)

# 用具体的 tf.function 包一层, 显式指定 input layer 名称
inp = tf.keras.layers.Input(shape=in_shape[1:], name="input")
out = model(inp)
m2 = tf.keras.Model(inp, out)

spec = (tf.TensorSpec(in_shape, tf.float32, name="input"),)
print("开始 tf2onnx 转换...")
model_def, _ = tf2onnx.convert.from_keras(m2, input_signature=spec, output_path=OUT_ONNX)
print("ONNX 导出成功:", OUT_ONNX, os.path.getsize(OUT_ONNX), "bytes")

m = onnx.load(OUT_ONNX)
print("ONNX 输入:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
print("ONNX 输出:", [o.name for o in m.graph.output])

# 校验: 用 onnxruntime 跑一次随机输入
import onnxruntime as ort
sess = ort.InferenceSession(OUT_ONNX)
x = np.random.rand(1, in_shape[1], in_shape[2], 3).astype(np.float32)
res = sess.run(None, {"input": x})[0]
print("onnxruntime 推理输出 shape:", res.shape, "sum:", float(np.sum(res)))
print("DONE")
