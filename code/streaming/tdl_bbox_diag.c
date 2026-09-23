/*
 * tdl_bbox_diag.c — 手部检测 bbox 尺寸诊断（用于标定 ROI 裁剪系数 K）
 *
 * 用法:
 *   ./tdl_bbox_diag off <img1> [img2 ...]   离线: 对图片文件跑检测(训练图)
 *   ./tdl_bbox_diag live [nframes]          在线: 摄像头取帧检测(默认 30 帧)
 *
 * 目的:
 *   量出「训练图中手 bbox 的 max(w,h) 占画面长边的比例 r_train」,
 *   据此 K = 1/r_train
 *     —— 让 ROI(边长 = max(bw,bh)*K) letterbox 到 480 后,
 *        手在线性尺度上与训练分布一致。
 *
 * 说明: 关键点模型 best480 训练用的就是 gesture_data 这批图, 故以它们为准。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>

#include "tdl_sdk.h"

#include "cvi_vb.h"
#include "cvi_sys.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "linux/cvi_comm_video.h"
#include "linux/cvi_comm_vi.h"
#include "linux/cvi_comm_vpss.h"

#include "sample_comm.h"

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>

#define MODEL_JSON "/mnt/data/model_factory.json"
#define MODEL_DIR  "/mnt/data/models/cv181x/"
#define MODEL_HAND MODEL_DIR "yolov8n_det_hand_384_640_INT8_cv181x.cvimodel"

#define VPSS_GRP 0
#define VPSS_CHN 0

static volatile int g_run = 1;
static void sig_handler(int s){ (void)s; g_run = 0; }

/* 收集占比用于统计 */
#define MAXR 512
static double g_r[MAXR];
static int g_rn = 0;
static int g_hit = 0, g_total = 0;

