#!/bin/bash
# 串口辅助脚本：向评估板发送命令并抓取输出
# 用法: serial_cmd.sh "命令" [等待秒数]
# 用法(推荐): serial_cmd.sh -c "cmd1; cmd2" -w 6
DEV=/dev/ttyUSB0
BAUD=115200

CMD="$1"
WAIT=6
if [ "$1" = "-c" ]; then
    CMD="$2"
    WAIT="${3:-6}"
fi

# 配置串口
stty -F $DEV $BAUD cs8 -cstopb -parenb -echo 2>/dev/null

# 后台读取
OUT=/tmp/serial_cmd_$$.log
timeout $((WAIT+10)) cat $DEV > $OUT 2>/dev/null &
CB=$!
sleep 1

# 发送命令
printf "%s\r\n" "$CMD" > $DEV
sleep $WAIT

# 发一个标记行便于定位
printf "echo ===END_CAPTURE===\r\n" > $DEV
sleep 1

kill $CB 2>/dev/null
wait $CB 2>/dev/null
cat $OUT
rm -f $OUT
