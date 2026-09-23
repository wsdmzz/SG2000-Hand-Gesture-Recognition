/*
 * kpt_model_probe.c — 离线验证 best480 cvimodel 能否在 SG2000 上加载/前向
 *
 * 用途: 不依赖摄像头(VI/VPSS)，单独验证:
 *   1. CVI_NN_RegisterModel 能否加载 cvimodel
 *   2. 输入/输出 tensor 的 count/fmt/qscale 是否与 tdl_gesture_kpt_stream.c 假设一致
 *      (det_raw = 71*4725, kpt_raw = 63*4725, 输入 3*480*480 INT8 qscale=127)
 *   3. 零输入能否完成一次 Forward
 *
 * 编译(build_probe.sh) → 上板运行 → 与宿主机 onnxruntime 参考值对照
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <cviruntime.h>

#define MODEL_POSE "/mnt/data/models/cv181x/best480_int8_cv181x.cvimodel"
#define POSE_SZ    480
#define POSE_ANCH  4725
#define DET_CH     71
#define KPT_CH     63

int main(int argc, char **argv)
{
	CVI_MODEL_HANDLE model = NULL;
	CVI_TENSOR *in = NULL, *out = NULL;
	int32_t in_num = 0, out_num = 0;
	CVI_RC rc;

	/* 可选: 从文件读入预量化的 INT8 输入 */
	const char *inbin = (argc > 1) ? argv[1] : NULL;

	printf("=== kpt_model_probe ===\n");
	printf("model: %s\n", MODEL_POSE);

	rc = CVI_NN_RegisterModel(MODEL_POSE, &model);
	if (rc != CVI_RC_SUCCESS) { printf("FAIL RegisterModel rc=%#x\n", rc); return 1; }
	printf("OK  RegisterModel\n");

	rc = CVI_NN_GetInputOutputTensors(model, &in, &in_num, &out, &out_num);
	if (rc != CVI_RC_SUCCESS) { printf("FAIL GetIO rc=%#x\n", rc); return 1; }
	printf("OK  GetIO  in_num=%d out_num=%d\n", in_num, out_num);

	int in_ok = 0;
	for (int i = 0; i < in_num; i++) {
		printf("  in[%d]  name=%-10s count=%-9zu fmt=%d qscale=%.6f zp=%d\n",
		       i, in[i].name ? in[i].name : "?", in[i].count,
		       (int)in[i].fmt, in[i].qscale, (int)in[i].zero_point);
		if (in[i].count == (size_t)3 * POSE_SZ * POSE_SZ) in_ok = 1;
	}

	int det_idx = -1, kpt_idx = -1;
	for (int i = 0; i < out_num; i++) {
		printf("  out[%d] name=%-24s count=%-9zu fmt=%d qscale=%.6f\n",
		       i, out[i].name ? out[i].name : "?", out[i].count,
		       (int)out[i].fmt, out[i].qscale);
		if (out[i].count == (size_t)DET_CH * POSE_ANCH) det_idx = i;
		else if (out[i].count == (size_t)KPT_CH * POSE_ANCH) kpt_idx = i;
	}

	printf("--- 布局检查 ---\n");
	printf("  input  3x480x480 (%zu)  : %s\n", (size_t)3 * POSE_SZ * POSE_SZ,
	       in_ok ? "OK" : "MISMATCH");
	printf("  det_raw 71x4725 (%zu) : %s\n", (size_t)DET_CH * POSE_ANCH,
	       (det_idx >= 0) ? "OK" : "MISMATCH");
	printf("  kpt_raw 63x4725 (%zu) : %s\n", (size_t)KPT_CH * POSE_ANCH,
	       (kpt_idx >= 0) ? "OK" : "MISMATCH");
	if (!in_ok || det_idx < 0 || kpt_idx < 0) {
		printf("RESULT: FAIL (tensor 布局与 C 代码假设不一致)\n");
		return 1;
	}

	/* 准备输入: 文件或全零 */
	int8_t *din = (int8_t *)in[0].sys_mem;
	if (inbin) {
		FILE *fp = fopen(inbin, "rb");
		if (!fp) { printf("FAIL 无法打开输入文件 %s\n", inbin); return 1; }
		size_t got = fread(din, 1, in[0].count, fp);
		fclose(fp);
		printf("输入: 从 %s 读入 %zu 字节\n", inbin, got);
		if (got != in[0].count) { printf("FAIL 输入字节数不符\n"); return 1; }
	} else {
		memset(din, 0, in[0].count);
		printf("输入: 全零 (%zu 字节)\n", in[0].count);
	}

	rc = CVI_NN_Forward(model, in, in_num, out, out_num);
	if (rc != CVI_RC_SUCCESS) { printf("FAIL Forward rc=%#x\n", rc); return 1; }
	printf("OK  Forward\n");

	printf("--- 输出统计 ---\n");
	for (int i = 0; i < out_num; i++) {
		if (i != det_idx && i != kpt_idx) continue;
		float *p = (float *)out[i].sys_mem;
		size_t n = out[i].count;
		double sum = 0.0; float mx = -1e30f, mn = 1e30f;
		int nan_cnt = 0;
		for (size_t k = 0; k < n; k++) {
			float v = p[k];
			if (isnan(v) || isinf(v)) { nan_cnt++; continue; }
			sum += v; if (v > mx) mx = v; if (v < mn) mn = v;
		}
		printf("  %-24s n=%zu mean=%.6f min=%.6f max=%.6f nan/inf=%d\n",
		       out[i].name ? out[i].name : "?", n, sum / (double)n, mn, mx, nan_cnt);
		/* 前 6 个值, 供与宿主机参考对照 */
		printf("     head: ");
		for (int k = 0; k < 6; k++) printf("%.6f ", p[k]);
		printf("\n");
	}

	CVI_NN_CleanupModel(model);
	printf("RESULT: ALL OK\n");
	return 0;
}
