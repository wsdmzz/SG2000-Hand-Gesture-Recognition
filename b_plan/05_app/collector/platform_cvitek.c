/*
 * platform_cvitek.c — CV181x(SG2000) 平台适配：VI/VPSS/VENC/VO/GT911
 *
 * 依据（均已对 sophpi SDK 源码核实，见验证日志 #140）：
 *   - VI/VPSS 取帧序列：vi_vpss_capture_test.c（板上验证过）
 *   - VPSS 双通道：chn0=1920x1080 NV21 depth2（供 VENC 抓帧），
 *                  chn1=800x450 NV21 depth1（供屏上预览）
 *   - VENC JPEG 非绑定：GetChnFrame→SendFrame→GetStream→拷贝（q90）
 *   - VO 8HD：SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD + VO_OUTPUT_USER 自定义时序
 *             （800x1280@60，dsi_milkv_8hd.h dev_cfg 对照推导）
 *   - 屏上 UI：CPU 绘 RGB_888_PLANAR 画布（独立 VB pool，NOCACHE 免 flush）
 *             → CVI_VO_SendFrame；预览 NV21→RGB 整数近似
 *   - 触摸：GT911 evdev（"goodix-ts"），ABS_MT_POSITION_X/Y + 抬起触发，
 *           EVIOCGABS 自动量程归一化 0..1000
 *
 * 平台契约（collector.h col_platform_t）：
 *   init 返回 0 成功；capture_jpeg 的 buf 平台持有、下次调用前有效；
 *   poll_touch 归一化 0..1000、抬起触发、0 无事件/1 有事件/-1 出错；render 可选。
 *
 * 注意：ctx 参数恒为 NULL（main 传 NULL），状态存静态 g_ctx。
 */
#define _GNU_SOURCE   /* strcasestr */
#include "collector.h"
#include "ui_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input.h>

#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "cvi_venc.h"
#include "cvi_vo.h"
#include "linux/cvi_comm_video.h"
#include "linux/cvi_comm_vi.h"
#include "linux/cvi_comm_vpss.h"
#include "linux/cvi_comm_venc.h"
#include "linux/cvi_comm_vo.h"
#include "linux/cvi_buffer.h"
#include "sample_comm.h"

/* ---- 几何/通道常量 ---- */
#define CAP_W        1920
#define CAP_H        1080
#define PREV_W       800
#define PREV_H       450          /* 16:9，VPSS chn1 输出 */
#define VO_W         800
#define VO_H         1280
#define STATUS_H     48           /* 状态栏像素高 */
#define PREV_X       0
#define PREV_Y       80           /* 预览图在画布中的纵向起点（状态栏下居中） */
#define JPEG_QFACTOR 90

#define COL_GRP     0
#define CHN_CAP      VPSS_CHN0
#define CHN_PREV     VPSS_CHN1
#define COL_VENC     0
#define COL_VODEV       0
#define COL_VOLAYER     0
#define COL_VOCHN       0

/* 触摸校准（#2 实测后调整）：GT911 与屏坐标轴关系 */
#ifndef COL_TOUCH_SWAP_XY
#define COL_TOUCH_SWAP_XY 0
#endif
#ifndef COL_TOUCH_INVERT_X
#define COL_TOUCH_INVERT_X 0
#endif
#ifndef COL_TOUCH_INVERT_Y
#define COL_TOUCH_INVERT_Y 0
#endif
/* 摄像头镜像/翻转（#6 现场标定后调整；默认自然方向） */
#ifndef COL_CAM_MIRROR
#define COL_CAM_MIRROR 0
#endif
#ifndef COL_CAM_FLIP
#define COL_CAM_FLIP 0
#endif

#define LOG(fmt, ...) fprintf(stderr, "[cvitek] " fmt "\n", ##__VA_ARGS__)

/* ---- 8HD VO 时序（dsi_milkv_8hd.h dev_cfg 对照 vo_sdk_layer.c 映射推导） ---- */
static const VO_SYNC_INFO_S s_8hd_sync = {
	.bSynm = 1, .bIop = 1, .u16FrameRate = 60,
	.u16Vact = 1280, .u16Vbb = 20, .u16Vfb = 20,
	.u16Hact = 800,  .u16Hbb = 20, .u16Hfb = 174, /* hfp 40→174：内核 mipi_tx 缺 u-boot _cal_htt_extra，
						       * htotal=878 时 DSI 每行欠载 ~1.5us 导致花屏；
						       * 与 dsi_milkv_8hd.h pixel_clk=80412 配套 */
	.u16Hpw = 18,   .u16Vpw = 4,
	.bIdv = 0, .bIhs = 1, .bIvs = 1,
};

typedef struct {
	int inited;
	SAMPLE_VI_CONFIG_S vi_cfg;
	SIZE_S sensor_size;

	VB_POOL canvas_pool;
	VB_CAL_CONFIG_S canvas_cal;   /* RGB planar 800x1280 的 stride/plane 尺寸 */
	CVI_U32 canvas_blk_size;

	int venc_created;
	uint8_t *jpeg_buf;
	size_t jpeg_cap;
	size_t jpeg_len;

	SAMPLE_VO_CONFIG_S vo_cfg;
	int vo_started;

	int touch_fd;
	int touch_x_max, touch_y_max;   /* EVIOCGABS 量程 */
	int raw_x, raw_y;               /* 最近原始坐标 */
	int down;                       /* 是否按下 */
	int release_pending;            /* 本 SYN 周期内检测到抬起 */

	int64_t last_render_ms;

	/* 渲染性能统计（每 120 帧打印一次，验证日志 #145） */
	int stat_n;
	int64_t stat_render_us, stat_blit_us;
	int stat_miss, stat_sendfail;
} cvitek_ctx_t;

