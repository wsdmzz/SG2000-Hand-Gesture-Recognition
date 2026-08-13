// sample_vi_hand.c
// 基于官方 sample_vi_fd.c 改造：从摄像头实时检测【手】，并 RTSP 推流。
// 改动点：
//   1. 推理：CVI_TDL_ScrFDFace (人脸) -> CVI_TDL_YOLOV8_Detection (手检测)
//   2. 数据结构：cvtdl_face_t -> cvtdl_object_t
//   3. 画框：CVI_TDL_Service_FaceDrawRect -> CVI_TDL_Service_ObjectDrawRect
//   4. 模型：CVI_TDL_SUPPORTED_MODEL_SCRFDFACE -> CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION
//   5. 日志："face count" -> "hand count"
//
// 用法：sample_vi_hand <手检测模型.cvimodel>
// 例如：./sample_vi_hand /root/yolov8n_det_hand_384_640_INT8_cv181x.cvimodel

#define LOG_TAG "SampleHand"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "middleware_utils.h"
#include "sample_utils.h"
#include "vi_vo_utils.h"
#include "gesture_cls.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <rtsp.h>
#include <sample_comm.h>
#include "cvi_tdl.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool bExit = false;

static cvtdl_object_t g_stObjectMeta = {0};

static uint32_t g_size = 0;

/* 当前手部手势类别（0=fist 1=five 2=none 3=two, -1=无），供画框标注 */
static int g_gest_cls = -1;
static float g_gest_prob = 0.f;
static uint32_t g_cls_frames = 0;   /* 手势分类降频计数 */

MUTEXAUTOLOCK_INIT(ResultMutex);

typedef struct {
  SAMPLE_TDL_MW_CONTEXT *pstMWContext;
  cvitdl_service_handle_t stServiceHandle;
} SAMPLE_TDL_VENC_THREAD_ARG_S;

static void CopyObjectMeta(const cvtdl_object_t *src, cvtdl_object_t *dst) {
  memset(dst, 0, sizeof(cvtdl_object_t));
  if (src->size <= 0) {
    dst->size = 0;
    return;
  }
  dst->size = src->size;
  dst->width = src->width;
  dst->height = src->height;
  uint32_t n = src->size;
  dst->info = (cvtdl_object_info_t *)malloc(sizeof(cvtdl_object_info_t) * n);
  for (uint32_t i = 0; i < n; i++) {
    dst->info[i] = src->info[i];
  }
}

void *run_venc(void *args) {
  printf("Enter encoder thread\n");
  SAMPLE_TDL_VENC_THREAD_ARG_S *pstArgs = (SAMPLE_TDL_VENC_THREAD_ARG_S *)args;
  VIDEO_FRAME_INFO_S stFrame;
  CVI_S32 s32Ret;
  cvtdl_object_t stObjectMeta = {0};

  while (bExit == false) {
    s32Ret = CVI_VPSS_GetChnFrame(0, 0, &stFrame, 2000);
    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame chn0 failed with %#x, retry\n", s32Ret);
      usleep(30000);
      continue;
    }

    {
      MutexAutoLock(ResultMutex, lock);
      CopyObjectMeta(&g_stObjectMeta, &stObjectMeta);
    }

    s32Ret = CVI_TDL_Service_ObjectDrawRect(pstArgs->stServiceHandle, &stObjectMeta, &stFrame, true,
                                            CVI_TDL_Service_GetDefaultBrush());
    if (s32Ret != CVI_TDL_SUCCESS) {
      CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
      printf("Draw fame fail!, ret=%x\n", s32Ret);
      goto error;
    }

    s32Ret = SAMPLE_TDL_Send_Frame_RTSP(&stFrame, pstArgs->pstMWContext);
    if (s32Ret != CVI_SUCCESS) {
      CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
      printf("Send Output Frame NG, ret=%x\n", s32Ret);
      goto error;
    }

  error:
    CVI_TDL_Free(&stObjectMeta);
    CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
    /* 不因单次 DrawRect/SendFrame 失败退出，持续重试保证推流 */
    usleep(30000);
  }
  printf("Exit encoder thread\n");
  pthread_exit(NULL);
}

