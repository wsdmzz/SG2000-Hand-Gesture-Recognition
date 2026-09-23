/*
 * tdl_gesture_kpt_stream.c - 自训 YOLOv8n-pose 单模型手势识别 + MJPEG 推流
 * 流程: VI/VPSS(手工)→VPSS Chn0取帧→bgr→letterbox 480→自训 best480 cvimodel(裸 cviruntime)
 *       →decode(DFL softmax+sigmoid)→21关键点→fa手指角度→B二值化(舵机管道)/G手势稳定输出
 * 单模型输出 det_raw(1,71,4725)+kpt_raw(1,63,4725), 7 手势类(gesture_0..6)
 * 用法: ./tdl_gesture_kpt_stream <port>
 * 浏览器: http://<board>:8881/
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>

#include "tdl_sdk.h"
#include "tdl_utils.h"
#include "cviruntime.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include "cvi_vb.h"
#include "cvi_sys.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "linux/cvi_comm_video.h"
#include "linux/cvi_comm_vi.h"
#include "linux/cvi_comm_vpss.h"
#include "sample_comm.h"

#define VPSS_GRP	0
#define VPSS_CHN	VPSS_CHN0

#define MODEL_POSE	"/mnt/data/models/cv181x/best480_int8_cv181x.cvimodel"
#define POSE_SZ		480
#define POSE_ANCH	4725	/* 60²+30²+15² */
#define POSE_NCLS	7

/* 自训 7 类: 0:gesture_0(拳) .. 6:gesture_6, 训练data.yaml同名 */
	static const char *gname(int id){ switch(id){case 0:return"g0";case 1:return"g1";case 2:return"g2";case 3:return"g3";case 4:return"g4";case 5:return"g5";case 6:return"g6";default:return"?";} }
	/* 7 类稳定手势名（跟随 G 判定输出，仅 5 帧确认后变化，推流标签用） */
	static const char *g7name(int id){ switch(id){case 0:return"FIST";case 1:return"ONE";case 2:return"TWO";case 3:return"THREE";case 4:return"FOUR";case 5:return"FIVE";case 6:return"LIKE";default:return"NONE";} }

static volatile int g_run = 1;
static void sig_handler(int s){ (void)s; g_run=0; }

/* ---- 共享 JPEG 缓冲（主线程写, HTTP线程读）---- */
static pthread_mutex_t g_jpg_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_jpg[256*1024];
static size_t g_jpg_len = 0;
static volatile int g_server_fd = -1;

static void push_jpeg_to_http(const unsigned char *data, size_t len)
{
	pthread_mutex_lock(&g_jpg_mtx);
	if (len <= sizeof(g_jpg)) { memcpy(g_jpg, data, len); g_jpg_len = len; }
	pthread_mutex_unlock(&g_jpg_mtx);
}
/* ---- 自训 YOLOv8n-pose 单模型(裸 cviruntime, 480×480 INT8) ---- */
static CVI_MODEL_HANDLE g_pose_model = NULL;
static CVI_TENSOR *g_pose_in=NULL,*g_pose_out=NULL;
static int32_t g_pose_in_num=0,g_pose_out_num=0;

static int pose_nn_init(void)
{
	CVI_RC rc;
	rc=CVI_NN_RegisterModel(MODEL_POSE,&g_pose_model);
	if(rc!=CVI_RC_SUCCESS){printf("pose RegisterModel %#x\n",rc);return -1;}
	rc=CVI_NN_GetInputOutputTensors(g_pose_model,&g_pose_in,&g_pose_in_num,&g_pose_out,&g_pose_out_num);
	if(rc!=CVI_RC_SUCCESS){printf("pose GetIO %#x\n",rc);return -1;}
	for(int i=0;i<g_pose_out_num;i++){
		printf("pose_out[%d] %s count=%zu fmt=%d qs=%.6f\n", i,
		       g_pose_out[i].name?g_pose_out[i].name:"?", g_pose_out[i].count,
		       (int)g_pose_out[i].fmt, g_pose_out[i].qscale);
	}
	return 0;
}

/* 手指弯曲角度（3 关键点夹角，单位度）
 * 关键点为归一化坐标(0~1)，夹角不受缩放影响 */
static float finger_angle(float kpt[21][2], int a, int b, int c)
{
	float bax = kpt[a][0] - kpt[b][0];
	float bay = kpt[a][1] - kpt[b][1];
	float bcx = kpt[c][0] - kpt[b][0];
	float bcy = kpt[c][1] - kpt[b][1];
	float dot = bax*bcx + bay*bcy;
	float mag = sqrtf(bax*bax+bay*bay) * sqrtf(bcx*bcx+bcy*bcy);
	if (mag < 1e-6f) return 180.0f;
	float cos_a = dot / mag;
	if (cos_a > 1.0f) cos_a = 1.0f;
	if (cos_a < -1.0f) cos_a = -1.0f;
	return acosf(cos_a) * 180.0f / 3.14159265f;
}

