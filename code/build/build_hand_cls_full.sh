#!/bin/bash
# 编译 test_img_hand_cls（板上库全链方案，复刻固件 NEEDED）
# 解决 __sync_fetch_and_add_1 hidden symbol 冲突：
#   用工具链 sysroot 的共享 libatomic.so.1 / libstdc++，避免从 libgcc.a 静态拉入 hidden atomic
set -e
cd /home/oooa/Milk-V-Duo/ai/duo-tdl-examples
CC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
SYS=/tmp/boardlib/sys
TPU=/tmp/boardlib/tpu
TLS=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/sysroot/lib64v0p7_xthead/lp64d
$CC -mcpu=c906fdv -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d \
  -O3 -DNDEBUG -Iinclude/system -Iinclude/tdl -DCV181X -D__CV181X__ \
  -o test_img_hand_cls test_img_hand_cls.c \
  /home/oooa/Milk-V-Duo/atomic_shim.so \
  -Wl,--allow-shlib-undefined \
  -L$SYS -L$TPU \
  -lcvi_tdl -lcvi_ive -lsys -lvi -lvo -lvpss -lgdc -lrgn \
  -lini -lsns_full -lsample -lisp -lvenc -lvdec -lawb -lae -laf \
  -lcvi_bin_isp -lcvi_bin -lmisc -lisp_algo -lcnpy \
  -lcvikernel -lcvimath -lcviruntime -lpthread \
  -Wl,-rpath-link,$TLS \
  -L$TLS -l:libatomic.so.1 -lstdc++ \
  -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lz -lm -lrt
echo "BUILD_OK"
