#!/usr/bin/env python3
# 救场：板上 busybox ps 写闭管道卡死，占满 dropbear 会话 ⇒ ssh 无响应。
# 串口杀卡死的 ps、杀旧 vo_probe、换新版二进制。短命令+提示符标记。
import serial, time, sys
LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_rescue.log'
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

def run(c, marker, wait=20):
    t.send_slow(c + '\n')
    ok = t.until(marker, wait)
    time.sleep(0.5); t.read()
    print(f"--- {c} -> {'OK' if ok else 'TIMEOUT'}", flush=True)
    sys.stdout.buffer.write(t.buf[-1200:]); print(b'\n====', flush=True)
    t.buf = b''
    return ok

t.send_slow('\n'); time.sleep(0.5); t.read()
sys.stdout.buffer.write(t.buf[-600:]); t.buf = b''
run('killall ps', b'~#', 10)
run('kill $(pidof vo_probe)', b'~#', 10)
run('mv /root/vo_probe.new /root/vo_probe', b'~#', 10)
run('ls /root', b'vo_probe', 10)
print("DONE")