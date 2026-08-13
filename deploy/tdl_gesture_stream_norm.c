/*
 * tdl_gesture_stream.c - 手势识别 + 软编码 MJPEG 推流一体化（独立 HTTP 线程，解决推流连接卡住）
 * 流程: VI/VPSS(手工)→VPSS Chn0取帧→CVI_SYS_Mmap→TDL_Detection(手部)→TDL_Classification(手势)
 *       →OpenCV画框标注→imencode→HTTP线程 multipart 推流
 * 主线程: 取帧+检测+分类+软编码(共享最新JPEG)
 * HTTP线程: 独立 accept + 断线检测 + 从共享缓冲发送JPEG（不阻塞于检测推理）
 * 动态链接评估板固件库(libtdl_core.so + opencv4.5)，绕开 VENC 内核 bug
 * 用法: ./tdl_gesture_stream <port>
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

#include "tdl_sdk.h"
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

#define MODEL_JSON	"/mnt/data/model_factory.json"
#define MODEL_DIR	"/mnt/data/models/cv181x/"
#define MODEL_HAND	MODEL_DIR "yolov8n_det_hand_384_640_INT8_cv181x.cvimodel"
/* 手势分类用裸 cviruntime 加载 gesture_7cls.cvimodel(已内置归一化, 输入0-255 fp32) */
#define MODEL_GEST	MODEL_DIR "gesture_7cls.cvimodel"

static const char *gname(int id){ switch(id){case 0:return"fist";case 1:return"number1";case 2:return"number2";case 3:return"number3";case 4:return"number4";case 5:return"number5";case 6:return"thumbs-up";default:return"?";} }

/* 1=ROI裁剪分类(官方标准,识别准), 0=整帧分类(对照,如噪点来自ROI则此版无噪点) */
#define USE_ROI_CLS 1

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
/* ---- 静态 buffer 用于裁剪ROI→NV21---- */
static unsigned char g_roi_nv21[128*128*3/2 + 64];
static VIDEO_FRAME_INFO_S g_roi_frame;

/* ---- 裸 cviruntime 手势分类(绕过 TDL FP32 预处理bug) ---- */
static CVI_MODEL_HANDLE g_cls_model = NULL;
static CVI_TENSOR *g_cls_in = NULL, *g_cls_out = NULL;
static int32_t g_cls_in_num = 0, g_cls_out_num = 0;
static float g_cls_buf[3*128*128];

/* 初始化裸 cviruntime 7类手势分类模型 */
static int cls_norm_init(const char *model_path)
{
	CVI_RC rc;
	rc = CVI_NN_RegisterModel(model_path, &g_cls_model);
	if (rc != CVI_RC_SUCCESS) { printf("cls_norm_init RegisterModel %#x\n", rc); return -1; }
	rc = CVI_NN_GetInputOutputTensors(g_cls_model, &g_cls_in, &g_cls_in_num,
	                                  &g_cls_out, &g_cls_out_num);
	if (rc != CVI_RC_SUCCESS) { printf("cls_norm_init GetIO %#x\n", rc); return -1; }
	printf("cls_norm_init OK: in_fmt=%d qscale=%.3f out_num=%d\n",
	       (int)g_cls_in[0].fmt, g_cls_in[0].qscale, g_cls_out_num);
	return 0;
}

/* 对 BGR ROI(任意尺寸) 居中裁剪+resize 到 128x128, 转 0-255 fp32 RGB NCHW,
 * 裸 cviruntime Forward, 返回 (label, softmax_score)。
 * 返回 0 成功, -1 失败。 */
