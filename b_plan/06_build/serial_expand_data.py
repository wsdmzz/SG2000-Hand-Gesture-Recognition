#!/usr/bin/env python3
# 把 mtdparts 里 DATA 扩到磁盘末尾(4387576K≈4.2GB)，saveenv 重启，
# 然后串口 shell 里 e2fsck+resize2fs 扩容 p6/p7 文件系统。
import serial, time, sys

LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_expand_data.log'
# 7634944 - (8192+2048+128+786560+40960+15240) = 4387576K
MTD = ("blkdevparts=mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS),"
       "40960K(SYSTEM),15240K(CFG),4387576K(DATA);"
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

def shell_cmd(c, wait=4):
    t.buf = b''
    t.send_slow(c + '\n')
    t.until(b'~#', wait)
    time.sleep(0.5); t.read()
    sys.stdout.buffer.write(t.buf); print(b'\n====', flush=True)

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

# 2) 慢发 setenv + 严格校验
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
shell_cmd('cat /proc/partitions')
if b'4387576 mmcblk0p7' not in t.buf:
    print("p7 size not updated!"); sys.exit(1)

# 4) 扩容文件系统（先卸载再 fsck/resize，避免 sync 挂载下 resize 问题）
shell_cmd('umount /mnt/data; umount /mnt/cfg; echo UMOUNT_DONE')
shell_cmd('e2fsck -fy /dev/mmcblk0p6', 20)
shell_cmd('resize2fs /dev/mmcblk0p6', 20)
shell_cmd('e2fsck -fy /dev/mmcblk0p7', 60)
shell_cmd('resize2fs /dev/mmcblk0p7', 90)
shell_cmd('mount -t ext4 -o sync /dev/mmcblk0p6 /mnt/cfg; mount -t ext4 -o sync /dev/mmcblk0p7 /mnt/data; echo REMOUNT_DONE')
shell_cmd('df -h')
shell_cmd('fw_setenv dl_flag done; echo FLAG_DONE')
print("DONE")