void *run_tdl_thread(void *pHandle) {
  printf("Enter TDL thread\n");
  cvitdl_handle_t pstTDLHandle = (cvitdl_handle_t)pHandle;

  VIDEO_FRAME_INFO_S stFrame;
  cvtdl_object_t stObjectMeta = {0};

  CVI_S32 s32Ret;
  while (bExit == false) {
    s32Ret = CVI_VPSS_GetChnFrame(0, VPSS_CHN1, &stFrame, 2000);

    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame failed with %#x\n", s32Ret);
      goto get_frame_failed;
    }

    s32Ret = CVI_TDL_Detection(pstTDLHandle, &stFrame, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, &stObjectMeta);
    if (s32Ret != CVI_TDL_SUCCESS) {
      printf("inference failed!, ret=%x\n", s32Ret);
      goto inf_error;
    }

    if (stObjectMeta.size != g_size) {
      printf("hand count: %u\n", stObjectMeta.size);
      g_size = stObjectMeta.size;
    }
    {
      /* 持锁区只快速拷贝 meta（含已标注的检测类别），立即放锁，避免阻塞 VENC 画框线程 */
      MutexAutoLock(ResultMutex, lock);
      CVI_TDL_Free(&g_stObjectMeta);
      CopyObjectMeta(&stObjectMeta, &g_stObjectMeta);
    }

    /* 手势分类：锁外执行，每 6 帧一次（降频繁开销，降低推流卡死概率） */
    int _cls = g_gest_cls;
    float _prob[4] = {0, 0, 0, 0};
    if (_cls >= 0 && _cls < 4) _prob[_cls] = g_gest_prob;
    if (stObjectMeta.size > 0 && (g_cls_frames++ % 30 == 0)) {
      int bx = (int)stObjectMeta.info[0].bbox.x1;
      int by = (int)stObjectMeta.info[0].bbox.y1;
      int bw = (int)(stObjectMeta.info[0].bbox.x2 - stObjectMeta.info[0].bbox.x1);
      int bh = (int)(stObjectMeta.info[0].bbox.y2 - stObjectMeta.info[0].bbox.y1);
      if (stFrame.stVFrame.enPixelFormat == PIXEL_FORMAT_NV21 && bw > 4 && bh > 4) {
        printf("[D] call gest_cls_run frame bbox=(%d,%d,%d,%d)\n", bx, by, bw, bh);
        _cls = gest_cls_run(stFrame.stVFrame.pu8VirAddr[0], stFrame.stVFrame.pu8VirAddr[1],
                            stFrame.stVFrame.u32Stride[0], stFrame.stVFrame.u32Stride[1],
                            stFrame.stVFrame.u32Width, stFrame.stVFrame.u32Height,
                            bx, by, bw, bh, _prob);
        printf("[D] gest_cls_run done cls=%d\n", _cls);
        if (_cls >= 0) {
          const char *nm[4] = {"fist", "five", "none", "two"};
          MutexAutoLock(ResultMutex, lock);
          snprintf(g_stObjectMeta.info[0].name, sizeof(g_stObjectMeta.info[0].name),
                   "%s", nm[_cls]);
        }
      }
      g_gest_cls = _cls;
      g_gest_prob = (_cls >= 0) ? _prob[_cls] : 0.f;
    }
    if (g_size % 90 == 0) {
      const char *nm[4] = {"fist", "five", "none", "two"};
      printf("[gest] size=%u cls=%d(%s) prob=%.2f\n", stObjectMeta.size, _cls,
             (_cls >= 0) ? nm[_cls] : "-", (_cls >= 0) ? _prob[_cls] : 0.f);
    }

  inf_error:
    CVI_VPSS_ReleaseChnFrame(0, 1, &stFrame);
  get_frame_failed:
    CVI_TDL_Free(&stObjectMeta);
    usleep(30000); /* 取帧/推理失败也重试，不退出；约 15fps */
  }

  printf("Exit TDL thread\n");
  pthread_exit(NULL);
}

