#!/usr/bin/env python3
# 串口修改官方 u-boot 的 mtdparts 环境变量为 7 分区版并 saveenv，
# 使内核看到 p5 SYSTEM / p6 CFG / p7 DATA，S10auto_mount 才能挂载+扩容。
import serial, time, sys

PORT = '/dev/ttyUSB0'
LOG = '/home/oooa/Milk-V-Duo/b_plan/06_build/serial_fix_parts.log'
MTD = ("mmcblk0:8192K(BOOT),2048K(MISC),128K(ENV),786560K(ROOTFS),"
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
    def until(self, marker, timeout, keep=True):
        end = time.time() + timeout
        while time.time() < end:
            if marker in self.buf:
                if not keep: self.buf = b''
                return True
            self.read()
        return False
    def send(self, s): self.ser.write(s.encode())

ser = serial.Serial(PORT, 115200, timeout=0.05)
t = Tee(ser)
time.sleep(0.3); t.read()

def step(name, ok):
    print(f"[{name}] {'OK' if ok else 'FAIL'}", flush=True)
    if not ok:
        print("---- tail of serial log ----")
        sys.stdout.buffer.write(t.buf[-3000:]); print()
        sys.exit(1)

# 1) 重启板子
t.send('\nreboot\n')
step('reboot issued', t.until(b'Restarting system', 15) or t.until(b'reboot: Restarting system', 15))
t.buf = b''

# 2) 持续打断 autoboot（u-boot 倒计时窗口内任意键即停）
end = time.time() + 30
hit = False
while time.time() < end:
    t.send(' ')
    if t.until(b'soph#', 0.12, keep=True):
        hit = True; break
step('uboot prompt', hit)
time.sleep(0.5); t.read(); t.buf = b''

# 3) 查看当前 mtdparts / root
t.send('printenv mtdparts\n')
step('printenv mtdparts', t.until(b'soph#', 8))
t.send('printenv root\n')
step('printenv root', t.until(b'soph#', 8))
t.buf = b''

# 4) 设置 7 分区 mtdparts（单引号保护分号不被 hush 解释）
t.send("setenv mtdparts '" + MTD + "'\n")
step('setenv mtdparts', t.until(b'soph#', 8))
t.send('printenv mtdparts\n')
ok = t.until(b'3145728K(DATA)', 8) and t.until(b'soph#', 5)
step('verify mtdparts', ok)
t.buf = b''

# 5) 持久化到 ENV 分区
t.send('saveenv\n')
step('saveenv', t.until(b'OK', 10) and t.until(b'soph#', 8))
t.buf = b''

# 6) 启动
t.send('boot\n')
step('linux shell', t.until(b'[root@sg200x]', 300))
time.sleep(3); t.read(); t.buf = b''

# 7) 验证分区与挂载
t.send('cat /proc/partitions\n')
step('p7 visible', t.until(b'mmcblk0p7', 10))
time.sleep(1); t.read(); t.buf = b''
t.send('df -h\n')
step('df', t.until(b'Filesystem', 10))
time.sleep(2); t.read(); t.buf = b''
t.send('mount | grep mmcblk\n')
t.until(b'~#', 10); time.sleep(1); t.read()
print("---- final serial tail ----")
sys.stdout.buffer.write(t.buf[-4000:]); print()
print("DONE")