/*
 * cls7_verify_raw.c - 裸 cviruntime 验证 gesture_norm_sym.cvimodel(内置归一化)
 * 输入: 预生成的 0-255 fp32 RGB NCHW bin (128x128x3)
 * 用法: ./cls7_verify_raw <cvimodel> <input_rgb_f32.bin>
 * 目的: 排除 TDL 干扰, 直接在评估板真机验证新模型能否正确识别 7 类手势
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cviruntime.h"

static const char *classes[7] = {"fist","number1","number2","number3","number4","number5","thumbs-up"};

int main(int argc, char *argv[]) {
  if (argc != 3) { printf("Usage: %s <cvimodel> <input_rgb_f32.bin>\n", argv[0]); return -1; }

  CVI_MODEL_HANDLE model=NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &model);
  printf("RegisterModel rc=%d\n", rc);
  if (rc != CVI_RC_SUCCESS) return -1;

  CVI_TENSOR *in=NULL,*out=NULL; int32_t in_num=0,out_num=0;
  CVI_NN_GetInputOutputTensors(model,&in,&in_num,&out,&out_num);
  int C = in[0].shape.dim[1], H = in[0].shape.dim[2], W = in[0].shape.dim[3];
  int total = C*H*W;
  printf("in_qscale=%.4f fmt=%d shape=[1,%d,%d,%d] out_num=%d elems=%d\n",
         in[0].qscale,(int)in[0].fmt,C,H,W,out_num,total);
  printf("fmt codes: FP32=%d UINT8=%d INT8=%d BF16=%d\n",
         (int)CVI_FMT_FP32,(int)CVI_FMT_UINT8,(int)CVI_FMT_INT8,(int)CVI_FMT_BF16);

  /* 读输入 bin (fp32 NCHW, 0-255 rgb) */
  FILE *f = fopen(argv[2],"rb");
  if(!f){ printf("cannot open %s\n",argv[2]); return -1; }
  float *ib = (float*)in[0].sys_mem;
  size_t got = fread(ib, sizeof(float), total, f);
  fclose(f);
  printf("read %zu/%d floats, first px: R=%.1f G=%.1f B=%.1f\n",
         got,total,ib[0],ib[1*H*W],ib[2*H*W]);

  CVI_NN_Forward(model, in, in_num, out, out_num);
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