/* 手指角度 → 舵机角度映射
 * 人手伸直(fa≈180°) → 舵机伸直(sa=180°)
 * 人手弯曲(fa≈30°)  → 舵机弯曲(sa=0°) */
static int finger_to_servo(float fa)
{
	int sa = (int)((fa - 30.0f) * 180.0f / 150.0f);
	if (sa < 0) sa = 0;
	if (sa > 180) sa = 180;
	return sa;
}

/* ============ 自训 YOLOv8n-pose 整帧推理 ============ */
/* anchor 表: int 格子坐标 + stride, 层序 P3(60×60,s8) P4(30×30,s16) P5(15×15,s32), row-major */
static float g_ax[POSE_ANCH], g_ay[POSE_ANCH], g_as[POSE_ANCH];

/* ---- 固定 ROI (方案A, 验证 #122) ----------------------------------------
 * 实测: 全图直送 pose 模型 bsc≈0.005(检不出); 裁到手占满后 bsc≈0.92(稳定)。
 * 故送模型前先裁出固定窗口。默认值来自实测手位(640x384 坐标系):
 *   手中心(260,208) 边长约278 → 取 280。宽容区 220~280(见 #122)。
 * 可用命令行覆盖: ./tdl_gesture_kpt_stream <port> [cx cy side] */
static int g_roi_cx = 260, g_roi_cy = 208, g_roi_side = 280;
static int g_roi_x = 0, g_roi_y = 0;   /* 本次实际裁剪左上角, 供坐标映射 */

/* ---- 自适应 ROI (验证 #127): 固定 ROI 无法适应距离变化 ----
 * 实验(#127): 手 272px→0.94, 136px→0.90, 82px→0.78, 60px→0.0003(失败)
 *             而自适应尺寸在 60px 仍 0.94。
 * 策略: 上帧成功的尺寸本帧优先 → 通常只需 1 次推理(帧率不掉);
 *       失败才依次试其它尺寸(手远近变化时最多 3 次)。 */
#define POSE_TH      0.30f              /* 检出手的置信度阈值 */
#define ROI_NSZ      3
static const int g_roi_cand[ROI_NSZ] = {280, 170, 100};   /* 近 / 中 / 远 */
static int g_roi_idx = 0;               /* 上帧成功的候选索引 */

/* 调试: --dump 时每 20 帧保存 ROI 与 letterbox 图到 /tmp, 供离线复现分析 */
static int g_dump = 0;
static int g_dump_seq = 0;

/* 方向对齐 (验证 #125): 模型只见过「手臂从左往右/手指朝右」的朝向,
 * 实测换方向后 sc 从 0.81 掉到 0.52。此处把 ROI 旋转到训练方向再送模型。
 *   0   = 不旋转 (画面已经是训练方向)
 *   90  = 顺时针 90°  (手指朝上 → 朝右)
 *   270 = 逆时针 90°  (手指朝下 → 朝右)
 * 可用 --rot 90 / --rot 270 开启。ROI 为正方形, 故旋转后尺寸不变。 */
static int g_rot = 0;

/* 旋转图坐标 → ROI 原图坐标 (与 g_rot 相反变换)
 * rw/rh 为旋转后图像的宽/高 */
static inline void unrot_pt(float &x, float &y, int rw, int rh)
{
	if (g_rot == 90) {
		/* dst(x',y') = src(rh-1-y, x) → 反解 x=y', y=rw-1-x'  (rw=原图高) */
		float xp = x, yp = y;
		x = yp;
		y = (float)rw - 1.0f - xp;
	} else if (g_rot == 270) {
		/* dst(x',y') = src(y, rw-1-x) → 反解 x=rh-1-y', y=x'  (rh=原图宽) */
		float xp = x, yp = y;
		x = (float)rh - 1.0f - yp;
		y = xp;
	}
}
static int pose_anchor_init(void)
{
	int k=0;
	const int W[3]={60,30,15};
	const float S[3]={8.f,16.f,32.f};
	for(int l=0;l<3;l++)
		for(int y=0;y<W[l];y++)
			for(int x=0;x<W[l];x++){
				g_ax[k]=(float)x; g_ay[k]=(float)y; g_as[k]=S[l]; k++;
			}
	if(k!=POSE_ANCH){printf("anchor cnt %d != %d\n",k,POSE_ANCH);return -1;}
	return 0;
}

/* pose_infer_roi: 单次推理 — 用指定边长裁剪 ROI → letterbox480 → 量化 → Forward → decode
 * 返回 top1 类别置信度 bsc; <0 表示无法推理
 * 输出(全图归一化坐标): kpt[21][2], box[4]像素, *pscore, *pcls */