static cvitek_ctx_t g_ctx;

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void clamp_tbl_init(void);   /* 定义在 blit_preview 处 */

/* ================= 工具：颜色/绘制 ================= */
typedef struct { uint8_t r, g, b; } rgb_t;
static rgb_t mkrgb(uint8_t r, uint8_t g, uint8_t b) { rgb_t c = {r, g, b}; return c; }

/* 画布平面指针（R/G/B），stride 由 canvas_cal 给出 */
typedef struct {
	uint8_t *plane[3];
	CVI_U32 stride;
	int w, h;
} canvas_t;

static void cv_fill(canvas_t *c, int x0, int y0, int x1, int y1, rgb_t col)
{
	if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
	if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
	if (x1 <= x0 || y1 <= y0) return;
	int n = x1 - x0;
	for (int y = y0; y < y1; y++) {
		memset(c->plane[0] + y * c->stride + x0, col.r, n);
		memset(c->plane[1] + y * c->stride + x0, col.g, n);
		memset(c->plane[2] + y * c->stride + x0, col.b, n);
	}
}

static void cv_pixel(canvas_t *c, int x, int y, rgb_t col)
{
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
	c->plane[0][y * c->stride + x] = col.r;
	c->plane[1][y * c->stride + x] = col.g;
	c->plane[2][y * c->stride + x] = col.b;
}

/* CJK 码点二分查找 */
static const font_cjk_glyph_t *find_cjk(unsigned int cp)
{
	int lo = 0, hi = FONT_CJK_COUNT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (font_cjk[mid].cp == cp) return &font_cjk[mid];
		if (font_cjk[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
	}
	return NULL;
}

/* 取一个 UTF-8 码点，*adv 推进字节数 */
static unsigned int utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	unsigned int cp; int n;
	if (p[0] < 0x80) { cp = p[0]; n = 1; }
	else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
	else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
	else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
	else { cp = '?'; n = 1; }
	for (int i = 1; i < n; i++) cp = (cp << 6) | (p[i] & 0x3F);
	*s += n;
	return cp;
}

/* 文本宽度（像素，含 scale） */
static int text_width(const char *s, int scale)
{
	int w = 0;
	while (*s) {
		unsigned int cp = utf8_next(&s);
		w += (cp < 0x80 ? FONT_ASCII_W : FONT_CJK_W) * scale;
	}
	return w;
}

/* 绘文本（scale 整数倍放大），返回结束 x */
static int cv_text(canvas_t *c, int x, int y, const char *s, rgb_t col, int scale)
{
	while (*s) {
		unsigned int cp = utf8_next(&s);
		const unsigned char *bits; int gw, gh, nbytes;
		if (cp < 0x80) {
			if (cp < FONT_ASCII_FIRST || cp >= FONT_ASCII_FIRST + FONT_ASCII_COUNT)
				cp = '?';
			bits = font_ascii[cp - FONT_ASCII_FIRST];
			gw = FONT_ASCII_W; gh = FONT_ASCII_H; nbytes = 1;
		} else {
			const font_cjk_glyph_t *g = find_cjk(cp);
			if (!g) { x += FONT_CJK_W * scale; continue; }  /* 缺字留空 */
			bits = g->bits; gw = FONT_CJK_W; gh = FONT_CJK_H; nbytes = 2;
		}
		for (int row = 0; row < gh; row++) {
			for (int cx = 0; cx < gw; cx++) {
				if (bits[row * nbytes + cx / 8] & (1 << (7 - cx % 8))) {
					for (int sy = 0; sy < scale; sy++)
						for (int sx = 0; sx < scale; sx++)
							cv_pixel(c, x + cx * scale + sx,
								 y + row * scale + sy, col);
				}
			}
		}
		x += gw * scale;
	}
	return x;
}

