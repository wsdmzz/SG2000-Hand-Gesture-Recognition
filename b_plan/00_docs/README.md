# B 方案：集成新版 tdl_sdk 重编固件 —— 目录结构说明与操作指南

> 目标：通过重编评估板固件，把新版 sophgo `tdl_sdk`（新式 `TDL_*` API，含可用的手势分类
> `TDL_Classification`）集成进去，从根上解决旧固件 `CVI_TDL_*` 分类 API 不可用（0109/010b）的问题。
> 硬件：Milk-V Duo S（SG2000 / CV181x），现固件 `milkv-duos-musl-riscv64-sd_v2.0.1`。

---

## 0. 为什么需要 B 方案（背景）

评估板当前固件（v2.0.1）自带的旧版 TDL SDK（`libcvi_tdl.so`，`CVI_TDL_*` 命名）分类通道有缺陷：
- `CVI_TDL_HandClassification` → 恒 `0xC0010109`（VPSS_GET_FRAME）
- `CVI_TDL_Image_Classification` → 恒 `0xC001010B`（NOT_YET_INITIALIZED）
- duo-tdl-examples 预编译 `.so` 与固件 ABI 不匹配；cviruntime 与 TDL 同进程共跑又冲突

**B 方案 = 用 duo-buildroot-sdk-v2 完整重编固件，并把 sophgo 上游 tdl_sdk（新版 `TDL_*`，
支持 CV181X/SG2000，sample_classification 手势分类完整）集成进去**，使固件自带可用的分类 API。

---

## 1. 目录结构（b_plan/）

```
b_plan/
├── 00_docs/          # B 方案文档：本 README + b_plan_troubleshooting.md（排查指南）
├── 01_sdk/           # duo-buildroot-sdk-v2（固件完整 Buildroot SDK，git clone）
│                      #   - 包含 buildroot/、cvi_mpi/、cviruntime/、cvikernel/ 等基础库源码
│                      #   - 重编产出固件镜像（SD 卡 .img）
├── 02_tdl_sdk/       # sophgo/tdl_sdk 上游（新版 TDL_* API 源码）
│                      #   - sample/（sample_classification.c 手势分类示例）
│                      #   - src/ include/（TDL SDK 库代码）
│                      #   - 需集成进 01_sdk 一起编译（见第 4 节）
├── 03_tpu/           # TPU 开发工具：tpu-mlir + tpu-sdk
│                      #   - tpu-mlir：模型(ONNX/PyTorch) → cvimodel 转换
│                      #   - tpu-sdk：cviruntime 运行时 + 模型验证（评估板用）
├── 04_toolchain/     # RISC-V 交叉编译工具链（riscv64-linux-musl）
│                      #   - 现有可直接复用：../ai/host-tools/gcc/riscv64-linux-musl-x86_64/
├── 05_app/           # 自制应用源码（新版 TDL_* API 改写）
│                      #   - 手势检测+分类+RTSP 推流（对应旧版 sample_vi_gesture_rtsp）
├── 06_build/         # 编译中间产物、临时文件、日志
├── 07_deploy/        # 部署脚本 + 编译出的固件镜像
```

---

## 2. 关键目录/概念对应关系（排查时对照）

| 概念 | 对应目录/文件 | 说明 |
|------|--------------|------|
| 交叉编译工具链 | `04_toolchain/` 或 `../ai/host-tools/gcc/riscv64-linux-musl-x86_64/` | `riscv64-unknown-linux-musl-gcc` |
| 固件构建（Buildroot） | `01_sdk/`（duo-buildroot-sdk-v2） | `make` 产出 SD 镜像 |
| TDL SDK（AI 推理库） | `02_tdl_sdk/`（sophgo/tdl_sdk） | `TDL_*` API，含手势分类 |
| 底层媒体库（VI/VPSS/ISP） | `01_sdk/cvi_mpi/` | libsample/libsys/libvi 等 |
| TPU 推理运行时 | `03_tpu/tpu-sdk`（cviruntime/libcviruntime.so） | `CVI_NN_*` API，在板上跑模型 |
| 模型文件 | `../ai/models/*.cvimodel` | TPU-MLIR 转换产物 |
| 应用源码 | `05_app/` | 新版 API 改写的手势识别程序 |

