/*
 * opencv_jpeg_test.c - 验证 opencv-mobile(milkv-prebuilt) 软编码 NV21→JPEG 是否可用
 * 输入: NV21 1280x720 原始帧（如 cap_0.yuv），输出: out.jpg
 * 用于 MJPEG 软编码推流（绕开触发内核 bug 的 VENC 硬件编码）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

int main(int argc, char *argv[])
{
	if (argc < 3) {
		printf("usage: %s in_nv21.yuv out.jpg [w] [h]\n", argv[0]);
		return -1;
	}
	int w = (argc > 3) ? atoi(argv[3]) : 640;
	int h = (argc > 4) ? atoi(argv[4]) : 360;
	int nv21_size = w * h * 3 / 2;
	(void)nv21_size;
	unsigned char *nv21 = (unsigned char *)malloc((size_t)w * h * 3 / 2);
	if (!nv21) { printf("malloc fail\n"); return -1; }
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) { printf("open %s fail\n", argv[1]); free(nv21); return -1; }
	size_t rd = fread(nv21, 1, (size_t)w * h * 3 / 2, fp);
	fclose(fp);
	printf("read %zu bytes\n", rd);

	double t0 = (double)cv::getTickCount();

	/* 构造 I420 (Y+U+V 三个平面) 用于 cvtColor; cap 帧只含 Y+部分UV, 补全 */
	unsigned char *i420 = (unsigned char *)malloc((size_t)w * h * 3 / 2);
	memset(i420, 0x80, (size_t)w * h * 3 / 2);              /* 灰底 UV */
	memcpy(i420, nv21, (size_t)w * h);                     /* 真实 Y(如有) */

	cv::Mat src2(h * 3 / 2, w, CV_8UC1, i420);
	cv::Mat bgr;
	cv::cvtColor(src2, bgr, cv::COLOR_YUV2BGR_I420);

	std::vector<int> params;
	params.push_back(cv::IMWRITE_JPEG_QUALITY);
	params.push_back(80);
	std::vector<unsigned char> jpg;
	bool ok = cv::imencode(".jpg", bgr, jpg, params);

	double t1 = (double)cv::getTickCount();
	double ms = (t1 - t0) / (double)cv::getTickFrequency() / 1000.0;

	printf("imencode=%d, jpg size=%zu, time=%.1f ms (%dx%d Q80)\n",
		ok ? 1 : 0, jpg.size(), ms, bgr.cols, bgr.rows);

	if (ok && argc > 2) {
		FILE *fo = fopen(argv[2], "wb");
		if (fo) { fwrite(jpg.data(), 1, jpg.size(), fo); fclose(fo); printf("wrote %s\n", argv[2]); }
	}
	free(nv21); free(i420);
	return ok ? 0 : 1;
}