static float pose_infer_roi(cv::Mat &bgr, int roi_side, float kpt_out[21][2],
                            int box[4], float *pscore, int *pcls)
{
	if(!g_pose_model) return -1.0f;
	const int full_w=bgr.cols, full_h=bgr.rows;
	/* --- 0. ROI 裁剪 (边长 roi_side, 中心 g_roi_cx/cy) --- */
	int rx1=g_roi_cx-roi_side/2, ry1=g_roi_cy-roi_side/2;
	int rx2=rx1+roi_side, ry2=ry1+roi_side;
	if(rx1<0)rx1=0; if(ry1<0)ry1=0;
	if(rx2>full_w)rx2=full_w; if(ry2>full_h)ry2=full_h;
	if(rx2-rx1<64||ry2-ry1<64) return -1.0f;
	static cv::Mat roi_img, roi_src;
	roi_img = bgr(cv::Rect(rx1,ry1,rx2-rx1,ry2-ry1));
	if (g_rot == 90)       cv::rotate(roi_img, roi_src, cv::ROTATE_90_CLOCKWISE);
	else if (g_rot == 270) cv::rotate(roi_img, roi_src, cv::ROTATE_90_COUNTERCLOCKWISE);
	else                   roi_src = roi_img;
	g_roi_x=rx1; g_roi_y=ry1;
	const int fw=roi_src.cols, fh=roi_src.rows;   /* 旋转后尺寸(正方形, 不变) */
	/* --- 1. letterbox 480×480 (pad 114, 居中) --- */
	const float scale=fminf((float)POSE_SZ/fw,(float)POSE_SZ/fh);
	const int nw=(int)(fw*scale+0.5f), nh=(int)(fh*scale+0.5f);
	const int px=(POSE_SZ-nw)/2, py=(POSE_SZ-nh)/2;
	static cv::Mat letter;
	letter = cv::Mat(POSE_SZ,POSE_SZ,CV_8UC3,cv::Scalar(114,114,114));
	{
		cv::Mat rsz;
		cv::resize(roi_src,rsz,cv::Size(nw,nh),0,0,cv::INTER_LINEAR);
		rsz.copyTo(letter(cv::Rect(px,py,nw,nh)));
	}
	/* 调试 dump: ROI 原图 + 送模型的 480x480, 每 20 帧一组 */
	if (g_dump) {
		if (g_dump_seq % 20 == 0) {
			char fn[80];
			snprintf(fn, sizeof(fn), "/tmp/roi_%04d.jpg", g_dump_seq / 20);
			cv::imwrite(fn, roi_img);
			snprintf(fn, sizeof(fn), "/tmp/lb_%04d.jpg", g_dump_seq / 20);
			cv::imwrite(fn, letter);
		}
		g_dump_seq++;
	}
	/* --- 2. 量化: RGB NCHW, v=pixel/255∈[0,1], q=v×qscale ---
	 * qscale 语义: 归一化值→INT8 的量化尺度, 模板 max_abs≈1.0 → qscale≈127
	 * 故 q = (pixel/255)×qscale, 即 kf = qscale/255 ∈ [0,127]
	 * 验证 #116: 旧式 kf=1/(255*qscale) 会把整幅图压成全 0(实测非零 0/691200),
	 *             模型实际只在看空白图 → 舵机抖动但不跟随手势 */
	{
		const float qs=g_pose_in[0].qscale;
		const float kf=qs/255.0f;          /* q = pixel×kf */
		int8_t *d=(int8_t*)g_pose_in[0].sys_mem;
		const int hw=POSE_SZ*POSE_SZ;
		for(int c=0;c<3;c++){
			const int ch=(c==0)?2:(c==1?1:0);   /* RGB: BGR Mat 中 R=+2 */
			for(int y=0;y<POSE_SZ;y++){
				const uchar *row=letter.ptr<uchar>(y);
				int8_t *dst=d+c*hw+y*POSE_SZ;
				for(int x=0;x<POSE_SZ;x++){
					float q=row[x*3+ch]*kf;
					if(q>127.f)q=127.f; if(q<-128.f)q=-128.f;
					dst[x]=(int8_t)(q+0.5f*(q>=0?1:-1));
				}
			}
		}
	}
	/* 调试: 量化统计 (验证 #116 量化修复; 只打前 3 帧; 走 stderr 不干扰 G 管道) */
	{
		static int s_qdbg = 0;
		if (s_qdbg < 3) {
			int nz = 0, mx = -128;
			int8_t *dd = (int8_t*)g_pose_in[0].sys_mem;
			for (size_t k = 0; k < g_pose_in[0].count; k++) {
				if (dd[k] != 0) nz++;
				if (dd[k] > mx) mx = dd[k];
			}
			fprintf(stderr, "[DBG-QUANT] qscale=%.1f kf=%.6f nonzero=%d/%zu max=%d\n",
			        g_pose_in[0].qscale, g_pose_in[0].qscale/255.0f,
			        nz, g_pose_in[0].count, mx);
			s_qdbg++;
		}
	}
	/* --- 3. Forward --- */
	if(CVI_NN_Forward(g_pose_model,g_pose_in,g_pose_in_num,g_pose_out,g_pose_out_num)!=CVI_RC_SUCCESS){
		printf("Forward fail\n");return -1;
	}
	/* --- 4. 找 det/kpt 输出 tensor --- */
	float *det=NULL,*kptr=NULL;
	for(int i=0;i<g_pose_out_num;i++){
		if(g_pose_out[i].count==(size_t)71*POSE_ANCH) det=(float*)g_pose_out[i].sys_mem;
		else if(g_pose_out[i].count==(size_t)63*POSE_ANCH) kptr=(float*)g_pose_out[i].sys_mem;
	}
	if(!det||!kptr){printf("pose out tensor miss\n");return -1;}
	/* --- 5. decode: top1 锚点(7类 sigmoid) --- */
	int best=-1,bcls=0; float bsc=0.f;
	for(int i=0;i<POSE_ANCH;i++){
		for(int c=0;c<POSE_NCLS;c++){
			float s=1.f/(1.f+expf(-det[(64+c)*POSE_ANCH+i]));
			if(s>bsc){bsc=s;best=i;bcls=c;}
		}
	}
	/* 调试: 置信度 (每次尝试计数, 带 roi 尺寸便于观察尺度切换) */
	{
		static int s_dbg_cnt = 0;
		if (s_dbg_cnt++ % 6 == 0) {
			fprintf(stderr, "[DBG-DET] roi=%d best=%d cls=%d bsc=%.4f (TH=%.2f)\n",
			        roi_side, best, bcls, bsc, (double)POSE_TH);
			fflush(stderr);
		}
	}
	if(bsc<POSE_TH) return bsc;   /* 未达阈值: 返回 bsc 交外层试其它尺度 */
	/* --- 6. DFL softmax → box --- */
	float cx,cy,w,h;
	{
		float dist[4];
		for(int k=0;k<4;k++){
			const float *pr=det+(k*16)*POSE_ANCH+best;
			float mx=pr[0]; for(int j=1;j<16;j++) if(pr[j]>mx)mx=pr[j];
			float sum=0.f,e[16];
			for(int j=0;j<16;j++){e[j]=expf(pr[j]-mx);sum+=e[j];}
			float ex=0.f; for(int j=0;j<16;j++) ex+=e[j]*(float)j/sum;
			dist[k]=ex;
		}
		cx=(g_ax[best]+0.5f+(dist[2]-dist[0])*0.5f)*g_as[best];
		cy=(g_ay[best]+0.5f+(dist[3]-dist[1])*0.5f)*g_as[best];
		w =(dist[0]+dist[2])*g_as[best];
		h =(dist[1]+dist[3])*g_as[best];
	}
	/* letterbox 坐标 → 旋转图坐标 → ROI 原图坐标 → 全图坐标 */
	{
		float ax=(cx-w*0.5f-px)/scale, ay=(cy-h*0.5f-py)/scale;
		float bx=(cx+w*0.5f-px)/scale, by=(cy+h*0.5f-py)/scale;
		unrot_pt(ax, ay, fw, fh);
		unrot_pt(bx, by, fw, fh);
		/* 旋转是镜像变换, 反解后需重排角点 */
		float x1=fminf(ax,bx)+g_roi_x, x2=fmaxf(ax,bx)+g_roi_x;
		float y1=fminf(ay,by)+g_roi_y, y2=fmaxf(ay,by)+g_roi_y;
		box[0]=x1<0?0:(int)x1; box[1]=y1<0?0:(int)y1;
		box[2]=x2>full_w?full_w:(int)x2; box[3]=y2>full_h?full_h:(int)y2;
	}
	/* --- 7. kpt: ROI 内 letterbox 坐标 → 全图归一化坐标 --- */
	for(int m=0;m<21;m++){
		float kx=(kptr[(m*3+0)*POSE_ANCH+best]*2.f+g_ax[best])*g_as[best];
		float ky=(kptr[(m*3+1)*POSE_ANCH+best]*2.f+g_ay[best])*g_as[best];
		float ux=(kx-px)/scale, uy=(ky-py)/scale;
		unrot_pt(ux, uy, fw, fh);
		kpt_out[m][0]=(ux+g_roi_x)/full_w;
		kpt_out[m][1]=(uy+g_roi_y)/full_h;
	}
	*pscore=bsc; *pcls=bcls;
	return bsc;
}

