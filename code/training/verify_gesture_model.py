#!/usr/bin/env python3
"""本地验证 best.h5 手势分类模型在验证集上的准确率"""
import os, glob
import numpy as np
from PIL import Image

# 用 tf_env 的 tensorflow
import tensorflow as tf

BASE = "/home/oooa/gesture_160_tflite_v2"
MODEL_H5 = os.path.join(BASE, "best.h5")
VALID_DIRS = [
    os.path.join(BASE, "ESP32S3 Gestures.v1i.folder/valid"),
    os.path.join(BASE, "ESP32S3 Gestures.v1i.folder/test"),
]
INPUT_SIZE = 160
N2ID = {'fist': 0, 'number1': 1, 'number2': 2, 'number3': 3,
        'number4': 4, 'number5': 5, 'thumbs-up': 6}
ID2N = {v: k for k, v in N2ID.items()}


def center_crop_resize(img, size):
    w, h = img.size
    if w > h:
        x0 = (w - h) // 2
        img = img.crop((x0, 0, x0 + h, h))
    elif h > w:
        y0 = (h - w) // 2
        img = img.crop((0, y0, w, y0 + w))
    return img.resize((size, size), Image.BILINEAR)


def main():
    print("加载模型:", MODEL_H5)
    model = tf.keras.models.load_model(MODEL_H5)
    print("模型输入:", model.inputs[0].shape)
    print("模型输出:", model.outputs[0].shape)

    total = 0
    correct = 0
    per_cls = {k: [0, 0] for k in N2ID}  # correct, total

    for sdir in VALID_DIRS:
        for cls_name, label in N2ID.items():
            cls_dir = os.path.join(sdir, cls_name)
            if not os.path.isdir(cls_dir):
                continue
            files = glob.glob(f'{cls_dir}/*.jpg') + glob.glob(f'{cls_dir}/*.jpeg') + glob.glob(f'{cls_dir}/*.png')
            for f in files:
                img = Image.open(f).convert('RGB')
                img = center_crop_resize(img, INPUT_SIZE)
                x = np.array(img, dtype=np.float32) / 255.0
                x = np.expand_dims(x, 0)
                pred = model.predict(x, verbose=0)[0]
                pred_cls = int(np.argmax(pred))
                total += 1
                per_cls[cls_name][1] += 1
                if pred_cls == label:
                    correct += 1
                    per_cls[cls_name][0] += 1

    print("\n========== 验证结果 ==========")
    print(f"总样本: {total}, 正确: {correct}, 准确率: {100.0*correct/total:.2f}%")
    print("\n每类准确率:")
    for cls_name in N2ID:
        c, t = per_cls[cls_name]
        acc = 100.0 * c / t if t else 0
        print(f"  {cls_name:10s}: {c}/{t} = {acc:.1f}%")


if __name__ == "__main__":
    main()
