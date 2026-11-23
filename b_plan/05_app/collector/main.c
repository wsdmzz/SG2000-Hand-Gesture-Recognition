/*
 * main.c — 单实例锁 / 启动自检 / 主循环
 *
 * 用法: collector [data_root]          （默认 /mnt/data）
 * 环境变量:
 *   COLLECTOR_AUTOREC=1  启动即自动录制（测试用，沿用旧版习惯）
 *   COLLECTOR_INTERVAL_MS=500  抓帧间隔
 *   COLLECTOR_LOW_SPACE_MB=500 低空间保护阈值
 *
 * 平台层由 platform_cvitek.c（板上）或 tests/platform_null.c（主机冒烟）提供，
 * 通过 weak 符号 platform_get() 绑定，main 逻辑两侧共用。
 */
#include "collector.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>

/* 平台实现提供（cvitek 或 null） */
extern const col_platform_t *platform_get(void);

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int sig) { (void)sig; g_quit = 1; }

static void apply_button(col_fsm_t *f, col_storage_t *st, const col_config_t *cfg,
                         const col_button_t *b)
{
    col_params_t np = f->params;
    switch (b->kind) {
    case COL_BTN_PERSON: np.person = b->index; break;
    case COL_BTN_TASK:   np.task = (col_task_t)b->index; break;
    case COL_BTN_DIST:   np.dist_idx = b->index; break;
    case COL_BTN_POS:    np.pos_h = b->index / COL_POSV_NUM;
                         np.pos_v = b->index % COL_POSV_NUM; break;
    case COL_BTN_LIGHT:  np.light = b->index; break;
    case COL_BTN_START:
        if (f->state == COL_ST_RECORDING) {
            col_fsm_pause(f);
        } else if (f->state == COL_ST_PAUSED) {
            if (f->low_space_paused) {
                fprintf(stderr, "[main] 空间不足，拒绝恢复\n");
            } else {
                col_fsm_resume(f, col_now_epoch());
            }
        } else {
            /* IDLE/READY：同档已有数据提示（仍允许续采） */
            if (col_storage_slot_exists(st, &np))
                fprintf(stderr, "[main] 提示: 该档已有数据，将续采\n");
            col_fsm_start(f, col_now_epoch());
        }
        break;
    case COL_BTN_SLOT_DONE:
        if (f->state == COL_ST_RECORDING || f->state == COL_ST_PAUSED) {
            col_storage_write_manifest(st, &f->params,
                                       f->slot_start_epoch, col_now_epoch());
            fprintf(stderr, "[main] 本档完成: %d 帧\n", f->slot_frames);
            col_fsm_slot_done(f);
        }
        break;
    case COL_BTN_STOP:
        if (f->state == COL_ST_RECORDING || f->state == COL_ST_PAUSED)
            col_storage_write_manifest(st, &f->params,
                                       f->slot_start_epoch, col_now_epoch());
        col_fsm_stop(f);
        break;
    default: break;
    }
    /* 录制中不允许换档（避免帧串档）；非录制态自由切换 */
    if (f->state != COL_ST_RECORDING && col_params_valid(&np, cfg->max_person))
        f->params = np;
}

