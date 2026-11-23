/*
 * vo_pattern_probe.c — 8HD 黑屏分层探针（验证 #144）
 *
 * 背景：u-boot logo 能显示（跨内核存活），但 collector 与官方 sample_panel
 * 两条 Linux 点屏路径都黑。本探针每次重启后只做一个最小动作，看屏是否出红，
 * 逐层隔离杀屏调用。红色测试图保持显示直到超时（不像 sample_panel 2s 就关）。
 *
 * 用法: vo_probe <mode> [hold_sec]      （hold 默认 120s）
 *   0 = 仅 CVI_VO_ShowPattern(0, RED)，不碰 MIPI/VO 配置（基线：u-boot 活配置）
 *   1 = 仅 MIPI 8HD 重初始化，不出图（看 logo 是否被 MIPI 重初始化杀掉）
 *   2 = MIPI 8HD 重初始化 + ShowPattern RED
 *   3 = 仅 SetPubAttr(USER 时序)+Enable + ShowPattern RED（不碰 MIPI）
 *   4 = 完整 StartVO（collector 配置）+ ShowPattern RED（不碰 MIPI）
 *   5 = MIPI 重初始化 + 完整 StartVO + ShowPattern RED（= collector 完整路径）
 *
 * 每步打印返回码；进入时先 GetPubAttr 打印当前（u-boot 遗留）时序。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vo.h"
#include "linux/cvi_comm_vo.h"
#include "linux/cvi_comm_video.h"
#include "sample_comm.h"

/* 与 platform_cvitek.c s_8hd_sync 完全一致 */
static const VO_SYNC_INFO_S s_8hd_sync = {
	.bSynm = 1, .bIop = 1, .u16FrameRate = 60,
	.u16Vact = 1280, .u16Vbb = 20, .u16Vfb = 20,
	.u16Hact = 800,  .u16Hbb = 20, .u16Hfb = 174, /* hfp 扩至 174：DSI 行周期欠载修复，见 panel 头注释 */
	.u16Hpw = 18,   .u16Vpw = 4,
	.bIdv = 0, .bIhs = 1, .bIvs = 1,
};

#define P(fmt, ...) do { fprintf(stderr, "[probe] " fmt "\n", ##__VA_ARGS__); } while (0)
#define CHK(call) do { CVI_S32 r_ = (call); P("%s => %#x", #call, r_); \
	if (r_ != CVI_SUCCESS) P("!! 上一步失败"); } while (0)

static void dump_pub(const char *tag)
{
	VO_PUB_ATTR_S a;
	memset(&a, 0, sizeof(a));
	CVI_S32 r = CVI_VO_GetPubAttr(0, &a);
	if (r != CVI_SUCCESS) { P("%s GetPubAttr => %#x", tag, r); return; }
	P("%s pub: intfType=%d intfSync=%d bg=%#x | Hact=%u Hbb=%u Hfb=%u Hpw=%u "
	  "Vact=%u Vbb=%u Vfb=%u Vpw=%u fps=%u synm=%u iop=%u idv=%u ihs=%u ivs=%u",
	  tag, a.enIntfType, a.enIntfSync, a.u32BgColor,
	  a.stSyncInfo.u16Hact, a.stSyncInfo.u16Hbb, a.stSyncInfo.u16Hfb, a.stSyncInfo.u16Hpw,
	  a.stSyncInfo.u16Vact, a.stSyncInfo.u16Vbb, a.stSyncInfo.u16Vfb, a.stSyncInfo.u16Vpw,
	  a.stSyncInfo.u16FrameRate, a.stSyncInfo.bSynm, a.stSyncInfo.bIop,
	  a.stSyncInfo.bIdv, a.stSyncInfo.bIhs, a.stSyncInfo.bIvs);
}

static int start_vo_collector_cfg(void)
{
	SAMPLE_VO_CONFIG_S cfg;
	CVI_S32 r = SAMPLE_COMM_VO_GetDefConfig(&cfg);
	if (r != CVI_SUCCESS) { P("GetDefConfig => %#x", r); return -1; }
	cfg.VoDev = 0;
	cfg.stVoPubAttr.enIntfType = VO_INTF_MIPI;
	cfg.stVoPubAttr.enIntfSync = VO_OUTPUT_USER;
	cfg.stVoPubAttr.stSyncInfo = s_8hd_sync;
	cfg.stVoPubAttr.u32BgColor = 0; /* COLOR_RGB_BLACK */
	cfg.stDispRect = (RECT_S){0, 0, 800, 1280};
	cfg.stImageSize = (SIZE_S){800, 1280};
	cfg.enPixFormat = PIXEL_FORMAT_RGB_888_PLANAR;
	cfg.u32DisBufLen = 3;
	cfg.enVoMode = VO_MODE_1MUX;
	r = SAMPLE_COMM_VO_StartVO(&cfg);
	P("StartVO(collector cfg) => %#x", r);
	return r == CVI_SUCCESS ? 0 : -1;
}

/* /dev/mem 直写 PAT_CFG（绕过驱动掩码写） */
static volatile CVI_U32 *map_pat_cfg(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { P("open /dev/mem fail"); return NULL; }
	void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x0a088000);
	close(fd);
	if (m == MAP_FAILED) { P("mmap fail"); return NULL; }
	return (volatile CVI_U32 *)((char *)m + 0x94);
}

