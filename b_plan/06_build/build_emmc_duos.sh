#!/bin/bash
# SG2000 Duo Module 01 eMMC 固件全量构建（本机闭环）
# 板级: sg2000_milkv_duos_musl_riscv64_emmc（#12 派生，含 6.5GB DATA 自动扩容）
# panel: MILKV_8HD（#13 移植）
set -x
cd /home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi || exit 1
# Fedora 缺 libpcre.so.3（make_ext4fs 依赖）：8月已建 .host_libs 软链，
# 必须导出 LD_LIBRARY_PATH（不能含空值/CWD，buildroot 依赖检查拒绝）
export LD_LIBRARY_PATH=/home/oooa/Milk-V-Duo/b_plan/.host_libs
source build/envsetup_soc.sh
defconfig sg2000_milkv_duos_musl_riscv64_emmc || exit 1
build_all
rc=$?
echo "BUILD_ALL_EXIT=$rc"
if [ $rc -eq 0 ]; then
  # USB 烧录需要 gpt.img（build_all 不含此步）
  pack_gpt
  echo "PACK_GPT_EXIT=$?"
  ls -la install/soc_sg2000_milkv_duos_musl_riscv64_emmc/rawimages/ 2>/dev/null
  ls -la install/soc_sg2000_milkv_duos_musl_riscv64_emmc/upgrade.zip 2>/dev/null
fi
exit $rc