static void add_r(double r)
{
	if (g_rn < MAXR) g_r[g_rn++] = r;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void report_stats(const char *tag)
{
	if (g_rn == 0) {
		printf("\n[%s] 无有效样本\n", tag);
		return;
	}
	qsort(g_r, g_rn, sizeof(double), cmp_d);
	double sum = 0;
	for (int i = 0; i < g_rn; i++) sum += g_r[i];
	printf("\n===== %s 统计 =====\n", tag);
	printf("样本数=%d  检出=%d/%d\n", g_rn, g_hit, g_total);
	printf("r (手bbox max边长 / 画面长边):\n");
	printf("  min=%.4f  p25=%.4f  median=%.4f  p75=%.4f  max=%.4f  mean=%.4f\n",
	       g_r[0], g_r[g_rn / 4], g_r[g_rn / 2], g_r[g_rn * 3 / 4], g_r[g_rn - 1], sum / g_rn);
	printf("=> K = 1/r :  min=%.2f  p25=%.2f  median=%.2f  p75=%.2f  max=%.2f  mean=%.2f\n",
	       1.0 / g_r[g_rn - 1], 1.0 / g_r[g_rn * 3 / 4], 1.0 / g_r[g_rn / 2],
	       1.0 / g_r[g_rn / 4], 1.0 / g_r[0], (double)g_rn / sum);
}

/* 对单个 TDLImage 检测并打印 */
static void detect_and_print(TDLHandle h, TDLImage img, const char *tag, int W, int H)
{
	TDLObject obj;
	memset(&obj, 0, sizeof(obj));
	int drc = TDL_Detection(h, TDL_MODEL_YOLOV8N_DET_HAND, img, &obj);
	g_total++;
	if (drc != 0 || obj.size == 0) {
		printf("  %-28s rc=%d obj.size=%u  (未检出)\n", tag, drc, obj.size);
		return;
	}
	g_hit++;
	for (uint32_t k = 0; k < obj.size && k < 4; k++) {
		double x1 = obj.info[k].box.x1, y1 = obj.info[k].box.y1;
		double x2 = obj.info[k].box.x2, y2 = obj.info[k].box.y2;
		double bw = x2 - x1, bh = y2 - y1;
		double bmax = (bw > bh) ? bw : bh;
		double long_side = (W > H) ? W : H;
		double r = bmax / long_side;
		add_r(r);
		printf("  %-28s bbox=(%.0f,%.0f,%.0f,%.0f) w=%.0f h=%.0f max=%.0f | "
		       "W=%d H=%d | w/W=%.3f h/H=%.3f max/长边=%.4f | score=%.3f\n",
		       k == 0 ? tag : "", x1, y1, x2, y2, bw, bh, bmax, W, H,
		       bw / W, bh / H, r, obj.info[k].score);
	}
}

/* ============ cascade 模式: 取帧→裁剪放大→构造NV21帧→检测 ============
 * 验证「把手放大后, 检测模型框是否变准」—— two-stage 方案的前提。
 * TDL_WrapFrame 只接受 VIDEO_FRAME, 故需手工把 BGR ROI 转回 NV21。 */
static void do_cascade_crop(TDLHandle h, int nframes, float crop_frac)
{
	CVI_S32 ret;
	VIDEO_FRAME_INFO_S stVpssFrame;
	static unsigned char *nvbuf = NULL;
	static size_t nbcap = 0;
	int i;

	sleep(2);
	for (i = 0; i < nframes && g_run; i++) {
		ret = CVI_VPSS_GetChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame, 2000);
		if (ret != CVI_SUCCESS) { printf("[%d] GetChnFrame fail %#x\n", i, ret); continue; }
		VIDEO_FRAME_S *f = &stVpssFrame.stVFrame;
		int fw = f->u32Width, fh = f->u32Height;
		void *va0 = (f->pu8VirAddr[0]) ? (void *)f->pu8VirAddr[0]
		           : CVI_SYS_Mmap(f->u64PhyAddr[0],
		                          (CVI_U32)f->u32Length[0] + (CVI_U32)f->u32Length[1] + 64);
		if (!va0) { CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame); continue; }

		cv::Mat nv21(fh * 3 / 2, fw, CV_8UC1, va0);
		cv::Mat bgr;
		cv::cvtColor(nv21, bgr, cv::COLOR_YUV2BGR_NV21);

		int side = (int)(fh * crop_frac);
		if (side < 64) side = 64;
		int x1 = (fw - side) / 2, y1 = (fh - side) / 2;
		int w = side, hh = side;
		if (x1 < 0) x1 = 0;
		if (y1 < 0) y1 = 0;
		if (x1 + w > fw) w = fw - x1;
		if (y1 + hh > fh) hh = fh - y1;
		if (w < 16 || hh < 16) { CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame); continue; }

		cv::Mat roi = bgr(cv::Rect(x1, y1, w, hh)).clone();

		cv::Mat yv12;
		cv::cvtColor(roi, yv12, cv::COLOR_BGR2YUV_YV12);
		size_t ysz = (size_t)w * hh, csz = ysz / 4, need = ysz + csz * 2;
		if (nbcap < need) {
			if (nvbuf) free(nvbuf);
			nvbuf = (unsigned char *)malloc(need);
			nbcap = nvbuf ? need : 0;
		}
		if (!nvbuf) { CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame); continue; }
		memcpy(nvbuf, yv12.data, ysz);
		{
			unsigned char *V = yv12.data + ysz;
			unsigned char *U = yv12.data + ysz + csz;
			for (size_t k = 0; k < csz; k++) {
				nvbuf[ysz + 2 * k] = V[k];      /* NV21: V 在前 */
				nvbuf[ysz + 2 * k + 1] = U[k];
			}
		}

		VIDEO_FRAME_INFO_S vf;
		memset(&vf, 0, sizeof(vf));
		vf.stVFrame.u32Width = (CVI_U32)w;
		vf.stVFrame.u32Height = (CVI_U32)hh;
		vf.stVFrame.enPixelFormat = PIXEL_FORMAT_NV21;
		vf.stVFrame.u32Stride[0] = (CVI_U32)w;
		vf.stVFrame.u32Stride[1] = (CVI_U32)w;
		vf.stVFrame.u32Length[0] = (CVI_U32)ysz;
		vf.stVFrame.u32Length[1] = (CVI_U32)(csz * 2);
		vf.stVFrame.pu8VirAddr[0] = nvbuf;
		vf.stVFrame.pu8VirAddr[1] = nvbuf + ysz;

		TDLImage img = TDL_WrapFrame(&vf, false, false);
		if (img) {
			char tag[64];
			snprintf(tag, sizeof(tag), "roi%dx%d@%d,%d", w, hh, x1, y1);
			detect_and_print(h, img, tag, w, hh);
		} else {
			printf("  roi%dx%d@%d,%d  WrapFrame=NULL\n", w, hh, x1, y1);
		}
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame);
		usleep(300 * 1000);
	}
}