/* ================= VI/VPSS/VENC/VO 初始化 ================= */
static int init_video(void)
{
	CVI_S32 ret;
	SAMPLE_INI_CFG_S ini = {0};
	VB_CONFIG_S vb = {0};
	PIC_SIZE_E enPicSize;
	CVI_U32 blk;

	/* 0. 清理残留 VPSS 组 + VB/SYS（vi_vpss_capture_test 验证过的开场） */
	for (int g = 0; g < VPSS_MAX_GRP_NUM; g++) {
		CVI_VPSS_StopGrp(g);
		CVI_VPSS_DestroyGrp(g);
	}
	CVI_VB_Exit();
	CVI_SYS_Exit();
	usleep(200 * 1000);

	/* 1. ParseIni（/mnt/data/sensor_cfg.ini，GC2083） */
	ini.enSource = VI_PIPE_FRAME_SOURCE_DEV;
	ini.devNum = 1;
	ret = SAMPLE_COMM_VI_ParseIni(&ini);
	if (ret != CVI_SUCCESS) { LOG("ParseIni fail %#x", ret); return -1; }
	CVI_VI_SetDevNum(ini.devNum);
	ret = SAMPLE_COMM_VI_IniToViCfg(&ini, &g_ctx.vi_cfg);
	if (ret != CVI_SUCCESS) { LOG("IniToViCfg fail %#x", ret); return -1; }
	ret = SAMPLE_COMM_VI_GetSizeBySensor(ini.enSnsType[0], &enPicSize);
	if (ret != CVI_SUCCESS) { LOG("GetSizeBySensor fail %#x", ret); return -1; }
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &g_ctx.sensor_size);
	if (ret != CVI_SUCCESS) { LOG("GetPicSize fail %#x", ret); return -1; }
	LOG("sensor %dx%d", g_ctx.sensor_size.u32Width, g_ctx.sensor_size.u32Height);

	/* 2. VB 双公共 pool（对齐验证过的 sample_vio 配置） */
	vb.u32MaxPoolCnt = 2;
	blk = COMMON_GetPicBufferSize(g_ctx.sensor_size.u32Width, g_ctx.sensor_size.u32Height,
				      SAMPLE_PIXEL_FORMAT, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	vb.astCommPool[0].u32BlkSize = blk;
	vb.astCommPool[0].u32BlkCnt = 5;
	blk = COMMON_GetPicBufferSize(768, 1280, SAMPLE_PIXEL_FORMAT,
				      DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	vb.astCommPool[1].u32BlkSize = blk;
	vb.astCommPool[1].u32BlkCnt = 8;
	ret = SAMPLE_COMM_SYS_Init(&vb);
	if (ret != CVI_SUCCESS) { LOG("SYS_Init fail %#x", ret); return -1; }

	/* 3. VI init */
	ret = SAMPLE_PLAT_VI_INIT(&g_ctx.vi_cfg);
	if (ret != CVI_SUCCESS) { LOG("VI_INIT fail %#x", ret); return -1; }

	/* 4. VPSS grp0 双通道：chn0 抓帧 1080p depth2 / chn1 预览 800x450 depth1 */
	{
		VPSS_GRP_ATTR_S grp = {0};
		VPSS_CHN_ATTR_S chn[VPSS_MAX_PHY_CHN_NUM] = {{0}};
		CVI_BOOL en[VPSS_MAX_PHY_CHN_NUM] = {0};

		grp.stFrameRate.s32SrcFrameRate = -1;
		grp.stFrameRate.s32DstFrameRate = -1;
		grp.enPixelFormat = SAMPLE_PIXEL_FORMAT;
		grp.u32MaxW = g_ctx.sensor_size.u32Width;
		grp.u32MaxH = g_ctx.sensor_size.u32Height;
		grp.u8VpssDev = 0;

		for (int i = 0; i < 2; i++) {
			chn[i].enVideoFormat = VIDEO_FORMAT_LINEAR;
			chn[i].enPixelFormat = SAMPLE_PIXEL_FORMAT;
			chn[i].stFrameRate.s32SrcFrameRate = 30;
			chn[i].stFrameRate.s32DstFrameRate = 30;
			chn[i].bMirror = COL_CAM_MIRROR ? CVI_TRUE : CVI_FALSE;
			chn[i].bFlip = COL_CAM_FLIP ? CVI_TRUE : CVI_FALSE;
			chn[i].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
			chn[i].stAspectRatio.bEnableBgColor = CVI_TRUE;
			chn[i].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
			chn[i].stNormalize.bEnable = CVI_FALSE;
		}
		chn[CHN_CAP].u32Width = CAP_W;
		chn[CHN_CAP].u32Height = CAP_H;
		chn[CHN_CAP].u32Depth = 2;
		chn[CHN_PREV].u32Width = PREV_W;
		chn[CHN_PREV].u32Height = PREV_H;
		chn[CHN_PREV].u32Depth = 1;
		en[CHN_CAP] = CVI_TRUE;
		en[CHN_PREV] = CVI_TRUE;

		ret = SAMPLE_COMM_VPSS_Init(COL_GRP, en, &grp, chn);
		if (ret != CVI_SUCCESS) { LOG("VPSS_Init fail %#x", ret); return -1; }
		ret = SAMPLE_COMM_VPSS_Start(COL_GRP, en, &grp, chn);
		if (ret != CVI_SUCCESS) { LOG("VPSS_Start fail %#x", ret); return -1; }
	}

	/* 5. VI→VPSS 绑定 */
	ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, COL_GRP);
	if (ret != CVI_SUCCESS) { LOG("Bind_VPSS fail %#x", ret); return -1; }

	/* 6. VENC JPEG 通道（非绑定，q90） */
	{
		VENC_CHN_ATTR_S attr = {0};
		VENC_JPEG_PARAM_S jp = {0};
		attr.stVencAttr.enType = PT_JPEG;
		attr.stVencAttr.u32MaxPicWidth = CAP_W;
		attr.stVencAttr.u32MaxPicHeight = CAP_H;
		attr.stVencAttr.u32PicWidth = CAP_W;
		attr.stVencAttr.u32PicHeight = CAP_H;
		attr.stVencAttr.u32BufSize = CAP_W * CAP_H * 2;  /* CV181x JPEGE 要求码流 buf ≥ 2×帧大小，
		                                                  用 YUV 帧大小(3.1MB)会 NOT_SUPPORT(0xc0078008)，probe 实测 */
		attr.stVencAttr.u32Profile = 0;
		attr.stVencAttr.bByFrame = CVI_TRUE;
		attr.stVencAttr.bEsBufQueueEn = CVI_FALSE;
		attr.stVencAttr.bIsoSendFrmEn = CVI_FALSE;
		attr.stVencAttr.stAttrJpege.bSupportDCF = CVI_FALSE;
		attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
		attr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
		attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
		attr.stGopAttr.stNormalP.s32IPQpDelta = 0;

		/* COL_VENC_PROBE=1：逐个变体试 CreateChn，定位 NOT_SUPPORT 根因 */
		if (getenv("COL_VENC_PROBE")) {
			VENC_CHN_ATTR_S a;
			VENC_PARAM_MOD_S mp;
			int v;
			for (v = 0; v < 6; v++) {
				memcpy(&a, &attr, sizeof(a));
				if (v == 1) a.stVencAttr.u32BufSize = CAP_W * CAP_H;
				if (v == 2) a.stVencAttr.bEsBufQueueEn = CVI_TRUE;
				if (v == 3) {
					memset(&mp, 0, sizeof(mp));
					mp.enVencModType = MODTYPE_JPEGE;
					ret = CVI_VENC_GetModParam(&mp);
					LOG("probe GetModParam rc=%#x", ret);
					mp.stJpegeModParam.enJpegeFormat = JPEGE_FORMAT_DEFAULT;
					ret = CVI_VENC_SetModParam(&mp);
					LOG("probe SetModParam rc=%#x", ret);
				}
				if (v == 4) a.stVencAttr.u32BufSize = CAP_W * CAP_H * 2;
				if (v == 5) { a.stVencAttr.bEsBufQueueEn = CVI_TRUE;
				              a.stVencAttr.u32BufSize = CAP_W * CAP_H; }
				ret = CVI_VENC_CreateChn(COL_VENC, &a);
				LOG("probe v%d CreateChn rc=%#x", v, ret);
				if (ret == CVI_SUCCESS) CVI_VENC_DestroyChn(COL_VENC);
			}
			return -1;  /* probe 模式：只报告，不进主流程 */
		}

		ret = CVI_VENC_CreateChn(COL_VENC, &attr);
		if (ret != CVI_SUCCESS) { LOG("VENC_CreateChn fail %#x", ret); return -1; }
		g_ctx.venc_created = 1;
		ret = CVI_VENC_GetJpegParam(COL_VENC, &jp);
		if (ret == CVI_SUCCESS) {
			jp.u32Qfactor = JPEG_QFACTOR;
			jp.u32MCUPerECS = 0;
			ret = CVI_VENC_SetJpegParam(COL_VENC, &jp);
		}
		if (ret != CVI_SUCCESS) { LOG("SetJpegParam fail %#x", ret); return -1; }
	}

	/* 7. VO：8HD panel + USER 时序 + RGB planar 层 */
	ret = SAMPLE_COMM_VO_Init_MIPI_MILKV_8HD(NULL);
	if (ret != CVI_SUCCESS) { LOG("Init_MIPI_8HD fail %#x", ret); return -1; }

	ret = SAMPLE_COMM_VO_GetDefConfig(&g_ctx.vo_cfg);
	if (ret != CVI_SUCCESS) { LOG("VO_GetDefConfig fail %#x", ret); return -1; }
	g_ctx.vo_cfg.VoDev = COL_VODEV;
	g_ctx.vo_cfg.stVoPubAttr.enIntfType = VO_INTF_MIPI;
	g_ctx.vo_cfg.stVoPubAttr.enIntfSync = VO_OUTPUT_USER;
	g_ctx.vo_cfg.stVoPubAttr.stSyncInfo = s_8hd_sync;
	g_ctx.vo_cfg.stVoPubAttr.u32BgColor = COLOR_RGB_BLACK;
	g_ctx.vo_cfg.stDispRect = (RECT_S){0, 0, VO_W, VO_H};
	g_ctx.vo_cfg.stImageSize = (SIZE_S){VO_W, VO_H};
	g_ctx.vo_cfg.enPixFormat = PIXEL_FORMAT_RGB_888_PLANAR;
	g_ctx.vo_cfg.u32DisBufLen = 3;
	g_ctx.vo_cfg.enVoMode = VO_MODE_1MUX;
	ret = SAMPLE_COMM_VO_StartVO(&g_ctx.vo_cfg);
	if (ret != CVI_SUCCESS) { LOG("VO_StartVO fail %#x", ret); return -1; }
	g_ctx.vo_started = 1;

	/* boot 默认 PAT_CFG=0x1002a 使能 pattern 发生器（bit3+16 为真使能位，
	 * 驱动位定义与 cv181x 硬件不符），会叠加在视频上花屏 ⇒ /dev/mem 整写 0。
	 * DSI 行周期欠载花屏另由 panel 头 hfp=174/pixel_clk=80412 + s_8hd_sync 修复。 */
	{
		int mfd = open("/dev/mem", O_RDWR | O_SYNC);
		if (mfd >= 0) {
			volatile uint32_t *disp = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
						     MAP_SHARED, mfd, 0x0a088000);
			if (disp != MAP_FAILED) {
				LOG("PAT_CFG boot=%#x -> 0", disp[0x94 / 4]);
				disp[0x94 / 4] = 0;
				munmap((void *)disp, 4096);
			}
			close(mfd);
		}
	}

	/* 8. 画布 VB pool（RGB planar，NOCACHE 免 flush） */
	COMMON_GetPicBufferConfig(VO_W, VO_H, PIXEL_FORMAT_RGB_888_PLANAR,
				  DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN, &g_ctx.canvas_cal);
	g_ctx.canvas_blk_size = g_ctx.canvas_cal.u32VBSize;
	{
		VB_POOL_CONFIG_S pc = {0};
		pc.u32BlkSize = g_ctx.canvas_blk_size;
		pc.u32BlkCnt = 4;   /* 3→4：30fps 渲染 + VO DisBufLen=3 防 GetBlock 饥饿（#145） */
		pc.enRemapMode = VB_REMAP_MODE_CACHED;   /* NOCACHE→CACHED：C906 非缓存逐字节写
							  * ~2.8MB/帧要 ~90ms；缓存写+送显前
							  * IonFlushCache 回写（#145） */
		snprintf(pc.acName, sizeof(pc.acName), "col_canvas");
		g_ctx.canvas_pool = CVI_VB_CreatePool(&pc);
		if (g_ctx.canvas_pool == VB_INVALID_POOLID) { LOG("canvas CreatePool fail"); return -1; }
		ret = CVI_VB_MmapPool(g_ctx.canvas_pool);
		if (ret != CVI_SUCCESS) { LOG("canvas MmapPool fail %#x", ret); return -1; }
	}
	LOG("canvas blk=%u stride=%u planeY=%u", g_ctx.canvas_blk_size,
	    g_ctx.canvas_cal.u32MainStride, g_ctx.canvas_cal.u32MainYSize);
	clamp_tbl_init();
	return 0;
}

