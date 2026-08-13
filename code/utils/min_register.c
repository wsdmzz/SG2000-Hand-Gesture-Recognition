/* min_register.c - cinruntime 模型注册 + SetConfig 诊断 */
#include <stdio.h>
#include <stdlib.h>
#include "cviruntime.h"

int main(int argc, char *argv[]) {
  if (argc != 2) { printf("Usage: %s <model.cvimodel>\n", argv[0]); return -1; }
  CVI_MODEL_HANDLE m = NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &m);
  printf("RegisterModel rc=%d\n", (int)rc);
  if (rc != CVI_RC_SUCCESS) return -1;
  /* 复现 tdl_sdk setup 关键调用 */
  rc = CVI_NN_SetConfig(m, OPTION_OUTPUT_ALL_TENSORS, 0);
  printf("SetConfig(OUTPUT_ALL_TENSORS,0) rc=%d\n", (int)rc);
  CVI_TENSOR *in=NULL, *out=NULL;
  int32_t in_num=0, out_num=0;
  rc = CVI_NN_GetInputOutputTensors(m, &in, &in_num, &out, &out_num);
  printf("GetInputOutputTensors rc=%d in=%d out=%d\n", (int)rc, in_num, out_num);
  return 0;
}
