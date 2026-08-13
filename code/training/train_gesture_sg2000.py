#!/usr/bin/env python3
"""
手势分类模型训练 + 导出规范 NCHW ONNX (SG2000/cv181x 兼容)

目的：
  在 SG2000 (cv181x) 上用 TPU-MLIR v1.28.1 编译手势分类 cvimodel。
  之前 Keras 3 导出 ONNX 是 NHWC 布局，导致 onnxruntime ConstantFolding
  把通道误判（C:160 vs kernel:3），后端 address-assign 越界崩溃。
  本脚本改用 PyTorch，导出规范 NCHW ONNX，彻底避开该问题。

用法（直接运行，默认全自动）：
  python3 train_gesture_sg2000.py
  或指定: python3 train_gesture_sg2000.py --input 128 --dataset /path/to/dataset

输出：
  <work_dir>/gesture_cls_sg2000.onnx  (NCHW 规范 ONNX，可转 cvimodel)
  <work_dir>/gesture_cls_sg2000.pt
  并在终端打印转换到 cvimodel 的命令。
"""
import argparse
import os
import shutil
import sys
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
from torchvision import datasets, transforms, models

CLASSES = ["fist", "number1", "number2", "number3", "number4", "number5", "thumbs-up"]
NUM_CLASSES = 7


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--dataset", default="/home/oooa/gesture_160_tflite_v2/dataset",
                   help="数据集根目录，按 train/valid/test 子目录 + 类别子目录组织")
    p.add_argument("--input", type=int, default=128, help="输入尺寸(正方形)，SG2000 手势分类标准 128")
    p.add_argument("--epochs", type=int, default=30, help="训练轮数")
    p.add_argument("--batch", type=int, default=32)
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument("--work", default=os.path.expanduser("~/gesture_sg2000_train"),
                   help="输出工作目录")
    return p.parse_args()