/* mode 6/7：真实帧路径（collector 的 SendFrame 方式），不用 pattern */
static int send_red_frames(int hold, int clear_pat)
{
	VB_CONFIG_S vb;
	memset(&vb, 0, sizeof(vb));
	vb.u32MaxPoolCnt = 1;
	vb.astCommPool[0].u32BlkSize = 4096;
	vb.astCommPool[0].u32BlkCnt = 1;
	CVI_S32 r = SAMPLE_COMM_SYS_Init(&vb);
	P("SYS_Init => %#x", r);
	if (r != CVI_SUCCESS) return -1;

	volatile CVI_U32 *pat = map_pat_cfg();
	if (pat) {
		P("PAT_CFG 送帧前 = %#x", *pat);
		if (clear_pat) { *pat = 0; P("PAT_CFG 整写0后 = %#x", *pat); }
	}

	VB_CAL_CONFIG_S cal;
	COMMON_GetPicBufferConfig(800, 1280, PIXEL_FORMAT_RGB_888_PLANAR,
				  DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN, &cal);
	VB_POOL_CONFIG_S pc;
	memset(&pc, 0, sizeof(pc));
	pc.u32BlkSize = cal.u32VBSize;
	pc.u32BlkCnt = 3;
	pc.enRemapMode = VB_REMAP_MODE_NOCACHE;
	snprintf(pc.acName, sizeof(pc.acName), "probe_canvas");
	VB_POOL pool = CVI_VB_CreatePool(&pc);
	if (pool == VB_INVALID_POOLID) { P("CreatePool fail"); return -1; }
	r = CVI_VB_MmapPool(pool);
	P("MmapPool => %#x blk=%u stride=%u ysize=%u", r, cal.u32VBSize,
	  cal.u32MainStride, cal.u32MainYSize);
	if (r != CVI_SUCCESS) return -1;

	int64_t t_end = (int64_t)hold * 1000;
	int64_t t0 = 0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t0 = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	int n = 0;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		if (now - t0 > t_end) break;

		VB_BLK blk = CVI_VB_GetBlock(pool, cal.u32VBSize);
		if (blk == VB_INVALID_HANDLE) { usleep(50000); continue; }
		void *vir = NULL;
		if (CVI_VB_GetBlockVirAddr(pool, blk, &vir) != CVI_SUCCESS || !vir) {
			CVI_VB_ReleaseBlock(blk); usleep(50000); continue;
		}
		CVI_U64 phys = CVI_VB_Handle2PhysAddr(blk);
		CVI_U32 stride = cal.u32MainStride, ysize = cal.u32MainYSize;
		memset(vir, 255, ysize);                 /* R */
		memset((char *)vir + ysize, 0, ysize);     /* G */
		memset((char *)vir + 2 * ysize, 0, ysize); /* B */

		VIDEO_FRAME_INFO_S vf;
		memset(&vf, 0, sizeof(vf));
		vf.stVFrame.u32Width = 800;
		vf.stVFrame.u32Height = 1280;
		vf.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888_PLANAR;
		vf.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
		vf.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
		vf.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
		vf.stVFrame.enColorGamut = COLOR_GAMUT_BT601;
		for (int i = 0; i < 3; i++) {
			vf.stVFrame.u32Stride[i] = stride;
			vf.stVFrame.u64PhyAddr[i] = phys + (CVI_U64)i * ysize;
			vf.stVFrame.pu8VirAddr[i] = (CVI_U8 *)vir + (CVI_U64)i * ysize;
			vf.stVFrame.u32Length[i] = ysize;
		}
		vf.stVFrame.u64PTS = now;
		vf.u32PoolId = pool;
		r = CVI_VO_SendFrame(0, 0, &vf, 100);
		if (n < 3 || n % 20 == 0) P("SendFrame#%d => %#x", n, r);
		CVI_VB_ReleaseBlock(blk);
		n++;
		usleep(100000);
	}
	P("送帧结束 共%d帧", n);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "用法: %s <mode 0..5> [hold_sec]\n", argv[0]);
		return 1;
	}
	int mode = atoi(argv[1]);
	int hold = argc > 2 ? atoi(argv[2]) : 120;
	if (hold < 5) hold = 5;

	P("mode=%d hold=%ds", mode, hold);
	dump_pub("进入时");

	switch (mode) {
	case 0:
		break;
	case 1:
		CHK(SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD(NULL));
		P("MIPI 已重初始化，不出图。看 logo 是否还在/变黑");
		goto hold;
	case 2:
		CHK(SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD(NULL));
		break;
	case 3: {
		VO_PUB_ATTR_S a;
		memset(&a, 0, sizeof(a));
		a.enIntfType = VO_INTF_MIPI;
		a.enIntfSync = VO_OUTPUT_USER;
		a.stSyncInfo = s_8hd_sync;
		a.u32BgColor = 0;
		CVI_S32 r = CVI_VO_SetPubAttr(0, &a);
		P("SetPubAttr(USER) => %#x", r);
		if (r != CVI_SUCCESS) {
			P("SetPubAttr 失败，先 Disable 再试");
			CHK(CVI_VO_Disable(0));
			CHK(CVI_VO_SetPubAttr(0, &a));
		}
		CHK(CVI_VO_Enable(0));
		break;
	}
	case 4:
		if (start_vo_collector_cfg() != 0) goto hold;
		break;
	case 5:
		CHK(SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD(NULL));
		if (start_vo_collector_cfg() != 0) goto hold;
		break;
	case 6:
	case 7:
		CHK(SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD(NULL));
		if (start_vo_collector_cfg() != 0) goto hold;
		send_red_frames(hold, mode == 6);
		return 0;
	default:
		P("未知 mode %d", mode);
		return 1;
	}

	CHK(CVI_VO_ShowPattern(0, VO_PAT_RED));
	dump_pub("出图后");

hold:
	P("保持 %ds，请看屏幕（红=成功，黑=失败，logo=未生效）", hold);
	sleep(hold);
	P("超时退出（测试图保持，不关闭）");
	return 0;
}