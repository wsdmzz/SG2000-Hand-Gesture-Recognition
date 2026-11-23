# Milk-V Duo S (SG2000) 新固件部署指南

> 本目录包含**完整编译成功**的自制固件（内置新版 `TDL_*` tdl_sdk，含 `TDL_Classification` 手势分类 API）。
> 编译详见 `b_plan/06_build/fw_build_sg2000_duos*.log`、验证日志 #35、`b_plan/00_docs/README.md`。

## 固件产物清单（本目录）

| 文件 | 大小 | 说明 |
|------|------|------|
| `upgrade.zip` | 34MB | **CVITEK USB 烧写套件格式**（`META/*.txt` + `utils/*` + `partition_sd.xml`），用于 `cvitek_usbburn`/Burntool USB 烧录 |
| `boot.sd` | 3.4MB | FIT 启动镜像（U-Boot + 内核 + dtb），`config-cv1813h_milkv_duos_sd` |
| `ramboot.itb` | 5.2MB | 内存启动镜像 |
| `fip.bin` / `fip_spl.bin` | 494KB | FSBL（可选烧 u-boot 分区） |
| `fw_payload_uboot.bin` | 2.6MB | U-Boot payload |
| `partition_sd.xml` | 303B | 分区布局：BOOT(128M) + MISC(2M) + ROOTFS(768M) |
| `rawimages/rootfs.sd` | 209MB | rootfs 分区镜像（含 `/mnt/system/lib/libtdl_core.so`） |
| `rawimages/data.sd` | 5.4MB | data 分区 |

**关键验证**：固件 `/mnt/system/lib/libtdl_core.so`（1.9MB）导出 **90 个 `TDL_*` 符号**，含
`TDL_Classification`/`TDL_CreateHandle`/`TDL_OpenModel`/`TDL_ReadImage`（新版 API，解决旧固件 `CVI_TDL_*` 分类死路）。
依赖的 `libstdc++`/`libgcc_s` 已一并入固件。

---

## 重要背景：评估板启动机制（已确认）

- **DuoS 从内置 eMMC（`/dev/mmcblk0`）固定启动**：
  `root=/dev/mmcblk0p4 blkdevparts=mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS)`
- **SD 卡（mmcblk1）不参与启动**——把固件写 SD 卡无法让评估板启动（除非 U-Boot 支持且 bootcmd 指向 SD）
- 评估板当前运行旧固件（Buildroot 2021.05，20240801），SSH 可用（`192.168.42.1`，USB-NCM gadget 网卡）

---

## 烧录路径（按推荐顺序）

### 路径 A：USB 官方烧写（推荐，最安全）
1. **开发机**：获取并运行算能/Milk-V 官方 USB 烧录工具（`cvitek_usbburn`/`Burntool`，选 `upgrade.zip`）
2. **评估板**：**按住 BOOT 键 + 上电**，进入 USB 烧写模式（dwc2 切换为烧写 gadget）
3. 烧录工具连接评估板 → 开始烧 `upgrade.zip` 到 eMMC
4. 完成后评估板从新固件启动
- **所需**：开发机烧录工具 + 评估板 BOOT 键进入方法（参考 Milk-V Duo 惯例：BOOT 键在板载，按住上电）

### 路径 B：SD 启动卡（需 U-Boot 支持从 SD 引导）
1. 开发机插 SD 卡（`/dev/sdX`），用 sophpi 自带：
   ```
   sudo ./build/tools/common/sd_tools/sd_gen_burn_image.sh \
        b_plan/01_sdk/sophpi/install/soc_sg2000_milkv_duos_musl_riscv64_sd
   ```
   生成 `sophpi-duo-<日期>.img`（boot fat32 128M + rootfs ext4 256M，含 fip.bin/boot.sd/rootfs）
2. dd 镜像到 SD 卡，插入评估板，若能 `Boot from SD` 则启动新固件
- **前提**：评估板 U-Boot bootcmd 支持从 SD（当前 20240801 固件 eMMC 优先，可能需 U-Boot 环境调整）

### 路径 C：评估板内 dd 写 eMMC（高危，仅当有完整 eMMC 备份时）
- 通过 SSH 在评估板内直接写 eMMC 的 BOOT/ROOTFS 分区 → **变砖风险极高**，不推荐
- 若必须：先完整备份 eMMC（`dd if=/dev/mmcblk0 of=... bs=1M`），再按布局写入

---

## 部署后验证

```sh
# 评估板新固件启动后（USB-NCM 应恢复 192.168.42.1）
ssh root@192.168.42.1
ls /mnt/system/lib/libtdl_core.so          # 新版 TDL_* 库存在
nm -D /mnt/system/lib/libtdl_core.so | grep TDL_Classification   # 分类 API 符号
uname -a                                    # 新内核（sophpi 编译版）
```

---

## 编译复现要点（如需重编）

完整排雷记录见 `b_plan/06_build/fw_build_sg2000_duos33-54.log` 与 `b_plan/00_docs/README.md`：
1. u-boot wifi_pin label 修复（改源头 `build/boards/default/dts/cv181x/cv181x_base.dtsi`）
2. cvi_mpi install 缺失文件加存在判断
3. buildroot host 包手动下载到 `buildroot-2021.05/dl` + 严格 hash 校验
4. `package/Makefile.in` 的 `HOST_CFLAGS` 加 `-std=gnu99`（解决 Fedora C23 兼容）
5. `make_ext4fs` 需 `libpcre.so.3`（`.host_libs` 软链 + `LD_LIBRARY_PATH`）
6. 把 `libtdl_core.so` + `libstdc++`/`libgcc_s` 补入 `rootfs/mnt/system/lib/` 后重打包

## 更新：SD 启动卡已制作（2026-08-10 验证 #37）
- SD 卡 `/dev/sdc`（7.5G）：sdc1(130M FAT32 boot) + sdc2(7.4G ext4 rootfs)
- boot 分区：`fip.bin` + `boot.sd`；rootfs 分区：sophpi 输出 `rootfs/` 展开树（108M，含 libtdl_core.so）
- **插入评估板上电观察：若 `Boot from SD`（config-cv1813h_milkv_duos_sd）则新固件生效；否则 U-Boot 仍 eMMC 优先，需进 U-Boot 手动 boot SD 或转路径 A**
