#!/usr/bin/env python3
# 修复 mtdparts：带 blkdevparts= 前缀的 7 分区串；逐字符慢发防串口丢字。
import serial, time, sys

PORT = '/dev/ttyUSB0'
LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_fix_parts2.log'
MTD = ("blkdevparts=mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS),"
       "40960K(SYSTEM),15240K(CFG),3145728K(DATA);"
       "mmcblk0boot0:1M(fip),1M(fip_bak)")

logf = open(LOG, 'wb')

class Tee:
    def __init__(self, ser): self.ser = ser; self.buf = b''
    def read(self, n=4096):
        d = self.ser.read(n)
        if d:
            logf.write(d); logf.flush(); self.buf += d
            if len(self.buf) > 400000: self.buf = self.buf[-200000:]
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

ser = serial.Serial(PORT, 115200, timeout=0.05)
t = Tee(ser)
time.sleep(0.3); t.read()

def step(name, ok):
    print(f"[{name}] {'OK' if ok else 'FAIL'}", flush=True)
    if not ok:
        print("---- tail of serial log ----")
        sys.stdout.buffer.write(t.buf[-3000:]); print()
        sys.exit(1)

# 1) panic 状态下回车可能无响应；直接持续发空格+回车，等复位后抓 u-boot。
#    若 shell 还活着则 reboot；panic 则只能等外部复位——先试 reboot。
t.send('\nreboot\n')
got_reset = t.until(b'U-Boot 2021.10', 20)
if not got_reset:
    # panic 后 shell 死了，reboot 无效。持续打断等待（需要用户按 RST 时这里会等到）
    print("no reset yet; keep interrupting (press RST if needed)", flush=True)
end = time.time() + 120
hit = False
while time.time() < end:
    t.send(' ')
    if t.until(b'soph#', 0.12):
        hit = True; break
step('uboot prompt', hit)
time.sleep(0.5); t.read(); t.buf = b''

# 2) 慢速设置 mtdparts（单引号保护分号）
t.send_slow("setenv mtdparts '" + MTD + "'\n")
step('setenv sent', t.until(b'soph#', 15))
t.buf = b''
t.send('printenv mtdparts\n')
ok = t.until(b'3145728K(DATA)', 10) and t.until(b'soph#', 5)
step('verify mtdparts', ok)
# 严格校验：回显必须与期望完全一致
expect = ('mtdparts=' + MTD).encode()
if expect not in t.buf:
    print("---- mtdparts mismatch, tail ----")
    sys.stdout.buffer.write(t.buf[-2000:]); print()
    sys.exit(1)
t.buf = b''

# 3) saveenv + boot
t.send('saveenv\n')
step('saveenv', t.until(b'OK', 10) and t.until(b'soph#', 8))
t.buf = b''
t.send('boot\n')
step('linux shell', t.until(b'[root@sg200x]', 300))
time.sleep(3); t.read(); t.buf = b''

# 4) 验证
t.send('cat /proc/partitions\n')
step('p7 visible', t.until(b'mmcblk0p7', 15))
time.sleep(1); t.read(); t.buf = b''
t.send('df -h\n')
step('df', t.until(b'Filesystem', 10))
time.sleep(2); t.read(); t.buf = b''
t.send('mount | grep mmcblk\n')
t.until(b'~#', 10); time.sleep(1); t.read()
print("---- final serial tail ----")
sys.stdout.buffer.write(t.buf[-4000:]); print()
print("DONE")