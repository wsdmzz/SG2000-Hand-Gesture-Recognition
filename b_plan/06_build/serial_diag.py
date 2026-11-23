#!/usr/bin/env python3
# 串口 shell 诊断：dl_flag / parted / fw_printenv / DATA 现状
import serial, time, sys
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.05)
logf = open('/home/oooa/Milk-V-Duo/b_plan/06_build/serial_diag.log', 'wb')
buf = b''
def read_for(sec):
    global buf
    end = time.time() + sec
    while time.time() < end:
        d = ser.read(4096)
        if d:
            logf.write(d); logf.flush(); buf += d
def send_slow(s, gap=0.012):
    for ch in s:
        ser.write(ch.encode()); ser.flush(); time.sleep(gap)
def cmd(c, wait=3):
    global buf
    buf = b''
    send_slow(c + '\n'); read_for(wait)
    sys.stdout.buffer.write(buf); print(b'\n====', flush=True)

read_for(0.5); buf = b''
cmd('echo DIAG_BEGIN')
cmd('fw_printenv dl_flag; echo RC=$?')
cmd('which parted resize2fs e2fsck fw_setenv')
cmd('cat /proc/partitions')
cmd('ls -la /mnt/data /mnt/cfg')
cmd('echo DIAG_END')