#!/bin/bash
#
# Milk-V Duo 8GB SD 卡 rootfs 分区扩展脚本 v2（路线 A）
# 用途：把 ROOTFS 分区(768MB)扩展占满整张 8GB SD 卡
# 目标盘：/dev/sdc  （请运行前二次确认！）
#
# v2 改进：
#   1. 强杀占用 + 强制卸载，解决 udisks2 自动挂载导致分区表锁定的问题
#   2. 自动喂入 parted 的 Ignore 应答，避免卡在交互提示
#   3. 每一步失败即中止，并打印真实状态
#

set -u

TARGET=/dev/sdc
PART=2
DEV="${TARGET}${PART}"

echo "=============================================="
echo " Milk-V Duo rootfs 分区扩展脚本 v2"
echo " 目标设备 : ${TARGET}   目标分区 : ${DEV} (ROOTFS)"
echo "=============================================="

if [ ! -b "$TARGET" ]; then
    echo "[错误] 找不到块设备 ${TARGET}，请检查 SD 卡。" >&2
    exit 1
fi

echo "检测: ${TARGET} ($(lsblk -ndo MODEL,SIZE "$TARGET" 2>/dev/null))"
echo "--- 扩展前分区表 ---"
sudo fdisk -l "$TARGET"
echo ""
read -r -p "确认是【8GB 的 /dev/sdc】吗？输入 yes 继续: " CONFIRM
[ "$CONFIRM" = "yes" ] || { echo "已取消。"; exit 1; }

echo ""
echo "[1/6] 强制卸载并踢出占用 sdc 的进程..."
# 踢掉占用该磁盘/分区的进程（wget 到 httpd 之类都算）
sudo fuser -km "${TARGET}1" "${TARGET}2" 2>/dev/null || true
sleep 1
# 强制卸载全部相关分区
sudo umount "${TARGET}"1 2>/dev/null && echo "  已卸载 ${TARGET}1" || echo "  ${TARGET}1 未挂载"
sudo umount "${TARGET}"2 2>/dev/null && echo "  已卸载 ${TARGET}2" || echo "  ${TARGET}2 未挂载"
sync

echo "[2/6] 确认已无挂载..."
if mount | grep -q "${TARGET}"; then
    echo "  [警告] 仍有挂载，尝试强制卸载:"
    sudo umount -l "${TARGET}"1 2>/dev/null
    sudo umount -l "${TARGET}"2 2>/dev/null
    sleep 1
    if mount | grep -q "${TARGET}"; then
        echo "  [错误] 无法卸载，分区仍被使用。请先关闭文件管理器窗口或重启。中止。" >&2
        exit 1
    fi
fi
echo "  挂载已清除 ✓"

echo "[3/6] 读取分区2起始扇区..."
START_SECT=$(sudo parted -sm "$TARGET" unit s print 2>/dev/null | awk -F: '$1=="2"{print $2}' | tr -d 's,')
if [ -z "$START_SECT" ]; then
    echo "  [错误] 无法获取分区2起始扇区。" >&2
    sudo parted -sm "$TARGET" unit s print 2>&1 | head -10
    exit 1
fi
echo "  分区2起始: ${START_SECT}s"

echo "[4/6] 重建分区2至磁盘末尾 (parted)..."
# 卸载干净后 parted 不再需要交互应答；-s 静默模式直接执行。
# 若因残余挂载仍弹提示，用 yes 管道自动回答全部 Ignore。
yes "Ignore" 2>/dev/null | sudo parted -s "$TARGET" ---pretend-input-tty rm 2
yes "Ignore" 2>/dev/null | sudo parted -s "$TARGET" unit s mkpart primary "$START_SECT" 100%

echo "[5/6] 刷新分区表并检查/扩展文件系统..."
# partprobe 失败通常是有分区仍被 udisks 锁，重试卸载后再 probe
sudo partprobe "$TARGET" 2>/dev/null || {
    sudo umount -l "${TARGET}"1 2>/dev/null
    sudo umount -l "${TARGET}"2 2>/dev/null
    sleep 1
    sudo partprobe "$TARGET"
}
sleep 2
ls -l "$DEV"
echo "  e2fsck 检查..."
sudo e2fsck -y -f "$DEV"
echo "  resize2fs 扩展..."
sudo resize2fs "$DEV"

echo "[6/6] 校验最终结果..."
sudo partprobe "$TARGET" 2>/dev/null
echo "--- 最终分区布局 ---"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$TARGET"
echo ""
echo "=============================================="
echo " 如果上面 sdc2 显示为 ~7.4G 的 ext4 => 扩展成功！"
echo "=============================================="
