#!/bin/bash
# 编译 tdl_hand_only：只检测手部的最小验证程序（复用 tdl_gesture_stream 的链接配置）
set -e
OUT=/home/oooa/Milk-V-Duo/build_out/gc2083
mkdir -p "$OUT"

SDK=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/tdl_sdk/install/CV181X
MW=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/cvi_mpi
GXX=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++
GCC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc

CFLAGS="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -fPIC \
  -D__CV181X__ -DOS_IS_LINUX -DSENSOR_GCORE_GC2083 \
  -I$SDK/include -I$SDK/include/c_apis -I$SDK/include/nn \
  -I$SDK/sample/utils/include \
  -I$SDK/sample/3rd/opencv/include \
  -I$MW/include -I$MW/include/isp/cv181x -I$MW/include/linux \
  -I$MW/component/isp/common -I$MW/component/panel/cv181x -I$MW/3rdparty/inih \
  -I$MW/sample/common"

echo "########## sample_common (gcc) ##########"
OBJS=""
common="\
  $MW/sample/common/sample_common_sys.c \
  $MW/sample/common/sample_common_platform.c \
  $MW/sample/common/sample_common_vi.c \
  $MW/sample/common/sample_common_vpss.c \
  $MW/sample/common/sample_common_isp.c \
  $MW/sample/common/sample_common_vo.c \
  $MW/sample/common/sample_common_bin.c \
  $MW/sample/common/sample_common_peripheral.c \
  $MW/sample/common/sample_common_sensor.c"
for c in $common; do
  base=$(basename "$c" .c); $GCC $CFLAGS -c "$c" -o "$OUT/hand_$base.o"
  OBJS="$OBJS $OUT/hand_$base.o"
done

echo "########## tdl_hand_only.c (g++) ##########"
$GXX $CFLAGS -c /home/oooa/Milk-V-Duo/tdl_hand_only.c -o "$OUT/hand_main.o"
OBJS="$OBJS $OUT/hand_main.o"

echo "########## 链接 ##########"
$GXX -o "$OUT/tdl_hand_only" $OBJS \
  -Wl,-dynamic-linker,/lib/ld-musl-riscv64v0p7_xthead.so.1 \
  -L$SDK/lib -L$SDK/sample/utils/lib -L$SDK/sample/3rd/opencv/lib \
  -L$SDK/sample/3rd/zlib/lib -L$SDK/sample/cvi_rtsp/lib \
  -L$MW/lib -L$MW/lib/3rd -L$SDK/sample/tpu/lib -L$SDK/sample/cvi_mpi/lib \
  -Wl,--start-group \
    -ltdl_core \
  -Wl,--end-group \
  -lsys -lvi -lvo -lvpss -lgdc -lrgn -lini -lsns_full -lsns_gc2083 -lisp -lisp_algo \
  -lae -laf -lawb -lcvi_bin -lcvi_bin_isp -lmisc -lvenc -lvdec -lcvi_ive \
  -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
  -lcviruntime -lcvikernel -lcnpy -latomic -lz -lcvi_rtsp \
  -Wl,-rpath,/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/data \
  -lpthread -ldl -lm -lstdc++ 2>&1 | tail -15
echo "BUILD_EXIT=${PIPESTATUS[0]}"
ls -la "$OUT/tdl_hand_only" 2>&1