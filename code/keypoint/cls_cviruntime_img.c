/*
 * cls_cviruntime_img.c
 * 最小输入验证：读一张手势图 → 转 128×128 RGB，量化为 int8 NCHW 填充 CVI_NN 输入
 * → Forward → 解析 4 类 float32 softmax 输出。
 * 目的：确认输入填充公式（pixel/48.107）能正确识别手势类别。
 * 用 OpenCV(3.2, 板上 /root/tpu_verify 自带) 读图与缩放。
 * 用法: ./cls_cviruntime_img <cls_model.cvimodel> <img.jpg>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "cviruntime.h"
#include <opencv2/opencv.hpp>

static const char *class_names[4] = {"fist(拳头)", "five(五指)", "none(无)", "two(二)"};

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <cvimodel> <image.jpg>\n", argv[0]);
    return -1;
  }
  /* 读图 → BGR 128x128 */
  cv::Mat img = cv::imread(argv[2], cv::IMREAD_COLOR);
  if (img.empty()) {
    printf("cannot read %s\n", argv[2]);
    return -1;
  }
  cv::Mat resized;
  cv::resize(img, resized, cv::Size(128, 128), 0, 0, cv::INTER_LINEAR);
  printf("img %dx%d -> 128x128 BGR\n", img.cols, img.rows);

  CVI_MODEL_HANDLE model = NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &model);
  printf("RegisterModel rc=%d\n", rc);
  if (rc != CVI_RC_SUCCESS) return -1;

  CVI_TENSOR *in = NULL, *out = NULL;
  int32_t in_num = 0, out_num = 0;
  rc = CVI_NN_GetInputOutputTensors(model, &in, &in_num, &out, &out_num);
  if (rc != CVI_RC_SUCCESS) return -1;
  printf("in qscale=%.3f shape=[1,%d,%d,%d] out_num=%d\n", in[0].qscale,
         in[0].shape.dim[1], in[0].shape.dim[2], in[0].shape.dim[3], out_num);

  /* 填充输入：BGR 转 RGB，量化为 int8 = round(pixel / qscale)，按 NCHW */
  float q = in[0].qscale;
  int C = in[0].shape.dim[1], H = in[0].shape.dim[2], W = in[0].shape.dim[3];
  uint8_t *ib = (uint8_t *)in[0].sys_mem;
  for (int c = 0; c < C; c++) {
    for (int h = 0; h < H; h++) {
      const uchar *row = resized.ptr<uchar>(h);
      for (int w = 0; w < W; w++) {
        float val = 0.f;
        if (c == 0)      val = row[w * 3 + 2]; /* R <- BGR[2] */
        else if (c == 1) val = row[w * 3 + 1]; /* G <- BGR[1] */
        else             val = row[w * 3 + 0]; /* B <- BGR[0] */
        /* int8 量化；模型训练通常 scale = 1/128 即 pixel/128，但这里探测 qscale=48.1 */
        int qv = (int)lrintf(val / q);
        if (qv < -128) qv = -128;
        if (qv > 127) qv = 127;
        ib[(c * H + h) * W + w] = (int8_t)qv;
      }
    }
  }

  rc = CVI_NN_Forward(model, in, in_num, out, out_num);
  printf("Forward rc=%d\n", rc);
  if (rc != CVI_RC_SUCCESS) return -1;

  /* 输出：float32 softmax [1,4,1,1]，直接解析 */
  if (out_num > 0 && out[0].sys_mem) {
    float *od = (float *)out[0].sys_mem;
    int cnt = (int)(out[0].mem_size / 4);
    int best = 0;
    float bestp = -1.f;
    printf("softmax probs[%d]:", cnt);
    for (int i = 0; i < cnt; i++) {
      printf(" %.3f", od[i]);
      if (od[i] > bestp) { bestp = od[i]; best = i; }
    }
    printf("\n>>> 手势识别结果: %s (prob=%.3f)\n", class_names[best], bestp);
  }

  CVI_NN_CleanupModel(model);
  return 0;
}