def main():
    args = parse_args()
    os.makedirs(args.work, exist_ok=True)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"[INFO] device={device}, input={args.input}x{args.input}, "
          f"classes={NUM_CLASSES}, dataset={args.dataset}")

    # ---------- 数据增强 + 归一化 (ImageNet 均值, 与 MobileNetV2 预训练一致) ----------
    # 注意: cvimodel 转换时用 --scale 0.0039215 (1/255) + mean=[0,0,0] 的方式更贴近
    # 摄像头 uint8 输入。但训练用 ImageNet 归一化。为了转换一致，这里在数据增强阶段
    # 用 Normalize(mean=[0.485,0.456,0.406], std=[0.229,0.224,0.225]) 训练，
    # 转换时需把该归一化 fold 进 scale/mean。若不想 fold，可改用
    # Normalize(mean=[0.5,0.5,0.5],std=[0.5,0.5,0.5])，转换时 scale=2*0.0039215, mean=[-1,-1,-1]。
    # 本项目推荐：训练与转换都用“0~1 输入 + Channel 均值”，转换参数固定用下面 TRANS_CFG。
    transform_train = transforms.Compose([
        transforms.Resize((args.input, args.input)),
        transforms.RandomHorizontalFlip(0.5),
        transforms.RandomRotation(15),
        transforms.RandomAffine(degrees=10, translate=(0.1, 0.1), scale=(0.9, 1.1)),
        transforms.RandomPerspective(distortion_scale=0.2, p=0.5),
        # 颜色扰动: 贴近 GC2083 ISP 实拍图 vs 训练图色彩差异 (对 five/none 误判尤其重要)
        transforms.ColorJitter(brightness=0.2, contrast=0.2, saturation=0.2),
        transforms.ToTensor(),
        # 手势图偏简单，用 0.5/0.5/0.5 归一化，便于转换时 fold 成 scale/mean
        transforms.Normalize([0.5, 0.5, 0.5], [0.5, 0.5, 0.5]),
    ])
    transform_eval = transforms.Compose([
        transforms.Resize((args.input, args.input)),
        transforms.ToTensor(),
        transforms.Normalize([0.5, 0.5, 0.5], [0.5, 0.5, 0.5]),
    ])

    train_ds = datasets.ImageFolder(os.path.join(args.dataset, "train"), transform_train)
    valid_ds = datasets.ImageFolder(os.path.join(args.dataset, "valid"), transform_eval)
    # 用 test 集当最终评估（若存在）
    test_path = os.path.join(args.dataset, "test")
    test_ds = datasets.ImageFolder(test_path, transform_eval) if os.path.isdir(test_path) else valid_ds

    print(f"[INFO] train={len(train_ds)}, valid={len(valid_ds)}, test={len(test_ds)}")
    print(f"[INFO] 类别映射 = {train_ds.class_to_idx}")

    train_loader = DataLoader(train_ds, batch_size=args.batch, shuffle=True, num_workers=0)
    valid_loader = DataLoader(valid_ds, batch_size=args.batch, shuffle=False, num_workers=0)
    test_loader = DataLoader(test_ds, batch_size=args.batch, shuffle=False, num_workers=0)

    # ---------- 模型: MobileNetV2 ----------
    # 官方 cv18x 已支持 MobileNetV2 全套算子；小输入 128 用 weights=None 从头训练
    model = models.mobilenet_v2(weights=None, num_classes=NUM_CLASSES)
    model = model.to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"[INFO] MobileNetV2 params = {n_params/1e6:.2f}M")

    criterion = nn.CrossEntropyLoss()
    optimizer = optim.Adam(model.parameters(), lr=args.lr)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs)

    best_acc = 0.0
    for epoch in range(1, args.epochs + 1):
        model.train()
        run_loss, run_correct, run_total = 0.0, 0, 0
        for x, y in train_loader:
            x, y = x.to(device), y.to(device)
            optimizer.zero_grad()
            out = model(x)
            loss = criterion(out, y)
            loss.backward()
            optimizer.step()
            run_loss += loss.item() * x.size(0)
            run_correct += (out.argmax(1) == y).sum().item()
            run_total += x.size(0)
        scheduler.step()

        # 验证
        model.eval()
        v_correct, v_total = 0, 0
        with torch.no_grad():
            for x, y in valid_loader:
                x, y = x.to(device), y.to(device)
                out = model(x)
                v_correct += (out.argmax(1) == y).sum().item()
                v_total += x.size(0)
        acc = v_correct / v_total
        print(f"[Epoch {epoch:02d}/{args.epochs}] loss={run_loss/run_total:.4f} "
              f"train_acc={run_correct/run_total:.4f} valid_acc={acc:.4f}")
        if acc > best_acc:
            best_acc = acc
            torch.save(model.state_dict(), os.path.join(args.work, "best_model.pt"))
            print(f"    -> saved best (valid_acc={acc:.4f})")

    # ---------- 最终评估 ----------
    model.load_state_dict(torch.load(os.path.join(args.work, "best_model.pt"), map_location=device))
    model.eval()
    t_correct, t_total = 0, 0
    conf = np.zeros((NUM_CLASSES, NUM_CLASSES), dtype=int)
    with torch.no_grad():
        for x, y in test_loader:
            x, y = x.to(device), y.to(device)
            pred = model(x).argmax(1)
            t_correct += (pred == y).sum().item()
            t_total += x.size(0)
            for p, t in zip(pred.cpu().numpy(), y.cpu().numpy()):
                conf[t, p] += 1
    print(f"\n[TEST] accuracy = {t_correct/t_total:.4f} ({t_correct}/{t_total})")
    print("[CONFUSION] 行=真实, 列=预测")
    print("             " + " ".join(f"{c[:6]:>6}" for c in CLASSES))
    for i, row in enumerate(conf):
        print(f"  {CLASSES[i][:6]:>6} " + " ".join(f"{v:6d}" for v in row))

    # ---------- 导出规范 NCHW ONNX ----------
    # 关键: PyTorch 导出即 NCHW。输入 batch 用动态轴，方便后续测试与部署。
    onnx_path = os.path.join(args.work, "gesture_cls_sg2000.onnx")
    dummy = torch.randn(1, 3, args.input, args.input, device=device)
    torch.onnx.export(
        model, dummy, onnx_path,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes={"input": {0: "batch"}, "output": {0: "batch"}},
        opset_version=13,
    )
    print(f"[EXP] ONNX 已导出: {onnx_path}")

    # ---------- onnxruntime 验证 (确认结构合法, 无 ConstantFolding 崩溃风险) ----------
    try:
        import onnx
        import onnxruntime as ort
        m = onnx.load(onnx_path)
        onnx.checker.check_model(m)
        inp = m.graph.input[0]
        shape = [d.dim_value for d in inp.type.tensor_type.shape.dim]
        print(f"[VERIFY] ONNX 输入 {inp.name} 形状 = {shape} (应为 NCHW: 1,3,{args.input},{args.input})")

        sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
        x = np.random.randn(1, 3, args.input, args.input).astype(np.float32)
        out = sess.run(None, {"input": x})[0]
        sm = np.exp(out - out.max(axis=1, keepdims=True))
        sm = sm / sm.sum(axis=1, keepdims=True)
        print(f"[VERIFY] onnxruntime 推理 OK, 输出形状={out.shape}, softmax sum={sm.sum():.4f} (=1 即正常)")
    except Exception as e:
        print(f"[WARN] onnxruntime 验证失败: {e}")
        print("       请检查 ONNX 结构")

    # ---------- 打印后续转换命令 ----------
    print("\n" + "=" * 70)
    print("下一步: 把下面这段命令复制到 tpuc_dev 容器内转换 cvimodel")
    print("=" * 70)
    print(f"""
# 1) 把 ONNX 放入容器挂载目录, 例如:
#    cp {onnx_path} /home/oooa/gesture_160_tflite_v2/gesture_cls_sg2000.onnx

# 2) 容器内转换 (工作目录 /workspace/model)
podman exec -it tpuc_conv bash
cd /workspace/work_v1281

# ONNX -> MLIR
/usr/local/bin/model_transform.py \\
  --model_name gesture_sg2000 \\
  --model_def /workspace/model/gesture_cls_sg2000.onnx \\
  --input_shapes "[[1,3,{args.input},{args.input}]]" \\
  --pixel_format rgb --scale 0.0078431,0.0078431,0.0078431 \\
  --mean 1.0,1.0,1.0 \\
  --mlir gesture_sg2000.mlir

# INT8 校准 (用你的手势图)
/usr/local/bin/run_calibration.py gesture_sg2000.mlir \\
  --dataset /workspace/calib_images --input_num 60 -o gesture_sg2000_cali

# 部署 cv181x cvimodel
/usr/local/bin/model_deploy.py \\
  --mlir gesture_sg2000.mlir \\
  --asymmetric --calibration_table gesture_sg2000_cali \\
  --chip cv181x --quantize INT8 \\
  --model gesture_sg2000_int8.cvimodel
""")
    print(f"[DONE] 训练+导出完成。目标 ONNX: {onnx_path}")
    print(f"       (输入规格: NCHW 1x3x{args.input}x{args.input}, 输出 1x{NUM_CLASSES})")


if __name__ == "__main__":
    main()
