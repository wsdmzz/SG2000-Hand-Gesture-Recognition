#!/usr/bin/env python3
# serial_enter_dl.py — 重启板子并在 u-boot 提示符执行 cvi_utask 进 USB 下载模式
import serial, subprocess, time, re, sys

LOG = "/home/oooa/Milk-V-Duo/b_plan/06_build/serial_uboot.log"
buf = bytearray()

def log(s):
    with open(LOG, "ab") as f:
        f.write(s if isinstance(s, bytes) else s.encode())

ser = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.2)
log(b"\n=== serial_enter_dl start %s ===\n" % time.strftime("%T").encode())

# 触发重启：ssh 已挂死，改从串口 shell 直接 reboot
ser.write(b"\rreboot\r")
time.sleep(0.5)
ser.read(4096)

# 20s 内持续敲键打断 autoboot
prompt = None
t0 = time.time()
while time.time() - t0 < 20:
    ser.write(b"\r\x03")
    data = ser.read(4096)
    if data:
        buf += data
        log(data)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    m = re.search(rb"([\w\-]+#\s*$|=>\s*$)", buf[-200:])
    if m and b"Hit any key" not in buf[-200:]:
        # 确认是 u-boot 提示符（发个回车看回显）
        ser.write(b"\r")
        time.sleep(0.3)
        d2 = ser.read(4096)
        buf += d2; log(d2)
        if re.search(rb"[\w\-]+#\s*$", buf[-100:]):
            prompt = True
            break

if not prompt:
    log("\n!!! 未截获 u-boot 提示符\n".encode())
    print("\n!!! 未截获 u-boot 提示符")
    sys.exit(1)

print("\n=== u-boot prompt captured, send help ===")
ser.write(b"help\r")
time.sleep(1.0)
d = ser.read(16384); buf += d; log(d)
sys.stdout.buffer.write(d)

print("\n=== send cvi_utask ===")
ser.write(b"cvi_utask\r")
t0 = time.time()
while time.time() - t0 < 15:
    d = ser.read(4096)
    if d:
        buf += d; log(d)
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()
    time.sleep(0.1)
log(b"\n=== serial_enter_dl end ===\n")
ser.close()