/*
 * storage.c — 分类目录 / JPEG 落盘 / sessions.csv / manifest / 剩余空间
 *
 * 契约（数据采集方案 §7.1，2026-11-20 实测锁定）：
 *   collect_YYYYmmdd_HHMM/P{n}/hand/D{dm}/pos_{h}_{v}/L{x}/img_%06d.jpg
 *   collect_YYYYmmdd_HHMM/P{n}/{face|eye|body}/D{dm}/L{x}/img_%06d.jpg
 *   collect_YYYYmmdd_HHMM/negative/...
 *   sessions.csv: file,person,task,distance_m,pos_h,pos_v,light,frame_seq,timestamp_ms,board_id
 *
 * 旧版 bug 修复：CSV 每行 fflush（进程被杀不丢行）；
 *               manifest/关闭时 fsync（掉电不丢）；每 50 帧 fsync 一次 CSV。
 */
#include "collector.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define COL_MAX_SLOTS 128
#define COL_CSV_FSYNC_EVERY 50

typedef struct {
    col_params_t p;
    char dir[384];      /* 相对会话根的档目录 */
    int  frames;        /* 已落盘帧数（含历史） */
    bool used;
} slot_t;

struct col_storage {
    char root[256];         /* 数据根 /mnt/data */
    char session_dir[384];  /* 绝对路径 */
    char session_name[64];  /* collect_YYYYmmdd_HHMM */
    char board_id[16];
    FILE *csv;
    int  csv_fd;
    int  rows_since_fsync;
    int  total_frames;
    slot_t slots[COL_MAX_SLOTS];
};

/* 递归建目录（path 会被临时改写） */
static int mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* 档相对目录：P1/hand/D10/pos_mid_high/L1 */
static void slot_rel_dir(const col_params_t *p, char *out, size_t cap)
{
    if (p->task == COL_TASK_NEGATIVE) {
        snprintf(out, cap, "negative");
        return;
    }
    if (p->task == COL_TASK_HAND) {
        snprintf(out, cap, "P%d/%s/D%02d/pos_%s_%s/L%d",
                 p->person, col_task_name[p->task], col_dist_dm[p->dist_idx],
                 col_posh_name[p->pos_h], col_posv_name[p->pos_v], p->light + 1);
    } else {
        snprintf(out, cap, "P%d/%s/D%02d/L%d",
                 p->person, col_task_name[p->task], col_dist_dm[p->dist_idx],
                 p->light + 1);
    }
}

/* 数目录里已有 img_*.jpg（续采接续帧序） */
static int count_existing(const char *abs_dir)
{
    DIR *d = opendir(abs_dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "img_", 4) == 0 &&
            strstr(e->d_name, ".jpg"))
            n++;
    }
    closedir(d);
    return n;
}

static slot_t *slot_get(col_storage_t *st, const col_params_t *p)
{
    for (int i = 0; i < COL_MAX_SLOTS; i++) {
        if (st->slots[i].used && col_params_same_slot(&st->slots[i].p, p))
            return &st->slots[i];
    }
    for (int i = 0; i < COL_MAX_SLOTS; i++) {
        if (!st->slots[i].used) {
            slot_t *s = &st->slots[i];
            s->used = true;
            s->p = *p;
            slot_rel_dir(p, s->dir, sizeof(s->dir));
            char abs[512];
            snprintf(abs, sizeof(abs), "%s/%s", st->session_dir, s->dir);
            s->frames = count_existing(abs);
            return s;
        }
    }
    return NULL;   /* 槽位表满（128 档/会话，足够） */
}

col_storage_t *col_storage_open(const char *data_root, const col_config_t *cfg)
{
    col_storage_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    snprintf(st->root, sizeof(st->root), "%s", data_root);
    snprintf(st->board_id, sizeof(st->board_id), "%s", cfg->board_id);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(st->session_name, sizeof(st->session_name),
             "collect_%04d%02d%02d_%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    snprintf(st->session_dir, sizeof(st->session_dir), "%s/%s",
             st->root, st->session_name);

    char path[512];
    snprintf(path, sizeof(path), "%s", st->session_dir);
    if (mkdirs(path) != 0) {
        fprintf(stderr, "[storage] mkdir %s: %s\n", path, strerror(errno));
        free(st);
        return NULL;
    }

    snprintf(path, sizeof(path), "%s/sessions.csv", st->session_dir);
    bool fresh = access(path, F_OK) != 0;
    st->csv = fopen(path, "a");
    if (!st->csv) {
        fprintf(stderr, "[storage] open csv: %s\n", strerror(errno));
        free(st);
        return NULL;
    }
    st->csv_fd = fileno(st->csv);
    if (fresh) {
        fprintf(st->csv,
                "file,person,task,distance_m,pos_h,pos_v,light,frame_seq,"
                "timestamp_ms,board_id\n");
        fflush(st->csv);
    }
    return st;
}

const char *col_storage_session_dir(const col_storage_t *st) { return st->session_dir; }
const char *col_storage_session_name(const col_storage_t *st) { return st->session_name; }
int col_storage_total_frames(const col_storage_t *st) { return st->total_frames; }

