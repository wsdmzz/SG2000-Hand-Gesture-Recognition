#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c_apis/tdl_sdk.h"

/* 7 类手势名 */
static const char *gname7(int id) {
  switch (id) {
    case 0: return "fist";
    case 1: return "number1";
    case 2: return "number2";
    case 3: return "number3";
    case 4: return "number4";
    case 5: return "number5";
    case 6: return "thumbs-up";
    default: return "?";
  }
}

int main(int argc, char *argv[]) {
  /* 用法: ./tdl_cls7 <cvimodel> <img> <model_factory.json> */
  if (argc < 3) {
    printf("Usage: %s <cvimodel> <image> [model_factory.json]\n", argv[0]);
    return -1;
  }
  const char *json_cfg = (argc >= 4) ? argv[3] : NULL;

  TDLHandle h = TDL_CreateHandle(0);
  if (h == NULL) { printf("CreateHandle failed\n"); return -1; }

  /* 直接用固定 model_id, 绕开文件名匹配限制 */
  int rc = TDL_OpenModel(h, TDL_MODEL_CLS_HAND_GESTURE, argv[1], json_cfg, 0);
  if (rc != 0) {
    printf("TDL_OpenModel failed rc=%#x\n", rc);
    TDL_DestroyHandle(h);
    return -1;
  }
  printf("TDL_OpenModel OK (7cls)\n");

  TDL_SetModelThreshold(h, TDL_MODEL_CLS_HAND_GESTURE, 0.5);

  TDLImage img = TDL_ReadImage(argv[2]);
  if (!img) {
    printf("TDL_ReadImage failed: %s\n", argv[2]);
    TDL_CloseModel(h, TDL_MODEL_CLS_HAND_GESTURE);
    TDL_DestroyHandle(h);
    return -1;
  }
  printf("TDL_ReadImage OK\n");

  TDLClassInfo info = {0};
  rc = TDL_Classification(h, TDL_MODEL_CLS_HAND_GESTURE, img, &info);
  if (rc != 0) {
    printf("TDL_Classification failed rc=%#x\n", rc);
  } else {
    printf(">>> gesture: %s (label=%d score=%.4f)\n",
           gname7((int)info.class_id), (int)info.class_id, info.score);
  }

  TDL_DestroyImage(img);
  TDL_CloseModel(h, TDL_MODEL_CLS_HAND_GESTURE);
  TDL_DestroyHandle(h);
  return 0;
}
