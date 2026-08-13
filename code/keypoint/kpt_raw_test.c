/* kpt_raw_test.c - 裸 cviruntime 测试 keypoint_hand 模型
 * 读入 128x128 图像(RGB 0-255 fp32 NCHW), 推理输出 21x2 关键点
 * 用法: ./kpt_raw_test <cvimodel> <input.bin>
 * input.bin: 3*128*128 fp32 RGB NCHW 0-255
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cviruntime.h"

int main(int argc, char **argv)
{
	if (argc != 3) { printf("Usage: %s <model> <input.bin>\n", argv[0]); return 1; }
	CVI_MODEL_HANDLE model = NULL;
	CVI_RC rc = CVI_NN_RegisterModel(argv[1], &model);
	if (rc != CVI_RC_SUCCESS) { printf("RegisterModel %#x\n", rc); return 1; }
	CVI_TENSOR *input, *output;
	int32_t in_num, out_num;
	rc = CVI_NN_GetInputOutputTensors(model, &input, &in_num, &output, &out_num);
	if (rc != CVI_RC_SUCCESS) { printf("GetIO %#x\n", rc); return 1; }
	printf("input[0] shape=%d,%d,%d,%d fmt=%d qscale=%.3f size=%d\n",
	       input[0].shape.dim[0], input[0].shape.dim[1], input[0].shape.dim[2], input[0].shape.dim[3],
	       (int)input[0].fmt, input[0].qscale, (int)input[0].mem_size);

	/* 读输入 bin */
	FILE *fp = fopen(argv[2], "rb");
	if (!fp) { printf("open %s fail\n", argv[2]); return 1; }
	fseek(fp, 0, SEEK_END); long fsz = ftell(fp); fseek(fp, 0, SEEK_SET);
	unsigned char *idata = malloc(fsz);
	fread(idata, 1, fsz, fp); fclose(fp);
	printf("read %ld bytes\n", fsz);

	/* 填充 input tensor */
	if (input[0].fmt == CVI_FMT_INT8 || input[0].fmt == CVI_FMT_UINT8) {
		/* INT8 输入: 需量化, qscale */
		printf("input is INT8, qscale=%.3f\n", input[0].qscale);
		float *f = (float*)idata;
		int8_t *d = (int8_t*)input[0].sys_mem;
		int n = input[0].mem_size;
		for (int i = 0; i < n; i++) {
			float v = f[i] * input[0].qscale;  /* fp32 * qscale -> int8 */
			if (v > 127) v = 127; if (v < -128) v = -128;
			d[i] = (int8_t)v;
		}
	} else {
		memcpy(input[0].sys_mem, idata, input[0].mem_size);
	}

	CVI_NN_Forward(model, input, in_num, output, out_num);
	printf("output[0] shape=%d,%d,%d,%d fmt=%d size=%d\n",
	       output[0].shape.dim[0], output[0].shape.dim[1], output[0].shape.dim[2], output[0].shape.dim[3],
	       (int)output[0].fmt, (int)output[0].mem_size);
	float *o = (float*)output[0].sys_mem;
	int cnt = output[0].mem_size / 4;
	printf("out floats (%d):\n", cnt);
	for (int i = 0; i < cnt; i++) {
		if (i % 2 == 0) printf("  kpt%d = (%.3f, ", i/2, o[i]);
		else            printf("%.3f)\n", o[i]);
	}
	CVI_NN_CleanupModel(model);
	return 0;
}
