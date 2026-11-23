#!/bin/bash
# 组装 DuoS SD 先行验证整卡镜像（#15，非破坏：只生成 img 文件，不碰任何实体卡）
#
# 布局对齐 milkv 官方 device/milkv-duos-musl-riscv64-sd/genimage.cfg：
#   MBR 三分区：p1 BOOT 128M FAT32(bootable, 内含 fip.bin + boot.sd)
#               p2 MISC 2M（logo.jpg 裸块）  p3 ROOTFS 768432K ext4
# CV181x bootrom 从 SD 的 FAT BOOT 分区读 fip.bin —— 8月用 GPT+dd 写卡不启动
# 即因布局不对（见验证日志 #138），并非"DuoS 固定 eMMC 启动"。
#
# 依赖：sfdisk / mkfs.vfat(--offset) / mcopy(@@offset) / dd（Fedora 42 均有）。
# genimage 预编译件缺 libconfuse.so.2，故手工组装，布局等价。
#
# 写实体卡（需用户确认 + 读卡器）：
#   sudo dd if=07_deploy/duos_sd_verify.img of=/dev/sdX bs=4M conv=fsync status=progress
#   （/dev/sdX 必须是整卡设备，写前 lsblk 核对容量，卡上数据全灭）
set -e
SDK=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi
INST=$SDK/install/soc_sg2000_milkv_duos_musl_riscv64_sd
OUT=/home/oooa/Milk-V-Duo/b_plan/07_deploy/duos_sd_verify.img
LOGO=$SDK/build/tools/common/bootlogo/logo.jpg

for f in "$INST/fip.bin" "$INST/rawimages/boot.sd" "$INST/rawimages/rootfs.sd" "$LOGO"; do
  [ -f "$f" ] || { echo "缺文件: $f（先跑 build_sd_duos.sh）"; exit 1; }
done

# 分区表（sector=512B）：起点 2048（1M 对齐）
P1_START=2048;        P1_SECT=262144     # 128M
P2_START=$((P1_START+P1_SECT)); P2_SECT=4096   # 2M
P3_START=$((P2_START+P2_SECT)); P3_SECT=1536864 # 768432K
TOTAL=$((P3_START+P3_SECT))

rm -f "$OUT"
truncate -s $((TOTAL*512)) "$OUT"
sfdisk "$OUT" << EOF
label: dos
start=$P1_START, size=$P1_SECT, type=c, bootable
start=$P2_START, size=$P2_SECT, type=83
start=$P3_START, size=$P3_SECT, type=83
EOF

# p1 FAT32 + fip.bin/boot.sd（bootrom 从 FAT 根目录读 fip.bin）
mkfs.vfat -F 32 -n boot --offset=$P1_START "$OUT" >/dev/null
mcopy -i "$OUT@@$((P1_START*512))" "$INST/fip.bin" "$INST/rawimages/boot.sd" ::/

# p2 MISC logo 裸块；p3 ROOTFS ext4 镜像
dd if="$LOGO" of="$OUT" bs=512 seek=$P2_START conv=notrunc status=none
dd if="$INST/rawimages/rootfs.sd" of="$OUT" bs=512 seek=$P3_START conv=notrunc status=none

echo "== 分区表 =="
sfdisk -d "$OUT"
echo "== BOOT 分区内容 =="
mdir -i "$OUT@@$((P1_START*512))" ::/
ls -la "$OUT"
echo "OK: $OUT（写卡命令见脚本头注释，需用户确认）"