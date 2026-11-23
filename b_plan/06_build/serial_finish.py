#!/usr/bin/env python3
# 收尾：重挂 p6/p7、rootfs 在线扩容、df 验证。短命令+唯一标记。
import serial, time, sys
LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_finish.log'
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

def run(c, marker, wait=60):
    t.send_slow(c + '\n')
    ok = t.until(marker, wait)   # 标记不出现在命令文本里，回显不会误命中
    time.sleep(0.5); t.read()
    print(f"--- {c} -> {'OK' if ok else 'TIMEOUT'}", flush=True)
    sys.stdout.buffer.write(t.buf[-1800:]); print(b'\n====', flush=True)
    t.buf = b''
    return ok

run('mount /dev/mmcblk0p6 /mnt/cfg', b'~#', 15)
run('mount /dev/mmcblk0p7 /mnt/data', b'~#', 15)
run('resize2fs -f /dev/mmcblk0p4', b'blocks long', 120)
run('df -h', b'/mnt/data', 15)
run('touch /mnt/data/.wtest && rm /mnt/data/.wtest', b'~#', 15)
run('ls /mnt/data', b'~#', 10)
print("DONE")