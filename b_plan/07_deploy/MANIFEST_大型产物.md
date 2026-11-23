# 07_deploy 收录策略与大型产物清单

本仓库只收录**可复现构建所需的源码/脚本/小工具**。大体积二进制产物不进仓库，
均可由 `b_plan/01_sdk_overlay` + 基线 SDK 重建（构建命令见验证日志 #131–#136、#139）。

## 已收录（本目录）

| 内容 | 说明 |
|---|---|
| `README_部署.md` | 部署指南（SD 时代版本；eMMC 现状以验证日志 #141–#143 为准） |
| `rootfs_patch/` | #141 新固件五处 rootfs 缺口的修补脚本/文件 |
| `usbnet/` + `S99usbnet` | USB RNDIS/NCM 网络起板脚本 |
| `partition_sd.xml` | SD 对照变体分区表 |

## 未收录的大型产物（本机路径 + 校验）

| 文件（相对 `b_plan/`） | 大小 | md5（如适用） | 性质 |
|---|---|---|---|
| `01_sdk/sophpi/install/soc_sg2000_milkv_duos_musl_riscv64_emmc/upgrade.zip` | 34M | `70daddac…` | **实际烧录进 eMMC 的套件**（#142 烧录用） |
| `01_sdk/sophpi/install/…_emmc/rawimages/`（gpt.img `65c42694…`、rootfs.emmc、boot.emmc 等） | 174M | — | eMMC 分区镜像 |
| `07_deploy/backup_sdc_old.img` | 7.6G | — | 旧固件整卡备份（回退用，勿删） |
| `07_deploy/duos_sd_verify.img` | 206M | — | SD 对照整卡镜像（#139） |
| `07_deploy/milkv-duos-musl-riscv64-emmc_v2.0.1.zip` | 85M | — | 官方固件原始包（参照/救砖） |
| `07_deploy/recovery_flash/` | 225M | — | bootROM usb_dl 烧录套件（#142/#16） |
| `07_deploy/rootfs_tree/` | 108M | — | rootfs 解包树（#141 修补现场） |
| `07_deploy/rawimages/`、`upgrade.zip`、`official_v201/` | 81M/33M/3.9M | `06e23c99…`(upgrade.zip) | SD 时代产物 |
| `07_deploy/board_backup_20261120/` | 24M | — | **烧录前板上 /mnt/data+collector 备份**（勿删） |
| `07_deploy/gesture_7cls_fix/`、`boot.sd`、`ramboot.itb`、`fip*.bin`、`fw_payload_uboot.bin` | ≤5M 各 | `5732f1b3…`(boot.sd)、`f360d936…`(fip.bin) | 旧产物/零件 |
| `01_sdk/`（整个 SDK 树，含 sophpi vendor 24G） | 29G | — | 基线+构建现场，由 git clone + tarball 重建 |
| `05_app` 之外的 SDK 编译中间产物（build/output、sophpi/install） | 数 G | — | 构建生成 |

## 板上当前软件（本分支对应版本）

| 板上路径 | md5 | 说明 |
|---|---|---|
| `/root/collector.fps3` | `718458d5e038326a7d345140a40c89a7` | **8HD 屏上 UI + 摄像头 30fps 预览版**（源码 `05_app/collector`，构建 `06_build/build_collector.sh`，验证日志 #145） |