/* ================= 触摸 ================= */
static int touch_open(void)
{
	DIR *d = opendir("/dev/input");
	if (!d) { LOG("opendir /dev/input: %s", strerror(errno)); return -1; }
	struct dirent *de;
	char path[64], name[128];
	int fd = -1;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "event", 5) != 0) continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		int f = open(path, O_RDONLY | O_NONBLOCK);
		if (f < 0) continue;
		if (ioctl(f, EVIOCGNAME(sizeof(name)), name) < 0) { close(f); continue; }
		/* goodix-ts / goodix-pen / gt9xx 均匹配 */
		if (strcasestr(name, "goodix") || strcasestr(name, "gt9")) {
			fd = f;
			LOG("触摸设备 %s = \"%s\"", path, name);
			break;
		}
		close(f);
	}
	closedir(d);
	if (fd < 0) { LOG("未找到 GT911 触摸设备（UI 不可点，AUTOREC 仍可采）"); return -1; }

	/* 量程：EVIOCGABS 自动获取 */
	struct input_absinfo ax, ay;
	g_ctx.touch_x_max = 800; g_ctx.touch_y_max = 1280;
	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 && ax.maximum > ax.minimum)
		g_ctx.touch_x_max = ax.maximum;
	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 && ay.maximum > ay.minimum)
		g_ctx.touch_y_max = ay.maximum;
	LOG("触摸量程 x_max=%d y_max=%d swap=%d invx=%d invy=%d",
	    g_ctx.touch_x_max, g_ctx.touch_y_max,
	    COL_TOUCH_SWAP_XY, COL_TOUCH_INVERT_X, COL_TOUCH_INVERT_Y);
	return fd;
}

