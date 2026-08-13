/*
 * mjpeg_stream_sw.c - GC2083 摄像头 MJPEG over HTTP 低延时推流（软编码版，绕开 VENC 内核 bug）
 * 流程: GC2083→VI→VPSS(Chn0 取帧)→CVI_SYS_Mmap→OpenCV 软编码 JPEG→HTTP multipart 推流
 * 不使用硬 VENC（避免 #56 的 cv181x_vpss 内核 Oops）
 * 用法: ./mjpeg_stream_sw [port] [outW] [outH]
 * 浏览器: http://<board>:8881/   ffplay -i http://<board>:8881/
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

#include <vector>
#include "cvi_vb.h"
#include "cvi_sys.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "linux/cvi_comm_video.h"
#include "linux/cvi_comm_vi.h"
#include "linux/cvi_comm_vpss.h"
#include "sample_comm.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#define VPSS_GRP	0
#define VPSS_CHN	VPSS_CHN0
#define MJPEG_DEFAULT_PORT 8881

static volatile int g_run = 1;
static void sig_handler(int s) { (void)s; g_run = 0; }

static char http_buf[1024];
static int server_fd = -1;

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = (const char *)buf;
	size_t off = 0;
	while (off < len && g_run) {
		/* 等待可写，500ms 超时；客户端不读会导致阻塞，超时判为断开 */
		struct pollfd pfd = { fd, POLLOUT, 0 };
		int pr = poll(&pfd, 1, 500);
		if (pr <= 0) return -1;                       /* 超时或错误 */
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
			return -1;
		}
		off += (size_t)n;
	}
	return (off == len) ? 0 : -1;
}

