# 01 - 旧固件死路与 ABI 错位（#0-#25）

## 结论先行

评估板旧固件（`milkv-duo-musl-riscv64-sd_v2.0.1`）的**手势分类 API 全部是死路**，且官方示例程序根本不检查返回值。真正突破是**换用固件配套的 duo-buildroot-sdk-v2 头文件 + 固件自带的 libsample.so**，并在实时通道上改用手部检测（`CVI_TDL_Detection`）绕开分类死路。

## 一、官方分类 API 死路分析（源码级证据）

### 1. CVI_TDL_HandClassification（手势分类）

- 实测恒返回 `0xC0010109`，即 **VPSS_GET_FRAME** 错误。
- **关键源码证据**：反汇编官方 `sample_img_hand_cls` 二进制，地址 `0x11070` 处调用后**不检查返回值(a0)**，直接读 `meta.info[0].name` 打印 "(空)"。官方样例把同一个 0109 悄然吞掉。
- 结论：这个 API 在固件上**从未真正工作**，是官方样例"假成功"造成的误导。

### 2. CVI_TDL_Image_Classification（图像分类）

- OpenModel 成功，推理返回 `0xC001010B`（**NOT_YET_INITIALIZED**）。
- 即使改 SkipVpssPreprocess / SetPreParam / CreateHandle vs CreateHandle2 各组合，均无效。
- RAW_IMAGE_CLASSIFICATION 返回 `0xC0010102`（open fail）。

**源码级结论**：这两个分类 API 都依赖完整媒体/VPSS 框架初始化，且固件版对自定义 FP32 输入模型存在系统性缺陷（见 docs/04、docs/05）。官方 `CVI_TDL_HandClassification` 是**确凿死路**。

## 二、ABI 错位的完整故事

### 1. 症状

用 `ai/duo-tdl-examples` 头文件编译的程序运行时报：
- `SAMPLE_TDL_Get_VI_Config` 报告 `s32WorkingViNum <= 0`（sensor 能解析但 VI 数量没填）
- 或 VI 初始化报 `0xffffffff`

### 2. 根因：结构体尺寸不匹配

- duo-tdl-examples：`SAMPLE_VI_INFO_S = 152`、`SAMPLE_VI_CONFIG_S = 788`
- 固件底层库：`SAMPLE_VI_INFO_S = 156`、`SAMPLE_VI_CONFIG_S = 808`

一个结构体字节数不对，导致整个 VI 初始化传参错位——这是 RISC-V 嵌入式交叉编译最常见的坑：**头文件版本必须与板上运行的库完全一致**。

### 3. 打破僵局（关键）

从 `https://github.com/milkv-duo/duo-buildroot-sdk-v2`（与固件配套）获取 `cvi_mpi/include/` 头文件与 `cvi_mpi/sample/common/*.c`。关键尺寸（与固件反汇编完全匹配）：

- `SAMPLE_VI_INFO_S = 156`
- `SAMPLE_VI_CONFIG_S = 808`
- `SAMPLE_INI_CFG_S = 232`

这就是本工程 `dbr2_inc/`、`dbr2_mw/` 的由来。

## 三、绕开分类死路：改为手部检测

```
CVI_TDL_OpenModel(handle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, model_path);  // yolov8n_det_hand
CVI_TDL_SetModelThreshold(handle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, 0.3);
每帧: CVI_TDL_Detection(handle, &frame, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, &obj);  // 手部 bbox
```

手部检测在实时通道（VI→VPSS→检测）上完整推理成功，为后续手势分类/关键点打基础。

## 四、给复现者的话

1. **API 死路判定必须源码级确认**——官方样例不检查返回值不代表 API 可用。
2. **头文件版本必须与板上固件库一致**——用配套 SDK（duo-buildroot-sdk-v2）。
3. **官方 CVI_TDL_HandClassification 是死路**，别再花时间。
4. 完整过程见 `timeline/验证日志-完整版.md`（#0-#25）。

## 关键文件

| 文件 | 说明 |
|------|------|
| dbr2_inc/ | duo-buildroot-sdk-v2 头文件（固件 ABI 匹配） |
| dbr2_mw/ | sample_common 源码（匹配固件） |
| code/streaming/sample_vi_gesture_rtsp.c | 手检 + RTSP 推流 |
| code/utils/atomic_shim.c | C906 无 A 扩展的 atomic 软件实现 |
