/*
 * platform_null.c — 主机冒烟测试平台（无摄像头/屏/触摸）
 * capture_jpeg 返回内置 8x8 测试图；poll_touch 按脚本吐事件。
 */
#include "collector.h"
#include "test_jpeg.h"
#include <string.h>
#include <unistd.h>

/* 脚本事件（归一化坐标），由 smoke_test 设置 */
typedef struct { int nx, ny; } null_ev_t;
static null_ev_t g_script[64];
static int g_script_n = 0, g_script_i = 0;
static int g_poll_calls = 0;
static int g_ev_every = 10;   /* 每 N 次 poll 吐一个事件 */

void null_platform_reset(void)
{
    g_script_n = 0; g_script_i = 0; g_poll_calls = 0;
}
void null_platform_push(int nx, int ny)
{
    if (g_script_n < 64) {
        g_script[g_script_n].nx = nx;
        g_script[g_script_n].ny = ny;
        g_script_n++;
    }
}

static int null_init(void *ctx, const col_config_t *cfg)
{
    (void)ctx; (void)cfg;
    return 0;
}
static int null_capture(void *ctx, const uint8_t **buf, size_t *len)
{
    (void)ctx;
    *buf = TEST_JPEG;
    *len = TEST_JPEG_LEN;
    return 0;
}
static int null_poll(void *ctx, int timeout_ms, int *nx, int *ny)
{
    (void)ctx;
    usleep(timeout_ms * 1000);
    g_poll_calls++;
    if (g_script_i < g_script_n && g_poll_calls % g_ev_every == 0) {
        *nx = g_script[g_script_i].nx;
        *ny = g_script[g_script_i].ny;
        g_script_i++;
        return 1;
    }
    return 0;
}
static void null_render(void *ctx, const col_fsm_t *f, const col_ui_t *ui,
                        const char *line)
{
    (void)ctx; (void)f; (void)ui; (void)line;
}
static void null_deinit(void *ctx) { (void)ctx; }

static const col_platform_t g_null = {
    .init = null_init,
    .capture_jpeg = null_capture,
    .poll_touch = null_poll,
    .render = null_render,
    .deinit = null_deinit,
};

const col_platform_t *platform_get(void) { return &g_null; }