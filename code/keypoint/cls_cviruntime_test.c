/*
 * cls_cviruntime_test.c
 * 用 CVI_NN 底层 API 加载 cls_hand_gesture cvimodel，打印输入 tensor 配置
 * （shape/pixel_format/qscale/mean/scale/mem_size），并填零 Forward 验证推理链。
 * 目的：零猜测确定 cviruntime 推理的输入填充格式。
 * 用法: ./cls_cviruntime_test <cls_model.cvimodel>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cviruntime.h"

static const char *pf_str(CVI_NN_PIXEL_FORMAT_E p) {
  switch (p) {
    case CVI_NN_PIXEL_RGB_PACKED: return "RGB_PACKED";
    case CVI_NN_PIXEL_BGR_PACKED: return "BGR_PACKED";
    case CVI_NN_PIXEL_RGB_PLANAR: return "RGB_PLANAR";
    case CVI_NN_PIXEL_BGR_PLANAR: return "BGR_PLANAR";
    case CVI_NN_PIXEL_YUV_NV12: return "YUV_NV12";
    case CVI_NN_PIXEL_YUV_NV21: return "YUV_NV21";
    case CVI_NN_PIXEL_YUV_420_PLANAR: return "YUV_420_PLANAR";
    default: return "?";
  }
}

static void print_tensor(CVI_TENSOR *t, const char *tag) {
  printf("[%s] name=%s dims=%zu shape=[%d,%d,%d,%d] qscale=%.3f zp=%d mem_size=%zu\n",
         tag, t->name, t->shape.dim_size, t->shape.dim[0], t->shape.dim[1],
         t->shape.dim[2], t->shape.dim[3], t->qscale, t->zero_point, t->mem_size);
  printf("[%s] pixel_format=%s mean=[%g,%g,%g] scale=[%g,%g,%g]\n", tag, pf_str(t->pixel_format),
         t->mean[0], t->mean[1], t->mean[2], t->scale[0], t->scale[1], t->scale[2]);
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("Usage: %s <cvimodel>\n", argv[0]);
    return -1;
  }
  CVI_MODEL_HANDLE model = NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &model);
  printf("RegisterModel(%s) rc=%d\n", argv[1], rc);
  if (rc != CVI_RC_SUCCESS) {
    printf("RegisterModel failed\n");
    return -1;
  }
  int32_t major = 0, minor = 0;
  CVI_NN_GetModelVersion(model, &major, &minor);
  printf("model version %d.%d\n", major, minor);

  CVI_TENSOR *in = NULL, *out = NULL;
  int32_t in_num = 0, out_num = 0;
  rc = CVI_NN_GetInputOutputTensors(model, &in, &in_num, &out, &out_num);
  printf("GetInputOutputTensors rc=%d in_num=%d out_num=%d\n", rc, in_num, out_num);
  if (rc != CVI_RC_SUCCESS) return -1;

  print_tensor(&in[0], "INPUT");
  for (int i = 0; i < out_num; i++) print_tensor(&out[i], "OUT");

  /* 填零 Forward：验证推理链 */
  size_t nbytes = in[0].mem_size;
  if (in[0].shape.dim_size >= 4) {
    /* dims = [N,C,H,W] */
    int N = in[0].shape.dim[0], C = in[0].shape.dim[1], H = in[0].shape.dim[2], W = in[0].shape.dim[3];
    printf("input NCHW=%dx%dx%dx%d\n", N, C, H, W);
  }
  rc = CVI_NN_Forward(model, in, in_num, out, out_num);
  printf("Forward(zero input) rc=%d\n", rc);

  /* 打印输出原始值 */
  if (out_num > 0 && out[0].sys_mem) {
    int n = (int)(out[0].mem_size / (out[0].shape.dim[0] > 0 ? 1 : 1));
    int8_t *od = (int8_t *)out[0].sys_mem;
    int cnt = (int)(out[0].mem_size);
    int show = cnt > 16 ? 16 : cnt;
    printf("output raw[%d]:", (int)cnt);
    for (int i = 0; i < show; i++) printf(" %d", od[i]);
    printf("\n");
  }

  CVI_NN_CleanupModel(model);
  printf("done\n");
  return 0;
}
