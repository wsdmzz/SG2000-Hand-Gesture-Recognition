/*
 * tdl_hand_only.c - 只检测手部的最小验证程序（排除推流/分类/画框干扰）
 * 手工取帧 → TDL_WrapFrame → TDL_Detection → 打印 obj.size/bbox → 退出
 * 定位: 伸手却检不出 (hand=0) 的根因 —— 输入帧格式 or 推理 或 画面无手
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "tdl_sdk.h"

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

static volatile int g_run = 1;
static void sig_handler(int s){ (void)s; g_run=0; }

int main(int argc, char *argv[])
{
	int nframes = (argc>1)?atoi(argv[1]):5;
	CVI_S32 ret; int opt=1;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	VB_CONFIG_S stVbConf;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize, stSizeIn, stSizeOut;
	VIDEO_FRAME_INFO_S stVpssFrame;
	CVI_U32 u32BlkSize;
	TDLHandle h=NULL;
	int total=0, any=0;
	int i;

	signal(SIGINT,sig_handler); signal(SIGTERM,sig_handler);
	printf("=== TDL hand-detect ONLY test (%d frames) ===\n", nframes);

	/* 0. 清理残留 VPSS */
	{ CVI_S32 g; for(g=0;g<(CVI_S32)VPSS_MAX_GRP_NUM;g++){ CVI_VPSS_StopGrp(g); CVI_VPSS_DestroyGrp(g);} usleep(200*1000); }

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

	/* 2. VB+SYS */
	memset(&stVbConf,0,sizeof(stVbConf));
	stVbConf.u32MaxPoolCnt=2;
	u32BlkSize=COMMON_GetPicBufferSize(stSize.u32Width,stSize.u32Height,SAMPLE_PIXEL_FORMAT,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkSize=u32BlkSize; stVbConf.astCommPool[0].u32BlkCnt=5;
	u32BlkSize=COMMON_GetPicBufferSize(768,1280,SAMPLE_PIXEL_FORMAT,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
	stVbConf.astCommPool[1].u32BlkSize=u32BlkSize; stVbConf.astCommPool[1].u32BlkCnt=8;
	ret=SAMPLE_COMM_SYS_Init(&stVbConf);
	if(ret!=CVI_SUCCESS){printf("SYS_Init %#x\n",ret);return -1;}

	/* 3. VI */
	ret=SAMPLE_PLAT_VI_INIT(&stViConfig);
	if(ret!=CVI_SUCCESS){printf("VI_INIT %#x\n",ret);goto err_sys;}

	/* 4. VPSS (输出 640x384 匹配 yolov8n_det_hand 输入) */
	stSizeIn=stSize;
	stSizeOut.u32Width=640; stSizeOut.u32Height=384;
	ret=SAMPLE_PLAT_VPSS_INIT(VPSS_GRP,stSizeIn,stSizeOut);
	if(ret!=CVI_SUCCESS){printf("VPSS_INIT %#x\n",ret);goto err_sys;}
	ret=SAMPLE_COMM_VI_Bind_VPSS(0,0,VPSS_GRP);
	if(ret!=CVI_SUCCESS){printf("Bind %#x\n",ret);goto err_vpss;}
	printf("VI->VPSS bind OK\n");

	/* 5. TDL init */
	h=TDL_CreateHandle(0);
	if(!h){printf("CreateHandle fail\n");goto err_vpss;}
	if(TDL_LoadModelConfig(h,MODEL_JSON)!=0)printf("WARN LoadModelConfig\n");
	if(TDL_OpenModel(h,TDL_MODEL_YOLOV8N_DET_HAND,MODEL_HAND,MODEL_JSON,0)!=0){printf("OpenModel HAND fail\n");goto err_handle;}
	TDL_SetModelThreshold(h,TDL_MODEL_YOLOV8N_DET_HAND,0.25f);
	printf("TDL hand model OK\n"); fflush(stdout);

	/* 6. 取 N 帧, 每帧检测, 打印 */
	sleep(2);  /* 等取帧稳定 */
	for(i=0;i<nframes&&g_run;i++){
		ret=CVI_VPSS_GetChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame,2000);
		if(ret!=CVI_SUCCESS){printf("[%d] GetChnFrame fail %#x\n",i,ret);fflush(stdout);continue;}
		VIDEO_FRAME_S*f=&stVpssFrame.stVFrame;
		printf("[%d] frame %dx%d fmt=%d pitch=%d len=%u\n",i,f->u32Width,f->u32Height,f->enPixelFormat,f->u32Stride[0],f->u32Length[0]);
		fflush(stdout);

		TDLObject obj={0};
		printf("[%d] about to WrapFrame\n",i); fflush(stdout);
		TDLImage img=TDL_WrapFrame(&stVpssFrame,false,false);  /* is_preprocessed=false: 让TDL内部做预处理(#63) */
		printf("[%d] WrapFrame done, img=%p\n",i,(void*)img); fflush(stdout);
		int drc=TDL_Detection(h,TDL_MODEL_YOLOV8N_DET_HAND,img,&obj);
		printf("[%d] TDL_Detection rc=%d obj.size=%u\n",i,drc,obj.size); fflush(stdout);
		if(drc==0 && obj.size>0){
			any++;
			for(uint32_t k=0;k<obj.size&&k<8;k++){
				printf("   [%d] bbox=(%.0f,%.0f,%.0f,%.0f) score=%.3f class=%d\n",
					k,obj.info[k].box.x1,obj.info[k].box.y1,obj.info[k].box.x2,obj.info[k].box.y2,
					obj.info[k].score,obj.info[k].class_id);
			}
		}
		total++;
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP,VPSS_CHN,&stVpssFrame);
		usleep(100*1000);  /* 每帧间隔 100ms */
	}
	printf("=== DONE: %d frames, %d with hand ===\n", total, any);

	TDL_DestroyHandle(h);
err_handle:
err_vpss:
	SAMPLE_COMM_VPSS_Stop(VPSS_GRP,NULL);
err_sys:
	SAMPLE_COMM_SYS_Exit();
	return 0;
}