/* pose_nn_infer: 自适应多尺度 ROI — 上帧成功的尺寸优先, 达标即早停。
 * 返回 0=检到手, -1=所有候选都不达标。 */
static int pose_nn_infer(cv::Mat &bgr, float kpt_out[21][2], int box[4],
                         float *pscore, int *pcls)
{
	if(!g_pose_model) return -1;
	float best_sc = -1.0f;
	int   best_idx = -1;
	float k_best[21][2];
	int   b_best[4];
	float p_best = 0.f;
	int   c_best = 0;

	for (int k = 0; k < ROI_NSZ; k++) {
		int idx = (g_roi_idx + k) % ROI_NSZ;   /* 上帧成功者优先 */
		float kk[21][2];
		int   bb[4];
		float ps = 0.f;
		int   pc = 0;
		float sc = pose_infer_roi(bgr, g_roi_cand[idx], kk, bb, &ps, &pc);
		if (sc < 0.f) continue;
		if (sc > best_sc) {                    /* 记录最佳(达标即返回, 否则留作备选) */
			best_sc = sc; best_idx = idx;
			memcpy(k_best, kk, sizeof(k_best));
			memcpy(b_best, bb, sizeof(b_best));
			p_best = ps; c_best = pc;
		}
		if (sc >= POSE_TH) {                   /* 达标 → 早停 */
			if (g_roi_idx != idx) {
				fprintf(stderr, "[ROI] 切换 %d → %d\n", g_roi_cand[g_roi_idx], g_roi_cand[idx]);
				fflush(stderr);
				g_roi_idx = idx;
			}
			memcpy(kpt_out, k_best, sizeof(k_best));
			memcpy(box, b_best, sizeof(b_best));
			*pscore = p_best; *pcls = c_best;
			return 0;
		}
	}
	return -1;   /* 全部候选均未达标 → 判无手 */
}
/* ---- HTTP 线程: 独立 accept + 断线检测 + 发送最新JPEG ---- */
static void *http_thread(void *arg)
{
	(void)arg;
	struct sockaddr_in cli_addr; socklen_t cli_len = sizeof(cli_addr);
	int cli_fd = -1;

	if (g_server_fd < 0) { printf("http thread: no server fd\n"); return NULL; }

	while (g_run) {
		if (cli_fd < 0) {
			struct pollfd sp = { g_server_fd, POLLIN, 0 };
			int spr = poll(&sp, 1, 200);
			if (spr > 0 && (sp.revents & POLLIN)) {
				cli_fd = accept(g_server_fd, (struct sockaddr *)&cli_addr, &cli_len);
				if (cli_fd >= 0) {
					int fl = fcntl(cli_fd, F_GETFL, 0); fcntl(cli_fd, F_SETFL, fl | O_NONBLOCK);
					printf("client connected\n"); fflush(stdout);
					/* 非阻塞读一次请求(丢弃内容), 避免阻塞 http 线程 */
					char b[512];
					(void)recv(cli_fd, b, 511, MSG_DONTWAIT);
					const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
					int l2=strlen(hdr), off=0;
					while(off<l2){ struct pollfd p={cli_fd,POLLOUT,0}; if(poll(&p,1,500)<=0)break; ssize_t w=write(cli_fd,hdr+off,l2-off); if(w>0)off+=w; else break; }
				}
			}
		} else {
			/* 断线检测 */
			char pk[1];
			int prk = recv(cli_fd, pk, 1, MSG_PEEK|MSG_DONTWAIT);
			if (prk == 0) { close(cli_fd); cli_fd = -1; continue; }

			/* 发送最新 JPEG */
			pthread_mutex_lock(&g_jpg_mtx);
			size_t len = g_jpg_len;
			unsigned char *buf = g_jpg;
			int has = (len > 0);
			pthread_mutex_unlock(&g_jpg_mtx);
			if (!has) { usleep(20*1000); continue; }

			char bnd[160];
			int bl = snprintf(bnd, sizeof(bnd), "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", len);
			int sr = 0;
			/* 发送 boundary */
			{struct pollfd p={cli_fd,POLLOUT,0}; if(poll(&p,1,500)<=0) sr=-1;
			 else { ssize_t w=write(cli_fd,bnd,bl); if(w!=bl) sr=-1; }}
			if (sr==0) {
				struct pollfd p={cli_fd,POLLOUT,0};
				if(poll(&p,1,1000)<=0) sr=-1;
				else { pthread_mutex_lock(&g_jpg_mtx); ssize_t w=write(cli_fd,buf,len); pthread_mutex_unlock(&g_jpg_mtx); if(w!=(ssize_t)len) sr=-1; }
			}
			if (sr==0) { struct pollfd p={cli_fd,POLLOUT,0}; if(poll(&p,1,500)<=0) sr=-1; else{ ssize_t w=write(cli_fd,"\r\n",2); (void)w; } }
			if (sr != 0) { close(cli_fd); cli_fd=-1; continue; }
			usleep(10*1000);
		}
	}
	if (cli_fd>=0) close(cli_fd);
	return NULL;
}

