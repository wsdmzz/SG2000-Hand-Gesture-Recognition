/*
 * mjpeg_stream.c - GC2083 摄像头 MJPEG over HTTP 低延时推流（不用 RTSP）
 * 流程: GC2083→VI→VPSS(Chn0 取帧)→VENC(JPEG)→HTTP multipart/x-mixed-replace 推流
 * 浏览器直接访问: http://<board>:8881/   (ffplay -i http://<board>:8881/ 亦可)
 * 用法: ./mjpeg_stream [port]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "cvi_vb.h"
#include "cvi_sys.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "cvi_venc.h"
#include "linux/cvi_comm_video.h"
#include "linux/cvi_comm_vi.h"
#include "linux/cvi_comm_vpss.h"
#include "linux/cvi_comm_venc.h"
#include "sample_comm.h"

#define VPSS_GRP      0
#define VPSS_CHN      VPSS_CHN0
#define VENC_CHN      0
#define MJPEG_DEFAULT_PORT 8881

/* 输出分辨率（VPSS Chn + JPEG 编码） */
#define OUT_W         1280
#define OUT_H         720

static volatile int g_run = 1;
static void sig_handler(int s) { (void)s; g_run = 0; }

/* HTTP 请求缓冲 */
static char http_buf[1024];

static int server_fd = -1;

static void send_all(int fd, const void *buf, size_t len)
{
	const char *p = (const char *)buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR) continue;
			return;
		}
		off += (size_t)n;
	}
}