int main(int argc, char **argv)
{
	CVI_S32 ret;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	VB_CONFIG_S stVbConf;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize, stSizeIn, stSizeOut;
	VIDEO_FRAME_INFO_S stVpssFrame;
	CVI_U32 u32BlkSize;
	TDLHandle h = NULL;
	int i;

	signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);

	if (argc < 2) {
		printf("用法:\n");
		printf("  %s off <img1> [img2 ...]   离线检测图片\n", argv[0]);
		printf("  %s live [nframes]          在线检测摄像头帧\n", argv[0]);
		return 1;
	}
	int mode_off = (strcmp(argv[1], "off") == 0);
	int mode_cascade = (strcmp(argv[1], "cascade") == 0);
	int nframes = 30;
	if (!mode_off && argc > 2) nframes = atoi(argv[2]);

	printf("=== TDL bbox 尺寸诊断 (%s) ===\n",
	       mode_off ? "离线图片" : (mode_cascade ? "cascade裁剪放大" : "在线摄像头"));

	/* 0. 清理残留 VPSS */
	{ CVI_S32 g; for (g = 0; g < (CVI_S32)VPSS_MAX_GRP_NUM; g++) {
		CVI_VPSS_StopGrp(g); CVI_VPSS_DestroyGrp(g);} usleep(200 * 1000); }

	/* 1. ParseIni */
	memset(&stIniCfg, 0, sizeof(stIniCfg));
	stIniCfg.enSource = (VI_PIPE_FRAME_SOURCE_E)VI_PIPE_FRAME_SOURCE_DEV;
	stIniCfg.devNum = 1;
	ret = SAMPLE_COMM_VI_ParseIni(&stIniCfg);
	if (ret != CVI_SUCCESS) { printf("ParseIni %#x\n", ret); return -1; }
	CVI_VI_SetDevNum(stIniCfg.devNum);
	ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (ret != CVI_SUCCESS) return -1;
	ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
	if (ret != CVI_SUCCESS) return -1;
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (ret != CVI_SUCCESS) return -1;
	printf("sensor %dx%d\n", stSize.u32Width, stSize.u32Height);

	/* 2. VB+SYS */
	memset(&stVbConf, 0, sizeof(stVbConf));
	stVbConf.u32MaxPoolCnt = 2;
	u32BlkSize = COMMON_GetPicBufferSize(stSize.u32Width, stSize.u32Height, SAMPLE_PIXEL_FORMAT,
	                                     DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkSize = u32BlkSize; stVbConf.astCommPool[0].u32BlkCnt = 5;
	u32BlkSize = COMMON_GetPicBufferSize(768, 1280, SAMPLE_PIXEL_FORMAT,
	                                     DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[1].u32BlkSize = u32BlkSize; stVbConf.astCommPool[1].u32BlkCnt = 8;
	ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (ret != CVI_SUCCESS) { printf("SYS_Init %#x\n", ret); return -1; }

	/* 3. VI */
	ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (ret != CVI_SUCCESS) { printf("VI_INIT %#x\n", ret); goto err_sys; }

	/* 4. VPSS (输出 640x384 匹配 yolov8n_det_hand 输入) */
	stSizeIn = stSize;
	stSizeOut.u32Width = 640; stSizeOut.u32Height = 384;
	ret = SAMPLE_PLAT_VPSS_INIT(VPSS_GRP, stSizeIn, stSizeOut);
	if (ret != CVI_SUCCESS) { printf("VPSS_INIT %#x\n", ret); goto err_sys; }
	ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP);
	if (ret != CVI_SUCCESS) { printf("Bind %#x\n", ret); goto err_vpss; }
	printf("VI->VPSS bind OK\n");

	/* 5. TDL init */
	h = TDL_CreateHandle(0);
	if (!h) { printf("CreateHandle fail\n"); goto err_vpss; }
	if (TDL_LoadModelConfig(h, MODEL_JSON) != 0) printf("WARN LoadModelConfig\n");
	if (TDL_OpenModel(h, TDL_MODEL_YOLOV8N_DET_HAND, MODEL_HAND, MODEL_JSON, 0) != 0) {
		printf("OpenModel HAND fail\n"); goto err_handle;
	}
	TDL_SetModelThreshold(h, TDL_MODEL_YOLOV8N_DET_HAND, 0.25f);
	printf("TDL hand model OK (threshold=0.25)\n"); fflush(stdout);

	/* 打印模型预处理参数: 判断 TDL 是否会把 640x384 拉伸变形 */
	{
		TDLPreprocessParams pp;
		memset(&pp, 0, sizeof(pp));
		if (TDL_GetPreprocessParameters(h, TDL_MODEL_YOLOV8N_DET_HAND, &pp) == 0) {
			printf("[PP] dst=%dx%d fmt=%d dtype=%d crop=(x=%d,y=%d,w=%d,h=%d) "
			       "mean=(%.3f,%.3f,%.3f) scale=(%.6f,%.6f,%.6f) keep_ar=%d\n",
			       pp.dst_width, pp.dst_height, (int)pp.dst_image_format, (int)pp.dst_pixdata_type,
			       pp.crop_x, pp.crop_y, pp.crop_width, pp.crop_height,
			       pp.mean[0], pp.mean[1], pp.mean[2],
			       pp.scale[0], pp.scale[1], pp.scale[2], (int)pp.keep_aspect_ratio);
		} else {
			printf("[PP] TDL_GetPreprocessParameters 调用失败\n");
		}
		fflush(stdout);
	}

	if (mode_off) {
		/* ---------- 离线: 逐张图片检测 ---------- */
		for (i = 2; i < argc && g_run; i++) {
			const char *path = argv[i];
			cv::Mat m = cv::imread(path, cv::IMREAD_COLOR);
			int W = m.cols, H = m.rows;
			TDLImage img = TDL_ReadImage(path);
			const char *base = strrchr(path, '/');
			base = base ? base + 1 : path;
			if (!img) { printf("  %-28s TDL_ReadImage 失败\n", base); continue; }
			if (W == 0 || H == 0) { printf("  %-28s cv::imread 失败\n", base); TDL_DestroyImage(img); continue; }
			detect_and_print(h, img, base, W, H);
			TDL_DestroyImage(img);
		}
		report_stats("离线图片(训练图)");
	} else if (mode_cascade) {
		/* ---------- cascade: 取帧后逐级裁剪放大再检测 ---------- */
		static const float FRACS[] = {1.00f, 0.80f, 0.65f, 0.50f, 0.40f};
		printf("\n手真实位置(640x384 坐标系): x130-390 y65-325, 中心约(260,195)\n");
		for (unsigned k = 0; k < sizeof(FRACS) / sizeof(FRACS[0]); k++) {
			printf("\n===== crop_frac=%.2f (取画面高度 %.0f%% 并放大到整帧) =====\n",
			       FRACS[k], FRACS[k] * 100);
			fflush(stdout);
			do_cascade_crop(h, nframes, FRACS[k]);
		}
		report_stats("cascade 各级裁剪");
	} else {
		/* ---------- 在线: 摄像头取帧检测 ---------- */
		sleep(2);
		int W = 0, H = 0;
		for (i = 0; i < nframes && g_run; i++) {
			ret = CVI_VPSS_GetChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame, 2000);
			if (ret != CVI_SUCCESS) { printf("[%d] GetChnFrame fail %#x\n", i, ret); continue; }
			VIDEO_FRAME_S *f = &stVpssFrame.stVFrame;
			W = f->u32Width; H = f->u32Height;
			TDLImage img = TDL_WrapFrame(&stVpssFrame, false, false);
			if (img) {
				char tag[32]; snprintf(tag, sizeof(tag), "frame%d", i);
				detect_and_print(h, img, tag, W, H);
			}
			CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame);
			usleep(300 * 1000);
		}
		report_stats("在线摄像头帧");
	}

	TDL_DestroyHandle(h);
err_handle:
err_vpss:
	SAMPLE_COMM_VPSS_Stop(VPSS_GRP, NULL);
err_sys:
	SAMPLE_COMM_SYS_Exit();
	printf("=== DONE ===\n");
	return 0;
}
