# 01_sdk_overlay —— 自制 eMMC 固件的 SDK 改动全集

> 本目录是 `b_plan/01_sdk/`（duo-buildroot-sdk-v2 + sophpi vendor 树，约 29GB，不进仓库）
> 中**我们新增/修改**的源文件子集，保持原相对路径。复现固件的方法：
> 1. 按下表获取基线 SDK；
> 2. 把本目录内容覆盖回 SDK 树（路径一一对应）；
> 3. `defconfig sg2000_milkv_duos_musl_riscv64_emmc && build_all`（详见验证日志 #131–#136）。

## 基线来源

| 部分 | 来源 | 说明 |
|---|---|---|
| `build/`、`buildroot/` 等顶层 | `github.com/milkv-duo/duo-buildroot-sdk-v2`（clone，main 分支，commit 6f8962c39） | git 跟踪部分 |
| `sophpi/` | 算能 sophpi vendor 树 tarball（SDK 内解包，git 不跟踪） | 24GB，含 cvi_mpi/linux_5.10/u-boot-2021.10/buildroot-2021.05 |
| 8HD panel 头 | `milkv-duo/duo-buildroot-sdk@develop` 的 `u-boot-2021.10/include/cvitek/cvi_panels/dsi_milkv_8hd.h` | 移植源 |

## 改动清单（按用途分组）

### A. Milk-V 8HD 屏（800×1280 MIPI-DSI）移植 —— 验证日志 #13/#131

| 文件 | 动作 |
|---|---|
| `sophpi/u-boot-2021.10/include/cvitek/cvi_panels/dsi_milkv_8hd.h` | 新增（duo-buildroot-sdk@develop 原样） |
| `sophpi/u-boot-2021.10/include/cvitek/cvi_panels/cvi_panels.h` | 加 `#elif defined(CONFIG_MIPI_PANEL_MILKV_8HD)` 分支 |
| `sophpi/u-boot-2021.10/drivers/video/cvitek/Kconfig` | 加 `MIPI_PANEL_MILKV_8HD` 布尔项 |
| `sophpi/cvi_mpi/component/panel/cv181x/dsi_milkv_8hd.h` | 新增（适配 sophpi 类型；**含 #144 花屏修复：vid_hfp_pixels=174、pixel_clk=80412**） |
| `sophpi/cvi_mpi/sample/sample_panel/sample_panel.{c,h}` | 注册 MILKV_8HD（enum/字符串表/switch 三处） |
| `sophpi/cvi_mpi/sample/common/sample_common_vo.c` | 新增 `SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD()`；**#145：GetWH(VO_OUTPUT_USER) 帧率 25→60** |
| `sophpi/cvi_mpi/sample/common/sample_comm.h` | Init_MIPI_MILKV_8HD 声明 |
| `build/panels/panel_list.json` | mipi 列表加 `milkv_8hd`（Kconfig 注册链） |

### B. duos eMMC 板级派生（Module 01，8GB eMMC）—— 验证日志 #12/#131

| 文件 | 动作 |
|---|---|
| `build/boards/cv181x/sg2000_milkv_duos_musl_riscv64_emmc/` | 新板目录（从 SD 板派生）：主 defconfig（STORAGE_TYPE_emmc、FLASH_SIZE_SHRINK、OD_CLK_SEL、8HD panel）、u-boot defconfig（ENV_IS_IN_MMC）、dts（asic_emmc.dtsi）、**partition_emmc.xml（BOOT/MISC/ENV/ROOTFS/SYSTEM/CFG/DATA，DATA 首启自动扩容至 ~6.5GB）**、config.json |
| `sophpi/u-boot-2021.10/arch/riscv/dts/sg2000_milkv_duos_musl_riscv64_emmc.dts` | 新板 u-boot dts |
| `build/boards/cv181x/sg2000_milkv_duos_musl_riscv64_sd/` | SD 对照变体（含 8HD panel defconfig 项），整目录一并收录 |

### C. rootfs 自动挂载链 —— 验证日志 #135/#136（修复 500MB 存储隐患的关键）

| 文件 | 动作 |
|---|---|
| `build/Makefile` | `br-rootfs-prepare` 内插入 create_automount.py + mkcvipart.py --fw_env + /mnt/cfg /mnt/data 挂载点（BUILDROOT_FS 路径原本不走 rootfs-prepare，automount 全缺失） |
| `sophpi/buildroot-2021.05/configs/cvitek_CV181X_musl_riscv64_defconfig` | 加 `BR2_PACKAGE_PARTED=y`、`BR2_PACKAGE_UBOOT_TOOLS=y`（dl 缓存件见验证日志，需自行 wget 到 buildroot-2021.05/dl） |

### D. 旧固件时代 PWM/杂项（保留作参考）

- `pwm_fix/`：硬件 PWM 使能操作说明 + defconfig.append + S90eth0 + 修改前内核文件备份；
- `patches/tracked_defconfig_pwm.patch`：01_sdk git 树内 tracked 文件的全部改动
  （`cvitek_cv1800b_milkv_duo_musl_riscv64_sd_defconfig`，PWM 两项）。

## 花屏根因摘要（#144，改在 A 组两个文件里）

内核 `vo_mipi_tx.c` 缺 u-boot `_cal_htt_extra` 的 DSI 行周期补偿：8HD 原时序
htotal=878 每行 DSI 需 14.04µs > 行周期 12.59µs，持续欠载 ⇒ 花屏。修复 =
`hfp 40→174`（htotal 1012）+ `pixel_clk 80412`，且 VO USER sync `u16Hfb` 必须同步改
（collector/platform_cvitek.c 与探针里各有一份）。collector 启动另需 `/dev/mem`
整写清 `PAT_CFG(0x0a088094)` boot 默认 0x1002a 的 pattern 叠加。