int col_storage_save_frame(col_storage_t *st, const col_params_t *p,
                           const void *jpeg, size_t len, int64_t timestamp_ms)
{
    slot_t *s = slot_get(st, p);
    if (!s) return -1;

    char abs[512], rel[448];
    snprintf(abs, sizeof(abs), "%s/%s", st->session_dir, s->dir);
    if (mkdirs(abs) != 0) return -1;

    int seq = s->frames + 1;
    snprintf(rel, sizeof(rel), "%s/img_%06d.jpg", s->dir, seq);
    snprintf(abs, sizeof(abs), "%s/%s", st->session_dir, rel);

    FILE *f = fopen(abs, "wb");
    if (!f) return -1;
    size_t w = fwrite(jpeg, 1, len, f);
    int rc = (w == len) ? 0 : -1;
    if (fflush(f) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(f)) != 0) rc = -1;   /* 掉电保护：每帧 fsync，2fps 开销可忽略 */
    fclose(f);
    if (rc != 0) {
        unlink(abs);
        return -1;
    }

    /* CSV 一行（file 相对会话根，不带前导 /；person=P1；board=B01；epoch 毫秒） */
    const char *ph = "-", *pv = "-";
    if (p->task == COL_TASK_HAND) {
        ph = col_posh_name[p->pos_h];
        pv = col_posv_name[p->pos_v];
    }
    fprintf(st->csv, "%s,P%d,%s,%.1f,%s,%s,L%d,%d,%lld,%s\n",
            rel, p->person, col_task_name[p->task],
            col_dist_dm[p->dist_idx] / 10.0, ph, pv, p->light + 1,
            seq, (long long)timestamp_ms, st->board_id);
    /* 关键修复：每行 fflush，进程被杀也不丢行 */
    if (fflush(st->csv) != 0) return -1;
    if (++st->rows_since_fsync >= COL_CSV_FSYNC_EVERY) {
        fsync(st->csv_fd);
        st->rows_since_fsync = 0;
    }

    s->frames = seq;
    st->total_frames++;
    return 0;
}

int col_storage_slot_frames(const col_storage_t *st, const col_params_t *p)
{
    /* 只读查询：不占槽位，直接数目录 */
    char rel[384], abs[512];
    slot_rel_dir(p, rel, sizeof(rel));
    snprintf(abs, sizeof(abs), "%s/%s", st->session_dir, rel);
    return count_existing(abs);
}

bool col_storage_slot_exists(const col_storage_t *st, const col_params_t *p)
{
    char rel[384], abs[512];
    slot_rel_dir(p, rel, sizeof(rel));
    snprintf(abs, sizeof(abs), "%s/%s", st->session_dir, rel);
    struct stat sb;
    return stat(abs, &sb) == 0 && S_ISDIR(sb.st_mode);
}

int col_storage_write_manifest(col_storage_t *st, const col_params_t *p,
                               long start_epoch, long end_epoch)
{
    slot_t *s = slot_get(st, p);
    if (!s) return -1;
    char abs[512];
    snprintf(abs, sizeof(abs), "%s/%s/manifest.json", st->session_dir, s->dir);
    FILE *f = fopen(abs, "w");
    if (!f) return -1;
    const char *ph = "-", *pv = "-";
    if (p->task == COL_TASK_HAND) {
        ph = col_posh_name[p->pos_h];
        pv = col_posv_name[p->pos_v];
    }
    fprintf(f,
            "{\n"
            "  \"person\": \"P%d\",\n"
            "  \"task\": \"%s\",\n"
            "  \"distance_m\": %.1f,\n"
            "  \"pos_h\": \"%s\",\n"
            "  \"pos_v\": \"%s\",\n"
            "  \"light\": \"L%d\",\n"
            "  \"frames\": %d,\n"
            "  \"start\": %ld,\n"
            "  \"end\": %ld,\n"
            "  \"board_id\": \"%s\"\n"
            "}\n",
            p->person, col_task_name[p->task], col_dist_dm[p->dist_idx] / 10.0,
            ph, pv, p->light + 1, s->frames, start_epoch, end_epoch,
            st->board_id);
    int rc = fflush(f);
    if (rc == 0) rc = fsync(fileno(f));
    fclose(f);
    /* 本档完成：CSV 强制落盘 */
    fflush(st->csv);
    fsync(st->csv_fd);
    st->rows_since_fsync = 0;
    return rc ? -1 : 0;
}

long col_storage_free_mb(const col_storage_t *st)
{
    struct statvfs vfs;
    if (statvfs(st->root, &vfs) != 0) return -1;
    return (long)((uint64_t)vfs.f_bavail * vfs.f_frsize / (1024 * 1024));
}

int col_storage_close(col_storage_t *st)
{
    if (!st) return 0;
    int rc = 0;
    if (st->csv) {
        if (fflush(st->csv) != 0) rc = -1;
        if (fsync(st->csv_fd) != 0) rc = -1;
        fclose(st->csv);
    }
    sync();
    free(st);
    return rc;
}