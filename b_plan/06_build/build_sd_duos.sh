#!/bin/bash
# SG2000 Duo Module 01 SD 对照固件构建（本机闭环，#15 先行验证用）
# 板级: sg2000_milkv_duos_musl_riscv64_sd（官方 milkv 板型，8HD panel 已使能）
# 注意: SD 布局 BOOT(FAT,含 fip.bin+boot.sd)/MISC/ROOTFS 三分区、无独立 DATA，
#       /mnt/data 落在 768MB rootfs 内 —— 只供功能验证，大量采集仍需 eMMC。
# 产物整卡镜像用 milkv genimage 布局另行组装（见 06_build/make_sd_image.sh）。
set -x
cd /home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi || exit 1
# Fedora 42 无 libpcre.so.3，prebuild make_ext4fs 需要（见验证日志 #134）
export LD_LIBRARY_PATH=/home/oooa/Milk-V-Duo/b_plan/.host_libs
source build/envsetup_soc.sh
defconfig sg2000_milkv_duos_musl_riscv64_sd || exit 1
build_all
rc=$?
echo "BUILD_ALL_EXIT=$rc"
if [ $rc -eq 0 ]; then
  ls -la install/soc_sg2000_milkv_duos_musl_riscv64_sd/rawimages/ 2>/dev/null
  ls -la install/soc_sg2000_milkv_duos_musl_riscv64_sd/fip.bin 2>/dev/null
fi
exit $rc