static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo) {
    bExit = true;
  }
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf(
        "\nUsage: %s HAND_MODEL_PATH CLS_MODEL_PATH.\n\n"
        "\tHAND_MODEL_PATH, path to hand detection model (yolov8n_det_hand).\n"
        "\tCLS_MODEL_PATH, path to gesture classification model (cls_hand_gesture).\n",
        argv[0]);
    return CVI_TDL_FAILURE;
  }

  signal(SIGINT, SampleHandleSig);
  signal(SIGTERM, SampleHandleSig);

  SAMPLE_TDL_MW_CONFIG_S stMWConfig = {0};

  CVI_S32 s32Ret = SAMPLE_TDL_Get_VI_Config(&stMWConfig.stViConfig);
  if (s32Ret != CVI_SUCCESS || stMWConfig.stViConfig.s32WorkingViNum <= 0) {
    printf("Failed to get senor infomation from ini file (/mnt/data/sensor_cfg.ini).\n");
    return -1;
  }

  // Get VI size
  PIC_SIZE_E enPicSize;
  s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stMWConfig.stViConfig.astViInfo[0].stSnsInfo.enSnsType,
                                          &enPicSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get senor size\n");
    return -1;
  }

  SIZE_S stSensorSize;
  s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSensorSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get senor size\n");
    return -1;
  }

  // Setup frame size of video encoder to 1080p
  SIZE_S stVencSize = {
      .u32Width = 1280,
      .u32Height = 720,
  };

  stMWConfig.stVBPoolConfig.u32VBPoolCount = 3;

  // VBPool 0 for VPSS Grp0 Chn0
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].enFormat = VI_PIXEL_FORMAT;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 4;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32Height = stVencSize.u32Height;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32Width = stVencSize.u32Width;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].bBind = true;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32VpssChnBinding = VPSS_CHN0;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[0].u32VpssGrpBinding = (VPSS_GRP)0;

  // VBPool 1 for VPSS Grp0 Chn1
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].enFormat = VI_PIXEL_FORMAT;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32BlkCount = 2;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Height = stVencSize.u32Height;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32Width = stVencSize.u32Width;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].bBind = true;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32VpssChnBinding = VPSS_CHN1;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[1].u32VpssGrpBinding = (VPSS_GRP)0;

  // VBPool 2 for TDL preprocessing
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].enFormat = PIXEL_FORMAT_BGR_888_PLANAR;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32BlkCount = 1;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32Height = 720;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].u32Width = 1280;
  stMWConfig.stVBPoolConfig.astVBPoolSetup[2].bBind = false;

  // Setup VPSS Grp0
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

  // Assign device 1 to VPSS Grp0, because device1 has 3 outputs in dual mode.
  VPSS_GRP_DEFAULT_HELPER2(&pstVpssConfig->stVpssGrpAttr, stSensorSize.u32Width,
                           stSensorSize.u32Height, VI_PIXEL_FORMAT, 1);
  pstVpssConfig->u32ChnCount = 2;
  pstVpssConfig->u32ChnBindVI = 0;
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0], stVencSize.u32Width,
                          stVencSize.u32Height, VI_PIXEL_FORMAT, true);
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[1], stVencSize.u32Width,
                          stVencSize.u32Height, VI_PIXEL_FORMAT, true);

  // Get default VENC configurations
  SAMPLE_TDL_Get_Input_Config(&stMWConfig.stVencConfig.stChnInputCfg);
  stMWConfig.stVencConfig.u32FrameWidth = stVencSize.u32Width;
  stMWConfig.stVencConfig.u32FrameHeight = stVencSize.u32Height;

  // Get default RTSP configurations
  SAMPLE_TDL_Get_RTSP_Config(&stMWConfig.stRTSPConfig.stRTSPConfig);

  SAMPLE_TDL_MW_CONTEXT stMWContext = {0};
  s32Ret = SAMPLE_TDL_Init_WM(&stMWConfig, &stMWContext);
  if (s32Ret != CVI_SUCCESS) {
    printf("init middleware failed! ret=%x\n", s32Ret);
    return -1;
  }

  /* 手势分类模型在完整媒体框架初始化之后注册 */
  if (gest_cls_init(argv[2]) != 0) {
    printf("gest_cls_init failed\n");
    return -1;
  }

  cvitdl_handle_t stTDLHandle = NULL;

  // Create TDL handle and assign VPSS Grp1 Device 0 to TDL SDK
  GOTO_IF_FAILED(CVI_TDL_CreateHandle2(&stTDLHandle, 1, 0), s32Ret, create_tdl_fail);

  GOTO_IF_FAILED(CVI_TDL_SetVBPool(stTDLHandle, 0, 2), s32Ret, create_service_fail);

  CVI_TDL_SetVpssTimeout(stTDLHandle, 1000);

  cvitdl_service_handle_t stServiceHandle = NULL;
  GOTO_IF_FAILED(CVI_TDL_Service_CreateHandle(&stServiceHandle, stTDLHandle), s32Ret,
                 create_service_fail);

    // 手检测：使用板上库原生支持的 HAND_DETECTION 专用通道
  CVI_TDL_SetModelThreshold(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, 0.2);
  CVI_TDL_SetModelNmsThreshold(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, 0.5);
  GOTO_IF_FAILED(CVI_TDL_OpenModel(stTDLHandle, CVI_TDL_SUPPORTED_MODEL_HAND_DETECTION, argv[1]),
                 s32Ret, setup_tdl_fail);

  pthread_t stVencThread, stTDLThread;
  SAMPLE_TDL_VENC_THREAD_ARG_S args = {
      .pstMWContext = &stMWContext,
      .stServiceHandle = stServiceHandle,
  };

  pthread_create(&stVencThread, NULL, run_venc, &args);
  pthread_create(&stTDLThread, NULL, run_tdl_thread, stTDLHandle);

  pthread_join(stVencThread, NULL);
  pthread_join(stTDLThread, NULL);

setup_tdl_fail:
  CVI_TDL_Service_DestroyHandle(stServiceHandle);
create_service_fail:
  CVI_TDL_DestroyHandle(stTDLHandle);
create_tdl_fail:
  SAMPLE_TDL_Destroy_MW(&stMWContext);

  return 0;
}
