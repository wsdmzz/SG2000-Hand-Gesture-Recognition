#include <stdio.h>
#include <stddef.h>
#include "sample_comm.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"

int main(void) {
  printf("SAMPLE_INI_CFG_S = %zu\n", sizeof(SAMPLE_INI_CFG_S));
  printf("SAMPLE_VI_CONFIG_S = %zu\n", sizeof(SAMPLE_VI_CONFIG_S));
  printf("SAMPLE_VI_INFO_S = %zu\n", sizeof(SAMPLE_VI_INFO_S));
  printf("offsetof(s32WorkingViNum) check\n");
  /* 固件版 memset 232 字节猜测对应 SAMPLE_INI_CFG_S; 若 ==232 则与固件版 ABI 一致 */
  printf("VI_MAX_DEV_NUM = %d\n", VI_MAX_DEV_NUM);
  printf("WDR_MAX_PIPE_NUM = %d\n", WDR_MAX_PIPE_NUM);
  return 0;
}
