#!/usr/bin/env python3
# send_s90eth0.py - 通过串口把 S90eth0 脚本传到评估板 /etc/init.d/ 并安装
# 用法: python3 send_s90eth0.py
import os, time, select, termios, base64

SER='/dev/ttyUSB0'
fd = os.open(SER, os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] &= ~(termios.ICANON|termios.ECHO); t[3] &= ~(termios.ECHO)
termios.tcsetattr(fd, termios.TCSANOW, t)

def drain(tm=0.6):
    out=b''; end=time.time()+tm
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.2)
        if r:
            try: out+=os.read(fd,8192)
            except OSError: break
    return out

# 读取 S90eth0 并 base64
with open('/home/oooa/Milk-V-Duo/b_plan/01_sdk/pwm_fix/S90eth0','rb') as f:
    b64 = base64.b64encode(f.read()).decode()

drain(0.5)
os.write(fd, b'echo "" > /tmp/s90.b64\n'); time.sleep(0.4); drain(0.4)
for i in range(0,len(b64),400):
    seg=b64[i:i+400]
    cmd=b"echo '%s' >> /tmp/s90.b64\n"%seg.encode()
    os.write(fd, cmd); time.sleep(0.35); drain(0.35)
os.write(fd, b'base64 -d /tmp/s90.b64 > /etc/init.d/S90eth0; chmod +x /etc/init.d/S90eth0; ls -la /etc/init.d/S90eth0; echo INSTALL_DONE\n')
time.sleep(1.2)
r=drain(1.8)
print(r.decode('utf-8','replace'))
os.close(fd)
