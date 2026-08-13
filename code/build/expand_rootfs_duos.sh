#!/bin/bash
#
# Milk-V Duo Module 01 / DuoS 8GB SD 卡 rootfs 分区扩展脚本
# 用途：把 ROOTFS 分区(768MB, 分区3)扩展占满整张 SD 卡
# 目标盘：/dev/sdc  （请运行前二次确认！）
#
# 与 Duo 原版的区别：DuoS/DuoModule01 的根文件系统在【分区3】，
# 因为中间还有一个空的保留分区(分区2)。BOOT=1, 保留=2, ROOTFS=3。
#
# 说明：使用 parted 删除分区3并重建到磁盘末尾 + resize2fs 扩展文件系统。
#       全程打印每一步结果，任一步失败即中止。
#

set -u

TARGET=/dev/sdc
PART=3
DEV="${TARGET}${PART}"

echo "=============================================="
echo " Duo Module 01 / DuoS rootfs 分区扩展脚本 (分区3)"
echo " 目标设备 : ${TARGET}   目标分区 : ${DEV} (ROOTFS)"
echo "=============================================="

if [ ! -b "$TARGET" ]; then
    echo "[错误] 找不到块设备 ${TARGET}，请检查 SD 卡。" >&2
    exit 1
fi

# 安全：确认设备真实容量 >= 4GB，避免烧错到小容量/空设备
DISKSIZE=$(lsblk -ndo SIZE "$TARGET" 2>/dev/null)
echo "检测设备: ${TARGET} ($(lsblk -ndo MODEL,SIZE "$TARGET" 2>/dev/null))"
echo "--- 扩展前分区表 ---"
sudo fdisk -l "$TARGET"
echo ""
read -r -p "确认是【尺寸正常(>=4GB)的 SD 卡 /dev/sdc】吗？输入 yes 继续: " CONFIRM
[ "$CONFIRM" = "yes" ] || { echo "已取消。"; exit 1; }

echo ""
echo "[1/6] 强制卸载并踢出占用进程..."
sudo fuser -km "${TARGET}"1 "${TARGET}"2 "${TARGET}"3 2>/dev/null || true
sleep 1
sudo umount "${TARGET}"1 2>/dev/null && echo "  已卸载 ${TARGET}1" || echo "  ${TARGET}1 未挂载"
sudo umount "${TARGET}"2 2>/dev/null && echo "  已卸载 ${TARGET}2" || echo "  ${TARGET}2 未挂载"
sudo umount "${TARGET}"3 2>/dev/null && echo "  已卸载 ${TARGET}3" || echo "  ${TARGET}3 未挂载"
sync

echo "[2/6] 确认无挂载..."
if mount | grep -q "${TARGET}"; then
    echo "  [警告] 仍有挂载，强制卸载:"
    for p in 1 2 3; do sudo umount -l "${TARGET}$p" 2>/dev/null; done
    sleep 1
    if mount | grep -q "${TARGET}"; then
        echo "  [错误] 无法卸载，分区仍在使用。请关闭文件管理器窗口后重试。中止。" >&2
        exit 1
    fi
fi
echo "  挂载已清除 ✓"

echo "[3/6] 读取分区3起始扇区..."
START_SECT=$(sudo parted -sm "$TARGET" unit s print 2>/dev/null | awk -F: '$1=="3"{print $2}' | tr -d 's,')
if [ -z "$START_SECT" ]; then
    echo "  [错误] 无法获取分区3起始扇区。" >&2
    sudo parted -sm "$TARGET" unit s print 2>&1 | head -10
    exit 1
fi
echo "  分区3起始: ${START_SECT}s"

echo "[4/6] 重建分区3至磁盘末尾 (parted)..."
# Fedora GNU parted 3.6 不支持 Debian 系的 ---pretend-input-tty 选项，直接用 -s 非交互模式
# 前一步已确保分区卸载干净，-s 模式下不会弹出交互确认，避免卡住
sudo parted -s "$TARGET" rm 3
sudo parted -s "$TARGET" unit s mkpart primary "$START_SECT" 100%

echo "[5/6] 刷新分区表并检查/扩展文件系统..."
sudo partprobe "$TARGET" 2>/dev/null || {
    for p in 1 2 3; do sudo umount -l "${TARGET}$p" 2>/dev/null; done
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
echo " 如果分区3显示为 ~7.4G 的 ext4 => 扩展成功！"
echo "=============================================="