**链路串起来**：应用(`05_app`) → TDL SDK(`02_tdl_sdk`/`libcvi_tdl.so`) → cviruntime(`03_tpu`) → TPU(=评估板硬件)
底层媒体(VI/VPSS) → cvi_mpi(`01_sdk`) → 固件
全部**同一次编译，ABI 完全一致** —— 这正是 B 方案解决的根因。

---

## 3. B 方案实施步骤（总览）

1. **拉取 duo-buildroot-sdk-v2** → `01_sdk/`（`git clone https://github.com/milkv-duo/duo-buildroot-sdk-v2.git`）
2. **拉取新版 tdl_sdk** → `02_tdl_sdk/`（`git clone https://github.com/sophgo/tdl_sdk.git`）
3. **确认 SG2000 平台标识**：用 `CV181X`（SG2000 属 CV181x 系列），TDK 的 CMakeLists 已支持
4. **集成 tdl_sdk 到 buildroot**：把 tdl_sdk 源码放进 duo-buildroot-sdk-v2 对应的 package/源码目录，配置 buildroot 使其编译 TDL 库（`libcvi_tdl.so`）——
   具体方式需看 duo-buildroot-sdk-v2 是否内置 tdl_sdk 集成（若内置则替换/升级源码；若不内置则需新增 package）
5. **配置 toolchain**（`04_toolchain/`）+ 首次编译 buildroot（数小时），产出固件镜像 → `06_build/`、`07_deploy/`
6. **烧录新固件**：用编译出的 `.img` 重烧评估板 SD 卡（有变砖风险，需保留原固件备份）
7. **应用改写**：把旧 `CVI_TDL_*` 程序改写为 `TDL_*`（参考 `02_tdl_sdk/sample/c/sample_classification.c` 手势分类用法），放入 `05_app/`
8. **部署验证**：手势检测+分类+RTSP 推流在新固件上跑通

---

## 4. 自查/排查指南（面向你）

遇到问题时按"链路"逐层排查（对应目录）：

1. **编译失败**：
   - 看 `06_build/` 的编译日志
   - 确认 toolchain 在 `04_toolchain/`、平台宏 `-DCV181X -D__CV181X__`
   - tdl_sdk 编译错误 → 查 `02_tdl_sdk` 的 CMakeLists 平台分支
2. **固件起不来/变砖**：
   - 确认 `07_deploy/` 里有原厂固件备份（`../ai/milkv-duos-musl-riscv64-sd_v2.0.1.img` 保留）
   - 烧录前先核对 md5
3. **新固件分类 API 仍报错**：
   - 用 `nm -D libcvi_tdl.so` 确认是否导出 `TDL_Classification`（新 API）
   - 若仍是旧 `CVI_TDL_*` → 说明 tdl_sdk 没成功升级，回到第 4 步集成检查
4. **应用 API 不对**：
   - 对照 `02_tdl_sdk/sample/c/sample_classification.c` 的标准 `TDL_*` 用法改写
   - 手势模型用 `TDL_MODEL_CLS_HAND_GESTURE`（0拳头/1五指/2无/3二）

**参考已验证事实（当前旧链路）**：见 `b_backup/docs/SG2000手势识别验证日志.md` 与
`SG2000手势检测推流实现指南.md`（手部检测+RTSP 推流 #24 里程碑已可用，B 方案即使失败可回退）。

---

## 5. 里程碑备份（可回退）

当前已验证成果已备份到 `b_backup/`：
- `b_backup/milestone_det/`：手部检测推流程序（sample_vi_gesture_rtsp.c）+ 分类封装（gesture_cls.c/.h）
- `b_backup/milestone_det/scripts/`：编译脚本 + atomic_shim
- `b_backup/docs/`：验证日志 + 实现指南
- `b_backup/raw/`：探针/测试程序

B 方案失败需回退时，直接复用 `b_backup/milestone_det/` 的源码 + 原固件镜像。

---

## 6. 阶段进展记录（持续更新）

### 2026-08-06：找到算能官方完整 SG200x SDK（决定性突破）
- ✅ **`sophgo/sophpi` 仓库 `sg200x-evb` 分支 = 算能官方 SG200x 完整 SDK**（含全套 buildroot + 固件构建链）
- ✅ 编译目标 **`sg2002_wevb_riscv64_sd`**（对应评估板 SG2002 + RISC-V 64 位 + SD 卡）
- ✅ 通过 `scripts/subtree.xml`（manifest）锁定全部配套组件版本，同一版本统一集成：
  `tdl_sdk`(revision=master，新版 `TDL_*` API) + `cvi_mpi` + `cviruntime` + `cvikernel` + `cvimath` + `cvi_rtsp` + `ive` + `linux_5.10` + `u-boot` + `fsbl` + `buildroot-2021.05` + `host-tools`
