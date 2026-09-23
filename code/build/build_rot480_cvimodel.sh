#!/bin/bash
# build_rot480_cvimodel.sh — best_rot_raw_fixed.onnx(480) → cv181x INT8 sym cvimodel
# 旋转增强重训模型 (models/best_rot.onnx) 的转换脚本
set -e

REPO=/home/oooa/Milk-V-Duo
SRC=${REPO}/models/best_rot_raw_fixed.onnx
WORK=${REPO}/ai/tpu-mlir-master/tmp/bestrot480
CALI_IMG=${REPO}/ai/tpu-mlir-master/tmp/best480/cali_images
SIZE=480
CHIP=cv181x
CONTAINER=sophgo/tpuc_dev:v3.1

C_WORK=/workspace/ai/tpu-mlir-master/tmp/bestrot480
C_SRC=/workspace/models/best_rot_raw_fixed.onnx
C_CALI=/workspace/ai/tpu-mlir-master/tmp/best480/cali_images

mkdir -p "$WORK"

echo "=== 校验输入 ==="
[ -f "$SRC" ] || { echo "ERROR: $SRC 不存在"; exit 1; }
IMG_COUNT=$(ls "$CALI_IMG"/*.jpg 2>/dev/null | wc -l)
echo "ONNX:  $(du -h "$SRC" | cut -f1)"
echo "校准图: ${IMG_COUNT} 张 (复用 best480 的校准集)"
[ "$IMG_COUNT" -ge 50 ] || { echo "ERROR: 校准图不足 50 张"; exit 1; }

cat > "${WORK}/_env.sh" <<'EOF'
export TPUC_ROOT=/workspace/ai/tpu-mlir
export PATH=$TPUC_ROOT/bin:$TPUC_ROOT/python/tools:$TPUC_ROOT/python/utils:$PATH
export LD_LIBRARY_PATH=$TPUC_ROOT/lib:$LD_LIBRARY_PATH
export PYTHONPATH=$TPUC_ROOT/python:/usr/local/python_packages/mlir_core:/usr/local/python_packages/
EOF

run_in_container() {
  podman run --rm --security-opt label=disable \
    -v ${REPO}:/workspace \
    -w ${C_WORK} \
    ${CONTAINER} \
    bash -c "source ${C_WORK}/_env.sh && $1"
}

echo ""
echo "=== 步骤1: model_transform (ONNX → mlir) ==="
run_in_container "
  model_transform.py \
    --model_name best_rot_${SIZE} \
    --model_def ${C_SRC} \
    --input_shapes '[[1,3,${SIZE},${SIZE}]]' \
    --mlir ${C_WORK}/best_rot_${SIZE}.mlir \
    --resize_dims ${SIZE},${SIZE} \
    --keep_aspect_ratio \
    --pad_value 114 \
    --mean 0,0,0 \
    --scale 0.003921568627,0.003921568627,0.003921568627 \
    --pixel_format rgb \
    --test_input ${C_CALI}/c5_img_001.jpg \
    --test_result ${C_WORK}/output_top.npz
" 2>&1 | tail -22

echo ""
echo "=== 步骤2: run_calibration (PTQ) ==="
run_in_container "
  run_calibration.py \
    ${C_WORK}/best_rot_${SIZE}.mlir \
    --dataset ${C_CALI} \
    --input_num 200 \
    -o ${C_WORK}/best_rot_cali_table
" 2>&1 | tail -8

echo ""
echo "=== 步骤3: model_deploy (INT8 sym → cvimodel) ==="
run_in_container "
  model_deploy.py \
    --mlir ${C_WORK}/best_rot_${SIZE}.mlir \
    --quantize INT8 \
    --quant_input \
    --calibration_table ${C_WORK}/best_rot_cali_table \
    --chip ${CHIP} \
    --tolerance 0.85,0.45 \
    --model ${C_WORK}/best_rot_${SIZE}_int8_${CHIP}.cvimodel
" 2>&1 | tail -18

echo ""
echo "=== 完成 ==="
ls -la ${WORK}/best_rot_${SIZE}_int8_${CHIP}.cvimodel 2>/dev/null || { echo "ERROR: 未生成"; exit 1; }
cp ${WORK}/best_rot_${SIZE}_int8_${CHIP}.cvimodel ${REPO}/models/
echo "已拷贝到 ${REPO}/models/best_rot_${SIZE}_int8_${CHIP}.cvimodel"
