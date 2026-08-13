#!/bin/bash
# 编译 test_img_hand_cls（duo 库方案，能编译；运行时加载固件库 + LD_PRELOAD 全框架）
set -e
cd /home/oooa/Milk-V-Duo/ai/duo-tdl-examples
CC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
$CC -mcpu=c906fdv -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d \
  -O3 -DNDEBUG -Iinclude/system -Iinclude/tdl -DCV181X -D__CV181X__ \
  -o test_img_hand_cls test_img_hand_cls.c \
  -Wl,--allow-shlib-undefined \
  -Llibs/system/musl_riscv64 -Llibs/tdl/cv181x_riscv64 \
  -lcvi_tdl -lcvi_ive -lvpss -lvo -lsys \
  -lcvikernel -lcvimath -lcviruntime -latomic -lpthread \
  -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lm -lrt
echo "BUILD_OK"
