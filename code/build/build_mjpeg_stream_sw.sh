#!/bin/bash
# 编译 mjpeg_stream_sw：软编码 MJPEG 推流（VPSS 取帧 + OpenCV，绕开 VENC 内核 bug）
# sample_common_*.c 用 gcc 编译（C 语言，允许 void* 隐式转换）
# mjpeg_stream_sw.c 用 g++ 编译（需要 OpenCV C++ API），链接混合
set -e

S=b_plan/01_sdk/sophpi/cvi_mpi
OUT_DIR=/home/oooa/Milk-V-Duo/build_out/gc2083
OCV=/tmp/opencv-mobile-milkv/opencv-mobile-5.0.0-milkv-duo
mkdir -p "$OUT_DIR"

GCC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
GXX=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++
KINC=$S/../linux_5.10/build/sg2000_milkv_duos_musl_riscv64_sd/riscv/usr/include
SENSOR_LIST=$S/component/isp/common
PANEL=$S/component/panel/cv181x

CFLAGS_COMMON="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -fPIC \
  -D__CV181X__ -DOS_IS_LINUX -DSENSOR_GCORE_GC2083 \
  -I$S/include -I$S/include/isp/cv181x -I$S/include/linux -I$S/3rdparty/inih \
  -I$KINC -I$SENSOR_LIST -I$PANEL -I$S/sample/common \
  -I$OCV/include/opencv5"

echo "########## 编译源文件 ##########"
echo "--- [gcc] sample_common (C) ---"
common_files="\
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
for c in $common_files; do
  base=$(basename "$c" .c)
  echo "  [gcc] $base.c"
  $GCC $CFLAGS_COMMON -c "$c" -o "$OUT_DIR/sw_$base.o"
  OBJS="$OBJS $OUT_DIR/sw_$base.o"
done

echo "--- [g++] mjpeg_stream_sw.c ---"
$GXX $CFLAGS_COMMON -c /home/oooa/Milk-V-Duo/mjpeg_stream_sw.c -o "$OUT_DIR/sw_mjpeg_stream_sw.o"
OBJS="$OBJS $OUT_DIR/sw_mjpeg_stream_sw.o"

echo "########## 链接 (g++) ##########"
LDFLAGS_LIBS="\
  $S/lib/libsys.a $S/lib/libvi.a $S/lib/libvo.a $S/lib/libvpss.a $S/lib/librgn.a $S/lib/libgdc.a \
  $S/lib/libvenc.a $S/lib/libcvi_bin.a $S/lib/libcvi_bin_isp.a \
  $S/lib/libisp.a $S/lib/libisp_algo.a $S/lib/libae.a $S/lib/libaf.a $S/lib/libawb.a \
  $S/lib/libsns_full.a $S/lib/libmisc.a $S/lib/3rd/libini.a"

$GXX -o "$OUT_DIR/mjpeg_stream_sw" \
  -Wl,--start-group $OBJS $LDFLAGS_LIBS \
    $OCV/lib/libopencv_highgui.a \
    $OCV/lib/libopencv_imgproc.a \
    $OCV/lib/libopencv_core.a \
    $OCV/lib/libopencv_features.a \
    $OCV/lib/libopencv_photo.a \
    $OCV/lib/libopencv_video.a \
    $OCV/lib/libopencv_geometry.a \
  -Wl,--end-group \
  -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d \
  -static -latomic -ldl -lpthread -lm -lstdc++

echo "########## 产物 ##########"
ls -la "$OUT_DIR/mjpeg_stream_sw"
md5sum "$OUT_DIR/mjpeg_stream_sw"
