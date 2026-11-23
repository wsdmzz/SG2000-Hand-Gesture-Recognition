#!/bin/bash
# 救砖烧录：官方 fip + 我方 CIMG（07_deploy/recovery_flash）
# 用法：bash flash_recovery.sh  然后按住 RECOVERY 插拔 USB（或按住 RECOVERY 按 RST）
# 全程串口捕获 + usb_dl 重试循环；真实结果看 USB_DL_EXIT 标记
cd "$(dirname "$0")"
PKG=../07_deploy/recovery_flash
SERLOG=serial_recovery_flash.log
DLLOG=usb_dl_recovery.log

# 1) 串口全程捕获（115200，原始字节；FSBL 高波特率阶段会呈乱码，属正常）
python3 - "$SERLOG" <<'PYEOF' &
import serial, sys, time
log = open(sys.argv[1], 'wb', buffering=0)
log.write(("=== serial capture start %s ===\n" % time.strftime('%H:%M:%S')).encode())
try:
    s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.2)
except Exception as e:
    log.write(("OPEN FAIL: %s\n" % e).encode()); sys.exit(1)
end = time.time() + 900
while time.time() < end:
    d = s.read(4096)
    if d:
        log.write(d)
log.write(b"\n=== serial capture end ===\n")
PYEOF
SERPID=$!
echo "serial capture pid=$SERPID -> $SERLOG"

# 2) usb_dl 重试循环（每轮内置等待 ~30-60s；RECOVERY 窗口 ~10s，循环覆盖）
: > "$DLLOG"
for i in $(seq 1 30); do
    echo "=== attempt $i $(date +%H:%M:%S) ===" >> "$DLLOG"
    "$PKG/tools/usb_dl/Linux/usb_dl" -c cv181x -i "$PKG" -v >> "$DLLOG" 2>&1
    rc=$?
    echo "USB_DL_EXIT=$rc (attempt $i)" >> "$DLLOG"
    echo "attempt $i: USB_DL_EXIT=$rc"
    if [ $rc -eq 0 ]; then break; fi
    sleep 1
done

# 3) 收尾：再抓 60s 串口看首启输出，然后停捕获
sleep 60
kill $SERPID 2>/dev/null
echo "DONE. 检查 $DLLOG 的 USB_DL_EXIT 与 $SERLOG 的首启输出"