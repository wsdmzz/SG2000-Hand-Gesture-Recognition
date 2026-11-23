/*
 * collector.h — 挂墙三任务采集器（8寸屏本地版）公共定义
 *
 * 依据：b_plan/00_docs/windows_session/采集程序设计_屏幕版.md
 *       数据采集方案_挂墙三任务.md §7.1（目录/CSV 契约，2026-11-20 实测锁定）
 *
 * 模块划分：
 *   config.c   — site_info.txt / board_id / session_meta.json（POSIX）
 *   storage.c  — 分类目录 / JPEG 落盘 / CSV 每行 flush / manifest / 剩余空间（POSIX）
 *   state.c    — 状态机 + 2fps 节流 + 低空间保护（纯逻辑）
 *   ui_model.c — 按钮布局 + 触摸命中 + 状态文本（纯逻辑，归一化坐标）
 *   main.c     — 单实例锁 / 主循环 / 自检（依赖 platform.h 抽象）
 *   platform_cvitek.c — VI/VPSS/VENC/VO/GT911 适配（待新固件 ABI，#14 后实现）
 *
 * 修复旧版已知 bug：CSV 丢行（95 帧仅 65 行）→ 每行 fflush，关闭/本档完成 fsync。
 */
#ifndef COLLECTOR_H
#define COLLECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 参数网格（与采集方案 §5/§6 对齐） ---- */
#define COL_MAX_PERSON   8
#define COL_TASK_NUM     5    /* hand face eye body negative */
#define COL_DIST_NUM     7    /* 0.3 0.5 1.0 1.5 2.0 3.0 4.0 m */
#define COL_POSH_NUM     3    /* left mid right */
#define COL_POSV_NUM     2    /* high low */
#define COL_LIGHT_NUM    3    /* L1 L2 L3 */

typedef enum {
    COL_TASK_HAND = 0, COL_TASK_FACE, COL_TASK_EYE, COL_TASK_BODY,
    COL_TASK_NEGATIVE
} col_task_t;

typedef enum {
    COL_ST_IDLE = 0, COL_ST_READY, COL_ST_RECORDING, COL_ST_PAUSED
} col_state_t;

typedef struct {
    int person;      /* 1..max_person */
    col_task_t task;
    int dist_idx;    /* col_dist_dm[] 下标 */
    int pos_h;       /* 0=left 1=mid 2=right（仅 hand 生效） */
    int pos_v;       /* 0=high 1=low（仅 hand 生效） */
    int light;       /* 0..2 → L1..L3 */
} col_params_t;

extern const int   col_dist_dm[COL_DIST_NUM];     /* 分米：3,5,10,15,20,30,40 */
extern const char *col_task_name[COL_TASK_NUM];   /* "hand"... */
extern const char *col_posh_name[COL_POSH_NUM];
extern const char *col_posv_name[COL_POSV_NUM];

/* 参数合法性校验（person 上限由 cfg 传入，<=0 表示用 COL_MAX_PERSON） */
bool col_params_valid(const col_params_t *p, int max_person);
/* 两个参数是否构成同一"档"（存储槽位键） */
bool col_params_same_slot(const col_params_t *a, const col_params_t *b);

/* ---- config ---- */
typedef struct {
    char board_id[16];        /* "B01" */
    char site[64];            /* 安装地点描述 */
    char height_cm[16];       /* 安装高度 */
    char fov_note[128];       /* FOV/对焦标定备注 */
    char operator_name[32];   /* 操作员 */
    int  max_person;          /* 人员代号上限，默认 5 */
    char data_root[256];      /* 数据根目录，默认 /mnt/data */
} col_config_t;

/* 从 data_root/site_info.txt 读 key=value；缺失项用默认值。返回 0 成功。 */
int  col_config_load(col_config_t *cfg, const char *data_root);
/* 写会话全局元数据（开始/停止时各调一次，end_epoch<0 表示未结束） */
int  col_config_write_session_meta(const col_config_t *cfg,
                                   const char *session_dir,
                                   long start_epoch, long end_epoch);

/* ---- storage ---- */
typedef struct col_storage col_storage_t;

/* 创建 collect_YYYYmmdd_HHMM 会话目录 + sessions.csv（新文件写表头）。
 * 失败返回 NULL（ perror 风格日志走 stderr）。 */
col_storage_t *col_storage_open(const char *data_root, const col_config_t *cfg);
const char    *col_storage_session_dir(const col_storage_t *st);
const char    *col_storage_session_name(const col_storage_t *st);

/* 落盘一帧：按参数建目录 → img_%06d.jpg → 追加 CSV 一行并 fflush。
 * timestamp_ms 由调用方提供（epoch 毫秒）。返回 0 成功。
 * 帧序号按"档"独立递增；重启后同档续采自动从现有文件数接续。 */
int col_storage_save_frame(col_storage_t *st, const col_params_t *p,
                           const void *jpeg, size_t len, int64_t timestamp_ms);

/* 写该档 manifest.json（帧数/参数/起止 epoch 秒），并 fsync CSV。返回 0 成功。 */
int col_storage_write_manifest(col_storage_t *st, const col_params_t *p,
                               long start_epoch, long end_epoch);

/* 该档已有帧数（含历史落盘） */
int col_storage_slot_frames(const col_storage_t *st, const col_params_t *p);
/* 该档目录是否已存在（用于"已有数据"提示） */
bool col_storage_slot_exists(const col_storage_t *st, const col_params_t *p);

