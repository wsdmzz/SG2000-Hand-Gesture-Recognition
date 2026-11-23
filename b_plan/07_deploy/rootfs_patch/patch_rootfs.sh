#!/bin/bash
# patch_rootfs.sh — 烧录前修补 sophpi eMMC rawimages（可复现，B02 复制同样执行）
#
# 修补项（2026-11-20 验证 #141 背景）：
#   1. S10auto_mount：跳过 SYSTEM 挂载（system.emmc 为空且分区 40MB < 媒体栈 61MB，
#      挂载会遮蔽 rootfs 内 /mnt/system 的 ko/lib，摄像头/VO 全失效）
#   2. 新增 S99usbnet：Module 01 GPIO453/510 USB 路由 + RNDIS gadget + dropbear
#   3. 装入 dropbear（旧板提取，musl riscv64 同 ABI）——新 rootfs 无任何 SSH 服务端
#   4. /etc/shadow：root 密码改 milkv（原哈希未知）
#   5. data.emmc 预置 sensor_cfg.ini(GC2083) + site_info.txt（首启摄像头即可用）
#
# 用法: bash patch_rootfs.sh [install_dir]
# 默认 install_dir = ../../01_sdk/sophpi/install/soc_sg2000_milkv_duos_musl_riscv64_emmc
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
EM="${1:-$HERE/../../01_sdk/sophpi/install/soc_sg2000_milkv_duos_musl_riscv64_emmc}"
EM="$(cd "$EM" && pwd)"
RAW="$EM/rawimages"
[ -f "$RAW/rootfs.emmc" ] || { echo "缺 $RAW/rootfs.emmc"; exit 1; }

# 幂等保护：已打过补丁则跳过 debugfs 部分（检查 S99usbnet 是否已存在）
ALREADY=0
if debugfs -R "stat /etc/init.d/S99usbnet" "$RAW/rootfs.emmc" 2>/dev/null | grep -q "Inode:"; then
    echo "rootfs.emmc 已含 S99usbnet，跳过 debugfs 补丁（如需重打先恢复 .orig）"
    ALREADY=1
fi

# 备份原始镜像（raw + 顶层 CIMG）
for f in rootfs.emmc data.emmc; do
    [ -f "$RAW/$f.orig" ] || cp "$RAW/$f" "$RAW/$f.orig"
    [ -f "$EM/$f.orig" ] || cp "$EM/$f" "$EM/$f.orig"
done

# ---- rootfs.emmc / data.emmc（debugfs 直补 raw 镜像）----
if [ "$ALREADY" = "0" ]; then
cat > /tmp/rootfs_patch.cmds << EOF
rm /etc/init.d/S10auto_mount
write $HERE/S10auto_mount /etc/init.d/S10auto_mount
set_inode_field /etc/init.d/S10auto_mount mode 0100755
write $HERE/S99usbnet /etc/init.d/S99usbnet
set_inode_field /etc/init.d/S99usbnet mode 0100755
write $HERE/dropbear /usr/sbin/dropbear
set_inode_field /usr/sbin/dropbear mode 0100755
rm /etc/shadow
write $HERE/shadow /etc/shadow
set_inode_field /etc/shadow mode 0100600
EOF
debugfs -w -f /tmp/rootfs_patch.cmds "$RAW/rootfs.emmc" 2>&1 | grep -vE "^debugfs|^$" || true

cat > /tmp/data_patch.cmds << EOF
write $HERE/sensor_cfg.ini /sensor_cfg.ini
write $HERE/site_info.txt /site_info.txt
EOF
debugfs -w -f /tmp/data_patch.cmds "$RAW/data.emmc" 2>&1 | grep -vE "^debugfs|^$" || true
fi

# ---- 重打 CIMG（usb_dl 消费顶层 CIMG 容器，官方固件同格式，验证 #141）----
IMGTOOL="$HERE/../../01_sdk/sophpi/build/tools/common/image_tool"
# logo.jpg：官方 rom 含 MISC logo，构建默认 ENABLE_BOOTLOGO=0 未生成，这里补齐
BOOTLOGO="$HERE/../../01_sdk/sophpi/build/tools/common/bootlogo/logo.jpg"
if [ -f "$BOOTLOGO" ] && [ ! -f "$RAW/logo.jpg" ]; then
    cp "$BOOTLOGO" "$RAW/logo.jpg"
fi
for f in rootfs.emmc data.emmc logo.jpg; do
    [ -f "$RAW/$f" ] || continue
    python3 "$IMGTOOL/raw2cimg.py" "$RAW/$f" "$EM" "$EM/partition_emmc.xml"
done

# ---- 验证 ----
echo "=== 验证 ==="
debugfs -R "stat /etc/init.d/S10auto_mount" "$RAW/rootfs.emmc" 2>/dev/null | grep Mode
debugfs -R "stat /etc/init.d/S99usbnet" "$RAW/rootfs.emmc" 2>/dev/null | grep Mode
debugfs -R "stat /usr/sbin/dropbear" "$RAW/rootfs.emmc" 2>/dev/null | grep Mode
debugfs -R "stat /etc/shadow" "$RAW/rootfs.emmc" 2>/dev/null | grep Mode
debugfs -R "cat /etc/shadow" "$RAW/rootfs.emmc" 2>/dev/null | head -1
debugfs -R "ls /" "$RAW/data.emmc" 2>/dev/null
for f in rootfs.emmc data.emmc logo.jpg; do
    [ -f "$EM/$f" ] && echo "$f CIMG magic: $(head -c4 "$EM/$f")"
done
echo "PATCH_OK"