- ✅ 官方拉取步骤：
  ```bash
  git clone -b sg200x-evb https://github.com/sophgo/sophpi.git
  ./sophpi/scripts/repo_clone.sh --gitclone sophpi/scripts/subtree.xml   # 拉全组件（用 manifest）
  ```
- ⚠️ `repo_clone.sh` 默认用 `git@github.com:`（SSH）拉取——需 GitHub SSH key，或用 HTTPS 改配置
- ⚠️ SDK 体量极大（GB 级），编译需数小时
- **结论**：B 方案正确实现 = 用 `sophpi`（sg200x-evb）跑 `sg2002_wevb_riscv64_sd`，固件内即为**算能官方新版 tdl_sdk（`TDL_*` 分类 API，含 `TDL_Classification` 手势分类）**，与底层库全配套，彻底解决旧 `CVI_TDL_*` 分类不可用（0109/010b）问题

### 2026-08-06：确认 duo-buildroot-sdk-v2 内置 tdl_sdk 是旧版（B 方案起点）
- ✅ `01_sdk/`（duo-buildroot-sdk-v2）已拉取（157540 文件，浅克隆），含完整 buildroot + cvi_mpi/cviruntime/cvikernel 等
- ⚠️ **该 SDK 内置的 `tdl_sdk/` 是旧版 `CVI_TDL_*` API**（`cvi_tdl_core.h` 含 `CVI_TDL_Image_Classification`/`HandClassification`，与评估板固件同源）——**不是** sophgo 新版 `TDL_*`
- ⚠️ 因此**不能**在 duo-buildroot-sdk-v2 上硬塞 sophgo 新版 tdl_sdk（API 体系不适配）；**正确路径见上一条（sophpi sg200x-evb）**
- 该 SDK 保留作参考 / 或后续对比，不作为 B 方案主路径

**自查思路**：遇到问题时先确认「用的是哪个 SDK / 哪个 tdl_sdk 版本」，对照本 README 第 2 节链路逐层定位。

### 2026-08-06（续3）：sophpi 全组件拉取完成 + tdl_sdk 新版确认（决定性）
- ✅ `b_plan/01_sdk/sophpi/` 完成 `repo_clone`（subtree.xml，HTTPS），**23 个组件全部拉取成功**：`tdl_sdk`/`cvi_mpi`/`cviruntime`/`cvikernel`/`cvimath`/`cvi_rtsp`/`linux_5.10`/`buildroot-2021.05`/`u-boot`/`ive`/`osdrv`/`ramdisk`/`host-tools` 等
- ✅ **`tdl_sdk` 确认为 sophgo 新版 `TDL_*` API**（git 2026-06-30，头文件 `include/c_apis/tdl_sdk.h`，含 `TDL_CreateHandle`/`TDL_Classification`，有完整 `src/` 实现）
- ✅ 这验证了 B 方案有效性：**重编固件后评估板将带可用 `TDL_Classification` 手势分类**，解决旧 `CVI_TDL_*` 分类不可用
- 🔄 下一步：`source build/envsetup_soc.sh` → `defconfig sg2002_wevb_riscv64_sd` → `build_all`（编译固件，数小时）

### 2026-08-06（续4）：固件编译已启动
- ✅ `defconfig sg2002_wevb_riscv64_sd` 配置成功（PROJECT=sg2002_wevb_riscv64_sd, CHIP_ARCH=CV181X, SDK=musl_riscv64, 分区=partition_sd.xml, 输出=install/soc_sg2002_wevb_riscv64_sd）
- 🔄 `build_all` 已在后台启动（build_uboot 阶段正常，日志 `b_plan/06_build/fw_build.log`）
- 编译顺序：build_uboot → build_kernel → build_ramboot → build_osdrv → ...（完整固件，数小时）
  - 完成后固件镜像在 `install/soc_sg2002_wevb_riscv64_sd/`（拷贝到 `b_plan/07_deploy/`）+ 烧录评估板

