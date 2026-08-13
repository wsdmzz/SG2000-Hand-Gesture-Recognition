#!/bin/bash
# 编译 collect_gesture：7类手势数据采集程序（评估板摄像头实时采集）
# 复用 tdl_gesture_stream 的连接配置(sample_common 静态 + opencv + media 库)
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
  -I$SDK/sample/tpu/include \
  -I$SDK/sample/utils/include \
  -I$SDK/sample/3rd/opencv/include \
  -I$MW/include -I$MW/include/isp/cv181x -I$MW/include/linux \
  -I$MW/component/isp/common -I$MW/component/panel/cv181x -I$MW/3rdparty/inih \
  -I$MW/sample/common"

echo "########## 编译 sample_common (gcc, C) ##########"
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
  base=$(basename "$c" .c); echo "  [gcc] $base.c"
  $GCC $CFLAGS -c "$c" -o "$OUT/cg_$base.o"
  OBJS="$OBJS $OUT/cg_$base.o"
done

echo "########## 编译 collect_gesture.c (g++) ##########"
$GXX $CFLAGS -c /home/oooa/Milk-V-Duo/collect_gesture.c -o "$OUT/cg_collect_gesture.o"
OBJS="$OBJS $OUT/cg_collect_gesture.o"

echo "########## 链接 ##########"
$GXX -o "$OUT/collect_gesture" $OBJS \
  -Wl,-dynamic-linker,/lib/ld-musl-riscv64v0p7_xthead.so.1 \
  -L$SDK/lib -L$SDK/sample/utils/lib -L$SDK/sample/3rd/opencv/lib \
  -L$SDK/sample/3rd/zlib/lib -L$SDK/sample/cvi_rtsp/lib \
  -L$MW/lib -L$MW/lib/3rd -L$SDK/sample/tpu/lib -L$SDK/sample/cvi_mpi/lib \
  -Wl,--start-group -ltdl_core $SDK/sample/utils/lib/libtdl_utils.a -Wl,--end-group \
  -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
  -lsys -lvi -lvo -lvpss -lgdc -lrgn -lini -lsns_full -lsns_gc2083 -lisp -lisp_algo \
  -lae -laf -lawb -lcvi_bin -lcvi_bin_isp -lmisc -lvenc -lvdec -lcvi_ive \
  -lcviruntime -lcvikernel -lcnpy -latomic -lz -lcvi_rtsp \
  -Wl,-rpath,/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/data \
  -lpthread -ldl -lm -lstdc++ 2>&1 | tail -15
echo "BUILD_EXIT=${PIPESTATUS[0]}"
ls -la "$OUT/collect_gesture" 2>&1
