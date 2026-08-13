/*
 * tdl_cls_conflict.c - 最小化定位 cviruntime 与 TDL 库的冲突来源。
 * 步骤：
 *   1. 仅初始化 TDL（CreateHandle2 + OpenModel 手部模型，不初始化 VI/VPSS 框架）
 *   2. 用裸 cviruntime 注册分类模型 + GetInputOutputTensors
 *   3. 打印 out_num（若正常=1，说明冲突来自 VI/VPSS 框架或后续；若异常，来自 TDL 模型加载）
 * 用法: ./tdl_cls_conflict <hand_model.cvimodel> <cls_model.cvimodel>
 */
#include <stdio.h>
#include <stdlib.h>
#include "cvi_tdl.h"
#include "cviruntime.h"

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <hand_model> <cls_model>\n", argv[0]);
    return -1;
  }

  printf("=== Step1: 初始化 TDL 并加载手部模型 ===\n");
  cvitdl_handle_t tdl = NULL;
  CVI_S32 rc = CVI_TDL_CreateHandle2(&tdl, 1, 0);
  printf("CreateHandle2 rc=%x\n", rc);
  rc = CVI_TDL_SetVBPool(tdl, 0, 2);
  printf("SetVBPool rc=%x\n", rc);
  rc = CVI_TDL_OpenModel(tdl, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, argv[1]);
  printf("TDL_OpenModel(hand) rc=%x\n", rc);
  if (rc != CVI_SUCCESS) return -1;

  printf("=== Step2: 裸 cviruntime 注册分类模型 ===\n");
  CVI_MODEL_HANDLE m = NULL;
  CVI_RC rc2 = CVI_NN_RegisterModel(argv[2], &m);
  printf("CVI_NN_RegisterModel rc=%d\n", rc2);
  if (rc2 != CVI_RC_SUCCESS) {
    printf("RegisterModel failed -> 冲突发生在 cviruntime RegisterModel\n");
    return -1;
  }
  CVI_TENSOR *in = NULL, *out = NULL;
  int32_t in_num = 0, out_num = 0;
  rc2 = CVI_NN_GetInputOutputTensors(m, &in, &in_num, &out, &out_num);
  printf("GetInputOutputTensors rc=%d in_num=%d out_num=%d\n", rc2, in_num, out_num);
  if (rc2 == CVI_RC_SUCCESS && out_num >= 1) {
    printf("out qscale=%.3f mem=%zu\n", out[0].qscale, out[0].mem_size);
    printf("=> cviruntime 注册/取tensor 正常，冲突不在 TDL 模型加载\n");
  } else {
    printf("=> GetInputOutputTensors 异常（out_num=%d），冲突发生在 TDL 加载之后\n", out_num);
  }

  CVI_NN_CleanupModel(m);
  CVI_TDL_DestroyHandle(tdl);
  printf("done\n");
  return 0;
}