int main(int argc, char **argv)
{
    const char *data_root = argc > 1 ? argv[1] : "/mnt/data";

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* 单实例锁（锁文件放数据根，掉电自动释放） */
    char lockpath[512];
    snprintf(lockpath, sizeof(lockpath), "%s/.collector.lock", data_root);
    int lockfd = open(lockpath, O_CREAT | O_RDWR, 0644);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[main] 已有 collector 实例在运行（或锁不可用），退出\n");
        return 1;
    }

    col_config_t cfg;
    col_config_load(&cfg, data_root);
    fprintf(stderr, "[main] board=%s site=%s root=%s\n",
            cfg.board_id, cfg.site, cfg.data_root);

    /* 启动自检 1：数据根可写 */
    if (access(cfg.data_root, W_OK) != 0) {
        fprintf(stderr, "[main] 自检失败: %s 不可写: %s\n",
                cfg.data_root, strerror(errno));
        return 1;
    }

    col_storage_t *st = col_storage_open(cfg.data_root, &cfg);
    if (!st) return 1;
    col_config_write_session_meta(&cfg, col_storage_session_dir(st),
                                  col_now_epoch(), -1);

    /* 启动自检 2：剩余空间 */
    long free_mb = col_storage_free_mb(st);
    fprintf(stderr, "[main] 会话 %s 剩余 %ldMB\n",
            col_storage_session_name(st), free_mb);
    if (free_mb >= 0 && free_mb < 500)
        fprintf(stderr, "[main] 警告: 剩余空间不足 500MB\n");

    const col_platform_t *plat = platform_get();
    if (plat->init && plat->init(NULL, &cfg) != 0) {
        fprintf(stderr, "[main] 自检失败: 平台初始化（摄像头/屏/触摸）\n");
        col_storage_close(st);
        return 1;
    }

    int interval = 500, low_mb = 500;
    const char *e;
    if ((e = getenv("COLLECTOR_INTERVAL_MS"))) interval = atoi(e);
    if ((e = getenv("COLLECTOR_LOW_SPACE_MB"))) low_mb = atoi(e);

    col_fsm_t fsm;
    col_fsm_init(&fsm, interval, low_mb);
    col_ui_t ui;
    col_ui_build(&ui, &fsm.params, cfg.max_person);

    if (getenv("COLLECTOR_AUTOREC") && atoi(getenv("COLLECTOR_AUTOREC")) == 1) {
        fprintf(stderr, "[main] AUTOREC: 自动开始录制\n");
        col_fsm_start(&fsm, col_now_epoch());
    }

    char status[256];
    int64_t last_free_check = 0;
    bool low_space_warned = false;
    while (!g_quit) {
        int64_t now = col_now_ms();

        /* 触摸事件（20ms 轮询，兼作循环节拍；有事件时 poll 立即返回）。
         * 旧 100ms 节拍把预览限到 ~10fps（验证日志 #145）。 */
        int nx, ny;
        int ev = plat->poll_touch ? plat->poll_touch(NULL, 20, &nx, &ny) : 0;
        if (ev > 0) {
            col_button_t b = col_ui_hit(&ui, nx, ny);
            if (b.kind != COL_BTN_NONE)
                apply_button(&fsm, st, &cfg, &b);
        } else if (ev < 0) {
            fprintf(stderr, "[main] 触摸读取错误，退出\n");
            break;
        }

        /* 每 2s 查一次剩余空间 */
        if (now - last_free_check > 2000) {
            free_mb = col_storage_free_mb(st);
            last_free_check = now;
        }

        /* 采集节流 */
        if (col_fsm_tick(&fsm, col_now_ms(), free_mb)) {
            const uint8_t *buf = NULL;
            size_t len = 0;
            if (plat->capture_jpeg(NULL, &buf, &len) == 0 && buf && len) {
                /* timestamp 用 epoch 毫秒（新格式；工具链兼容） */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                int64_t epoch_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
                if (col_storage_save_frame(st, &fsm.params, buf, len, epoch_ms) == 0)
                    col_fsm_frame_done(&fsm, col_now_ms());
                else
                    fprintf(stderr, "[main] 落盘失败（空间？）\n");
            }
        }
        if (fsm.low_space_paused && !low_space_warned) {
            fprintf(stderr, "[main] 低空间自动暂停: %ldMB < %dMB\n",
                    free_mb, fsm.low_space_mb);
            low_space_warned = true;
        }
        if (!fsm.low_space_paused)
            low_space_warned = false;

        /* 渲染（状态栏 + 预览 + 按钮高亮） */
        col_ui_status_line(&fsm, &cfg, col_storage_total_frames(st),
                           free_mb, status, sizeof(status));
        if (plat->render)
            plat->render(NULL, &fsm, &ui, status);
    }

    /* 收尾：当前档 manifest + 会话 meta + flush/fsync/sync */
    if (fsm.state == COL_ST_RECORDING || fsm.state == COL_ST_PAUSED)
        col_storage_write_manifest(st, &fsm.params,
                                   fsm.slot_start_epoch, col_now_epoch());
    col_config_write_session_meta(&cfg, col_storage_session_dir(st),
                                  col_now_epoch(), col_now_epoch());
    if (plat->deinit) plat->deinit(NULL);
    col_storage_close(st);
    flock(lockfd, LOCK_UN);
    close(lockfd);
    fprintf(stderr, "[main] 会话结束，数据已 sync，可安全导出\n");
    return 0;
}