int main(int argc, char *argv[])
{
	int port = (argc>1)?atoi(argv[1]):8881;
	/* 可选: ROI 覆盖 ./tdl_gesture_kpt_stream <port> [roi_cx roi_cy roi_side]
	 * ROI 为 640x384 坐标系; 默认(260,208,280) 来自实测手位(验证 #122)
	 * 另可选 --dump: 每 20 帧保存 ROI/letterbox 图到 /tmp 供离线分析 */
	{
		int npos = 0, pos[3] = {0, 0, 0};
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--dump") == 0) { g_dump = 1; continue; }
			if (strncmp(argv[i], "--rot", 5) == 0) { g_rot = atoi(argv[i] + 5); continue; }
			if (npos < 3) pos[npos++] = atoi(argv[i]);
		}
		if (npos >= 3) {
			g_roi_cx = pos[0]; g_roi_cy = pos[1]; g_roi_side = pos[2];
			if (g_roi_side < 64) g_roi_side = 64;
			if (g_roi_side > 640) g_roi_side = 640;
		}
	}
	CVI_S32 ret; int opt=1;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	VB_CONFIG_S stVbConf;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize, stSizeIn, stSizeOut;
	VIDEO_FRAME_INFO_S stVpssFrame;
	struct sockaddr_in sa;
	CVI_U32 u32BlkSize;
	TDLHandle h=NULL;
	long total=0;

	signal(SIGINT,sig_handler); signal(SIGTERM,sig_handler); signal(SIGPIPE,SIG_IGN);
	printf("=== TDL Gesture + MJPEG stream (port %d) ===\n", port);
	printf("ROI: cx=%d cy=%d side=%d (640x384 坐标系; 改此值: %s <port> cx cy side)\n",
	       g_roi_cx, g_roi_cy, g_roi_side, argv[0]);
	printf("旋转对齐: %d° %s (--rot 0|90|270; 90=顺时针 手指朝上→朝右)\n",
	       g_rot, g_rot ? "(已开启)" : "(未开启, 画面需已是训练方向)");

	/* 0. 清理残留 VPSS */
	{ CVI_S32 g; for(g=0;g<(CVI_S32)VPSS_MAX_GRP_NUM;g++){ CVI_VPSS_StopGrp(g); CVI_VPSS_DestroyGrp(g); } usleep(200*1000); }

	/* 1. ParseIni */
	memset(&stIniCfg,0,sizeof(stIniCfg));
	stIniCfg.enSource=(VI_PIPE_FRAME_SOURCE_E)VI_PIPE_FRAME_SOURCE_DEV;
	stIniCfg.devNum=1;
	ret=SAMPLE_COMM_VI_ParseIni(&stIniCfg);
	if(ret!=CVI_SUCCESS){printf("ParseIni %#x\n",ret);return -1;}
	CVI_VI_SetDevNum(stIniCfg.devNum);
	ret=SAMPLE_COMM_VI_IniToViCfg(&stIniCfg,&stViConfig);
	if(ret!=CVI_SUCCESS)return -1;
	ret=SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0],&enPicSize);
	if(ret!=CVI_SUCCESS)return -1;
	ret=SAMPLE_COMM_SYS_GetPicSize(enPicSize,&stSize);
	if(ret!=CVI_SUCCESS)return -1;
	printf("sensor %dx%d\n",stSize.u32Width,stSize.u32Height);

	/* 2. VB+SYS(双pool) */
	memset(&stVbConf,0,sizeof(stVbConf));
	stVbConf.u32MaxPoolCnt=2;
	u32BlkSize=COMMON_GetPicBufferSize(stSize.u32Width,stSize.u32Height,SAMPLE_PIXEL_FORMAT,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkSize=u32BlkSize; stVbConf.astCommPool[0].u32BlkCnt=5;
	u32BlkSize=COMMON_GetPicBufferSize(768,1280,SAMPLE_PIXEL_FORMAT,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
	stVbConf.astCommPool[1].u32BlkSize=u32BlkSize; stVbConf.astCommPool[1].u32BlkCnt=8;
	ret=SAMPLE_COMM_SYS_Init(&stVbConf);
	if(ret!=CVI_SUCCESS)return -1;

	/* 3. VI */
	ret=SAMPLE_PLAT_VI_INIT(&stViConfig);
	if(ret!=CVI_SUCCESS){printf("VI_INIT %#x\n",ret);goto err_sys;}

	/* 4. VPSS (输出 640x384) */
	stSizeIn=stSize; stSizeOut.u32Width=640; stSizeOut.u32Height=384;
	ret=SAMPLE_PLAT_VPSS_INIT(VPSS_GRP,stSizeIn,stSizeOut);
	if(ret!=CVI_SUCCESS){printf("VPSS_INIT %#x\n",ret);goto err_sys;}
	ret=SAMPLE_COMM_VI_Bind_VPSS(0,0,VPSS_GRP);
	if(ret!=CVI_SUCCESS){printf("Bind %#x\n",ret);goto err_vpss;}
	printf("VI->VPSS bind OK\n");

	/* 5. HTTP server (非阻塞 listen) */
	g_server_fd=socket(AF_INET,SOCK_STREAM,0);
	setsockopt(g_server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
	memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_addr.s_addr=htonl(INADDR_ANY); sa.sin_port=htons((unsigned short)port);
	if(bind(g_server_fd,(struct sockaddr*)&sa,sizeof(sa))<0){printf("bind fail\n");goto err_vpss;}
	listen(g_server_fd,4);
	{int fl=fcntl(g_server_fd,F_GETFL,0);fcntl(g_server_fd,F_SETFL,fl|O_NONBLOCK);}
	printf("HTTP listening :%d\n",port);

	/* 启动 HTTP 线程 */
	pthread_t hthr;
	pthread_create(&hthr,NULL,http_thread,NULL);

	/* 6. 自训 pose 单模型(裸 cviruntime) */
	if(pose_nn_init()!=0){printf("pose_nn_init fail\n");goto err_handle;}
	if(pose_anchor_init()!=0){printf("anchor fail\n");goto err_handle;}
	printf("pose model OK (best480 INT8 cviruntime)\n"); fflush(stdout);

	/* 7. 主循环: 取帧→检测→分类→画框→软编码→push_jpeg */
	while(g_run){
		ret=CVI_VPSS_GetChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame,1000);
		if(ret!=CVI_SUCCESS){usleep(20*1000);continue;}
		VIDEO_FRAME_S*f=&stVpssFrame.stVFrame;
		int fw=f->u32Width,fh=f->u32Height,ylen=f->u32Length[0];
		{
			static int dbg=0;
			if(total%60==0 && dbg<3){
				printf("[DBG] fw=%d fh=%d stride0=%d stride1=%d len0=%d len1=%d fmt=%d vir0=%p vir1=%p\n",
				       fw,fh,(int)f->u32Stride[0],(int)f->u32Stride[1],
				       (int)f->u32Length[0],(int)f->u32Length[1],
				       (int)f->enPixelFormat,(void*)f->pu8VirAddr[0],(void*)f->pu8VirAddr[1]);
				dbg++;
			}
		}

		void*va0=(f->pu8VirAddr[0])?(void*)f->pu8VirAddr[0]:CVI_SYS_Mmap(f->u64PhyAddr[0],(CVI_U32)ylen+(CVI_U32)(fw*fh/2)+64);
		if(!va0){CVI_VPSS_ReleaseChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame);continue;}

		/* 提前声明变量避免 continue 跨越初始化 */
		unsigned char*yuv=NULL;
		cv::Mat nv21,bgr;

		yuv=(unsigned char*)va0;
		nv21=cv::Mat(fh*3/2,fw,CV_8UC1,yuv);
		cv::cvtColor(nv21,bgr,cv::COLOR_YUV2BGR_NV21);

		/* 自训 pose 单模型: 整帧推理(检测+关键点+类别) */
		float kpt[21][2];
		int box[4];
		float psc=0.f; int pcl=-1;
		int have_hand = (pose_nn_infer(bgr, kpt, box, &psc, &pcl) == 0);
		uint32_t nh = have_hand ? 1 : 0;

		if (have_hand) {
			/* 画检测框 + 置信度 */
			cv::rectangle(bgr,cv::Rect(box[0],box[1],box[2]-box[0],box[3]-box[1]),cv::Scalar(0,255,0),2);
			{
				/* EMA 平滑关键点坐标，减少帧间抖动 */
				static float kpt_ema[21][2];
				static int kpt_init = 0;
				if (!kpt_init) {
					memcpy(kpt_ema, kpt, sizeof(kpt_ema));
					kpt_init = 1;
				} else {
					for (int i = 0; i < 21; i++) {
						kpt_ema[i][0] = 0.3f * kpt[i][0] + 0.7f * kpt_ema[i][0];
						kpt_ema[i][1] = 0.3f * kpt[i][1] + 0.7f * kpt_ema[i][1];
					}
				}
				memcpy(kpt, kpt_ema, sizeof(kpt_ema));
			}
					/* 计算 5 根手指弯曲角度 → 舵机角度 → stdout(管道给 gpio_soft_pwm) */
					float fa[5];
					fa[0] = finger_angle(kpt, 2, 3, 4);   /* 拇指 IP */
					fa[1] = finger_angle(kpt, 5, 6, 7);   /* 食指 PIP */
					fa[2] = finger_angle(kpt, 9, 10, 11); /* 中指 PIP */
					fa[3] = finger_angle(kpt, 13, 14, 15);/* 无名指 PIP */
					fa[4] = finger_angle(kpt, 17, 18, 19);/* 小指 PIP */
					/* 3帧中值滤波去除异常跳变 */
					static float fa_buf[5][3]; static int fa_ready=0;
					if(!fa_ready){for(int i=0;i<5;i++)fa_buf[i][0]=fa_buf[i][1]=fa_buf[i][2]=fa[i];fa_ready=1;}
					else{for(int i=0;i<5;i++){fa_buf[i][2]=fa_buf[i][1];fa_buf[i][1]=fa_buf[i][0];fa_buf[i][0]=fa[i];}}
					for(int i=0;i<5;i++){float a=fa_buf[i][0],b=fa_buf[i][1],c=fa_buf[i][2],t;if(a>b){t=a;a=b;b=t;}if(b>c){t=b;b=c;c=t;}if(a>b){t=a;a=b;b=t;}fa[i]=b;}
					/* 二值化+迟滞阈值 → 手指弯曲状态(0=弯,1=直) */
					static int finger_state[5] = {0,0,0,0,0};
					for (int i=0;i<5;i++) {
						if (fa[i] > 150.0f) finger_state[i] = 1;
						else if (fa[i] < 100.0f) finger_state[i] = 0;
						/* 中间区间保持上一帧状态 */
					}
				fprintf(stderr, "FA %.0f %.0f %.0f %.0f %.0f | B %d %d %d %d %d\n",
					fa[0],fa[1],fa[2],fa[3],fa[4], finger_state[0],finger_state[1],finger_state[2],finger_state[3],finger_state[4]);
				/* #116: 自训模型 7 类逐帧输出(cls: gesture_0..6) + top1 */
				fprintf(stderr, "POSE cls=%d(%s) sc=%.2f\n", pcl, gname(pcl), psc);
					/* 手势判定：四指组合 → 手势 id
					 * G0=拳(石头) G1=1 G2=2/剪刀 G3=3 G4=4 G5=5/布 G6=点赞
					 * 4/5 靠拇指状态区分，其余拇指不参与（侧面拇指关键点不可靠）
					 * 非法组合 gid=-1 保持上一手势 */
					/* 稳定手势输出 id（5 帧确认后更新；推流标签与 G 行共用） */
					static int gesture_out = -1;
					int n_ext = finger_state[1]+finger_state[2]+finger_state[3]+finger_state[4];
					int gid = -1;
					if (n_ext == 0) {
						gid = 0;
						/* 点赞：四指全弯且拇指尖(TIP4)明显高于手腕(WRIST0)，归一化 y 差 0.25 */
						if (kpt[4][1] < kpt[0][1] - 0.25f) gid = 6;
					} else if (n_ext == 4) {
						/* 数字4/5 区分：四指全直 + 拇指状态（fa 迟滞已算好）
						 * 拇指直=finger_state[0]→G5(布/5)；拇指弯→G4(数字4) */
						gid = finger_state[0] ? 5 : 4;
					} else if (finger_state[1] && !finger_state[2] && !finger_state[3] && !finger_state[4]) {
						gid = 1;
					} else if (finger_state[1] && finger_state[2] && !finger_state[3] && !finger_state[4]) {
						gid = 2;
					} else if (finger_state[1] && finger_state[2] && finger_state[3] && !finger_state[4]) {
						gid = 3;
					}
					/* 5 帧稳定确认 → 输出 G 行（机械手 pose 切换） */
					{
						static int cand = -1, cand_cnt = 0;
						if (gid == cand) { if (cand_cnt < 1000) cand_cnt++; }
						else { cand = gid; cand_cnt = 1; }
						if (cand_cnt >= 5 && gid >= 0 && gesture_out != gid) {
							gesture_out = gid;
							printf("G %d\n", gid);
							fflush(stdout);
							fprintf(stderr, "GEST %d\n", gid);
						}
					}
					/* 绘制 21 个手部关键点(原帧归一化坐标×帧尺寸) */
					for (int q=0;q<21;q++){
						int px = (int)(kpt[q][0]*fw);
						int py = (int)(kpt[q][1]*fh);
						if (px>=0&&px<fw&&py>=0&&py<fh){
							cv::circle(bgr, cv::Point(px,py), 3,
							           cv::Scalar(0,255,255), -1); /* 黄色点 */
						}
					}
					/* 标签显示实际控制用的稳定手势（G 判定 5 帧确认结果） */
					char t[64];snprintf(t,sizeof(t),"G%d %s", gesture_out, g7name(gesture_out));
					cv::putText(bgr,t,cv::Point(box[0],box[1]>20?box[1]-6:box[1]+16),cv::FONT_HERSHEY_SIMPLEX,0.7,cv::Scalar(255,0,0),2,cv::LINE_AA);
					if(total%10==0){printf("pose sc=%.2f cls=%d(%s) bbox=(%d,%d,%d,%d) kpt0=(%.2f,%.2f)\n",
					       psc,pcl,gname(pcl),box[0],box[1],box[2],box[3],kpt[0][0],kpt[0][1]);fflush(stdout);}
			}

		/* 画 ROI 辅助框 (绿框): 手需放入此框内才能被识别 */
		{
			int rw = g_roi_side, rh = g_roi_side;
			if (g_roi_x + rw > fw) rw = fw - g_roi_x;
			if (g_roi_y + rh > fh) rh = fh - g_roi_y;
			if (rw > 0 && rh > 0)
				cv::rectangle(bgr, cv::Rect(g_roi_x, g_roi_y, rw, rh),
				              cv::Scalar(0, 255, 0), 2);
		}
		/* 缩放+软编码 */
		cv::Mat small;cv::resize(bgr,small,cv::Size(640,360));
		std::vector<int>pp;pp.push_back(cv::IMWRITE_JPEG_QUALITY);pp.push_back(80);
		std::vector<unsigned char>jpg;
		bool ok=cv::imencode(".jpg",small,jpg,pp);
		total++;

		if(f->pu8VirAddr[0]==NULL)CVI_SYS_Munmap(va0,(CVI_U32)ylen+(CVI_U32)(fw*fh/2)+64);
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame);

		if(ok) push_jpeg_to_http(jpg.data(),jpg.size());
		if(total%30==0){printf("frames=%ld hand=%u\n",total,nh);fflush(stdout);}
		usleep(10*1000);
	}

	g_run=0;
	pthread_join(hthr,NULL);
err_handle:
err_vpss:
	SAMPLE_COMM_VPSS_Stop(VPSS_GRP,NULL);
err_sys:
	SAMPLE_COMM_SYS_Exit();
	printf("exit\n");
	return 0;
}