static void touch_normalize(int raw_x, int raw_y, int *nx, int *ny)
{
	int x = raw_x, y = raw_y;
	if (COL_TOUCH_SWAP_XY) { int t = x; x = y; y = t; }
	int vx = x * 1000 / (g_ctx.touch_x_max > 0 ? g_ctx.touch_x_max : 1);
	int vy = y * 1000 / (g_ctx.touch_y_max > 0 ? g_ctx.touch_y_max : 1);
	if (COL_TOUCH_INVERT_X) vx = 1000 - vx;
	if (COL_TOUCH_INVERT_Y) vy = 1000 - vy;
	if (vx < 0) vx = 0; if (vx > 1000) vx = 1000;
	if (vy < 0) vy = 0; if (vy > 1000) vy = 1000;
	*nx = vx; *ny = vy;
}

/* ================= col_platform_t 实现 ================= */
static int plat_init(void *ctx, const col_config_t *cfg)
{
	(void)ctx; (void)cfg;
	memset(&g_ctx, 0, sizeof(g_ctx));
	g_ctx.touch_fd = -1;
	g_ctx.canvas_pool = VB_INVALID_POOLID;

	if (init_video() != 0) {
		LOG("视频链路初始化失败");
		/* 尽力回收已建资源 */
		if (g_ctx.venc_created) CVI_VENC_DestroyChn(COL_VENC);
		if (g_ctx.vo_started) SAMPLE_COMM_VO_StopVO(&g_ctx.vo_cfg);
		SAMPLE_COMM_SYS_Exit();
		return -1;
	}
	g_ctx.touch_fd = touch_open();   /* 失败不致命 */
	g_ctx.inited = 1;
	LOG("平台初始化完成");
	return 0;
}

static int plat_capture_jpeg(void *ctx, const uint8_t **buf, size_t *len)
{
	(void)ctx;
	if (!g_ctx.inited) return -1;
	VIDEO_FRAME_INFO_S frame;
	CVI_S32 ret = CVI_VPSS_GetChnFrame(COL_GRP, CHN_CAP, &frame, 1000);
	if (ret != CVI_SUCCESS) { LOG("GetChnFrame(cap) fail %#x", ret); return -1; }

	VENC_RECV_PIC_PARAM_S recv = { .s32RecvPicNum = 1 };
	ret = CVI_VENC_StartRecvFrame(COL_VENC, &recv);
	if (ret != CVI_SUCCESS) {
		LOG("StartRecvFrame fail %#x", ret);
		CVI_VPSS_ReleaseChnFrame(COL_GRP, CHN_CAP, &frame);
		return -1;
	}
	ret = CVI_VENC_SendFrame(COL_VENC, &frame, 1000);
	CVI_VPSS_ReleaseChnFrame(COL_GRP, CHN_CAP, &frame);
	if (ret != CVI_SUCCESS) { LOG("VENC_SendFrame fail %#x", ret); return -1; }

	/* 等编码完成 */
	VENC_CHN_STATUS_S stat;
	int waited = 0;
	do {
		ret = CVI_VENC_QueryStatus(COL_VENC, &stat);
		if (ret != CVI_SUCCESS) { LOG("QueryStatus fail %#x", ret); return -1; }
		if (stat.u32LeftStreamFrames > 0) break;
		usleep(2000); waited += 2;
	} while (waited < 1000);
	if (stat.u32LeftStreamFrames == 0) { LOG("JPEG 编码超时"); return -1; }

	VENC_STREAM_S stream = {0};
	stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * stat.u32CurPacks);
	if (!stream.pstPack) { LOG("malloc pack fail"); return -1; }
	stream.u32PackCount = stat.u32CurPacks;
	ret = CVI_VENC_GetStream(COL_VENC, &stream, 1000);
	if (ret != CVI_SUCCESS) {
		LOG("GetStream fail %#x", ret);
		free(stream.pstPack);
		return -1;
	}

	/* 累计有效长度并扩容拷贝缓冲 */
	size_t total = 0;
	for (CVI_U32 i = 0; i < stream.u32PackCount; i++)
		total += stream.pstPack[i].u32Len - stream.pstPack[i].u32Offset;
	if (total > g_ctx.jpeg_cap) {
		uint8_t *nb = (uint8_t *)realloc(g_ctx.jpeg_buf, total);
		if (!nb) { LOG("realloc jpeg fail"); CVI_VENC_ReleaseStream(COL_VENC, &stream); free(stream.pstPack); return -1; }
		g_ctx.jpeg_buf = nb; g_ctx.jpeg_cap = total;
	}
	size_t off = 0;
	for (CVI_U32 i = 0; i < stream.u32PackCount; i++) {
		VENC_PACK_S *p = &stream.pstPack[i];
		size_t n = p->u32Len - p->u32Offset;
		memcpy(g_ctx.jpeg_buf + off, p->pu8Addr + p->u32Offset, n);
		off += n;
	}
	g_ctx.jpeg_len = off;
	CVI_VENC_ReleaseStream(COL_VENC, &stream);
	free(stream.pstPack);
	CVI_VENC_StopRecvFrame(COL_VENC);

	*buf = g_ctx.jpeg_buf;
	*len = g_ctx.jpeg_len;
	return 0;
}

