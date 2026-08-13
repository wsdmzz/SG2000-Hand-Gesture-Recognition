import onnxruntime as ort, numpy as np, glob, os
from PIL import Image

s = ort.InferenceSession('/home/oooa/gesture_sg2000_train/gesture_cls_sg2000.onnx')
names = ['fist', 'number1', 'number2', 'number3', 'number4', 'number5', 'thumbs-up']

# 用数据集 test 级每类取第一张做基准
base = '/home/oooa/gesture_160_tflite_v2/dataset/test'
for cls in names:
    d = os.path.join(base, cls)
    files = glob.glob(os.path.join(d, '*'))
    if not files:
        print(cls, 'NO FILES'); continue
    img = Image.open(files[0]).convert('RGB').resize((128, 128))
    x = (np.array(img, dtype=np.float32) / 127.5) - 1.0
    x = x.transpose(2, 0, 1)[None]
    y = s.run(None, {'input': x})[0][0]
    print(f'{cls}: -> {names[int(y.argmax())]} logits={np.round(y,2).tolist()}')