static int cls_norm_infer(cv::Mat &bgr_roi, int *label, float *score)
{
	if (!g_cls_model) return -1;
	cv::Mat res;
	cv::resize(bgr_roi, res, cv::Size(128,128), 0,0, cv::INTER_LINEAR);
	/* BGR->RGB, 填 fp32 NCHW 0-255 */
	float *dst = g_cls_buf;
	for (int c=0;c<3;c++){
		for (int y=0;y<128;y++){
			const uchar *row = res.ptr<uchar>(y);
			for (int x=0;x<128;x++){
				float v;
				if (c==0)      v = (float)row[x*3+2]; /* R */
				else if (c==1) v = (float)row[x*3+1]; /* G */
				else           v = (float)row[x*3+0]; /* B */
				dst[(c*128+y)*128+x] = v;
			}
		}
	}
	memcpy(g_cls_in[0].sys_mem, g_cls_buf, sizeof(g_cls_buf));
	CVI_RC rc = CVI_NN_Forward(g_cls_model, g_cls_in, g_cls_in_num, g_cls_out, g_cls_out_num);
	if (rc != CVI_RC_SUCCESS) return -1;
	float *od = (float*)g_cls_out[0].sys_mem;
	int cnt = g_cls_out[0].mem_size / 4;
	if (cnt < 1) return -1;
	/* softmax */
	int best = 0; float bz = -1e30f;
	for (int i=0;i<cnt;i++) if (od[i] > bz) { bz = od[i]; best = i; }
	float sum = 0;
	for (int i=0;i<cnt;i++) sum += expf(od[i]-bz);
	*label = best;
	*score = expf(od[best]-bz) / sum;
	return 0;
}
/* 从 BGR Mat 裁剪+resize 到 w,h, 转 NV21 填入 g_roi_frame */
static TDLImage make_roi_image(cv::Mat &bgr, int bx1,int by1,int bx2,int by2,int w,int h)
{
	cv::Rect r(bx1,by1,bx2-bx1,by2-by1);
	cv::Mat roi = bgr(r).clone();
	cv::Mat res;
	cv::resize(roi, res, cv::Size(w,h));
	/* BGR→NV21 */
	int ylen=w*h, uvlen=w*h/2;
	for(int y=0;y<h;y++){
		for(int x=0;x<w;x++){
			cv::Vec3b p=res.at<cv::Vec3b>(y,x);
			int B=p[0],G=p[1],R=p[2];
			int Y=((66*R+129*G+25*B+128)>>8)+16;
			g_roi_nv21[y*w+x]=(unsigned char)(Y<0?0:(Y>255?255:Y));
		}
	}
	/* NV21 UV 交错 (VU) 每2x2共用一组 */
	for(int y=0;y<h;y+=2){
		for(int x=0;x<w;x+=2){
			int v=0,u=0;
			{int xx=x,yy=y; cv::Vec3b p=res.at<cv::Vec3b>(yy,xx); int B=p[0],G=p[1],R=p[2];
			 int _v=((112*R-94*G-18*B+128)>>8)+128; int _u=((-38*R-74*G+112*B+128)>>8)+128; v+=_v;u+=_u;}
			{int xx=x+1,yy=y; if(xx<w){cv::Vec3b p=res.at<cv::Vec3b>(yy,xx); int B=p[0],G=p[1],R=p[2]; v+=((112*R-94*G-18*B+128)>>8)+128;u+=((-38*R-74*G+112*B+128)>>8)+128;}}
			{int xx=x,yy=y+1; if(yy<h){cv::Vec3b p=res.at<cv::Vec3b>(yy,xx); int B=p[0],G=p[1],R=p[2]; v+=((112*R-94*G-18*B+128)>>8)+128;u+=((-38*R-74*G+112*B+128)>>8)+128;}}
			{int xx=x+1,yy=y+1; if(xx<w&&yy<h){cv::Vec3b p=res.at<cv::Vec3b>(yy,xx); int B=p[0],G=p[1],R=p[2]; v+=((112*R-94*G-18*B+128)>>8)+128;u+=((-38*R-74*G+112*B+128)>>8)+128;}}
			v=v/4; u=u/4;
			int uv_off=ylen + (y/2)*(w/2) + (x/2);
			g_roi_nv21[uv_off]=(unsigned char)(v<0?0:(v>255?255:v));   /* V 先 */
			g_roi_nv21[uv_off+ (w/2)*(h/2)]=(unsigned char)(u<0?0:(u>255?255:u)); /* U 第二平面 */
		}
	}
	/* 填 VIDEO_FRAME (NV21: PIXEL_FORMAT_NV21=19) */
	memset(&g_roi_frame,0,sizeof(g_roi_frame));
	g_roi_frame.stVFrame.u32Width=w; g_roi_frame.stVFrame.u32Height=h;
	g_roi_frame.stVFrame.enPixelFormat=(PIXEL_FORMAT_E)19; /* NV21 */
	g_roi_frame.stVFrame.u32Stride[0]=w; g_roi_frame.stVFrame.u32Length[0]=ylen;
	g_roi_frame.stVFrame.u32Length[1]=uvlen;
	g_roi_frame.stVFrame.pu8VirAddr[0]=g_roi_nv21;
	g_roi_frame.stVFrame.enVideoFormat=VIDEO_FORMAT_LINEAR;
	return TDL_WrapFrame(&g_roi_frame,false,false);
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
					char b[512]; (void)recv(cli_fd, b, 511, 0);
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

	/* 6. TDL 初始化 */
	h=TDL_CreateHandle(0);
	if(!h){printf("CreateHandle fail\n");goto err_handle;}
	if(TDL_LoadModelConfig(h,MODEL_JSON)!=0)printf("WARN LoadModelConfig\n");
	if(TDL_OpenModel(h,TDL_MODEL_YOLOV8N_DET_HAND,MODEL_HAND,MODEL_JSON,0)!=0){printf("OpenModel HAND fail\n");goto err_handle;}
	/* 手势分类绕过 TDL FP32 预处理bug, 用裸 cviruntime(输入 0-255 fp32 RGB) */
	if(cls_norm_init(MODEL_GEST)!=0){printf("cls_norm_init CLS fail\n");goto err_handle;}
	TDL_SetModelThreshold(h,TDL_MODEL_YOLOV8N_DET_HAND,0.45f);
	printf("TDL models OK\n"); fflush(stdout);

	/* 7. 主循环: 取帧→检测→分类→画框→软编码→push_jpeg */
	while(g_run){
		ret=CVI_VPSS_GetChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame,1000);
		if(ret!=CVI_SUCCESS){usleep(20*1000);continue;}
		VIDEO_FRAME_S*f=&stVpssFrame.stVFrame;
		int fw=f->u32Width,fh=f->u32Height,ylen=f->u32Length[0];

		void*va0=(f->pu8VirAddr[0])?(void*)f->pu8VirAddr[0]:CVI_SYS_Mmap(f->u64PhyAddr[0],(CVI_U32)ylen+(CVI_U32)(fw*fh/2)+64);
		if(!va0){CVI_VPSS_ReleaseChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame);continue;}
		unsigned char*yuv=(unsigned char*)va0;

		cv::Mat nv21(fh*3/2,fw,CV_8UC1,yuv);
		cv::Mat bgr;
		cv::cvtColor(nv21,bgr,cv::COLOR_YUV2BGR_NV21);

		/* TDL 手部检测 */
		TDLImage tdl_img=TDL_WrapFrame(&stVpssFrame,false,false);
		TDLObject obj={0};
		uint32_t nh=0;
		int drc=TDL_Detection(h,TDL_MODEL_YOLOV8N_DET_HAND,tdl_img,&obj);
		if(drc==0) nh=obj.size;

