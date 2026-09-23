#!/bin/bash
# build_probe.sh — 编译 kpt_model_probe (离线模型探针，只依赖 cviruntime)
set -e
SDK=/home/oooa/Milk-V-Duo/b_plan/01_sdk/sophpi/tdl_sdk/install/CV181X
GCC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
OUT=/home/oooa/Milk-V-Duo/build_out/gc2083
mkdir -p "$OUT"

CFLAGS="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -fPIC \
  -D__CV181X__ -I$SDK/sample/tpu/include"

echo "=== 编译 kpt_model_probe ==="
$GCC $CFLAGS -o "$OUT/kpt_model_probe" /home/oooa/Milk-V-Duo/kpt_model_probe.c \
  -Wl,-dynamic-linker,/lib/ld-musl-riscv64v0p7_xthead.so.1 \
  -L$SDK/sample/tpu/lib -L$SDK/sample/3rd/zlib/lib \
  -lcviruntime -lcvikernel -lcnpy -lz -latomic \
  -Wl,-rpath,/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd \
  -lpthread -lm

ls -la "$OUT/kpt_model_probe"
echo "=== 完成 ==="
