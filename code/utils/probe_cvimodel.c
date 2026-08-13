/*
 * probe_cvimodel.c - 探测 cvimodel 的输入/输出张量规格（尺寸、分配、量化）
 * 用于确认 yolov8n_det_hand / cls_hand_gesture 在新固件 cviruntime 下的结构，
 * 是实现裸 cviruntime 推理（检测后处理/分类）的前提。
 * 用法: ./probe_cvimodel <model.cvimodel>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cviruntime.h"

static const char *memtype_str(CVI_MEM_TYPE_E t) {
  switch (t) {
    case CVI_MEM_SYSTEM: return "SYSTEM";
    case CVI_MEM_DEVICE: return "DEVICE";
    default: return "?";
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) { printf("Usage: %s <model.cvimodel>\n", argv[0]); return -1; }
  CVI_MODEL_HANDLE m = NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &m);
  printf("RegisterModel rc=%d\n", (int)rc);
  if (rc != CVI_RC_SUCCESS) return -1;

  CVI_NN_SetConfig(m, OPTION_OUTPUT_ALL_TENSORS, 0);

  CVI_TENSOR *in = NULL, *out = NULL;
  int32_t in_num = 0, out_num = 0;
  rc = CVI_NN_GetInputOutputTensors(m, &in, &in_num, &out, &out_num);
  printf("GetInputOutputTensors rc=%d in=%d out=%d\n", (int)rc, in_num, out_num);
  if (rc != CVI_RC_SUCCESS) return -1;

  printf("\n===== INPUT (in_num=%d) =====\n", in_num);
  for (int i = 0; i < in_num; i++) {
    CVI_TENSOR *t = &in[i];
    printf("[in%d] shape=[%d,%d,%d,%d] fmt=%d qscale=%.6f memsize=%zu memtype=%s\n",
      i, t->shape.dim[0], t->shape.dim[1], t->shape.dim[2], t->shape.dim[3],
      t->fmt, t->qscale, t->mem_size, memtype_str(t->mem_type));
  }

  printf("\n===== OUTPUT (out_num=%d) =====\n", out_num);
  for (int i = 0; i < out_num; i++) {
    CVI_TENSOR *t = &out[i];
    printf("[out%d] shape=[%d,%d,%d,%d] fmt=%d qscale=%.6f memsize=%zu memtype=%s\n",
      i, t->shape.dim[0], t->shape.dim[1], t->shape.dim[2], t->shape.dim[3],
      t->fmt, t->qscale, t->mem_size, memtype_str(t->mem_type));
  }
  CVI_NN_CleanupModel(m);
  return 0;
}