#if USE_ROI_CLS==1
		/* 对每个手部: 裁剪 ROI → 分类 → 画框+标注 */
		for(uint32_t k=0;k<nh&&k<8;k++){
			int x1=(int)obj.info[k].box.x1,y1=(int)obj.info[k].box.y1,
			    x2=(int)obj.info[k].box.x2,y2=(int)obj.info[k].box.y2;
			if(x1<0)x1=0;if(y1<0)y1=0;if(x2>fw)x2=fw;if(y2>fh)y2=fh;
			if(x2<=x1||y2<=y1)continue;
			/* 画框 */
			cv::rectangle(bgr,cv::Rect(x1,y1,x2-x1,y2-y1),cv::Scalar(0,255,0),2);
			/* 手势分类: 裁剪手部ROI(膨胀1.1)→128x128→裸cviruntime分类 */
			int bw=x2-x1,bh=y2-y1;
			int cx1=x1-bw/20, cy1=y1-bh/20, cx2=x2+bw/20, cy2=y2+bh/20;  /* ~1.1x 膨胀 */
			if(cx1<0)cx1=0; if(cy1<0)cy1=0; if(cx2>fw)cx2=fw; if(cy2>fh)cy2=fh;
			{
				cv::Rect r(cx1,cy1,cx2-cx1,cy2-cy1);
				cv::Mat roi_mat = bgr(r);
				int fid = -1; float sscore = 0;
				if (cls_norm_infer(roi_mat, &fid, &sscore) == 0) {
					if (sscore < 0.40f) fid = 2;  /* 置信度过低判 none=2 */
					char t[64];snprintf(t,sizeof(t),"%s %.2f",gname(fid),sscore);
					cv::putText(bgr,t,cv::Point(x1,y1>20?y1-6:y1+16),cv::FONT_HERSHEY_SIMPLEX,0.6,cv::Scalar(255,0,0),2,cv::LINE_AA);
					if(total%10==0){printf("class[%d] bbox=(%d,%d,%d,%d) label=%d %s score=%.2f\n",k,x1,y1,x2,y2,fid,gname(fid),sscore);fflush(stdout);}
				}
			}
		}
#else
		/* 整帧分类(对照测试) */
		for(uint32_t k=0;k<nh&&k<8;k++){
			int x1=(int)obj.info[k].box.x1,y1=(int)obj.info[k].box.y1,
			    x2=(int)obj.info[k].box.x2,y2=(int)obj.info[k].box.y2;
			if(x1<0)x1=0;if(y1<0)y1=0;if(x2>fw)x2=fw;if(y2>fh)y2=fh;
			if(x2<=x1||y2<=y1)continue;
			cv::rectangle(bgr,cv::Rect(x1,y1,x2-x1,y2-y1),cv::Scalar(0,255,0),2);
		}
		TDLClassInfo cls={0};
		if(nh>0 && TDL_Classification(h,TDL_MODEL_CLS_HAND_GESTURE,tdl_img,&cls)==0){
			int fid=(cls.score<0.35f)?2:cls.class_id;
			char t[64];snprintf(t,sizeof(t),"%s %.2f",gname(fid),cls.score);
			cv::putText(bgr,t,cv::Point(8,30),cv::FONT_HERSHEY_SIMPLEX,0.8,cv::Scalar(255,0,0),2,cv::LINE_AA);
			if(total%10==0){printf("hand_cls label=%d %s score=%.2f\n",fid,gname(fid),cls.score);fflush(stdout);}
		}
#endif

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
	TDL_DestroyHandle(h);
err_handle:
err_vpss:
	SAMPLE_COMM_VPSS_Stop(VPSS_GRP,NULL);
err_sys:
	SAMPLE_COMM_SYS_Exit();
	printf("exit\n");
	return 0;
}
