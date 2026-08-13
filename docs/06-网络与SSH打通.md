# 06 - 网络与 SSH 打通（#70 及之后）

## 背景

新固件默认**无 SSH 服务端**（无 dropbear/sshd），且 USB 网络/串口不稳定。为解决可靠远程操作，打通了：
1. **USB gadget 网络**（USB-NCM，192.168.42.1）
2. **SSH over USB**（交叉编译 dropbear）
3. **网线直连**（eth0，192.168.50.1）

## 一、USB 网络根因（#70）

### 误区
误以为新固件"缺 USB gadget 模块"（无 usb_f_* .ko）。实际**内核已内建支持**（`CONFIG_USB_DWC2/CONFIG_USB_CONFIGFS/CONFIG_USB_F_ECM` 全是 `=y`，编译进内核非模块）。

### 真正根因
dwc2 默认在 **host 模式**运行（`drivers/usb/dwc2/platform.c` probe 时 `id_override=0` 初始化成 host）。不作 device 角色 → 开发机枚举不到 USB 网卡。

### 解决（一行命令）
```bash
echo device > /proc/cviusb/otg_role   # 切换 dvice 角色
```
切换后开发机立即枚举出 `1d6b:0104 Multifunction Composite Gadget` + USB 网卡（如 `enp0s20f0u4`）。

### 网络配置
```bash
# 评估板 (usb0)
ip addr add 192.168.42.1/24 dev usb0
ip link set usb0 up
# 开发机 (usb 网卡, 名字随机 enp*s*u*)
ip addr add 192.168.42.2/24 dev <usb_if>
ip link set <usb_if> up
```
ping 通（0.2-0.4ms）。

## 二、SSH over USB（交叉编译 dropbear）

### 坑：评估板无 SSH 服务端
新固件无 dropbear/sshd。用旧 SDK 的 musl_riscv64 dropbear 报 `not found`——链接器名不匹配（旧要求 `ld-musl-riscv64v_xthead.so.1`，评估板是 `ld-musl-riscv64v0p7_xthead.so.1`）。

### 解决：交叉编译 dropbear v2022.83
```bash
# GitHub mirror 下载源码, 用 Xuantie-900 工具链
./configure --host=riscv64-linux-musl --disable-zlib --disable-pam \
  --disable-lastlog --disable-syslog --without-libtomcrypt-dev
make PROGRAMS="dropbear dropbearkey" MULTI=0
```
关键编译 flag：`-mcpu=c906fdv -march=rv64imafdcv0p7xthead -mcmodel=medany`（匹配评估板 musl interpreter）。

### 部署（评估板）
```bash
mkdir -p /etc/dropbear
cp dropbear dropbearkey /usr/sbin/
chmod 755 /usr/sbin/dropbear /usr/sbin/dropbearkey
/usr/sbin/dropbear -R -p 22 -r /etc/dropbear/dropbear_ed25519_host_key &
# root 密码设为 milkv
```

## 三、开机自启固化

评估板 `/etc/init.d/S99usbnet`：挂载 configfs → 切 device 角色 → 建 ECM gadget → 配 usb0 IP → 启动 dropbear。

**重启验证**：断电重启后自动作为 device 枚举，USB 网卡自动出现，SSH over USB 自动可用，host key 持久化。

## 四、开发机一键脚本

`code/build/usbnet_ssh.sh`：
```bash
./usbnet_ssh.sh          # 自动找 USB 网卡, 配 IP, SSH 进入评估板
./usbnet_ssh.sh --scp <file>   # 传文件(dropbear 无 sftp, 用 ssh 管道)
./usbnet_ssh.sh --ip     # 只配 IP
```

## 五、网线直连（最稳定）

评估板 `eth0=192.168.50.1/24`，开发机 `enp2s0=192.168.50.2/24`。
```bash
ip addr add 192.168.50.2/24 dev enp2s0
sshpass -p milkv ssh root@192.168.50.1   # 稳定 SSH
```
比 USB 更可靠，推荐日常使用。

## 六、WiFi（备用）

```bash
ip link set wlan0 up
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211
dhcpcd wlan0   # 或 udhcpc -i wlan0 -q -n
```

## 七、给复现者的话

1. **USB 网络"缺的不是模块是角色切换"**：`echo device > /proc/cviusb/otg_role`。
2. **SSH 服务端要交叉编译**（评估板无现成二进制），链接器必须匹配 `v0p7_xthead`。
3. **网线直连最稳**，优先用 `192.168.50.1`。
4. `/etc/init.d/S99usbnet` 已固化开机自启，掉电重启自动恢复。

## 关键文件

| 文件 | 说明 |
|------|------|
| deploy/S99usbnet | 评估板开机自启（USB 网络 + dropbear） |
| deploy/usbnet_ssh.sh | 开发机一键连接脚本 |
| deploy/dropbear.riscv64-musl | 交叉编译的 dropbear |
| code/build/usbnet_boot.sh | USB 启动配置 |
