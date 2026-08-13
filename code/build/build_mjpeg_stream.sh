#!/bin/bash
# 编译 mjpeg_stream：GC2083 摄像头 MJPEG over HTTP 低延时推流
set -e

S=b_plan/01_sdk/sophpi/cvi_mpi
OUT_DIR=/home/oooa/Milk-V-Duo/build_out/gc2083
mkdir -p "$OUT_DIR"

GCC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
GXX=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++
KINC=$S/../linux_5.10/build/sg2000_milkv_duos_musl_riscv64_sd/riscv/usr/include
SENSOR_LIST=$S/component/isp/common
PANEL=$S/component/panel/cv181x

CFLAGS_COMMON="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -fPIC -ffunction-sections -fdata-sections \
  -D__CV181X__ -DOS_IS_LINUX -DSENSOR_GCORE_GC2083 \
  -I$S/include -I$S/include/isp/cv181x -I$S/include/linux -I$S/3rdparty/inih \
  -I$KINC -I$SENSOR_LIST -I$PANEL -I$S/sample/common"

echo "########## 编译 mjpeg_stream 源文件 ##########"
cfiles="\
  /home/oooa/Milk-V-Duo/mjpeg_stream.c \
  $S/sample/common/sample_common_sys.c \
  $S/sample/common/sample_common_platform.c \
  $S/sample/common/sample_common_vi.c \
  $S/sample/common/sample_common_vpss.c \
  $S/sample/common/sample_common_isp.c \
  $S/sample/common/sample_common_vo.c \
  $S/sample/common/sample_common_bin.c \
  $S/sample/common/sample_common_peripheral.c \
  $S/sample/common/sample_common_sensor.c"

OBJS=""
for c in $cfiles; do
  base=$(basename "$c" .c)
  echo "  [gcc] $base.c"
  $GCC $CFLAGS_COMMON -c "$c" -o "$OUT_DIR/$base.o"
  OBJS="$OBJS $OUT_DIR/$base.o"
done

echo "########## 链接 ##########"
LDFLAGS_LIBS="\
  $S/lib/libsys.a $S/lib/libvi.a $S/lib/libvo.a $S/lib/libvpss.a $S/lib/librgn.a $S/lib/libgdc.a \
  $S/lib/libvenc.a $S/lib/libcvi_bin.a $S/lib/libcvi_bin_isp.a \
  $S/lib/libisp.a $S/lib/libisp_algo.a $S/lib/libae.a $S/lib/libaf.a $S/lib/libawb.a \
  $S/lib/libsns_full.a $S/lib/libmisc.a $S/lib/3rd/libini.a"

$GXX -o "$OUT_DIR/mjpeg_stream" \
  -Wl,--start-group $OBJS $LDFLAGS_LIBS -Wl,--end-group \
  -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d \
  -static -lpthread -lm -ldl -latomic

echo "########## 产物 ##########"
ls -la "$OUT_DIR/mjpeg_stream"
file "$OUT_DIR/mjpeg_stream"
md5sum "$OUT_DIR/mjpeg_stream"
