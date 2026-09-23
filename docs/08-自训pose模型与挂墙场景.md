# 08 - 自训 Pose 模型上线与挂墙场景结论

> 阶段：日志 **#92 – #128**（2026-09-18 ~ 09-23）
> 本文接续 `05-关键点手势识别.md`（官方模型阶段）。本阶段全面切换到
> **自训 YOLOv8-pose 模型**，打通完整实时链路，并最终定位到**场景失配**问题。

## 一、为什么换掉官方关键点模型

日志 #112 的定量分析（六手势 fa 日志）：

| 手势 | 帧数 | 该弯的指 median | 帧级判定分布 | 命中 |
|------|------|-----------------|--------------|------|
| G1 | 72 | 拇164/中166/小162 | 5:9, -1:63 | **0%** |
| G2 | 74 | 无172/小173 | 5:74 | **0%** |
| G3 | 88 | 小175 | 5:77, -1:11 | **0%** |
| G4 | 114 | 拇159 | 5:106, -1:8 | **0%** |
| G5 | 104 | — | 5:104 | 100% |
| G6 | 143 | 食100/中42 | 6:60, 1:40 | 42% |

**结论**：收起的指头 `fa` 中位数 162~176°，与真正伸直的 170+ **完全重叠**——
不是阈值问题，而是**被立指遮挡的手指 PIP/TIP 关键点被模型"猜到伸直位置"**
（持续性漂移，滤波救不了）。官方 `YOLOV8N_DET_HAND`（128×128）只对
"无遮挡全伸/全卷"可靠，半握 + 遮挡失效。**阈值调整无解 → 必须重训。**

## 二、自训模型规格与转换

**模型**：YOLOv8-pose，21 关键点 + 7 类手势
```
输入 1×3×640×640 → 输出 output0(1,74,8400)
74 = 4(box DFL 4×16=64 已在内部展开) + 7(cls) + 63(21 kpt × 3)
```
训练用 ultralytics 导出，C 端需要的是**裸头输出**（不带后处理），
因此需把中间张量 `440`(det_raw 71×4725) 与 `372`(kpt_raw 63×4725) 提升为图输出。

### 2.1 三个必须处理的 ONNX 坑

| 坑 | 现象 | 解决 |
|---|---|---|
| **Slice 缺 steps** | TPU-MLIR v1.3.228 报 `SliceAxisOp op operand #3 ... got 'none'` | 给 4 输入 Slice 补 `steps=[1]`（`fix_onnx_slice_steps.py`） |
| **anchors 数算错** | 只改 graph 声明会撞 `ReshapeOp::shape_inference: Assertion num_input == num_output` | 真 anchors = **4725**（60²+30²+15²，**单 anchor/cell**），不是 3-anchor 的 14175 |
| **网格常量仍按 640** | 同上 | 重建 5 处网格/步长常量，见 2.3 |

### 2.2 TPU-MLIR 转换三步（`build_rot480_cvimodel.sh`）

```bash
model_transform.py --model_name best_rot_480 --model_def best_rot_raw_fixed.onnx \
  --input_shapes '[[1,3,480,480]]' --mlir best_rot_480.mlir \
  --resize_dims 480,480 --keep_aspect_ratio --pad_value 114 \
  --mean 0,0,0 --scale 0.003921568627,0.003921568627,0.003921568627 \
  --pixel_format rgb --test_input <一张校准图> --test_result output_top.npz

run_calibration.py best_rot_480.mlir --dataset <校准图目录> --input_num 200 -o cali_table

model_deploy.py --mlir best_rot_480.mlir --quantize INT8 --quant_input \
  --calibration_table cali_table --chip cv181x --tolerance 0.85,0.45 \
  --model best_rot_480_int8_cv181x.cvimodel        # ← 是 --model，没有 -o
```

- 不加 `--asymmetric`（会撞 AvgPool assert）
- 加 `--quant_input`（C 端手工量化）、**不加** `--quant_output`（C 端按 float 读）
- 容器：`podman run --security-opt label=disable sophgo/tpuc_dev:v3.1`，
  挂载宿主机 `ai/tpu-mlir`（v1.3.228，与容器 `mlir_core` 匹配）

