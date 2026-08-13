#!/usr/bin/env python3
"""解析 cls_hand_gesture_128_128 cvimodel 的输入元数据（shape/pixel_format/量化）。
cvimodel 是 flatbuffer 格式，CVI_NN_* API 运行时通过 GetInputOutputTensors 暴露。
这里尝试用简单二进制检索 + tpuc 库读取，确定模型输入配置。"""
import struct, sys, os

def scan(model_path):
    data = open(model_path, 'rb').read()
    print(f"模型大小: {len(data)} 字节")
    # cvimodel 通常以 'TPUMLIR' 或版本魔数开头
    print(f"文件头: {data[:16]}")
    # 检索可读字符串中的输入/形状线索
    imports = []
    try:
        # 尝试导入 tpu_mlir 的模型解析
        sys.path.insert(0, '/home/oooa/Milk-V-Duo/ai/tpu-mlir/python')
        from tpu_mlir.python.mlir_converters.converter.onnx_converter import _api
        from tpu_mlir import pack_model
        print("尝试 tpuc pack_model 读取...")
        model = pack_model.PackModel()
        meta = model.pack_model_data(data) if hasattr(model, 'pack_model_data') else None
        print("pack_model_data:", meta)
    except Exception as e:
        print(f"tpu_mlir 解析不可用: {e}")
    return
    # 可读字符串
    strings = []
    for mtc in data.split(b'\x00'):
        if len(mtc) > 3 and all(32 <= b < 127 for b in mtc[:20]):
            s = mtc.decode('latin-1', 'ignore')
            if any(k in s.lower() for k in ['input', 'shape', 'pixel', 'uint8', 'int8', 'rgb', 'nv12', '128', 'name']):
                strings.append(s)
    for s in strings[:30]:
        print(s)

if __name__ == '__main__':
    scan('/home/oooa/Milk-V-Duo/ai/models/cls_hand_gesture_128_128_INT8_cv181x.cvimodel')