int main(int argc, char *argv[])
{
	int port = (argc > 1) ? atoi(argv[1]) : MJPEG_DEFAULT_PORT;
	CVI_S32 ret;
	SAMPLE_INI_CFG_S stIniCfg = {0};
	SAMPLE_VI_CONFIG_S stViConfig;
	VB_CONFIG_S stVbConf;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize, stSizeIn, stSizeOut;
	VENC_CHN_ATTR_S stVencChnAttr;
	VENC_RECV_PIC_PARAM_S stRecvParam;
	VIDEO_FRAME_INFO_S stVpssFrame;
	struct sockaddr_in sa;
	struct sockaddr_in cli_addr;
	socklen_t cli_len = sizeof(cli_addr);
	int cli_fd = -1;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	printf("=== MJPEG over HTTP streamer (port %d) ===\n", port);

	/* 1. 读取 GC2083 sensor 配置 */
	stIniCfg.enSource = VI_PIPE_FRAME_SOURCE_DEV;
	stIniCfg.devNum   = 1;
	ret = SAMPLE_COMM_VI_ParseIni(&stIniCfg);
	if (ret != CVI_SUCCESS) { printf("ParseIni fail %#x\n", ret); return -1; }
	printf("sensor=%d devNum=%d\n", stIniCfg.devNum, stIniCfg.enSnsType[0]);
	CVI_VI_SetDevNum(stIniCfg.devNum);

	/* 2. VI config + size */
	ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (ret != CVI_SUCCESS) { printf("IniToViCfg fail %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
	if (ret != CVI_SUCCESS) { printf("GetSizeBySensor fail %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (ret != CVI_SUCCESS) { printf("GetPicSize fail %#x\n", ret); return -1; }
	printf("sensor input %dx%d\n", stSize.u32Width, stSize.u32Height);

	/* 3. VB + SYS init */
	memset(&stVbConf, 0, sizeof(VB_CONFIG_S));
	stVbConf.u32MaxPoolCnt = 1;
	stVbConf.astCommPool[0].u32BlkSize = COMMON_GetPicBufferSize(
		stSize.u32Width, stSize.u32Height, SAMPLE_PIXEL_FORMAT,
		DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkCnt = 10;
	printf("pool[0] BlkSize %d\n", stVbConf.astCommPool[0].u32BlkSize);
	ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (ret != CVI_SUCCESS) { printf("SYS_Init fail %#x\n", ret); return -1; }

	/* 4. VI init (GC2083) */
	ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (ret != CVI_SUCCESS) { printf("VI_INIT fail %#x\n", ret); goto err_sys; }

	/* 5. VPSS grp0 init (输出 1280x720) */
	/* 先清理残留的 VPSS 组，避免 "Grp(0) is occupied" */
	{
		CVI_S32 g;
		printf("VPSS cleanup: stop+destroy residual groups\n");
		for (g = 0; g < (CVI_S32)VPSS_MAX_GRP_NUM; g++) {
			CVI_VPSS_StopGrp(g);
			CVI_VPSS_DestroyGrp(g);
		}
	}
	stSizeIn.u32Width = stSize.u32Width;
	stSizeIn.u32Height = stSize.u32Height;
	stSizeOut.u32Width = OUT_W;
	stSizeOut.u32Height = OUT_H;
	ret = SAMPLE_PLAT_VPSS_INIT(VPSS_GRP, stSizeIn, stSizeOut);
	if (ret != CVI_SUCCESS) { printf("VPSS_INIT fail %#x\n", ret); goto err_sys; }
	ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP);
	if (ret != CVI_SUCCESS) { printf("Bind_VPSS fail %#x\n", ret); goto err_sys; }
	printf("VI->VPSS bind OK\n");

	/* 6. VENC JPEG 通道 */
	memset(&stVencChnAttr, 0, sizeof(stVencChnAttr));
	stVencChnAttr.stVencAttr.enType = PT_JPEG;
	stVencChnAttr.stVencAttr.u32MaxPicWidth  = OUT_W;
	stVencChnAttr.stVencAttr.u32MaxPicHeight = OUT_H;
	stVencChnAttr.stVencAttr.u32PicWidth  = OUT_W;
	stVencChnAttr.stVencAttr.u32PicHeight = OUT_H;
	stVencChnAttr.stVencAttr.bByFrame = CVI_TRUE;
	stVencChnAttr.stVencAttr.stAttrJpege.bSupportDCF = CVI_FALSE;
	stVencChnAttr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
	stVencChnAttr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
	stVencChnAttr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	ret = CVI_VENC_CreateChn(VENC_CHN, &stVencChnAttr);
	if (ret != CVI_SUCCESS) { printf("VENC_CreateChn fail %#x\n", ret); goto err_vpss; }

	memset(&stRecvParam, 0, sizeof(stRecvParam));
	stRecvParam.s32RecvPicNum = -1;
	ret = CVI_VENC_StartRecvFrame(VENC_CHN, &stRecvParam);
	if (ret != CVI_SUCCESS) { printf("VENC_StartRecvFrame fail %#x\n", ret); goto err_venc; }
	printf("VENC JPEG chn ok\n");

	/* 7. HTTP server */
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) { printf("socket fail\n"); goto err_venc; }
	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((unsigned short)port);
	if (bind(server_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("bind port %d fail: %s\n", port, strerror(errno));
		goto err_venc;
	}
	listen(server_fd, 4);
	printf("HTTP listening on port %d ...\n", port);
	printf("浏览器访问: http://<board_ip>:%d/\n", port);

	sleep(2);

	/* 8. 主循环：取帧→JPEG→HTTP push */
	while (g_run) {
		/* 有 HTTP 客户端连接则 accept */
		if (cli_fd < 0) {
			cli_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
			if (cli_fd >= 0) {
				printf("client connected\n");
				/* 读请求头（丢弃内容，可读到空行即请求结束） */
				memset(http_buf, 0, sizeof(http_buf));
				(void)recv(cli_fd, http_buf, sizeof(http_buf) - 1, 0);
				/* 发送 multipart 响应头 */
				const char *hdr =
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
					"Cache-Control: no-cache\r\n"
					"Connection: close\r\n"
					"\r\n";
				send_all(cli_fd, hdr, strlen(hdr));
			}
		}

		if (cli_fd < 0) {
			usleep(50 * 1000);
			continue;
		}

		/* 取 VPSS 帧 */
		ret = CVI_VPSS_GetChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame, 1000);
		if (ret != CVI_SUCCESS) {
			/* 客户端可能断开 */
			close(cli_fd); cli_fd = -1;
			continue;
		}

		/* 送 JPEG 编码 */
		ret = CVI_VENC_SendFrame(VENC_CHN, &stVpssFrame, 1000);
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP, VPSS_CHN, &stVpssFrame);
		if (ret != CVI_SUCCESS) {
			close(cli_fd); cli_fd = -1;
			continue;
		}

		/* 取 JPEG 流 */
		VENC_STREAM_S stStream;
		memset(&stStream, 0, sizeof(stStream));
		VENC_CHN_STATUS_S stStat;
		ret = CVI_VENC_QueryStatus(VENC_CHN, &stStat);
		if (ret == CVI_SUCCESS && stStat.u32CurPacks > 0) {
			stStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * stStat.u32CurPacks);
			if (stStream.pstPack) {
				ret = CVI_VENC_GetStream(VENC_CHN, &stStream, 1000);
				if (ret == CVI_SUCCESS) {
					/* 组帧发送 */
					char bnd[128];
					int bl = snprintf(bnd, sizeof(bnd),
						"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
						stStream.u32PackCount > 0 ? stStream.pstPack[0].u32Len : 0);
					send_all(cli_fd, bnd, (size_t)bl);
					CVI_U32 total = 0;
					for (CVI_U32 p = 0; p < stStream.u32PackCount; p++) {
						void *addr = (void *)stStream.pstPack[p].pu8Addr;
						send_all(cli_fd, addr, stStream.pstPack[p].u32Len);
						total += stStream.pstPack[p].u32Len;
					}
					send_all(cli_fd, (void *)"\r\n", 2);
					CVI_VENC_ReleaseStream(VENC_CHN, &stStream);
					(void)total;
				}
				free(stStream.pstPack);
			}
		}
	}

	/* 清理 */
	if (cli_fd >= 0) close(cli_fd);
	close(server_fd);

err_venc:
	CVI_VENC_StopRecvFrame(VENC_CHN);
	CVI_VENC_DestroyChn(VENC_CHN);
err_vpss:
	/* vpss/vi cleanup */
err_sys:
	SAMPLE_COMM_SYS_Exit();
	printf("streamer exit\n");
	return 0;
}