### 2026-08-07：host 包装链编译障碍（Fedora 42 vs 老 buildroot 系统性不匹配）
- **现象**：`build_all` 编到 `br-rootfs-pack` 阶段持续失败，原因是 **buildroot 的 host 工具链逐个下载/编译失败**，且每次只暴露下一个包
- **已排雷修复的 host 包**（全部通过）：pkgconf / zlib / util-linux / e2fsprogs / attr / acl / m4 / libtool / autoconf / automake
- **两类问题**：
  1. **下载失败**：buildroot 的 wget 在此机器+源上失败（`sources.buildroot.net`/kernel.org 等 curl 可下但 wget 超时/证书）→ 用 `curl -skL` 手动下载到 `buildroot-2021.05/dl/`（且必须核对 `package/<pkg>/<pkg>.hash` 的 sha256 完全匹配，否则 buildroot 拒绝）
  2. **编译失败（C23）**：Fedora GCC 默认 `-std=c23`，老 host 包源码（`typedef int bool` 等）与之冲突 → 打 patch / 改源码：
     - `util-linux`：pidfd-send-sig inline 兜底（patch 在 `package/util-linux/0001-fix-pidfd-fedora.patch`）
     - `e2fsprogs`：`lib/ext2fs/tdb.c` 的 `typedef int bool` → `#include <stdbool.h>`（patch 在 `package/e2fsprogs/0001-fix-tdb-bool-c23.patch`）
- **当前卡点**：`host-fakeroot-1.25.3` 编译失败（多个旧源码 bug：`config.h:271 unknown type 'unknown'`、`wrapstruct.h next_setgroups undeclared`、`send_get_stat64` 指针类型不兼容）——**configure 探测阶段已出错，生成的 config.h 损坏**，非简单源码 bug
- **评估**：sophpi 官方在旧 Ubuntu 编译，Fedora 42（GCC 15 + 新 glibc）与 buildroot-2021.05 老 host 包**系统性不兼容**；即便修完 fakeroot，后续 bison/flex/jsmin 等 host 包大概率还有同类深坑
- **可选方向**：
  1. **换编译容器**：用配套旧 Ubuntu（如 20.04/22.04，GCC 10/11 C11 默认）编译，从根上避免 C23/新 glibc 兼容问题（推荐，但需 docker/容器环境）
  2. 继续逐包硬修（高往返、不确定）
- **状态**：`b_plan/06_build/fw_build_sg2000_duos*.log`（依次累进），当前卡 fakeroot

