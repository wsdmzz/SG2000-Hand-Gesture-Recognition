#!/bin/bash
# build_collector.sh — 交叉编译挂墙三任务采集器（纯 C，无 TDL/OpenCV）
#
# 核心六模块 + platform_cvitek.c（VI/VPSS/VENC/VO/GT911）+ 所需 sample_common_*.c
# 静态编入，链接 cvi_mpi 媒体库。模板沿用 build_tdl_gesture_kpt_stream.sh，
# 去掉 TDL/OpenCV/rtsp 部分。产物：06_build/out_collector/collector
#
# 用法：bash build_collector.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
COL="$HERE/../05_app/collector"
OUT="$HERE/out_collector"
mkdir -p "$OUT"

SDK=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/tdl_sdk/install/CV181X
MW=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/cvi_mpi
TC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin
GCC=$TC/riscv64-unknown-linux-musl-gcc
GXX=$TC/riscv64-unknown-linux-musl-g++

CFLAGS="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -fPIC \
  -D__CV181X__ -DOS_IS_LINUX -DSENSOR_GCORE_GC2083 \
  -I$COL/include \
  -I$MW/include -I$MW/include/isp/cv181x -I$MW/include/linux \
  -I$MW/component/isp/common -I$MW/component/panel/cv181x -I$MW/3rdparty/inih \
  -I$MW/sample/common"

# 字库（缺失则用系统 Noto CJK 重新生成）
if [ ! -f "$COL/include/ui_font.h" ]; then
  echo "########## 生成 ui_font.h ##########"
  python3 "$COL/gen_ui_font.py"
fi

echo "########## 编译 sample_common (gcc) ##########"
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
  $GCC $CFLAGS -c "$c" -o "$OUT/$base.o"
  OBJS="$OBJS $OUT/$base.o"
done

echo "########## 编译 collector 核心 + 平台 (gcc) ##########"
for c in config storage state ui_model main platform_cvitek; do
  echo "  [gcc] $c.c"
  $GCC $CFLAGS -c "$COL/$c.c" -o "$OUT/col_$c.o"
  OBJS="$OBJS $OUT/col_$c.o"
done

echo "########## 链接 (g++) ##########"
$GXX -o "$OUT/collector" $OBJS \
  -Wl,-dynamic-linker,/lib/ld-musl-riscv64v0p7_xthead.so.1 \
  -L$MW/lib -L$MW/lib/3rd \
  -L$SDK/sample/tpu/lib -L$SDK/sample/3rd/zlib/lib \
  -lsys -lvi -lvo -lvpss -lgdc -lrgn -lini -lsns_full -lsns_gc2083 -lisp -lisp_algo \
  -lae -laf -lawb -lcvi_bin -lcvi_bin_isp -lmisc -lvenc -lvdec -lcvi_ive \
  -lcviruntime -lcvikernel -lcnpy -latomic -lz \
  -Wl,-rpath,/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/data \
  -lpthread -ldl -lm -lstdc++ 2>&1 | tail -25
echo "BUILD_EXIT=${PIPESTATUS[0]}"
ls -la "$OUT/collector" 2>&1