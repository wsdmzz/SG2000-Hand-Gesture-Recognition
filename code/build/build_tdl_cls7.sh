#!/bin/bash
# 编译 tdl_cls7：用 TDL SDK 直接加载自定义 7 类分类模型并分类
# 无相机依赖（只 TDL_ReadImage 吃图片），不需要 sample_common
set -e
OUT=/home/oooa/Milk-V-Duo/build_out/gc2083
mkdir -p "$OUT"

SDK=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/tdl_sdk/install/CV181X
MW=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/cvi_mpi
GXX=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++

CFLAGS="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d \
  -D__CV181X__ -DOS_IS_LINUX -DSENSOR_GCORE_GC2083 \
  -I$SDK/include -I$SDK/include/c_apis -I$SDK/include/nn \
  -I$SDK/sample/utils/include \
  -I$SDK/sample/3rd/opencv/include \
  -I$MW/include -I$MW/include/isp/cv181x -I$MW/include/linux"

echo "########## 编译 tdl_cls7.c (g++) ##########"
$GXX $CFLAGS -o "$OUT/tdl_cls7" tdl_cls7.c \
  -L$SDK/lib -L$SDK/sample/utils/lib -L$SDK/sample/3rd/opencv/lib \
  -L$SDK/sample/3rd/zlib/lib -L$SDK/sample/cvi_rtsp/lib \
  -L$MW/lib -L$MW/lib/3rd -L$SDK/sample/tpu/lib -L$SDK/sample/cvi_mpi/lib \
  -Wl,-dynamic-linker,/lib/ld-musl-riscv64v0p7_xthead.so.1 \
  -ltdl_core \
  $SDK/sample/utils/lib/libtdl_utils.a \
  -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
  -lsys -lvi -lvo -lvpss -lgdc -lrgn -lini -lsns_full -lsns_gc2083 -lisp -lisp_algo \
  -lae -laf -lawb -lcvi_bin -lcvi_bin_isp -lmisc -lvenc -lvdec -lcvi_ive \
  -lcviruntime -lcvikernel -lcnpy -latomic -lz -lcvi_rtsp \
  -lpthread -ldl -lm -lstdc++ 2>&1 | tail -15

echo "BUILD_EXIT=${PIPESTATUS[0]}"
ls -la "$OUT/tdl_cls7" 2>&1