### 2026-08-07：固件编译完整成功 + 发现 TDL 库未入固件（关键）
- ✅ **`build_all` 编译完整成功**：u-boot/内核/rootfs/data 分区全部打包完成，产出固件：
  `install/soc_sg2002_wevb_riscv64_sd/` 下 `boot.sd`(2.9MB)/`rootfs.sd`(14MB)/`data.sd`(5.5MB)/**`upgrade.zip`(4.3MB)**
- ✅ 编译过程环境适配已修复（记录在排查指南）：KERNEL_INC(`mpi_param.mk`)→指向内核usr/include；全局 `-Werror` 移除；flatbuffers host `-Wno-error=class-memaccess`；isp_daemon_tool `-Wl,--allow-shlib-undefined`；Fedora 缺 `libpcre.so.3` → 本地 `.host_libs/libpcre.so.3→libpcre.so.1` 软链 + `LD_LIBRARY_PATH`
- ⚠️ **关键发现**：新版 tdl_sdk 编译产出 **`libtdl_core.so`/`libtdl_ex.so`（非旧 `libcvi_tdl.so`，API 为 `TDL_*`）**，但**未打进固件 rootfs**（`/mnt/system/lib` 里没有）——tdl_sdk 库安装到固件的环节缺失/未生效
- ⏭️ 下一步：
  1. 确认固件如何装 TDL 库（tdl_sdk install → 固件根文件系统，或 buildroot package）
  2. 把 `libtdl_core.so` 等 + 新版 tdl_sdk 应用装进固件
  3. 验证新固件里 `TDL_Classification`（手势分类）可用
  4. 烧录评估板验证（保留原固件镜像 `../ai/milkv-duos-musl-riscv64-sd_v2.0.1.img` 作回退）

### 2026-08-07（续）：方案2——手动并入 TDL 库
- **方案1（固件自动装 TDL 库）确认无此环节**：`build_tdl_sdk` 只把库装到 `tdl_sdk/install/CV181X/lib/`，固件打包流程没有"把 tdl_sdk 库拷进 `/mnt/system/lib`"的默认配置（旧 Milk-V 固件的 libcvi_tdl.so 是 Milk-V buildroot 自己集成的，sophpi 这套不含）
- ✅ **方案2 可行**：`libtdl_core.so`（手势分类核心库）依赖 `libcnpy/libcvikernel/libcviruntime/libcvi_ive/libopencv_core.4.5/libz/libstdc++` —— **固件 rootfs 里全部已具备**（`/mnt/system/lib` 有 opencv4.5/cvikernel/cviruntime/cnpy；`/mnt/system/usr/lib` 有 libcvi_ive）
- ✅ `libtdl_ex.so`（音频扩展，依赖 websockets/ssl/VoiceEngine 等）**不需要**（手势分类只用 libtdl_core.so）
- ▶️ 操作：把 `libtdl_core.so` 复制进 `rootfs/mnt/system/lib/` → 重新打包固件（pack_system_sd）→ 烧录验证

### 2026-08-07（续2）：固件含 TDL 库打包完成（里程碑）
- ✅ **执行方案2**：`libtdl_core.so`(1.9MB) 复制进 `rootfs/mnt/system/lib/`；重跑 `build_all`（增量，u-boot/内核/中间件缓存）→ rootfs 从 14.13MB 增到 14.73MB（TDL 库并入成功）
- ✅ **产出完整固件**：`install/soc_sg2002_wevb_riscv64_sd/` 下 `upgrade.zip` + `rawimages/boot.sd`/`rootfs.sd`/`data.sd`
- 🔄 **下一步（高风险）**：烧录到评估板验证：
  1. 备份原厂固件 `../ai/milkv-duos-musl-riscv64-sd_v2.0.1.img`（回退用）
  2. 确认烧录方式（SD 卡/工具），烧录新固件
  3. 启动后确认：boot 正常 + 新版 `libtdl_core.so`（`nm -D | grep TDL_Classification`）+ GC2083 摄像头 + WiFi
  4. 验证 `TDL_Classification` 手势分类可用

### 2026-08-07（续3）：确认评估板从 eMMC 启动，SD 卡烧录对启动无效（关键发现）
- ✅ 评估板型号：**Milk-V DuoS**；固件：Buildroot 2021.05（旧 Milk-V）——SSH host key 变化因换系统，但实际仍是 eMMC 旧固件
- **关键**：评估板从**内置 eMMC（`/dev/mmcblk0`）启动**，内核 `cmdline: root=/dev/mmcblk0p4 blkdevparts=mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS);mmcblk0boot0:1M(fip),1M(fip_bak)` —— 分区布局在 eMMC 上（BOOT 8M/MISC 2M/ENV 128K/ROOTFS 768M）
- **教训**：用 sgdisk 烧录的 **SD 卡（mmcblk1）分区表未被评估板用作启动介质**，SD 卡上的新固件没有生效
- 🔄 **下一步（需决策/高危）**：
  1. 确认 DuoS 是否支持**从 SD 卡强制启动**（U-Boot 启动源选择）→ 若支持，用 SD 卡验证新固件（安全可回退）
  2. 或**烧写内置 eMMC（mmcblk0）**（BOOT/MISC/ENV/ROOTFS 分区）→ 高危，写坏可能变砖，需确认正确的 SOPHGO 烧写方式与回退手段
  3. 或确认 DuoS 是否有 USB 烧录模式（`cvitek_usbburn`/`fip` 方式）官方升级固件

### 2026-08-07（续4）：串口日志确认 DuoS 固定从 eMMC 启动（SD 卡不参与启动）
- **串口日志关键行**（milkv-duo 串口 /ttyUSB0,115200）：
  - `U-Boot 2021.10 (Aug 01 2024) cvitek_cv181x`；`MMC: cv-emmc@4300000(0), cv-sd@4310000(1), wifi-sd(2)`
  - `Using 'config-cv1813h_milkv_duos_emmc' configuration`（FIT 内核用 eMMC 配置）
  - 内核 `cmdline: root=/dev/mmcblk0p4 blkdevparts=mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS)`；`mount /dev/mmcblk0p1 on /boot failed`
  - `mmcblk1(p1)` = SD 卡被识别但不是启动介质
- **结论**：**DuoS 出厂固化为从内置 eMMC（mmcblk0）固定启动**，内核 root=/dev/mmcblk0p4（eMMC ROOTFS），**SD 卡（mmcblk1）不参与启动**——因此烧录 SD 卡无法让新固件生效
- 🔄 **下一步（必须烧 eMMC）**：
  1. DuoS 官方 USB 烧写（Burntool/cvitek_usbburn，接 USB+BOOT 键）烧 eMMC — 需确认官方工具/镜像格式（sophpi 输出 upgrade.zip + boot.sd/rootfs.sd/data.sd，需按 DuoS eMMC 的 blkdevparts 布局转换）
  2. 或 U-Boot 环境改启动源到 SD 卡（需能进 U-Boot：autoboot 0s，需按住键+上电）
  3. 或确认 DuoS eMMC 裸烧（dd 按 blkdevparts 写 mmcblk0 的 BOOT/MISC/ENV/ROOTFS）
- **回退保障**：原厂 eMMC 未动（新固件未烧写前，评估板仍可启动旧固件）# flash memory 'fip_bak' is 'not found'..# Flash image: "tthreads" not found "espi_boot" not defined" not defined

---

### 2026-08-07（续5）：sophpi 支持 SG2000 —— 修正芯片兼容性判断（重要）
- ✅ **修正此前错误判断**：sophpi (sg200x-evb) **支持 SG2000**（`build/boards/chip_list.json`：`cv181x` 芯片族明确含 `sg2000`、`sg2002`；也含 `cv1800b` → CV1800B 也支持）
- ✅ sophpi `build/boards/cv181x/` 有专门板卡：**`sg2000_wevb_riscv64_sd`**（SG2000 SDK 板，riscv64 SD 卡）及 `cv1812cp_sophpi_duo_sd`（Duo 板型）
- ⚠️ 关键注意：`sg2000_wevb` 是 sophgo 自家评估板板型，**不是 Milk-V Duo S 板型**——需确认其 dts（GC2083 摄像头/USB-NCM/WiFi 外设节点）能否适配 Milk-V Duo S 硬件
- ✅ **候选最优路径**：sophpi `sg2000_wevb_riscv64_sd`（芯片 SG2000 匹配 + 新版 tdl_sdk `TDL_*` + 官方配套新链）——若 dts 适配 Duo S，则是从根上解决"可用手势分类"的方案
- 🔄 待办：检查 sophpi sg2000 板型的 dts 外设配置（GC2083/USB-NCM）是否匹配 Milk-V Duo S；若匹配则用该目标重编固件验证

### 2026-08-07（续6）：移植源已确认 —— duo-buildroot-sdk-v2 有完整 Duo S 板型
- ✅ **移植源**：duo-buildroot-sdk-v2 里已有 **`sg2000_milkv_duos_musl_riscv64_sd`** 板型（Milk-V Duo S 官方板型）：
  `b_plan/01_sdk/build/boards/cv181x/sg2000_milkv_duos_musl_riscv64_sd/`
  - `dts_riscv/sg2000_milkv_duos_musl_riscv64_sd.dts`：model="Milk-V DuoS"、sensor reset、MIPI/I2C/SPI、MIPI DSI 屏、GT911 触摸
  - include 的公共 dtsi（`cv181x_base_riscv`/`asic_bga`/`asic_sd`）与 sophpi 的 `sg2000_wevb` **完全相同** → 两套 SDK 同构，移植可行
  - `linux/`、`u-boot/`、`partition/`、`memmap.py`、`rootfs_script/` 齐全
- ✅ **GC2083 传感器驱动**：duo-buildroot-sdk-v2 的 cvi_mpi 已有 `cvi_mpi/component/isp/sensor/{cv180x,sg200x}/gcore_gc2083/`（两边架构都有）
- ✅ duo-buildroot-sdk-v2 的 tdl_sdk 确认**旧版 `CVI_TDL_*`**（头文件 `cvi_tdl_core.h`，无新版 `c_apis/tdl_sdk.h`）
- ▶️ **移植方案**：把 duo-buildroot-sdk-v2 的 `sg2000_milkv_duos_*` 板级文件复制到 sophpi `build/boards/cv181x/`，用 sophpi 的**新版 tdl_sdk** 重编 → 同时获得 Duo S 硬件适配 + `TDL_Classification` 手势分类
- 🔄 待办：确认 sophpi 侧 GC2083/sg2000 支持现状，然后执行板型复制与适配

### 2026-08-07（续7）：基础问题确认 + GC2083 tuning bin 关键发现
初步排查（sophio 默认各板型现状）：
- **sensor tuning bin 注意**：sophpi 与 duo-buildroot-sdk-v2 的 `isp_tuning/` 资源里**都只有 `gcore_gc4653` 等 tuning bin，没有 `gcore_gc2083` 的 sdr.bin**。但官方 v2.0.1 固件里 GC2083 摄像头能正常工作（sensor 驱动`libsns_gc2083.so` + 默认 ISP 参数即可，不强制专属 tuning bin）——GC2083 摄像头在新固件应可正常出画面，只是无专属 AE/AWB 优化。
- **cvi_mpi sensor 驱动**：sophpi 有 `cvi_mpi/component/isp/sensor/cv182x/gcore_gc2083/`（driver 全），duo-buildroot-sdk-v2 有 `{cv180x,sg200x,cv181x,cv182x}/gcore_gc2083`。
- **板型移植已执行**：sophpi `defconfig sg2000_milkv_duos_musl_riscv64_sd` 已成功识别 Milk-V Duo S 板型（PROJECT/CHIP_ARCH=CV181X/musl_riscv64/riscv64 工具链/partition_sd.xml），`CONFIG_SENSOR_GCORE_GC2083=y` 已使能。
### 2026-08-07（续8）：Milk-V Duo S (SG2000) 固件编译完整成功 + 新版 TDL_* 库入固件（里程碑）
- ✅ **`build_all` BUILD_ALL_EXIT=0 完整成功**（`fw_build_sg2000_duos33-54.log`），产出：
  `boot.sd`(3.4MB) + `ramboot.itb` + `fw_payload_uboot.bin`(2.6MB) + `rootfs.sd`(209MB) + `data.sd`(5.4MB) + `upgrade.zip`(34MB)
- ✅ **固件 `/mnt/system/lib/libtdl_core.so`**（1.9MB，从 tdl_sdk/install 手动并入）**导出 90 个 `TDL_*` 符号**，含 `TDL_Classification`/`TDL_CreateHandle`/`TDL_OpenModel`/`TDL_ReadImage`——新版分类 API 确认
- ✅ 补入固件的依赖：`libstdc++.so.6.0.28` + `libgcc_s.so.1`（TDL 依赖的 C++ 运行时，buildroot rootfs 原本无，从工具链 sysroot 拷入 + symlink）
- **编译排雷全清单**（Fedora 42 vs buildroot-2021.05 老工具链系统性适配，详见验证日志 #35）：
  1. u-boot dts：`build/boards/default/dts/cv181x/cv181x_base.dtsi:671` wifi_pin 加 label + 删 dtb 缓存（u-boot/arch 内 dtsi 是构建产物，改源头）
  2. cvi_mpi install 缺失文件 cp 加存在判断（sample_dsi/cipher/cvg/venc/self_test/mipi_tx/pqtool_definition.json）
  3. buildroot host 包下载失败（wget 失败，curl 可下）→ 手动下载到 `buildroot-2021.05/dl` + 严格核对 hash：pkgconf/zlib/util-linux/e2fsprogs/attr/acl/m4/libtool/autoconf/automake/fakeroot/ncurses(35补丁)/kmod/busybox/evtest/gdb/json-c/bison/strace/wpa_supplicant(+2补丁)/xz/flex/libnl/patchelf/gettext-tiny(+gettext)
  4. C23 兼容：`package/Makefile.in` 的 `HOST_CFLAGS` 统一加 `-std=gnu99`（一次解决 util-linux/e2fsprogs/fakeroot/bison 等 host 包）；fakeroot 需 config.h 类型宏 + `-Wno-incompatible-pointer-types`
  5. host 工具：`make_ext4fs` 需 `libpcre.so.3` → `.host_libs/libpcre.so.3→libpcre.so.1` 软链 + `LD_LIBRARY_PATH`（不能含空值/CWD，buildroot 依赖检查拒绝）
- ⏭️ **下一步（部署）**：
  1. 镜像拷入 `b_plan/07_deploy/`，确认评估板连接方式（串口/USB）
  2. 烧录新固件到评估板（注意 Duo S 从 eMMC 启动，SD 卡烧录无效——需确认烧录法）
  3. 启动后确认 `/mnt/system/lib/libtdl_core.so` 可用
  4. 写 `TDL_Classification` 手势分类演示（新版 API，参考 `tdl_sdk/sample/c/sample_classification.cpp`）
  5. 验证摄像头 → VI/VPSS → `TDL_Classification` 实时分类
