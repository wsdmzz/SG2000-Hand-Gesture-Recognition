/*
 * cls7_norm_verify.c - 裸 cviruntime 验证 gesture_norm_sym.cvimodel(内置归一化)
 * 输入: fp32 0-255 RGB NCHW [1,3,128,128] (与 TDL 直通语义一致)
 * 用法: ./cls7_norm_verify <cvimodel> <img.jpg>
 * 目的: 排除 TDL 干扰, 直接在评估板真机验证新模型能否正确识别 7 类手势
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cviruntime.h"

static const char *classes[7] = {"fist","number1","number2","number3","number4","number5","thumbs-up"};

int main(int argc, char *argv[]) {
  if (argc != 3) { printf("Usage: %s <cvimodel> <img.jpg>\n", argv[0]); return -1; }
  FILE *fp = fopen(argv[2], "rb");
  if (!fp) { printf("cannot open %s\n", argv[2]); return -1; }
  fseek(fp,0,SEEK_END); long sz=ftell(fp); rewind(fp);
  unsigned char *jpg = malloc(sz); fread(jpg,1,sz,fp); fclose(fp);
  printf("img %ld bytes\n", sz);

  CVI_MODEL_HANDLE model=NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &model);
  printf("RegisterModel rc=%d\n", rc);
  if (rc != CVI_RC_SUCCESS) return -1;

  CVI_TENSOR *in=NULL,*out=NULL; int32_t in_num=0,out_num=0;
  CVI_NN_GetInputOutputTensors(model,&in,&in_num,&out,&out_num);
  printf("in_qscale=%.4f in_fmt=%d shape=[1,%d,%d,%d] out_num=%d\n",
         in[0].qscale,(int)in[0].fmt, in[0].shape.dim[1],in[0].shape.dim[2],in[0].shape.dim[3], out_num);

  int C = in[0].shape.dim[1], H = in[0].shape.dim[2], W = in[0].shape.dim[3];
  printf("input fmt=%d (CVI_FMT_FP32=%d UINT8=%d INT8=%d BF16=%d)\n",
         (int)in[0].fmt, (int)CVI_FMT_FP32, (int)CVI_FMT_UINT8,(int)CVI_FMT_INT8,(int)CVI_FMT_BF16);

  /* 这里用 CVI_NN_SetTensorFormat/SetTensor? 直接用 sys_mem 填 0-255 fp32 */
  float *ib = (float *)in[0].sys_mem;
  /* 用 cv::imdecode 不可用(无opencv依赖), 用简单 JPEG 解压? 改用读取预生成的 raw RGB bin */
  /* 简化: 读取同目录 .raw 或从 /tmp 读 pre-built 128x128x3 rgb fp32 */
  FILE *raw = fopen("/tmp/frame_128_rgb_f32.bin","rb");
  if (!raw) { printf("缺少预生成输入 /tmp/frame_128_rgb_f32.bin, 请先准备\n"); return -1; }
  fread(ib, sizeof(float), C*H*W, raw); fclose(raw);
  printf("已读取 0-255 fp32 rgb 输入 %d floats\n", C*H*W);
  printf("首个像素: %.1f %.1f %.1f\n", ib[0],ib[1*H*W],ib[2*H*W]);

  /* 设置输入为 fp32 */
  rc = CVI_NN_SetTensorDescriptor(in, in_num);
  printf("SetTensor rc=%d\n", rc);

  rc = CVI_NN_Forward(model, in, in_num, out, out_num);
  printf("Forward rc=%d\n", rc);

  for (int o=0;o<out_num;o++){
    float *od=(float*)out[o].sys_mem;
    int cnt = out[o].mem_size / 4;
    printf("out[%d] cnt=%d:", o, cnt);
    for(int i=0;i<cnt;i++) printf(" %.3f", od[i]);
    printf("\n");
    if(cnt==7){
      float mx=od[0]; int bi=0;
      for(int i=1;i<7;i++) if(od[i]>mx){mx=od[i];bi=i;}
      printf(">>> gesture = %s (label=%d)\n", classes[bi],bi);
    }
  }
  CVI_NN_CleanupModel(model);
  return 0;
}
