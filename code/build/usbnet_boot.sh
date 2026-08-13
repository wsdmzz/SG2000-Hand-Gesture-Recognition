#!/bin/sh
# USB gadget ECM 网络自启脚本 (Milk-V Duo S / SG2000 自制固件, BusyBox)
# 通过 configfs 配置 USB 网络 gadget(ECM), 评估板侧 usb0 = 192.168.42.1
# 之后开发机插上 USB 线即可看到 usb 网卡(enp*), 直接 ssh root@192.168.42.1
# 依赖: 内核 CONFIG_USB_DWC2/CONFIG_USB_CONFIGFS/CONFIG_USB_F_ECM=y (已确认内建)

# 通常 /sys/kernel/config 已由内核默认挂载; 若无则挂载
[ -d /sys/kernel/config ] || mount -t configfs none /sys/kernel/config

CFG=/sys/kernel/config/usb_gadget
GADGET=$CFG/usbnet

# 若上次已配置, 先清理以免冲突
if [ -d "$GADGET" ]; then
    echo "" > "$GADGET/UDC" 2>/dev/null
    [ -e "$GADGET/functions/ecm.usb0" ] && rm -rf "$GADGET/functions/ecm.usb0"
    [ -e "$GADGET/configs/c.1" ] && rm -rf "$GADGET/configs/c.1"
    rmdir "$GADGET" 2>/dev/null
fi

mkdir -p "$GADGET"
echo 0x1d6b > "$GADGET/idVendor"          # Linux Foundation
echo 0x0104 > "$GADGET/idProduct"         # Multifunction Composite Gadget
echo 0x0100 > "$GADGET/bcdDevice"
echo 0x0200 > "$GADGET/bcdUSB"

# 字符串描述 (可选)
if [ ! -d "$GADGET/strings/0x409" ]; then
    mkdir -p "$GADGET/strings/0x409"
fi
echo "MilkV" > "$GADGET/strings/0x409/manufacturer"
echo "USB ECM Gadget" > "$GADGET/strings/0x409/product"
echo "deadbeef0001" > "$GADGET/strings/0x409/serialnumber"

# ECM 网络功能
mkdir -p "$GADGET/configs/c.1/strings/0x409"
echo 100 > "$GADGET/configs/c.1/MaxPower"
mkdir -p "$GADGET/functions/ecm.usb0"
ln -s "$GADGET/functions/ecm.usb0" "$GADGET/configs/c.1/"

# 绑定 UDC (此前已确认存在 4340000.usb)
echo 4340000.usb > "$GADGET/UDC" 2>/dev/null

# 评估板侧配置 usb0 静态地址
sleep 1
ip link set usb0 up 2>/dev/null
ip addr flush dev usb0 2>/dev/null
ip addr add 192.168.42.1/24 dev usb0 2>/dev/null

exit 0
