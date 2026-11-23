# Milk-V Duo S (SG2000) 手势识别完整项目实录

> 一个从「官方死路」到「关键点手势识别稳定运行」的完整嵌入式 AI 项目复盘。
> 包含：**旧固件探索 → SD 卡固件移植 → 摄像头实时推流 → 7类手势分类 → 官方关键点手势识别 → 网络/SSH 打通** 全过程的代码、踩坑、解决与交付物。

## 项目目标

在 **Milk-V Duo S（SG2000, RISC-V CV181x, 512MB）** 上部署自定义手势识别：摄像头实时采集 → 手部检测 → 手势分类 → MJPEG/RTSP 推流，全程可在评估板实时运行。

**最终达成的能力**（关键点方案）：
- 实时检测手部 + 提取 21 个手部关键点 + 分类手势
- **number1/2/3/4/5 稳定识别**（对应官方 one/two/three/four/five）
- 全程推流可视化（手部框 + 关键点 + 手势标签）

## 里程碑时间线（核心故事线）

| # | 阶段 | 状态 |
|---|------|------|
| #1-9 | 官方分类 API 全死路（HandClassification=0109, Image_Classification=010B） | ❌ 死路 |
| #19-24 | dbr2 头文件 + 固件库打破 ABI 僵局，手检+RTSP 推流通 | ✅ |
| #42-44 | 手工移植新固件到 SD 卡（绕开 eMMC 烧录），WiFi 连通 | ✅ |
| #52-60 | GC2083 取帧 + MJPEG 软编码推流（绕开 VENC 内核 bug） | ✅ |
| #61-65 | model_factory 解 OpenModel 阻塞，手势识别完整闭环 | ✅ |
| #66-69 | 7类模型崩溃/恒输问题源码级定位 | 🔍 |
| #70 | USB 网络 + SSH 打通（`echo device > /proc/cviusb/otg_role`） | ✅ |
| #71-72 | TDL `convertPixelFormat` FP32 崩溃修复 + 裸 cviruntime 分类 | ✅ |
| #73-89 | **官方关键点手势识别**（det_hand → keypoint_hand → 9类分类），1-5 稳定 | ✅ |
| #92-115 | 换自训 **YOLOv8-pose** 模型；发现官方模型对「半握+遮挡」系统性失效（G1~G4 命中 0%） | ✅ |
| **#116-127** | **自训 pose 模型全链路打通**：量化 bug、ROI 裁剪、旋转对齐、640→480 转换、自适应 ROI | ✅ |
| **#128** | **定位挂墙多人场景失配**（远距离/任意位置失效）→ 需面向实际场景重训 | ⚠️ |
| **#129-143** | **本机全闭环**：自编译 eMMC 固件（duos emmc 板级 + 6.5GB DATA 自动扩容）烧录成功、collector 采集器重建（VENC 根因修复） | ✅ |
| **#144** | **8HD 屏花屏根因实锤并修复**：内核 mipi_tx 缺 u-boot `_cal_htt_extra` DSI 行周期补偿 ⇒ hfp 40→174/pixel_clk 80412 | ✅ |
| **#145** | **摄像头 30fps 预览 + 完整 UI 上屏流畅**：非缓存内存读写瓶颈（150ms→31ms/帧） | ✅ |

---

> **重要提醒（#128）**：技术链路（量化/裁剪/旋转/转换/部署/舵机管道）已全部打通，
> **近距离可用**（`bsc` 0.78~0.83，`G` 行输出，舵机跟随）；
> **远距离/手偏离 ROI 中心不可用**——根因是训练场景（单人/近距/居中）与
> 实际使用场景（**挂墙/多人/远距/任意位置/多光照**）失配，**需重训**。
> 详见 `docs/08-自训pose模型与挂墙场景.md`。

## 目录结构