/* 数据分区剩余 MB（statvfs），失败返回 -1 */
long col_storage_free_mb(const col_storage_t *st);

/* 会话累计落盘帧数 */
int col_storage_total_frames(const col_storage_t *st);

/* flush + fsync + fclose + sync()，释放。返回 0 成功。 */
int col_storage_close(col_storage_t *st);

/* ---- state（纯逻辑状态机） ---- */
typedef struct {
    col_state_t state;
    col_params_t params;
    int  capture_interval_ms;   /* 默认 500（2fps） */
    int  low_space_mb;          /* 默认 500，低于自动暂停 */
    int64_t last_capture_ms;    /* 上次落盘时刻（单调钟） */
    long slot_start_epoch;      /* 当前档开始（epoch 秒） */
    int  slot_frames;           /* 当前档本次会话已采帧数 */
    bool low_space_paused;      /* 因低空间自动暂停（UI 提示用） */
} col_fsm_t;

void col_fsm_init(col_fsm_t *f, int interval_ms, int low_space_mb);
/* 状态迁移；非法迁移返回 -1（state 不变） */
int  col_fsm_start(col_fsm_t *f, long now_epoch);        /* IDLE/READY → RECORDING */
int  col_fsm_pause(col_fsm_t *f);                        /* RECORDING → PAUSED */
int  col_fsm_resume(col_fsm_t *f, long now_epoch);       /* PAUSED → RECORDING */
int  col_fsm_slot_done(col_fsm_t *f);                    /* RECORDING/PAUSED → READY */
int  col_fsm_stop(col_fsm_t *f);                         /* 任意 → IDLE */

/* 每 tick 调用：now_ms 单调毫秒，free_mb 剩余空间。
 * 返回 true 表示本 tick 应抓帧落盘；低空间时自动 PAUSED 并置 low_space_paused。 */
bool col_fsm_tick(col_fsm_t *f, int64_t now_ms, long free_mb);
/* 抓帧成功后调用，推进节流与计数 */
void col_fsm_frame_done(col_fsm_t *f, int64_t now_ms);

/* ---- ui_model（纯逻辑，坐标归一化 0..1000） ---- */
typedef enum {
    COL_BTN_NONE = 0,
    COL_BTN_PERSON, COL_BTN_TASK, COL_BTN_DIST, COL_BTN_POS, COL_BTN_LIGHT,
    COL_BTN_START, COL_BTN_SLOT_DONE, COL_BTN_STOP
} col_btn_kind_t;

typedef struct {
    col_btn_kind_t kind;
    int index;        /* PERSON:1..N TASK/DIST/LIGHT:下标 POS:h*2+v */
    int x0, y0, x1, y1;   /* 归一化 0..1000 */
    char label[24];
} col_button_t;

#define COL_UI_MAX_BUTTONS 48

typedef struct {
    col_button_t btn[COL_UI_MAX_BUTTONS];
    int nbtn;
    /* 触摸原始坐标 → 归一化映射（GT911 校准参数，#2 实测后填） */
    int touch_x_min, touch_x_max, touch_y_min, touch_y_max;
    bool touch_swap_xy;
} col_ui_t;

/* 按当前参数重建按钮表（选中项由 ui 渲染层比对 params 高亮） */
void col_ui_build(col_ui_t *ui, const col_params_t *p, int max_person);
/* 归一化坐标命中；未命中返回 kind=COL_BTN_NONE */
col_button_t col_ui_hit(const col_ui_t *ui, int nx, int ny);
/* 原始触摸坐标 → 归一化（含校准映射） */
void col_ui_map_touch(const col_ui_t *ui, int raw_x, int raw_y, int *nx, int *ny);
/* 状态栏文本，如 "REC P1 D1.0m mid_high L1 HAND | 128帧 | 6200MB" */
void col_ui_status_line(const col_fsm_t *f, const col_config_t *cfg,
                        int total_frames, long free_mb, char *out, size_t outsz);

/* ---- 平台抽象（main.c 依赖；platform_cvitek.c / tests 用 null 实现） ---- */
typedef struct {
    /* 初始化摄像头/屏/触摸；返回 0 成功 */
    int  (*init)(void *ctx, const col_config_t *cfg);
    /* 抓一帧 JPEG（1080p q>=90）。buf 由平台持有，下次调用前有效。返回 0 成功。 */
    int  (*capture_jpeg)(void *ctx, const uint8_t **buf, size_t *len);
    /* 轮询触摸/按键事件（归一化坐标 nx,ny；抬起触发）。timeout_ms 内无事件返回 0，
     * 有事件返回 1 并填 *nx,*ny；出错返回 -1。 */
    int  (*poll_touch)(void *ctx, int timeout_ms, int *nx, int *ny);
    /* 刷新屏上状态栏/预览（可选实现，NULL 则忽略） */
    void (*render)(void *ctx, const col_fsm_t *f, const col_ui_t *ui,
                   const char *status_line);
    void (*deinit)(void *ctx);
} col_platform_t;

int64_t col_now_ms(void);      /* CLOCK_MONOTONIC 毫秒 */
long    col_now_epoch(void);   /* epoch 秒 */

#ifdef __cplusplus
}
#endif
#endif /* COLLECTOR_H */