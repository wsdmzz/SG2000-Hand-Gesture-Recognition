#!/usr/bin/env python3
# resize2fs -f 强制扩容 p6/p7；顺带在线扩容 rootfs(p4)；核实 dl_flag。
import serial, time, sys
LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_resize_force.log'
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
    def send_slow(self, s, gap=0.012):
        for ch in s:
            self.ser.write(ch.encode()); self.ser.flush(); time.sleep(gap)
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.05)
t = Tee(ser)
time.sleep(0.3); t.read(); t.buf = b''

def shell_cmd(c, marker, wait=60):
    t.send_slow(c + '\n')
    t.until((c + '\n').encode(), 30)
    idx = t.buf.find((c + '\n').encode())
    if idx >= 0: t.buf = t.buf[idx + len(c) + 1:]
    ok = t.until(marker, wait)
    time.sleep(0.5); t.read()
    sys.stdout.buffer.write(t.buf[-2500:]); print(b'\n====', flush=True)
    t.buf = b''
    return ok

shell_cmd('fw_printenv dl_flag; echo ENV_RC=$?', b'ENV_RC=', 15)
shell_cmd('cat /etc/fw_env.config 2>/dev/null; echo CFG_RC=$?', b'CFG_RC=', 10)
shell_cmd('umount /mnt/data; umount /mnt/cfg; echo UMOUNT_DONE', b'UMOUNT_DONE', 20)
shell_cmd('e2fsck -fy /dev/mmcblk0p6; resize2fs -f /dev/mmcblk0p6; echo RS6_RC=$?', b'RS6_RC=', 90)
shell_cmd('e2fsck -fy /dev/mmcblk0p7; resize2fs -f /dev/mmcblk0p7; echo RS7_RC=', b'RS7_RC=', 300)
shell_cmd('mount -t ext4 -o sync /dev/mmcblk0p6 /mnt/cfg; mount -t ext4 -o sync /dev/mmcblk0p7 /mnt/data; echo REMOUNT_DONE', b'REMOUNT_DONE', 30)
shell_cmd('resize2fs /dev/mmcblk0p4; echo RS4_RC=$?', b'RS4_RC=', 120)
shell_cmd('df -h; echo DF_DONE', b'DF_DONE', 15)
shell_cmd('fw_setenv dl_flag done; fw_printenv dl_flag; echo FLAG_DONE', b'FLAG_DONE', 15)
print("DONE")