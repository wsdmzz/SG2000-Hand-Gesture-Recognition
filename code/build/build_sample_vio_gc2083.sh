#!/bin/bash
# 手工重编 sample_vio，强制启用 GC2083/GC4653 sensor 支持（Milk-V Duo S / SG2000，duos 板型 build/.config）
# 背景：SDK 的 sample/vio Makefile 宏注入链破碎，直接 make 产出的 GetSnsObj 为空壳（GC2083 分支被裁剪）。
# 本脚本直接用正确宏 -DSENSOR_GCORE_GC2083 编译全部所需源文件并链接 cvi_mpi 静态库。
set -e

S=b_plan/01_sdk/sophpi/cvi_mpi
BUILD_DIR=$S/sample/vio
OUT_DIR=/home/oooa/Milk-V-Duo/build_out/gc2083
mkdir -p "$OUT_DIR"

GCC=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
GXX=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++
KINC=$S/../linux_5.10/build/sg2000_milkv_duos_musl_riscv64_sd/riscv/usr/include
SENSOR_LIST=$S/component/isp/common
PANEL=$S/component/panel/cv181x
MW_INC=$S/include
ISP_INC=$S/include/isp/cv181x

# 通用编译 flags（含内核头、sensor 头、macro 注入）
CFLAGS_COMMON="-Os -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -fPIC -ffunction-sections -fdata-sections \
  -D__CV181X__ -DOS_IS_LINUX \
  -DSENSOR_GCORE_GC2083 \
  -I$MW_INC -I$ISP_INC -I$S/include/linux -I$S/3rdparty/inih \
  -I$KINC -I$SENSOR_LIST -I$PANEL -I$S/sample/common"

echo "########## [1/2] 编译 sample_vio 所需源文件 ##########"
cfiles="\
  $S/sample/vio/sample_vio_main.c \
  $S/sample/vio/sample_vio.c \
  $S/sample/common/sample_common_vo.c \
  $S/sample/common/sample_common_sys.c \
  $S/sample/common/sample_common_platform.c \
  $S/sample/common/sample_common_vpss.c \
  $S/sample/common/sample_common_vi.c \
  $S/sample/common/sample_common_isp.c \
  $S/sample/common/sample_common_bin.c \
  $S/sample/common/sample_common_peripheral.c \
  $S/sample/common/sample_common_sensor.c"

for c in $cfiles; do
  base=$(basename "$c" .c)
  echo "  [gcc] $base.c"
  $GCC $CFLAGS_COMMON -c "$c" -o "$OUT_DIR/$base.o"
done

echo "########## [2/2] 链接 sample_vio ##########"
OBJS=""
for c in $cfiles; do
  base=$(basename "$c" .c)
  OBJS="$OBJS $OUT_DIR/$base.o"
done

# 中间静态库全列表（含 isp 三件套 + sns_full），--start-group 解决循环引用
LDFLAGS_LIBS="\
  $S/lib/libsys.a $S/lib/libvi.a $S/lib/libvo.a $S/lib/libvpss.a $S/lib/librgn.a $S/lib/libgdc.a \
  $S/lib/libvenc.a $S/lib/libcvi_bin.a $S/lib/libcvi_bin_isp.a \
  $S/lib/libisp.a $S/lib/libisp_algo.a $S/lib/libae.a $S/lib/libaf.a $S/lib/libawb.a \
  $S/lib/libsns_full.a \
  $S/lib/libmisc.a $S/lib/3rd/libini.a"

$GXX -o "$OUT_DIR/sample_vio" \
  -Wl,--start-group $OBJS $LDFLAGS_LIBS -Wl,--end-group \
  -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d \
  -static -lpthread -lm -ldl -latomic

echo "########## 产物 ##########"
ls -la "$OUT_DIR/sample_vio"
file "$OUT_DIR/sample_vio"

echo "########## 验证 GC2083 支持 ##########"
NM=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-nm
echo "--- GetSnsObj 反汇编(应含 GC2083 分支, 检查 sensor type 38 等) ---"
OBJDUMP=/home/oooa/Milk-V-Duo/ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-objdump
$OBJDUMP -d "$OUT_DIR/sample_vio" 2>/dev/null | awk '/<SAMPLE_COMM_SNS_GetSnsObj>:/{f=1} f{print} f&&/ret/{c++} c>=2{exit}' | head -12
echo "--- GC2083 sensor 对象符号 ---"
$NM "$OUT_DIR/sample_vio" 2>/dev/null | grep -iE "stSnsGc2083_Obj|gc2083_init" | head
