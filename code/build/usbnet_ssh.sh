#!/bin/bash
# usbnet_ssh.sh - 开发机侧一键连接评估板 USB 网络 (Milk-V Duo S / SG2000)
#
# 用法:
#   ./usbnet_ssh.sh            # 配置 USB 网卡 IP 并 SSH 进入评估板
#   ./usbnet_ssh.sh --scp <f>  # 先配 IP 再用 scp 传文件到 /tmp
#   ./usbnet_ssh.sh --ip       # 只配置 USB 网卡 IP, 不 SSH
#
# 原理:
#   评估板开机自启(S99usbnet)已把 USB 控制器切到 device 并启用 ECM 网络
#   (usb0=192.168.42.1)。开发机插上 USB 数据线后会出现一个 usb 网卡
#   (enp*s*u*, 名字/MAC 每次枚举会变), 本脚本自动找到它并配 192.168.42.2/24。
#
# 依赖: sshpass (sudo 检测), root 密码 milkv

BOARD_IP=192.168.42.1
HOST_IP=192.168.42.2
PW=milkv
SUDO=""
[ "$(id -u)" != "0" ] && SUDO="sudo"

# 找 USB 网卡接口(名字含 'u' 且不是已用网卡; 优先 CDC cdc_ether)
find_usb_if() {
    for iface in $(ls /sys/class/net/ | grep -E '^enp.*u[0-9]+$' 2>/dev/null); do
        # 已被分配 42.42.42 网段则跳过(说明已配)
        if ip addr show "$iface" 2>/dev/null | grep -q "$HOST_IP"; then
            echo "$iface"; return 0
        fi
    done
    # 否则选第一个 usb 网卡
    for iface in $(ls /sys/class/net/ | grep -E '^enp.*u[0-9]+$' 2>/dev/null); do
        state=$(cat /sys/class/net/$iface/operstate 2>/dev/null)
        if [ "$state" = "up" ] || [ "$state" = "unknown" ]; then
            echo "$iface"; return 0
        fi
    done
    return 1
}

# 配置 USB 网卡 IP
setup_ip() {
    local iface
    iface=$(find_usb_if)
    if [ -z "$iface" ]; then
        echo "[USB-NET] 未找到 USB 网卡。请确认:" >&2
        echo "          1) 评估板 USB-C(device)口已用数据线接到开发机 USB-A 口" >&2
        echo "          2) 评估板已开机(上电后 ~60 秒内 gadget 会枚举)" >&2
        return 1
    fi
    # 检查是否需要 sudo(sudo 自举: 若 sudo -n 需密码则提示一次)
    if ! ip addr show "$iface" 2>/dev/null | grep -q "$HOST_IP/"; then
        # 加 IP 需要 root
        ip addr add $HOST_IP/24 dev "$iface" 2>/dev/null || \
            $SUDO ip addr add $HOST_IP/24 dev "$iface"
        ip link set "$iface" up 2>/dev/null || $SUDO ip link set "$iface" up
        echo "[USB-NET] $iface => $HOST_IP/24 (评估板 $BOARD_IP)"
    else
        echo "[USB-NET] $iface 已配置 $HOST_IP/24"
    fi
    echo "$iface"
}

case "$1" in
  --ip)
    setup_ip
    ;;
  --scp)
    # dropbear 默认无 sftp-server, scp 不可用; 改用 ssh 管道传文件
    # 用法: usbnet_ssh.sh --scp <file1> [file2 ...]   -> 传到评估板 /tmp/
    iface=$(setup_ip) || exit 1
    shift
    [ $# -eq 0 ] && { echo "[USB-NET] 用法: --scp <file...>"; exit 1; }
    for f in "$@"; do
        [ -f "$f" ] || { echo "[USB-NET] 跳过不存在: $f"; continue; }
        base=$(basename "$f")
        sshpass -p $PW ssh -o StrictHostKeyChecking=accept-new \
            root@$BOARD_IP "cat > /tmp/$base" < "$f" && \
            echo "[USB-NET] 已传 $f -> 评估板:/tmp/$base"
    done
    ;;
  *)
    iface=$(setup_ip) || exit 1
    # ping 确认
    if ! ping -c 2 -W 2 $BOARD_IP >/dev/null 2>&1; then
        echo "[USB-NET] 无法 ping 通评估板 $BOARD_IP" >&2
        exit 1
    fi
    echo "[USB-NET] 连入评估板 (ctrl-d 退出)..."
    sshpass -p $PW ssh -o StrictHostKeyChecking=accept-new root@$BOARD_IP
    ;;
esac
exit 0