### 2.3 640 → 480 的正确做法（`make_rot_480_full.py`）

**只改 graph 声明不够**——YOLOv8 decode 的网格常量仍按 640 编码。必须重建
**全部 5 处**（用**形状**而非值来搜，否则会漏，因为它们值是 float 坐标）：

| 常量 | 形状 | 内容 |
|---|---|---|
| `Constant_264/266` | (1,2,8400) | 网格中心坐标（索引 + 0.5） |
| `Constant_284` | (2,8400) | 网格整数索引 |
| `Constant_272/286` | (1,8400) | stride 表 {8,16,32} |

生成逻辑（**先用 640 反推验证 5 项全匹配，再用于 480**）：
```python
y, x = np.meshgrid(np.arange(n), np.arange(n), indexing='ij')
grid = np.stack((x, y), 2).reshape(-1, 2)      # 顺序 P3(80²)→P4(40²)→P5(20²)
# 中心坐标 = grid + 0.5 ; stride = 按层填充
```
另需同步：`graph.input` 640→480、`graph.output` anchors 8400→4725、
2 个硬编码 anchors 的 Reshape shape 常量（`[1,4,16,8400]`、`[1,4,8400]`）。

**语义验证**（同一张图对比 640 原版与 480 新版）：
```
640 原始: top1 类别=4  置信=0.9090
480 新建: top1 类别=4  置信=0.8841     ← 类别一致、置信接近 ✓
```

> **收益**：以后任意 YOLOv8-pose 640 导出都能直接转 480，不必再依赖训练侧重新导出。

## 三、端到端链路打通的四个关键修复

### #116 输入量化 bug（**最重要**）

```c
- const float kf = 1.0f/(255.0f*qs);   /* 错: q = pixel/(255×127) → 全部取整为 0 */
+ const float kf = qs/255.0f;          /* 对: q = pixel/255×qscale ∈ [0,127] */
```

**实测**：错误公式下 **691200 个像素全部量化为 0**（非零数 0/691200）——
模型一直在看空白图，输出只由偏置决定。用同一帧的两种输入对比：

| 输入 | vs FP32 参考 (L1) | vs 全零参考 (L1) |
|---|---|---|
| A `pixel/255×127` | **0.5495**（接近 ✓） | 3.0556 |
| B 代码现状 | 2.7178 | **0.8084**（几乎一致 ✗） |

→ 这解释了长期未解的「**舵机一直抖但不跟随手势**」，而此前多轮（#96–#101）
都在调 EMA / 死区 / 稳定门控——**在错误的输入上做补救**。

> **教训**：怀疑精度问题前，先验证"模型实际收到了什么"。加一个量化统计
> （非零像素数 + max）一行代码就能看出来。

### #123 固定 ROI 裁剪

全图直送模型 `bsc≈0.005`；裁剪到手占满的窗口 → `bsc≈0.92`（12 帧 mean 0.8888）。

```c
/* 默认 (260,208,280) 来自实测手位(640x384 坐标系) */
static int g_roi_cx = 260, g_roi_cy = 208, g_roi_side = 280;
```

**坐标必须映射回全图**（否则推流的框/关键点位置全错）：
```c
kpt_out[m][0] = ((kx-px)/scale + g_roi_x) / full_w;
```

### #125 旋转朝向对齐

模型只见过**「手臂从左往右 / 手指朝右」**一种朝向。改用「从下往上」后：

| 指标 | 从下往上 | 按训练方向 |
|---|---|---|
| `G` 行输出 | G0, G5 | **G0,G1,G2,G3,G5** |
| `sc` 均值 | 0.52 | **0.81** |
| 低置信帧 | 41% | **8%** |

解决：`--rot 0|90|270`（ROI 是正方形 → 旋转后尺寸不变；框/关键点做**反向映射**）。
**并重训了带旋转增强的模型**（`best_rot.onnx`）。

### #127 自适应 ROI（多尺度 + 上帧记忆）

