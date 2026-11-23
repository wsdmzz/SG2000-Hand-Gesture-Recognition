/*
 * state.c — 状态机 + 2fps 节流 + 低空间保护（纯逻辑，可主机单测）
 *
 *   IDLE ──start──► RECORDING ◄──resume── PAUSED
 *     ▲                │  pause              ▲
 *     │                ▼                     │
 *     └──stop──── READY ◄──slot_done─────────┘
 *
 * 说明：start 从 IDLE/READY 进 RECORDING；slot_done 回 READY（连续下一档）；
 *       stop 回 IDLE（关会话由 main 处理）。
 */
#include "collector.h"
#include <string.h>

void col_fsm_init(col_fsm_t *f, int interval_ms, int low_space_mb)
{
    memset(f, 0, sizeof(*f));
    f->state = COL_ST_IDLE;
    f->last_capture_ms = INT64_MIN;
    f->capture_interval_ms = interval_ms > 0 ? interval_ms : 500;
    f->low_space_mb = low_space_mb > 0 ? low_space_mb : 500;
    /* 默认参数：P1 hand 1.0m mid_high L1 */
    f->params.person = 1;
    f->params.task = COL_TASK_HAND;
    f->params.dist_idx = 2;   /* D10 */
    f->params.pos_h = 1;
    f->params.pos_v = 0;
    f->params.light = 0;
}

int col_fsm_start(col_fsm_t *f, long now_epoch)
{
    if (f->state != COL_ST_IDLE && f->state != COL_ST_READY)
        return -1;
    f->state = COL_ST_RECORDING;
    f->slot_start_epoch = now_epoch;
    f->slot_frames = 0;
    f->last_capture_ms = INT64_MIN;   /* 立即抓第一帧（哨兵勿用 0，与时钟 0 冲突） */
    f->low_space_paused = false;
    return 0;
}

int col_fsm_pause(col_fsm_t *f)
{
    if (f->state != COL_ST_RECORDING) return -1;
    f->state = COL_ST_PAUSED;
    return 0;
}

int col_fsm_resume(col_fsm_t *f, long now_epoch)
{
    if (f->state != COL_ST_PAUSED) return -1;
    (void)now_epoch;
    f->state = COL_ST_RECORDING;
    f->last_capture_ms = INT64_MIN;
    f->low_space_paused = false;
    return 0;
}

int col_fsm_slot_done(col_fsm_t *f)
{
    if (f->state != COL_ST_RECORDING && f->state != COL_ST_PAUSED)
        return -1;
    f->state = COL_ST_READY;
    return 0;
}

int col_fsm_stop(col_fsm_t *f)
{
    f->state = COL_ST_IDLE;
    f->low_space_paused = false;
    return 0;
}

bool col_fsm_tick(col_fsm_t *f, int64_t now_ms, long free_mb)
{
    if (f->state != COL_ST_RECORDING)
        return false;
    /* 低空间保护：自动暂停并提示（free_mb<0 表示查询失败，不触发） */
    if (free_mb >= 0 && free_mb < f->low_space_mb) {
        f->state = COL_ST_PAUSED;
        f->low_space_paused = true;
        return false;
    }
    if (f->last_capture_ms != INT64_MIN &&
        now_ms - f->last_capture_ms < f->capture_interval_ms)
        return false;
    return true;
}

void col_fsm_frame_done(col_fsm_t *f, int64_t now_ms)
{
    f->last_capture_ms = now_ms;
    f->slot_frames++;
}