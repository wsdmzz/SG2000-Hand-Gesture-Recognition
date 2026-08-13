/*
 * tdl_gesture_cls_test.c - 新版 TDL_* API 手势分类测试（修正版）
 * 正确流程：TDL_CreateHandle → TDL_SetModelDir → TDL_OpenModel(model_path=NULL 自动定位)
 * 模型路径: <model_dir>/cv181x/cls_hand_gesture_128_128_INT8_cv181x.cvimodel
 * 用法:    ./tdl_gesture_cls_test <model_dir> <input.jpg>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tdl_sdk.h"
#include "c_apis/tdl_types.h"
#include "c_apis/tdl_model_def.h"
#include "c_apis/tdl_utils.h"

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <model_dir> <input_image.jpg>\n", argv[0]);
    return -1;
  }

  TDLHandle tdl_handle = TDL_CreateHandle(0);
  if (!tdl_handle) {
    printf("TDL_CreateHandle failed\n");
    return -1;
  }
  printf("TDL_CreateHandle OK\n");

  /* 设置模型目录（tdl_models 仓库风格，下面含 cv181x 子目录） */
  int ret = TDL_SetModelDir(tdl_handle, argv[1]);
  printf("TDL_SetModelDir(%s) ret=%d\n", argv[1], ret);

  /* 读图 */
  TDLImage image = TDL_ReadImage(argv[2]);
  if (!image) {
    printf("TDL_ReadImage failed: %s\n", argv[2]);
    TDL_DestroyHandle(tdl_handle);
    return -1;
  }
  printf("TDL_ReadImage OK\n");

  /* OpenModel：传实际模型路径（该 tdl_sdk 版本 model_path=NULL 有 bug 崩溃），config 传 NULL 用默认 */
  const char *model_path = "/tmp/cv181x/cls_hand_gesture_128_128_INT8_cv181x.cvimodel";
  ret = TDL_OpenModel(tdl_handle, TDL_MODEL_CLS_HAND_GESTURE, model_path, NULL, 0);
  printf("TDL_OpenModel(HAND_GESTURE, %s) ret=%d\n", model_path, ret);
  if (ret != 0) {
    TDL_DestroyImage(image);
    TDL_DestroyHandle(tdl_handle);
    return -1;
  }
  printf("TDL_OpenModel OK\n");

  TDL_SetModelThreshold(tdl_handle, TDL_MODEL_CLS_HAND_GESTURE, 0.01);

  TDLClassInfo obj_info = {0};
  ret = TDL_Classification(tdl_handle, TDL_MODEL_CLS_HAND_GESTURE, image, &obj_info);
  printf("TDL_Classification ret=%d\n", ret);
  if (ret == 0) {
    const char *name[4] = {"fist", "five", "none", "two"};
    int cls = obj_info.class_id;
    printf("RESULT: class_id=%d(%s) score=%f\n", cls,
           (cls >= 0 && cls < 4) ? name[cls] : "?", obj_info.score);
  } else {
    printf("RESULT: failed 0x%x\n", ret);
  }

  TDL_CloseModel(tdl_handle, TDL_MODEL_CLS_HAND_GESTURE);
  TDL_DestroyImage(image);
  TDL_DestroyHandle(tdl_handle);
  return 0;
}
