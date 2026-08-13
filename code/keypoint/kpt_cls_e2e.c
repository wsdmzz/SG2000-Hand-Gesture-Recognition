/* kpt_cls_e2e.c - 端到端: keypoint_hand 关键点 → cls_keypoint_hand_gesture 分类
 * 输入: 128x128 fp32 RGB NCHW 0-255 手部图
 * 输出: 21个关键点 + 9类手势
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cviruntime.h"

int main(int argc, char **argv)
{
	/* keypoint模型 输入图 / 分类模型 */
	if (argc != 4) { printf("Usage: %s <kpt_model> <cls_model> <img.bin>\n", argv[0]); return 1; }
	CVI_MODEL_HANDLE kmodel = NULL, cmodel = NULL;
	if (CVI_NN_RegisterModel(argv[1], &kmodel)!=CVI_RC_SUCCESS){printf("kpt reg fail\n");return 1;}
	if (CVI_NN_RegisterModel(argv[2], &cmodel)!=CVI_RC_SUCCESS){printf("cls reg fail\n");return 1;}
	CVI_TENSOR *kin,*kout,*cin,*cout;
	int32_t kin_n,kout_n,cin_n,cout_n;
	CVI_NN_GetInputOutputTensors(kmodel,&kin,&kin_n,&kout,&kout_n);
	CVI_NN_GetInputOutputTensors(cmodel,&cin,&cin_n,&cout,&cout_n);
	printf("kpt in: fmt=%d qs=%.3f size=%d | out: %d bytes\n", (int)kin[0].fmt, kin[0].qscale, (int)kin[0].mem_size, (int)kout[0].mem_size);
	printf("cls in: fmt=%d qs=%.3f zp=%d size=%d | out: %d bytes (9 cls)\n", (int)cin[0].fmt, cin[0].qscale, cin[0].zero_point, (int)cin[0].mem_size, (int)cout[0].mem_size);

	/* 读图 */
	FILE *fp=fopen(argv[3],"rb"); if(!fp){printf("open fail\n");return 1;}
	fseek(fp,0,SEEK_END); long fsz=ftell(fp); fseek(fp,0,SEEK_SET);
	float *img=malloc(fsz); fread(img,1,fsz,fp); fclose(fp);

	/* 关键点模型输入量化 (INT8, qscale=48.1) + ImageNet 归一化
	   keypoint_hand model_config: mean = {R_MEAN/R_SCALE...}, std = {1/R_SCALE...}
	   实际预处理: (x/max) 归一化到 [0,1], 再 (v-mean)/std:
	   R: mean=123.68 std=58.395  G: mean=116.78 std=57.13  B: mean=103.94 std=57.38
	   (来自 ImageNet mean/std 0.485/0.456/0.406, 0.229/0.224/0.225)
	*/
	{
		const float imean[3] = {123.675f, 116.28f, 103.53f};
		const float istd[3]  = {58.395f, 57.12f, 57.375f};  /* = 255*std */
		float qs=kin[0].qscale;
		int8_t *d=(int8_t*)kin[0].sys_mem;
		int n=kin[0].mem_size;  /* 3*128*128, NCHW */
		int hw = n/3;
		for(int i=0;i<n;i++){
			int c = i / hw;            /* 0=R 1=G 2=B */
			float v = (img[i] - imean[c]) / istd[c];
			float q = v * qs;
			if(q>127)q=127; if(q<-128)q=-128;
			d[i]=(int8_t)q;
		}
	}
	CVI_NN_Forward(kmodel,kin,kin_n,kout,kout_n);
	float *kpt=(float*)kout[0].sys_mem;

	/* 关键点坐标(42个)已得到, 用于分类模型输入 */
	/* 分类模型输入: INT8 1x42 qscale, 完全匹配官方 hand_keypoint_classification:
	   temp_float = qscale * coord; clamp: <-128→-128, >127→128, 否则 round */
	{
		float qs=cin[0].qscale;
		int8_t *d=(int8_t*)cin[0].sys_mem;
		for(int i=0;i<42;i++){
			float tf = qs * kpt[i];
			if (tf < -128) d[i] = -128;
			else if (tf > 127) d[i] = 128;
			else d[i] = (int8_t)lroundf(tf);
		}
	}
	CVI_NN_Forward(cmodel,cin,cin_n,cout,cout_n);
	float *oc=(float*)cout[0].sys_mem;
	/* softmax */
	int best=0; float bz=-1e30f;
	for(int i=0;i<9;i++) if(oc[i]>bz){bz=oc[i];best=i;}
	float sum=0; for(int i=0;i<9;i++) sum+=expf(oc[i]-bz);
	printf("关键点: kpt0=(%.3f,%.3f) kpt4=(%.3f,%.3f) kpt8=(%.3f,%.3f) kpt20=(%.3f,%.3f)\n",
	       kpt[0],kpt[1],kpt[8],kpt[9],kpt[16],kpt[17],kpt[40],kpt[41]);
	printf("9类logits: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", oc[0],oc[1],oc[2],oc[3],oc[4],oc[5],oc[6],oc[7],oc[8]);
	printf(">>> 手势 = %d (0:fist 1:five 2:four 3:none 4:ok 5:one 6:three 7:three2 8:two) prob=%.1f%%\n",
	       best, expf(oc[best]-bz)/sum*100);
	CVI_NN_CleanupModel(kmodel); CVI_NN_CleanupModel(cmodel);
	return 0;
}
