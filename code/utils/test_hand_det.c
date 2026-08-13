/*
 * test_hand_det.c - 裸 cviruntime 手部检测（yolov8n_det_hand）离线验证（无 OpenCV 依赖）
 * 输入：预处理后的 int8 NCHW RGB 数据（*_prep_int8.bin，由开发机 Python 生成）
 * 流程：加载模型 → 读入 bin 填入输入 tensor → Forward → YOLOv8 DFL 解码 → 打印 bbox
 * 用法: ./test_hand_det <hand_det.cvimodel> <prep_int8.bin>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "cviruntime.h"

#define REG_MAX 16

typedef struct { float x1, y1, x2, y2, score; } det_box;

static void softmax16(const float *x, float *out) {
  float maxv = x[0];
  for (int i = 1; i < REG_MAX; i++) if (x[i] > maxv) maxv = x[i];
  float sum = 0.f;
  for (int i = 0; i < REG_MAX; i++) { out[i] = expf(x[i] - maxv); sum += out[i]; }
  for (int i = 0; i < REG_MAX; i++) out[i] /= sum;
}

int main(int argc, char *argv[]) {
  if (argc != 3) { printf("Usage: %s <hand_det.cvimodel> <prep_int8.bin>\n", argv[0]); return -1; }

  FILE *f = fopen(argv[2], "rb");
  if (!f) { printf("cannot open %s\n", argv[2]); return -1; }
  fseek(f, 0, SEEK_END);
  long fsz = ftell(f);
  rewind(f);
  uint8_t *fdata = (uint8_t *)malloc(fsz);
  if (fread(fdata, 1, fsz, f) != (size_t)fsz) { printf("read fail\n"); return -1; }
  fclose(f);
  printf("read preprocessed data %ld bytes\n", fsz);

  CVI_MODEL_HANDLE m = NULL;
  CVI_RC rc = CVI_NN_RegisterModel(argv[1], &m);
  printf("RegisterModel rc=%d\n", rc);
  if (rc != CVI_RC_SUCCESS) return -1;
  CVI_NN_SetConfig(m, OPTION_OUTPUT_ALL_TENSORS, 0);

  CVI_TENSOR *in = NULL, *out = NULL;
  int32_t in_num = 0, out_num = 0;
  rc = CVI_NN_GetInputOutputTensors(m, &in, &in_num, &out, &out_num);
  if (rc != CVI_RC_SUCCESS) return -1;
  int N=in[0].shape.dim[0],C=in[0].shape.dim[1],H=in[0].shape.dim[2],W=in[0].shape.dim[3];
  printf("in[0]=[%d,%d,%d,%d] qscale=%.3f  out_num=%d\n", N,C,H,W, in[0].qscale, out_num);
  printf("  in[0].pixel_format=%d mean=[%.3f,%.3f,%.3f] scale=[%.4f,%.4f,%.4f] fmt=%d aligned=%d\n",
    in[0].pixel_format, in[0].mean[0],in[0].mean[1],in[0].mean[2],
    in[0].scale[0],in[0].scale[1],in[0].scale[2], in[0].fmt, in[0].aligned?1:0);

  /* 数据尺寸必须匹配输入 */
  if ((long)(N*C*H*W) > fsz) {
    printf("ERR: bin too small %ld vs need %d\n", fsz, N*C*H*W);
    return -1;
  }
  memcpy(in[0].sys_mem, fdata, (size_t)(N*C*H*W));
  free(fdata);

  rc = CVI_NN_Forward(m, in, in_num, out, out_num);
  printf("Forward rc=%d\n", rc);
  if (rc != CVI_RC_SUCCESS) return -1;

  /* ---- YOLOv8 DFL 解码 ----
   * 输出 6 tensors:
   *  out0/1/2 = [1,64,hs,ws] bbox 头 (stride 8/16/32)
   *  out3/4/5 = [1,1, hs,ws] score 头
   */
  int strides[3] = {8, 16, 32};
  det_box dets[512];
  int total_det = 0;

  for (int s = 0; s < 3; s++) {
    CVI_TENSOR *bt = &out[s];
    CVI_TENSOR *st = &out[s + 3];
    if (bt->shape.dim[1] != 64) { printf("WARN: out%d channels=%d not 64\n", s, bt->shape.dim[1]); continue; }
    float *box_ptr = (float *)bt->sys_mem;
    float *score_ptr = (float *)st->sys_mem;
    int hs = bt->shape.dim[2], ws = bt->shape.dim[3];
    int stride = strides[s];
    printf("layer %d: box[%d,%d,%d,%d] score[%d,%d,%d,%d] stride=%d\n",
      s, bt->shape.dim[0],bt->shape.dim[1],hs,ws,
      st->shape.dim[0],st->shape.dim[1],st->shape.dim[2],st->shape.dim[3], stride);

    /* 诊断: score 头值域统计 */
    float smax=-1e9f,smin=1e9f,ssum=0.f; int sbig=0, snum=hs*ws;
    for (int j=0;j<snum;j++){ float v=score_ptr[j]; if(v>smax)smax=v; if(v<smin)smin=v; ssum+=v;
      if(v>0) sbig++; }
    printf("  score: max=%.4f min=%.4f avg=%.4f (>0: %d/%d)\n", smax,smin,ssum/snum,sbig,snum);

    /* 诊断: bbox 头值域统计(out0 全64通道) */
    {
      int bc = bt->shape.dim[1]; float bmax=-1e9f,bmin=1e9f;
      int bn = bc*hs*ws; int finf=0;
      for(int k=0;k<bn;k++){ float v=box_ptr[k]; if(v>bmax)bmax=v; if(v<bmin)bmin=v;
        if(v!=v || v>1e9f || v<-1e9f) finf++; }
      printf("  bbox: max=%.4f min=%.4f nan/inf=%d\n", bmax,bmin,finf);
    }

    /* 诊断: score 头数据类型判断 — 打印前 8 个 float 值对应字节 */
    {
      uint8_t *sb=(uint8_t*)score_ptr;
      printf("  score[0] float=%.3f hex=", score_ptr[0]);
      for(int k=0;k<8;k++) printf("%02x", sb[k]);
      printf(" ; score[1]=%.3f hex=", score_ptr[1]);
      for(int k=0;k<8;k++) printf("%02x", sb[4+k]);
      printf("\n");
      /* 若像 int8 数据, 打印 int8 解读 */
      int8_t *si=(int8_t*)score_ptr;
      printf("  score[0..3] as int8: %d %d %d %d\n", si[0],si[1],si[2],si[3]);
    }

    for (int j = 0; j < hs * ws; j++) {
      float scores = score_ptr[j];
      float prob;
      /* 先尝试直接当概率(可能已是sigmoid输出)，同时打印原始便于诊断 */
      if (scores >= 0.f && scores <= 1.f) prob = scores;
      else prob = 1.f / (1.f + expf(-scores));
      if (prob < 0.30f) continue;
      int gy = j / ws, gx = j % ws;
      float m16[REG_MAX + 1];
      /* 64ch = 4 coords * 16 bins, 布局 c (0..63) = coord(c/16)*16 + bin(c%16) */
      /* 每坐标 16 bin 在 channel 方向: coord k -> channel [k*16, k*16+16) */
      float lv[4];
      for (int k = 0; k < 4; k++) {
        float raw[REG_MAX];
        for (int i = 0; i < REG_MAX; i++) {
          int ch = k * REG_MAX + i;
          raw[i] = box_ptr[ch * (hs * ws) + j];
        }
        softmax16(raw, m16);
        float acc = 0.f;
        for (int i = 0; i < REG_MAX; i++) acc += m16[i] * (float)i;
        lv[k] = acc;
      }
      float l = lv[0], t = lv[1], r = lv[2], b = lv[3];
      float cx = (gx + 0.5f) * stride;
      float cy = (gy + 0.5f) * stride;
      float x1 = cx - l * stride;
      float y1 = cy - t * stride;
      float x2 = cx + r * stride;
      float y2 = cy + b * stride;
      if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
      if (x2 > W) x2 = W; if (y2 > H) y2 = H;
      if (total_det < 512) {
        dets[total_det].x1 = x1; dets[total_det].y1 = y1;
        dets[total_det].x2 = x2; dets[total_det].y2 = y2;
        dets[total_det].score = prob;
        total_det++;
      }
    }
  }

  printf("\n=== HANDS (raw pre-NMS): %d ===\n", total_det);
  /* 按分数排序打印前 20 */
  for (int i = 0; i < total_det && i < 20; i++) {
    printf("  [%d] bbox=(%.0f,%.0f,%.0f,%.0f) score=%.3f (w=%.0f h=%.0f)\n",
      i, dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2, dets[i].score,
      dets[i].x2 - dets[i].x1, dets[i].y2 - dets[i].y1);
  }
  if (total_det == 0) printf("  -> NO HAND DETECTED\n");

  CVI_NN_CleanupModel(m);
  return 0;
}