static int plat_poll_touch(void *ctx, int timeout_ms, int *nx, int *ny)
{
	(void)ctx;
	if (g_ctx.touch_fd < 0) { usleep(timeout_ms * 1000); return 0; }

	struct pollfd pfd = { .fd = g_ctx.touch_fd, .events = POLLIN };
	int pr = poll(&pfd, 1, timeout_ms);
	if (pr <= 0) return 0;   /* 超时或无事件 */

	struct input_event ev[64];
	ssize_t n = read(g_ctx.touch_fd, ev, sizeof(ev));
	if (n < 0) {
		if (errno == EAGAIN) return 0;
		LOG("touch read: %s", strerror(errno));
		return -1;
	}
	int cnt = (int)(n / sizeof(struct input_event));
	int emitted = 0;
	for (int i = 0; i < cnt; i++) {
		struct input_event *e = &ev[i];
		if (e->type == EV_ABS) {
			if (e->code == ABS_MT_POSITION_X || e->code == ABS_X)
				g_ctx.raw_x = e->value;
			else if (e->code == ABS_MT_POSITION_Y || e->code == ABS_Y)
				g_ctx.raw_y = e->value;
			else if (e->code == ABS_MT_TRACKING_ID) {
				if (e->value >= 0) g_ctx.down = 1;
				else if (g_ctx.down) { g_ctx.release_pending = 1; }
			}
		} else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
			if (e->value) g_ctx.down = 1;
			else if (g_ctx.down) g_ctx.release_pending = 1;
		} else if (e->type == EV_SYN && e->code == SYN_REPORT) {
			if (g_ctx.release_pending) {
				g_ctx.release_pending = 0;
				g_ctx.down = 0;
				touch_normalize(g_ctx.raw_x, g_ctx.raw_y, nx, ny);
				emitted = 1;   /* 抬起触发：取最后坐标 */
			}
		}
	}
	return emitted ? 1 : 0;
}

/* 预览 NV21→RGB 整数近似，blit 到画布 (PREV_X,PREV_Y)。
 * 优化（#145）：
 *  - VPSS 帧缓冲默认非缓存映射，C906 逐字节读 ~120ms/帧；改用 MmapCache
 *    持久映射（按物理地址缓存，VPSS 缓冲块固定），每帧读前 invalidate。
 *  - 每 2 像素共享一次 UV 增量；夹取用查表代替逐像素分支。 */
