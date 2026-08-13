/*
 * sample_vi_gesture.c
 * 基于 duo-buildroot-sdk-v2 配套头文件 + 固件 libsample.so 的实时手势分类验证程序。
 * 复用 trio sample_vi_fd 的 VI/VPSS 初始化（middleware_utils），但推理用
 * CVI_TDL_Image_Classification（固件正确分类 API），输出 cvtdl_class_meta_t。
 * 前提：用 dbr2 头文件编译 + 固件 libsample.so 链接（ABI 匹配固件）。
 */
#define LOG_TAG "SampleGesture"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "middleware_utils.h"
#include "sample_utils.h"
#include "vi_vo_utils.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <sample_comm.h>
#include "cvi_tdl.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool bExit = false;
static int g_infer_count = 0;

static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo) {
    bExit = true;
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("\nUsage: %s GESTURE_MODEL_PATH\n", argv[0]);
    return CVI_TDL_FAILURE;
  }

  signal(SIGINT, SampleHandleSig);
  signal(SIGTERM, SampleHandleSig);

  SAMPLE_TDL_MW_CONFIG_S stMWConfig = {0};

  CVI_S32 s32Ret = SAMPLE_TDL_Get_VI_Config(&stMWConfig.stViConfig);
  if (s32Ret != CVI_SUCCESS || stMWConfig.stViConfig.s32WorkingViNum <= 0) {
    printf("Failed to get sensor infomation from ini file (/mnt/data/sensor_cfg.ini).\n");
    return -1;
  }

  PIC_SIZE_E enPicSize;
  s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stMWConfig.stViConfig.astViInfo[0].stSnsInfo.enSnsType,
                                          &enPicSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get sensor size\n");
    return -1;
  }

  SIZE_S stSensorSize;
  s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSensorSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get sensor size\n");
    return -1;
  }

  printf("Sensor size: %ux%u\n", stSensorSize.u32Width, stSensorSize.u32Height);

  // VBPool 配置：3 池（VPSS Chn0 / Chn1 / TDL 预处理）
  stMWConfig.stVBPoolConfig.u32VBPoolCount = 3;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].enFormat = VI_PIXEL_FORMAT;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 5;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32Height = stSensorSize.u32Height;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32Width = stSensorSize.u32Width;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].bBind = true;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32VpssChnBinding = VPSS_CHN0;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32VpssGrpBinding = (VPSS_GRP)0;

  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].enFormat = VI_PIXEL_FORMAT;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32BlkCount = 5;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Height = stSensorSize.u32Height;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Width = stSensorSize.u32Width;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].bBind = true;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32VpssChnBinding = VPSS_CHN1;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32VpssGrpBinding = (VPSS_GRP)0;

  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].enFormat = PIXEL_FORMAT_BGR_888_PLANAR;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32BlkCount = 3;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32Height = 720;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32Width = 1280;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].bBind = false;

  // VPSS Grp0：VBPool 0 预留给 VI 绑定，Grp0 用 VBPool 1/2
  stMWConfig.stVPSSPoolConfig.u32VpssGrpCount = 1;
#ifndef CV186X
  stMWConfig.stVPSSPoolConfig.stVpssMode.aenInput[0] = VPSS_INPUT_MEM;
  stMWConfig.stVPSSPoolConfig.stVpssMode.enMode = VPSS_MODE_DUAL;
  stMWConfig.stVPSSPoolConfig.stVpssMode.ViPipe[0] = 0;
  stMWConfig.stVPSSPoolConfig.stVpssMode.aenInput[1] = VPSS_INPUT_ISP;
  stMWConfig.stVPSSPoolConfig.stVpssMode.ViPipe[1] = 0;
