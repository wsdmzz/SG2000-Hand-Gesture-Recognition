#!/usr/bin/env python3
# 修正 DATA=6781816K(≈6.5GB 磁盘末尾)，saveenv 重启，e2fsck+resize2fs 扩容 p6/p7。
import serial, time, sys

LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_expand_data2.log'
MTD = ("blkdevparts=mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS),"
       "40960K(SYSTEM),15240K(CFG),6781816K(DATA);"
       "mmcblk0boot0:1M(fip),1M(fip_bak)")

logf = open(LOG, 'wb')
class Tee:
    def __init__(self, ser): self.ser = ser; self.buf = b''
    def read(self, n=4096):
        d = self.ser.read(n)
        if d:
            logf.write(d); logf.flush(); self.buf += d
            if len(self.buf) > 600000: self.buf = self.buf[-300000:]
        return d
    def until(self, marker, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if marker in self.buf: return True
            self.read()
        return False
    def send(self, s): self.ser.write(s.encode())
    def send_slow(self, s, gap=0.012):
        for ch in s:
            self.ser.write(ch.encode()); self.ser.flush(); time.sleep(gap)

ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.05)
t = Tee(ser)
time.sleep(0.3); t.read()

def step(name, ok):
    print(f"[{name}] {'OK' if ok else 'FAIL'}", flush=True)
    if not ok:
        sys.stdout.buffer.write(t.buf[-3000:]); print()
        sys.exit(1)

def shell_cmd(c, marker, wait=60):
    """发命令，先跳过回显行，再等命令真实输出里的完成标记"""
    t.send_slow(c + '\n')
    # 回显以命令文本+\n 结束（慢发期间输出已在累积）
    if not t.until((c + '\r\n').encode(), 30) and not t.until((c + '\n').encode(), 5):
        print("echo not seen for:", c, flush=True)
    # 丢弃回显及其之前的内容
    idx = t.buf.find((c + '\n').encode())
    if idx >= 0: t.buf = t.buf[idx + len(c) + 1:]
    ok = t.until(marker, wait)
    time.sleep(0.5); t.read()
    sys.stdout.buffer.write(t.buf[-2500:]); print(b'\n====', flush=True)
    t.buf = b''
    return ok

# 1) 重启进 u-boot
t.send('\nreboot\n')
end = time.time() + 60
hit = False
while time.time() < end:
    t.send(' ')
    if t.until(b'soph#', 0.12):
        hit = True; break
step('uboot prompt', hit)
time.sleep(0.5); t.read(); t.buf = b''

# 2) setenv + 严格校验 + saveenv + boot
t.send_slow("setenv mtdparts '" + MTD + "'\n")
step('setenv sent', t.until(b'soph#', 20))
t.buf = b''
t.send('printenv mtdparts\n')
step('printenv', t.until(b'soph#', 10))
if ('mtdparts=' + MTD).encode() not in t.buf:
    print("---- mismatch ----"); sys.stdout.buffer.write(t.buf[-2000:]); print(); sys.exit(1)
t.buf = b''
t.send('saveenv\n')
step('saveenv', t.until(b'OK', 10) and t.until(b'soph#', 8))
t.buf = b''
t.send('boot\n')
step('linux shell', t.until(b'[root@sg200x]', 300))
time.sleep(3); t.read(); t.buf = b''

# 3) 验证 p7 新尺寸
step('p7=6781816K', shell_cmd('cat /proc/partitions', b'6781816 mmcblk0p7', 15))

# 4) 卸载 → fsck → resize → 重挂
step('umount', shell_cmd('umount /mnt/data; umount /mnt/cfg; echo UMOUNT_DONE', b'UMOUNT_DONE', 20))
step('fsck p6', shell_cmd('e2fsck -fy /dev/mmcblk0p6; echo FSCK6_RC=$?', b'FSCK6_RC=', 60))
step('resize p6', shell_cmd('resize2fs /dev/mmcblk0p6; echo RS6_RC=$?', b'RS6_RC=', 60))
step('fsck p7', shell_cmd('e2fsck -fy /dev/mmcblk0p7; echo FSCK7_RC=$?', b'FSCK7_RC=', 120))
step('resize p7', shell_cmd('resize2fs /dev/mmcblk0p7; echo RS7_RC=$?', b'RS7_RC=', 180))
step('remount', shell_cmd('mount -t ext4 -o sync /dev/mmcblk0p6 /mnt/cfg; mount -t ext4 -o sync /dev/mmcblk0p7 /mnt/data; echo REMOUNT_DONE', b'REMOUNT_DONE', 30))
step('df', shell_cmd('df -h', b'/mnt/data', 15))
step('dl_flag', shell_cmd('fw_setenv dl_flag done; echo FLAG_RC=$?', b'FLAG_RC=', 15))
print("DONE")