int main(int argc, char *argv[])
{
	int port = (argc > 1) ? atoi(argv[1]) : MJPEG_DEFAULT_PORT;
	int out_w = (argc > 2) ? atoi(argv[2]) : 640;
	int out_h = (argc > 3) ? atoi(argv[3]) : 360;   /* 软编码降分辨率加速 */
	CVI_S32 ret;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	VB_CONFIG_S stVbConf;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize, stSizeIn, stSizeOut;
	VIDEO_FRAME_INFO_S stVpssFrame;
	struct sockaddr_in sa, cli_addr;
	socklen_t cli_len = sizeof(cli_addr);
	int cli_fd = -1;
	CVI_U32 u32BlkSize;
	int opt = 1;
	long n_total_frames = 0, n_enc_fail = 0;
	double enc_time_ms_sum = 0.0;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);   /* 客户端断开时 send 触发 SIGPIPE,忽略后 send_all 返回即可 */

	printf("=== MJPEG SW-encode streamer (port %d, out %dx%d) ===\n", port, out_w, out_h);

	/* 0. 清理残留 VPSS 组（与 capture_test.v5 一致，#55 关键修复）
	 *    注意: 不能调用 CVI_VB_Exit/CVI_SYS_Exit(媒体未初始化时会导致挂起) */
	{
		CVI_S32 g;
		printf("VPSS cleanup: stop+destroy residual groups\n");
		for (g = 0; g < (CVI_S32)VPSS_MAX_GRP_NUM; g++) {
			CVI_VPSS_StopGrp(g);
			CVI_VPSS_DestroyGrp(g);
		}
		fflush(stdout);
		usleep(200*1000);
	}

	/* 1. ParseIni */
	memset(&stIniCfg, 0, sizeof(stIniCfg));
	stIniCfg.enSource = (VI_PIPE_FRAME_SOURCE_E)VI_PIPE_FRAME_SOURCE_DEV;
	stIniCfg.devNum   = 1;
	ret = SAMPLE_COMM_VI_ParseIni(&stIniCfg);
	if (ret != CVI_SUCCESS) { printf("ParseIni fail %#x\n", ret); return -1; }
	printf("sensor=%d devNum=%d\n", stIniCfg.enSnsType[0], stIniCfg.devNum);
	CVI_VI_SetDevNum(stIniCfg.devNum);

	/* 2. VI cfg + size */
	ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (ret != CVI_SUCCESS) { printf("IniToViCfg fail %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
	if (ret != CVI_SUCCESS) { printf("GetSizeBySensor fail %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (ret != CVI_SUCCESS) { printf("GetPicSize fail %#x\n", ret); return -1; }
	printf("sensor input %dx%d\n", stSize.u32Width, stSize.u32Height);

	/* 3. VB + SYS (sample_vio 同款双 pool) */
	memset(&stVbConf, 0, sizeof(stVbConf));
	stVbConf.u32MaxPoolCnt = 2;
	u32BlkSize = COMMON_GetPicBufferSize(stSize.u32Width, stSize.u32Height, SAMPLE_PIXEL_FORMAT,
		DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkSize = u32BlkSize;
	stVbConf.astCommPool[0].u32BlkCnt  = 5;
	u32BlkSize = COMMON_GetPicBufferSize(768, 1280, SAMPLE_PIXEL_FORMAT,
		DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[1].u32BlkSize = u32BlkSize;
	stVbConf.astCommPool[1].u32BlkCnt  = 8;
	printf("pool[0] %dx%d blk=%u\n", stSize.u32Width, stSize.u32Height,
		stVbConf.astCommPool[0].u32BlkSize);
	ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (ret != CVI_SUCCESS) { printf("SYS_Init fail %#x\n", ret); return -1; }

	/* 4. VI init */
	ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (ret != CVI_SUCCESS) { printf("VI_INIT fail %#x\n", ret); goto err_sys; }

	/* 5. VPSS grp0 (输出 raw sensor 尺寸，供裁剪/缩放) */
	stSizeIn.u32Width  = stSize.u32Width;
	stSizeIn.u32Height = stSize.u32Height;
	stSizeOut.u32Width  = stSize.u32Width;   /* VPSS 全尺寸，软编码时再缩 */
	stSizeOut.u32Height = stSize.u32Height;
	ret = SAMPLE_PLAT_VPSS_INIT(VPSS_GRP, stSizeIn, stSizeOut);
	if (ret != CVI_SUCCESS) { printf("VPSS_INIT fail %#x\n", ret); goto err_sys; }
	ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP);
	if (ret != CVI_SUCCESS) { printf("Bind_VPSS fail %#x\n", ret); goto err_vpss; }
	printf("VI->VPSS bind OK\n");

	/* 6. HTTP server */
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) { printf("socket fail\n"); goto err_vpss; }
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((unsigned short)port);
	if (bind(server_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("bind %d fail: %s\n", port, strerror(errno));
		goto err_vpss;
	}
	listen(server_fd, 4);
	/* 非阻塞监听 socket，配合 poll 无阻塞 accept */
	{
		int fl = fcntl(server_fd, F_GETFL, 0);
		fcntl(server_fd, F_SETFL, fl | O_NONBLOCK);
	}
	printf("HTTP listening on port %d ...\n", port);

	sleep(2);

	/* 7. 主循环：取帧→软编码→HTTP push */
	while (g_run) {
		if (cli_fd < 0) {
			struct pollfd spfd = { server_fd, POLLIN, 0 };
			int spr = poll(&spfd, 1, 200);          /* 200ms 轮询新连接 */
			if (spr > 0 && (spfd.revents & POLLIN)) {
				cli_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
				if (cli_fd >= 0) {
					int cfl = fcntl(cli_fd, F_GETFL, 0);
					fcntl(cli_fd, F_SETFL, cfl | O_NONBLOCK);
					printf("client connected\n");
					memset(http_buf, 0, sizeof(http_buf));
					(void)recv(cli_fd, http_buf, sizeof(http_buf) - 1, 0);
					const char *hdr =
						"HTTP/1.1 200 OK\r\n"
						"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
						"Cache-Control: no-cache\r\n"
						"Connection: close\r\n"
						"\r\n";
					if (send_all(cli_fd, hdr, strlen(hdr)) != 0) {
						close(cli_fd); cli_fd = -1;  /* 客户端拒绝，立即复位 */
					}
				}
			}
		}
		if (cli_fd < 0) { usleep(20*1000); continue; }

		ret = CVI_VPSS_GetChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame, 1000);
		if (ret != CVI_SUCCESS) { close(cli_fd); cli_fd = -1; continue; }

		VIDEO_FRAME_S *f = &stVpssFrame.stVFrame;
		int fw = (int)f->u32Width, fh = (int)f->u32Height;
		int ylen = (int)f->u32Length[0];   /* Y 平面字节数 */

		/* VPSS 帧虚拟地址为空 → CVI_SYS_Mmap 映射物理地址到用户态 */
		void *va0 = (f->pu8VirAddr[0]) ? (void*)f->pu8VirAddr[0]
				: CVI_SYS_Mmap(f->u64PhyAddr[0], (CVI_U32)ylen + (CVI_U32)(fw*fh/2) + 64);
		if (!va0) {
			CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame);
			printf("mmap frame fail\n");
			n_enc_fail++;
			continue;
		}
		/* NV21: 前 fw*fh 为 Y，后 fw*fh/2 为 UV 交错 */
		unsigned char *yuv = (unsigned char *)va0;

		double t0 = (double)cv::getTickCount();

		cv::Mat nv21(fh * 3 / 2, fw, CV_8UC1, yuv);
		cv::Mat bgr, small;
		cv::cvtColor(nv21, bgr, cv::COLOR_YUV2BGR_NV21);
		cv::resize(bgr, small, cv::Size(out_w, out_h));

		std::vector<int> params;
		params.push_back(cv::IMWRITE_JPEG_QUALITY);
		params.push_back(80);
		std::vector<unsigned char> jpg;
		bool ok = cv::imencode(".jpg", small, jpg, params);

		double t1 = (double)cv::getTickCount();
		double ms = (t1 - t0) / (double)cv::getTickFrequency() / 1000.0;
		enc_time_ms_sum += ms; n_total_frames++;

		if (f->pu8VirAddr[0] == NULL) CVI_SYS_Munmap(va0, (CVI_U32)ylen + (CVI_U32)(fw*fh/2) + 64);
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame);

		if (!ok) { n_enc_fail++; continue; }

		char bnd[128];
		int bl = snprintf(bnd, sizeof(bnd),
			"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
			(unsigned)jpg.size());
		int sret = send_all(cli_fd, bnd, (size_t)bl);
		if (sret == 0) sret = send_all(cli_fd, jpg.data(), jpg.size());
		if (sret == 0) sret = send_all(cli_fd, (void*)"\r\n", 2);
		if (sret != 0) {
			/* 客户端断开，关闭并回到 accept 等待新客户端 */
			close(cli_fd); cli_fd = -1;
			continue;
		}

		if (n_total_frames % 30 == 0) {
			printf("frames=%ld enc_fail=%ld avg_enc=%.1fms (%dx%d)\n",
				n_total_frames, n_enc_fail,
				enc_time_ms_sum / (double)n_total_frames, out_w, out_h);
			fflush(stdout);
		}
		usleep(20*1000);   /* ~50fps 上限，实际受软编码耗时限制 */
	}

	if (cli_fd >= 0) close(cli_fd);
	close(server_fd);

err_vpss:
	SAMPLE_COMM_VPSS_Stop(VPSS_GRP, NULL);
err_sys:
	SAMPLE_COMM_SYS_Exit();
	printf("streamer exit (frames=%ld avg_enc=%.1fms)\n", n_total_frames,
		enc_time_ms_sum / (double)(n_total_frames ?: 1));
	return 0;
}