#endif

  SAMPLE_TDL_VPSS_CONFIG_S *pstVpssConfig = &stMWConfig.stVPSSPoolConfig.astVpssConfig[0];
  pstVpssConfig->bBindVI = true;

  VPSS_GRP_DEFAULT_HELPER2(&pstVpssConfig->stVpssGrpAttr, stSensorSize.u32Width,
                           stSensorSize.u32Height, VI_PIXEL_FORMAT, 1);
  pstVpssConfig->u32ChnCount = 2;
  pstVpssConfig->u32ChnBindVI = 0;
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0], stSensorSize.u32Width,
                          stSensorSize.u32Height, VI_PIXEL_FORMAT, true);
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[1], stSensorSize.u32Width,
                          stSensorSize.u32Height, VI_PIXEL_FORMAT, true);

  // VENC/RTSP 配置（SAMPLE_TDL_Init_WM 内部会初始化 VENC/RTSP，必须填充）
  SIZE_S stVencSize = {.u32Width = 1280, .u32Height = 720};
  SAMPLE_TDL_Get_Input_Config(&stMWConfig.stVencConfig.stChnInputCfg);
  stMWConfig.stVencConfig.u32FrameWidth = stVencSize.u32Width;
  stMWConfig.stVencConfig.u32FrameHeight = stVencSize.u32Height;
  SAMPLE_TDL_Get_RTSP_Config(&stMWConfig.stRTSPConfig.stRTSPConfig);

  SAMPLE_TDL_MW_CONTEXT stMWContext = {0};
  s32Ret = SAMPLE_TDL_Init_WM(&stMWConfig, &stMWContext);
  if (s32Ret != CVI_SUCCESS) {
    printf("init middleware failed! ret=%x\n", s32Ret);
    return -1;
  }

  cvitdl_handle_t stTDLHandle = NULL;
  s32Ret = CVI_TDL_CreateHandle2(&stTDLHandle, 1, 0);
  if (s32Ret != CVI_SUCCESS) {
    printf("CreateHandle2 failed! ret=%x\n", s32Ret);
    return -1;
  }

  s32Ret = CVI_TDL_SetVBPool(stTDLHandle, 0, 2);
  printf("SetVBPool ret=%x\n", s32Ret);

  CVI_TDL_SetVpssTimeout(stTDLHandle, 1000);

  s32Ret = CVI_TDL_OpenModel(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, argv[1]);
  if (s32Ret != CVI_SUCCESS) {
    printf("open yolov8 hand model failed! ret=%x\n", s32Ret);
    return -1;
  }
  printf("open yolov8 hand model OK\n");

  CVI_TDL_SetModelThreshold(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, 0.3);
  CVI_TDL_SetModelNmsThreshold(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, 0.5);

  printf("Start inference loop...\n");
  VIDEO_FRAME_INFO_S stFrame;
  while (bExit == false) {
    memset(&stFrame, 0, sizeof(VIDEO_FRAME_INFO_S));
    s32Ret = CVI_VPSS_GetChnFrame(0, VPSS_CHN1, &stFrame, 2000);
    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame failed with %#x\n", s32Ret);
      usleep(10000);
      continue;
    }

    cvtdl_object_t obj = {0};
    s32Ret = CVI_TDL_Detection(stTDLHandle, &stFrame, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, &obj);
    if (s32Ret != CVI_TDL_SUCCESS) {
      printf("Detection failed ret=%x\n", s32Ret);
    } else {
      if (g_infer_count % 15 == 0) {
        printf("hand detect count=%u (frame %ux%u)\n", obj.size, stFrame.stVFrame.u32Width,
               stFrame.stVFrame.u32Height);
        for (uint32_t i = 0; i < obj.size; i++) {
          printf("  hand[%u] bbox=(%.0f,%.0f,%.0f,%.0f) score=%.3f\n", i, obj.info[i].bbox.x1,
                 obj.info[i].bbox.y1, obj.info[i].bbox.x2, obj.info[i].bbox.y2,
                 obj.info[i].bbox.score);
        }
      }
    }
    g_infer_count++;

    CVI_TDL_Free(&obj);
    CVI_VPSS_ReleaseChnFrame(0, VPSS_CHN1, &stFrame);
    usleep(30000);  // ~30fps
  }

  CVI_TDL_DestroyHandle(stTDLHandle);
  SAMPLE_TDL_Destroy_MW(&stMWContext);
  printf("done, total inference %d\n", g_infer_count);
  return 0;
}