```c
static const int g_roi_cand[3] = {280, 170, 100};   /* 近 / 中 / 远 */
static int g_roi_idx = 0;                           /* 上帧成功的索引 */
/* 上帧成功的尺寸优先 → 达标早停；失败才依次试其它 */
```
距离稳定时 **1 次推理/帧**（帧率不掉），切换时最多 3 次。

## 四、挂墙多人场景的结论（未解决，需重训）

**实测**（日志 #128）：远距离时 `roi=100` 仍 `bsc≈0.0000`，`[ROI] 切换` 从未触发。

**根因：训练场景与使用场景整体失配**

| | 训练数据 | 实际使用（用户明确的最终形态） |
|---|---|---|
| 人数 | 单人 | **挂墙，多人上前互动** |
| 距离 | 近 | **远** |
| 位置 | 居中 | **任意（左中右 × 高低）** |
| 光照 | 单一 | **多样** |

**为什么软件侧无解**：
- ROI 越小，位置容错越低（ROI 100 → 仅 ±30px）
- TDL 手部检测框实测**偏移 83px**（#121），无法用于定位
- 位置×尺寸全扫描 3×3×3 = 27 次推理/帧 → 约 0.3fps
- 改 640×640 输入仅 **1.33 倍**线性增益，代价 **1.78 倍**延迟

**已评估并排除的方案**（勿重试）：全扫描、检测定位、640 输入。

## 五、下一步：面向实际场景重训

**采集清单**：

| 维度 | 要求 |
|---|---|
| 安装 | **按实际挂墙高度/角度采集**（勿用桌面手持——那正是失配来源） |
| 距离 | 0.5 / 1.0 / 1.5 / 2.0 m（重点覆盖远距离） |
| 位置 | 手在画面 **左/中/右 × 高/低**（**当前最大缺口**） |
| 人员 | **至少 3~5 人**，不同身高/手型/肤色 |
| 光照 | 白天 / 夜间开灯 / 侧光 |
| 朝向 | 朝上 / 朝右 / 斜向 |
| 数量 | 姿态 × 距离 × 位置组合，每类 50~100 张 |
| 训练增强 | Mosaic / Scale / 随机旋转 |

**目标**：模型对「手在画面任意位置 + 任意大小」鲁棒 → 推理端**可去掉 ROI**。

## 六、可复用资产

| 资产 | 用途 |
|---|---|
| `code/streaming/tdl_gesture_kpt_stream.c` | 主程序：ROI 裁剪 / 旋转 / 自适应尺度 / 管道 |
| `code/utils/make_rot_480_full.py` | **640→480 通用转换**（重建网格常量 + 语义验证） |
| `code/utils/make_rot_raw2.py` | 暴露 `det_raw`/`kpt_raw` 为图输出 |
| `code/utils/tdl_bbox_diag.c` | 手部检测框诊断（off/live/cascade） |
| `code/streaming/kpt_model_probe.c` | **不依赖摄像头**的模型加载/前向/布局探针 |
| `code/utils/analyze_run_log.py` | 运行日志分析（bsc 分布 / 检出率 / FA 对照） |
| `code/utils/test_distance_roi.py` | 距离-尺度响应实验 |
| `--dump` 开关 | 每 20 帧保存**实际送进模型的那张图**到 `/tmp`，离线诊断利器 |

## 七、本阶段新增的坑位

- **TDL/VI 反复启停会损坏状态**：`TDL_ReadImage` 先 segfault → 最终
  `VI_INIT 0xffffffff` + `ioctl SYS_ION_ALLOC failed`（**ION 池**耗尽，
  普通内存仍余 ~260MB）。**重启板子即恢复**；长时间单实例运行不受影响。
- **scp 不可用**（板子无 sftp-server）→ `cat file | ssh root@ip 'cat > /path'`
- `model_deploy.py` 用 `--chip` 与 `--model`（**没有 `-o` / `--processor`**）
- podman 必须 `--security-opt label=disable`（SELinux Enforcing，`:z` 会因
  仓库内大文件打标签失败）
- 量化的 `qscale` 语义：**归一化值 → INT8**，模板 max_abs≈1.0 → `qscale≈127`，
  所以 `q = 像素/255 × qscale`