static uint8_t clamp_tbl[768];   /* 索引 = val+256，覆盖 [-256,511] */
static void clamp_tbl_init(void)
{
	for (int i = 0; i < 768; i++) {
		int v = i - 256;
		clamp_tbl[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
	}
}

#define FMAP_MAX 16
typedef struct { CVI_U64 phys; CVI_U32 len; void *va; } fmap_t;
static fmap_t g_fmap[FMAP_MAX];   /* Y/VU 平面各占一项 */
static int g_fmap_n;

static void *fmap_get(CVI_U64 phys, CVI_U32 len)
{
	for (int i = 0; i < g_fmap_n; i++)
		if (g_fmap[i].phys == phys && g_fmap[i].len == len)
			return g_fmap[i].va;
	if (g_fmap_n >= FMAP_MAX) return NULL;
	void *va = CVI_SYS_MmapCache(phys, len);   /* 映射时已 invalidate */
	if (!va) return NULL;
	g_fmap[g_fmap_n].phys = phys;
	g_fmap[g_fmap_n].len = len;
	g_fmap[g_fmap_n].va = va;
	g_fmap_n++;
	return va;
}

static void fmap_release_all(void)
{
	for (int i = 0; i < g_fmap_n; i++)
		CVI_SYS_Munmap(g_fmap[i].va, g_fmap[i].len);
	g_fmap_n = 0;
}

static void blit_preview(canvas_t *c)
{
	VIDEO_FRAME_INFO_S pf;
	if (CVI_VPSS_GetChnFrame(COL_GRP, CHN_PREV, &pf, 0) != CVI_SUCCESS) {
		g_ctx.stat_miss++;
		return;   /* 无新帧则保留上一画面/底色 */
	}
	VIDEO_FRAME_S *f = &pf.stVFrame;
	void *yva = f->pu8VirAddr[0] ? (void *)f->pu8VirAddr[0]
		: fmap_get(f->u64PhyAddr[0], f->u32Length[0]);
	void *vuva = f->pu8VirAddr[1] ? (void *)f->pu8VirAddr[1]
		: fmap_get(f->u64PhyAddr[1], f->u32Length[1]);
	if (yva && vuva) {
		/* 硬件可能已复用该缓冲写入新帧：读前失效缓存 */
		if (!f->pu8VirAddr[0])
			CVI_SYS_IonInvalidateCache(f->u64PhyAddr[0], yva, f->u32Length[0]);
		if (!f->pu8VirAddr[1])
			CVI_SYS_IonInvalidateCache(f->u64PhyAddr[1], vuva, f->u32Length[1]);
		uint8_t *Y = (uint8_t *)yva, *VU = (uint8_t *)vuva;
		CVI_U32 ys = f->u32Stride[0], vs = f->u32Stride[1];
		int w = f->u32Width, h = f->u32Height;
		if (w > PREV_W) w = PREV_W;
		if (h > PREV_H) h = PREV_H;
		int w2 = w & ~1;
		for (int y = 0; y < h; y++) {
			const uint8_t *yrow = Y + y * ys;
			const uint8_t *vurow = VU + (y / 2) * vs;
			int cy = PREV_Y + y;
			uint8_t *rp = c->plane[0] + cy * c->stride + PREV_X;
			uint8_t *gp = c->plane[1] + cy * c->stride + PREV_X;
			uint8_t *bp = c->plane[2] + cy * c->stride + PREV_X;
			int x = 0;
			for (; x < w2; x += 2) {
				int v = vurow[x] - 128;
				int u = vurow[x + 1] - 128;
				int dr = (359 * v) >> 8;
				int dg = -((88 * u + 183 * v) >> 8);
				int db = (454 * u) >> 8;
				int y0 = yrow[x], y1 = yrow[x + 1];
				rp[x]     = clamp_tbl[y0 + dr + 256];
				gp[x]     = clamp_tbl[y0 + dg + 256];
				bp[x]     = clamp_tbl[y0 + db + 256];
				rp[x + 1] = clamp_tbl[y1 + dr + 256];
				gp[x + 1] = clamp_tbl[y1 + dg + 256];
				bp[x + 1] = clamp_tbl[y1 + db + 256];
			}
			if (x < w) {   /* 奇数宽收尾 */
				int v = vurow[x & ~1] - 128;
				int u = vurow[(x & ~1) + 1] - 128;
				int yy = yrow[x];
				rp[x] = clamp_tbl[yy + ((359 * v) >> 8) + 256];
				gp[x] = clamp_tbl[yy - ((88 * u + 183 * v) >> 8) + 256];
				bp[x] = clamp_tbl[yy + ((454 * u) >> 8) + 256];
			}
		}
	}
	/* 映射持久保留（fmap_release_all 在 deinit 统一释放），不再逐帧 Munmap */
	CVI_VPSS_ReleaseChnFrame(COL_GRP, CHN_PREV, &pf);
}

/* 按钮是否选中（比对当前参数） */
static int btn_selected(const col_button_t *b, const col_fsm_t *f)
{
	const col_params_t *p = &f->params;
	switch (b->kind) {
	case COL_BTN_PERSON: return b->index == p->person;
	case COL_BTN_TASK:   return b->index == (int)p->task;
	case COL_BTN_DIST:   return b->index == p->dist_idx;
	case COL_BTN_POS:    return b->index == p->pos_h * COL_POSV_NUM + p->pos_v;
	case COL_BTN_LIGHT:  return b->index == p->light;
	case COL_BTN_START:  return f->state == COL_ST_RECORDING;
	default: return 0;
	}
}

static void plat_render(void *ctx, const col_fsm_t *f, const col_ui_t *ui,
			const char *status_line)
{
	(void)ctx;
	if (!g_ctx.inited || g_ctx.canvas_pool == VB_INVALID_POOLID) return;

	/* 节流 ~30fps（对齐 VPSS CHN_PREV 30fps；主循环 20ms 一拍，#145） */
	int64_t now = col_now_ms();
	if (now - g_ctx.last_render_ms < 33) return;
	g_ctx.last_render_ms = now;
	int64_t t0 = now_us();

	VB_BLK blk = CVI_VB_GetBlock(g_ctx.canvas_pool, g_ctx.canvas_blk_size);
	if (blk == VB_INVALID_HANDLE) return;
	void *vir = NULL;
	if (CVI_VB_GetBlockVirAddr(g_ctx.canvas_pool, blk, &vir) != CVI_SUCCESS || !vir) {
		CVI_VB_ReleaseBlock(blk);
		return;
	}
	CVI_U64 phys = CVI_VB_Handle2PhysAddr(blk);
	CVI_U32 stride = g_ctx.canvas_cal.u32MainStride;
	CVI_U32 ysize = g_ctx.canvas_cal.u32MainYSize;

	canvas_t c;
	c.stride = stride; c.w = VO_W; c.h = VO_H;
	c.plane[0] = (uint8_t *)vir;
	c.plane[1] = (uint8_t *)vir + ysize;
	c.plane[2] = (uint8_t *)vir + 2 * ysize;

	/* 底色：只填按钮区（状态栏/黑边/预览各自覆盖，省 ~1.5MB/帧写入，#145）。
	 * blit 无新帧时预览区保留池块里的上一画面，可接受。 */
	cv_fill(&c, 0, STATUS_H + 563, VO_W, VO_H, mkrgb(18, 18, 22));
	/* 预览区 letterbox 黑边（只填上下两条，中间由 blit 覆盖，#145） */
	cv_fill(&c, 0, STATUS_H, VO_W, PREV_Y, mkrgb(0, 0, 0));
	cv_fill(&c, 0, PREV_Y + PREV_H, VO_W, STATUS_H + 563, mkrgb(0, 0, 0));
	int64_t tb = now_us();
	blit_preview(&c);
	g_ctx.stat_blit_us += now_us() - tb;

	/* 状态栏 */
	rgb_t barbg = f->low_space_paused ? mkrgb(120, 20, 20) : mkrgb(30, 34, 44);
	cv_fill(&c, 0, 0, VO_W, STATUS_H, barbg);
	cv_text(&c, 8, 16, status_line, mkrgb(235, 235, 235), 1);

	/* 按钮 */
	for (int i = 0; i < ui->nbtn; i++) {
		const col_button_t *b = &ui->btn[i];
		int x0 = b->x0 * VO_W / 1000, x1 = b->x1 * VO_W / 1000;
		int y0 = b->y0 * VO_H / 1000, y1 = b->y1 * VO_H / 1000;
		int sel = btn_selected(b, f);
		rgb_t fill, border, txt;
		if (b->kind == COL_BTN_STOP) {
			fill = mkrgb(70, 30, 30); border = mkrgb(150, 70, 70); txt = mkrgb(240, 220, 220);
		} else if (sel) {
			fill = mkrgb(30, 110, 60); border = mkrgb(90, 220, 130); txt = mkrgb(255, 255, 255);
		} else {
			fill = mkrgb(44, 48, 58); border = mkrgb(90, 96, 110); txt = mkrgb(210, 214, 222);
		}
		cv_fill(&c, x0, y0, x1, y1, fill);
		/* 边框（2px） */
		cv_fill(&c, x0, y0, x1, y0 + 2, border);
		cv_fill(&c, x0, y1 - 2, x1, y1, border);
		cv_fill(&c, x0, y0, x0 + 2, y1, border);
		cv_fill(&c, x1 - 2, y0, x1, y1, border);
		/* 标签居中，scale2（CJK 32px / ASCII 16px） */
		int scale = 2;
		int tw = text_width(b->label, scale);
		int th = FONT_CJK_H * scale;
		int tx = x0 + (x1 - x0 - tw) / 2;
		int ty = y0 + (y1 - y0 - th) / 2;
		cv_text(&c, tx, ty, b->label, txt, scale);
	}

	/* 送显 */
	VIDEO_FRAME_INFO_S vf = {0};
	vf.stVFrame.u32Width = VO_W;
	vf.stVFrame.u32Height = VO_H;
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
	vf.u32PoolId = g_ctx.canvas_pool;
	/* 画布为缓存映射：送显前回写整块，保证 VO DMA 读到最新像素（#145） */
	CVI_SYS_IonFlushCache(phys, vir, g_ctx.canvas_blk_size);
	CVI_S32 ret = CVI_VO_SendFrame(COL_VOLAYER, COL_VOCHN, &vf, 40);
	if (ret != CVI_SUCCESS) { LOG("VO_SendFrame fail %#x", ret); g_ctx.stat_sendfail++; }
	CVI_VB_ReleaseBlock(blk);

	/* 性能统计：每 120 帧（约 4s）打印一次 */
	g_ctx.stat_render_us += now_us() - t0;
	if (++g_ctx.stat_n >= 120) {
		LOG("render: n=%d avg=%lldus blit=%lldus miss=%d sendfail=%d",
		    g_ctx.stat_n, (long long)(g_ctx.stat_render_us / g_ctx.stat_n),
		    (long long)(g_ctx.stat_blit_us / g_ctx.stat_n),
		    g_ctx.stat_miss, g_ctx.stat_sendfail);
		g_ctx.stat_n = 0; g_ctx.stat_render_us = 0; g_ctx.stat_blit_us = 0;
		g_ctx.stat_miss = 0; g_ctx.stat_sendfail = 0;
	}
}

static void plat_deinit(void *ctx)
{
	(void)ctx;
	if (!g_ctx.inited) return;
	fmap_release_all();
	if (g_ctx.venc_created) {
		CVI_VENC_StopRecvFrame(COL_VENC);
		CVI_VENC_DestroyChn(COL_VENC);
		g_ctx.venc_created = 0;
	}
	SAMPLE_COMM_VI_UnBind_VPSS(0, 0, COL_GRP);
	{
		CVI_BOOL en[VPSS_MAX_PHY_CHN_NUM] = {CVI_TRUE, CVI_TRUE, CVI_FALSE, CVI_FALSE};
		SAMPLE_COMM_VPSS_Stop(COL_GRP, en);
	}
	CVI_VPSS_DestroyGrp(COL_GRP);
	if (g_ctx.vo_started) {
		SAMPLE_COMM_VO_StopVO(&g_ctx.vo_cfg);
		g_ctx.vo_started = 0;
	}
	if (g_ctx.canvas_pool != VB_INVALID_POOLID) {
		CVI_VB_MunmapPool(g_ctx.canvas_pool);
		CVI_VB_DestroyPool(g_ctx.canvas_pool);
		g_ctx.canvas_pool = VB_INVALID_POOLID;
	}
	SAMPLE_COMM_VI_DestroyIsp(&g_ctx.vi_cfg);
	SAMPLE_COMM_VI_DestroyVi(&g_ctx.vi_cfg);
	SAMPLE_COMM_SYS_Exit();
	if (g_ctx.touch_fd >= 0) { close(g_ctx.touch_fd); g_ctx.touch_fd = -1; }
	free(g_ctx.jpeg_buf);
	g_ctx.jpeg_buf = NULL; g_ctx.jpeg_cap = 0; g_ctx.jpeg_len = 0;
	g_ctx.inited = 0;
	LOG("平台已释放（VI 残留状态如有异常需重启板子恢复）");
}

static const col_platform_t g_platform = {
	.init = plat_init,
	.capture_jpeg = plat_capture_jpeg,
	.poll_touch = plat_poll_touch,
	.render = plat_render,
	.deinit = plat_deinit,
};

const col_platform_t *platform_get(void) { return &g_platform; }