```
├── README.md                  # 本文件（项目总览）
├── docs/                      # 核心经验文档（详细版）
│   ├── 01-旧固件死路与ABI.md
│   ├── 02-固件移植SD卡启动.md
│   ├── 03-摄像头取帧与推流.md
│   ├── 04-手势分类7类修复.md
│   ├── 05-关键点手势识别.md
│   ├── 06-网络与SSH打通.md
│   ├── 07-坑位速查表.md
│   └── 08-自训pose模型与挂墙场景.md   # ← 本阶段（量化bug/ROI/旋转/640→480/挂墙结论）
├── code/                      # 全部自研源码
│   ├── streaming/            # 推流程序（手检/分类/关键点）
│   ├── keypoint/             # 关键点手势识别
│   ├── utils/                # 模型探针/验证工具
│   ├── training/             # 训练/ONNX转换/校准脚本
│   └── build/                # 全部编译脚本
├── deploy/                    # 可部署产物
│   ├── models/               # cvimodel 模型（含自训 best_rot_480_int8_cv181x）
│   └── configs/              # model_factory.json 等
├── b_plan/                    # 本机 eMMC 固件路线（#129-#145，当前主线）
│   ├── 01_sdk_overlay/       #   自制固件全部 SDK 改动（8HD panel/eMMC 板级/automount；用法见其 README）
│   ├── 05_app/collector/     #   挂墙采集器源码（屏上 UI + 30fps 预览 + JPEG 落盘）
│   ├── 06_build/             #   构建/烧录/串口自动化脚本与全部构建日志
│   └── 07_deploy/            #   rootfs 修补脚本 + 大型产物 MANIFEST
├── timeline/                  # 验证日志（#0-#145 完整）
└── assets/                    # 截图/示意图
```

## 快速开始

### 硬件
- Milk-V Duo S（SG2000, CV181x RISC-V）
- GC2083 MIPI 摄像头
- USB-C 数据线（连开发机，供电+网络）、网线（可选）

### 环境
- 开发机：Linux，`riscv64-unknown-linux-musl` 交叉工具链（见 `docs/`）
- 评估板：musl 固件，root/milkv

### 编译一个推流程序
```bash
# 见 code/build/build_tdl_gesture_kpt_stream.sh（关键点手势识别版）
# 见 code/build/build_tdl_gesture_stream.sh（7类图像分类版）
bash code/build/build_tdl_gesture_kpt_stream.sh
```

### 部署
```bash
# 网络：USB 直连 / 网线直连见 docs/06
sshpass -p milkv scp build_out/gc2083/tdl_gesture_kpt_stream root@<board>:/mnt/data/
sshpass -p milkv ssh root@<board> 'LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd /mnt/data/tdl_gesture_kpt_stream 8881'
# 浏览器打开 http://<board>:8881/
```

## 核心经验（一句话）

1. **API 死路判定要源码级确认**：`CVI_TDL_HandClassification` 恒 `0109`，固件样例不过是没查返回值。
2. **ABI 错位是一切 VI 初始化失败的根源**：头文件版本必须匹配固件（`SAMPLE_VI_CONFIG_S=808` 等）→ dbr2_inc 解决。
3. **固件分类 API 对自定义 FP32 模型不可用**：7类模型输入必 FP32 → TDL `convertPixelFormat` 只认 INT8/UINT8 → 崩溃。修复见 `docs/04`。
4. **裸 cviruntime 绕过 TDL 预处理 bug**：手检用 TDL，分类/关键点用裸 cviruntime（已验证可靠）。
5. **官方关键点方案是手势识别正解**：21 关键点坐标不受 ROI 裁剪形态干扰 → 1-5 稳定。
6. **USB 网络"缺的不是模块是角色切换"**：`echo device > /proc/cviusb/otg_role`。
7. **【关键点正常基线】勿加 PWM 内核驱动**：加入 `CONFIG_PWM_CVITEK` 后，关键点模型推理会坍缩成中心"一团"（分类恒 none）。根因：PWM 与 TPU **共享 `clk_fpll` 父时钟**（cv181x_dtsi，devmem 实测 src_sel 均指向 fpll），dts 改 osc 隔离未奏效。恢复正常必须用**无 PWM 内核 boot.sd**（`deploy/boot.sd.normal`，md5 `1d54008c`）+ **repo 基线版程序**（`code/streaming/tdl_gesture_kpt_stream.c`，产物 md5 `75d256e0`）。舵机若需 PWM 请用 **GPIO 软 PWM 或外置 PCA9685**，勿重编内核。详见验证 #84-#89。

## 交付物清单

- ✅ 完整验证日志（#0-#128，`timeline/`）
- ✅ 运行中的推流程序源码（`code/streaming/`）
- ✅ 关键点手势识别（`code/keypoint/`）
- ✅ 训练 + ONNX→cvimodel 转换脚本（`code/training/`）
- ✅ 修复过的 TDL lib（`deploy/`，含 convertPixelFormat patch）
- ✅ **关键点正常基线 boot.sd**（`deploy/boot.sd.normal`，md5 `1d54008c`，无 PWM 内核）
- ✅ 网络/SSH 一键脚本
- ✅ 全部踩坑与绕过方案（`docs/`）

## 许可证

MIT（`LICENSE`）

## 致谢

Sophgo（TPU-MLIR, tdl_sdk, tdl_models）、Milk-V、相关开源社区。
