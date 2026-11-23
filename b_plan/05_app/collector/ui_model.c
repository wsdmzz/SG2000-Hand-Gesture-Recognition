/*
 * ui_model.c — 按钮布局 + 触摸命中 + 状态文本（纯逻辑）
 *
 * 坐标归一化 0..1000（x,y 同），与分辨率解耦：
 *   VO 实际 720×1280（硬限）/ 触摸 GT911 原始量程由校准参数映射（#2 实测）。
 * 布局对应设计文档 §3（竖屏）：状态栏 / 预览 / 人员 / 任务 / 距离 / 位置 / 光照 / 操作。
 */
#include "collector.h"
#include <string.h>
#include <stdio.h>

static void add_btn(col_ui_t *ui, col_btn_kind_t kind, int index,
                    int x0, int y0, int x1, int y1, const char *label)
{
    if (ui->nbtn >= COL_UI_MAX_BUTTONS) return;
    col_button_t *b = &ui->btn[ui->nbtn++];
    b->kind = kind;
    b->index = index;
    b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1;
    snprintf(b->label, sizeof(b->label), "%s", label);
}

/* 一行 n 个等宽按钮（左右各留 10 边距，按钮间 8 间隙） */
static void add_row(col_ui_t *ui, col_btn_kind_t kind, int y0, int y1,
                    int n, const char *const *labels, int index_base)
{
    int margin = 10, gap = 8;
    int w = (1000 - 2 * margin - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++) {
        int x0 = margin + i * (w + gap);
        add_btn(ui, kind, index_base + i, x0, y0, x0 + w, y1, labels[i]);
    }
}

void col_ui_build(col_ui_t *ui, const col_params_t *p, int max_person)
{
    memset(ui, 0, sizeof(*ui));
    /* 触摸校准默认值：GT911 量程暂按屏物理分辨率 800×1280，#2 实测后修正 */
    ui->touch_x_min = 0;   ui->touch_x_max = 800;
    ui->touch_y_min = 0;   ui->touch_y_max = 1280;
    ui->touch_swap_xy = false;

    if (max_person <= 0 || max_person > COL_MAX_PERSON)
        max_person = COL_MAX_PERSON;
    (void)p;   /* 选中高亮由渲染层比对 params，这里只建几何 */

    /* 人员行 */
    char plabel[COL_MAX_PERSON][8];
    const char *pl[COL_MAX_PERSON];
    for (int i = 0; i < max_person; i++) {
        snprintf(plabel[i], sizeof(plabel[i]), "P%d", i + 1);
        pl[i] = plabel[i];
    }
    add_row(ui, COL_BTN_PERSON, 440, 520, max_person, pl, 1);

    /* 任务行 */
    const char *tl[] = {"HAND", "FACE", "EYE", "BODY", "NEG"};
    add_row(ui, COL_BTN_TASK, 530, 610, COL_TASK_NUM, tl, 0);

    /* 距离行 */
    char dlabel[COL_DIST_NUM][8];
    const char *dl[COL_DIST_NUM];
    for (int i = 0; i < COL_DIST_NUM; i++) {
        snprintf(dlabel[i], sizeof(dlabel[i]), "%.1f", col_dist_dm[i] / 10.0);
        dl[i] = dlabel[i];
    }
    add_row(ui, COL_BTN_DIST, 620, 700, COL_DIST_NUM, dl, 0);

    /* 位置行（左高 左低 中高 中低 右高 右低） */
    const char *pol[] = {"左高", "左低", "中高", "中低", "右高", "右低"};
    add_row(ui, COL_BTN_POS, 710, 790, COL_POSH_NUM * COL_POSV_NUM, pol, 0);

    /* 光照行 */
    const char *ll[] = {"L1", "L2", "L3"};
    add_row(ui, COL_BTN_LIGHT, 800, 880, COL_LIGHT_NUM, ll, 0);

    /* 操作行 */
    const char *al[] = {"开始/暂停", "本档完成", "停止会话"};
    int margin = 10, gap = 8;
    int w = (1000 - 2 * margin - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        int x0 = margin + i * (w + gap);
        col_btn_kind_t k = (i == 0) ? COL_BTN_START :
                           (i == 1) ? COL_BTN_SLOT_DONE : COL_BTN_STOP;
        add_btn(ui, k, 0, x0, 900, x0 + w, 990, al[i]);
    }
}

col_button_t col_ui_hit(const col_ui_t *ui, int nx, int ny)
{
    col_button_t none;
    memset(&none, 0, sizeof(none));
    for (int i = 0; i < ui->nbtn; i++) {
        const col_button_t *b = &ui->btn[i];
        if (nx >= b->x0 && nx < b->x1 && ny >= b->y0 && ny < b->y1)
            return *b;
    }
    return none;
}

void col_ui_map_touch(const col_ui_t *ui, int raw_x, int raw_y, int *nx, int *ny)
{
    int x = raw_x, y = raw_y;
    if (ui->touch_swap_xy) { int t = x; x = y; y = t; }
    int xr = ui->touch_x_max - ui->touch_x_min;
    int yr = ui->touch_y_max - ui->touch_y_min;
    if (xr <= 0) xr = 1;
    if (yr <= 0) yr = 1;
    int vx = (x - ui->touch_x_min) * 1000 / xr;
    int vy = (y - ui->touch_y_min) * 1000 / yr;
    if (vx < 0) vx = 0; if (vx > 1000) vx = 1000;
    if (vy < 0) vy = 0; if (vy > 1000) vy = 1000;
    *nx = vx; *ny = vy;
}

void col_ui_status_line(const col_fsm_t *f, const col_config_t *cfg,
                        int total_frames, long free_mb, char *out, size_t outsz)
{
    const char *st = "IDLE";
    switch (f->state) {
    case COL_ST_IDLE:      st = "IDLE"; break;
    case COL_ST_READY:     st = "READY"; break;
    case COL_ST_RECORDING: st = "REC"; break;
    case COL_ST_PAUSED:    st = f->low_space_paused ? "LOW-SPACE!" : "PAUSE"; break;
    }
    const col_params_t *p = &f->params;
    char pos[32];
    if (p->task == COL_TASK_HAND)
        snprintf(pos, sizeof(pos), "%s_%s", col_posh_name[p->pos_h],
                 col_posv_name[p->pos_v]);
    else
        snprintf(pos, sizeof(pos), "-");
    snprintf(out, outsz, "%s %s P%d D%.1fm %s L%d %s | 档%d 总%d | %ldMB %s",
             st, cfg->board_id, p->person, col_dist_dm[p->dist_idx] / 10.0,
             pos, p->light + 1, col_task_name[p->task],
             f->slot_frames, total_frames, free_mb,
             f->low_space_paused ? "空间不足已暂停" : "");
}