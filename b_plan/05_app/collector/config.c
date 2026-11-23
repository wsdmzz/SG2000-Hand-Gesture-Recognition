/*
 * config.c — site_info.txt / board_id / session_meta.json
 * 纯 POSIX，无 SDK 依赖。
 */
#include "collector.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

int64_t col_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long col_now_epoch(void)
{
    return (long)time(NULL);
}

const int col_dist_dm[COL_DIST_NUM] = {3, 5, 10, 15, 20, 30, 40};
const char *col_task_name[COL_TASK_NUM] = {"hand", "face", "eye", "body", "negative"};
const char *col_posh_name[COL_POSH_NUM] = {"left", "mid", "right"};
const char *col_posv_name[COL_POSV_NUM] = {"high", "low"};

bool col_params_valid(const col_params_t *p, int max_person)
{
    if (max_person <= 0 || max_person > COL_MAX_PERSON)
        max_person = COL_MAX_PERSON;
    return p->person >= 1 && p->person <= max_person &&
           (int)p->task >= 0 && p->task < COL_TASK_NUM &&
           p->dist_idx >= 0 && p->dist_idx < COL_DIST_NUM &&
           p->pos_h >= 0 && p->pos_h < COL_POSH_NUM &&
           p->pos_v >= 0 && p->pos_v < COL_POSV_NUM &&
           p->light >= 0 && p->light < COL_LIGHT_NUM;
}

bool col_params_same_slot(const col_params_t *a, const col_params_t *b)
{
    return a->person == b->person && a->task == b->task &&
           a->dist_idx == b->dist_idx && a->light == b->light &&
           /* pos 仅 hand 参与槽位键 */
           (a->task != COL_TASK_HAND ||
            (a->pos_h == b->pos_h && a->pos_v == b->pos_v));
}

/* 去掉首尾空白 */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static void set_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

int col_config_load(col_config_t *cfg, const char *data_root)
{
    memset(cfg, 0, sizeof(*cfg));
    set_str(cfg->board_id, sizeof(cfg->board_id), "B01");
    set_str(cfg->site, sizeof(cfg->site), "unset");
    set_str(cfg->height_cm, sizeof(cfg->height_cm), "unset");
    set_str(cfg->fov_note, sizeof(cfg->fov_note), "unset");
    set_str(cfg->operator_name, sizeof(cfg->operator_name), "unset");
    cfg->max_person = 5;
    set_str(cfg->data_root, sizeof(cfg->data_root),
            data_root && *data_root ? data_root : "/mnt/data");

    char path[512];
    snprintf(path, sizeof(path), "%s/site_info.txt", cfg->data_root);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;   /* 无 site_info 用默认值，不算错误（首次部署） */

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if      (!strcmp(k, "board_id"))  set_str(cfg->board_id, sizeof(cfg->board_id), v);
        else if (!strcmp(k, "site"))      set_str(cfg->site, sizeof(cfg->site), v);
        else if (!strcmp(k, "height_cm")) set_str(cfg->height_cm, sizeof(cfg->height_cm), v);
        else if (!strcmp(k, "fov_note"))  set_str(cfg->fov_note, sizeof(cfg->fov_note), v);
        else if (!strcmp(k, "operator"))  set_str(cfg->operator_name, sizeof(cfg->operator_name), v);
        else if (!strcmp(k, "max_person")) {
            int n = atoi(v);
            if (n >= 1 && n <= COL_MAX_PERSON) cfg->max_person = n;
        }
    }
    fclose(f);
    return 0;
}

int col_config_write_session_meta(const col_config_t *cfg,
                                  const char *session_dir,
                                  long start_epoch, long end_epoch)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/session_meta.json", session_dir);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
            "{\n"
            "  \"board_id\": \"%s\",\n"
            "  \"site\": \"%s\",\n"
            "  \"height_cm\": \"%s\",\n"
            "  \"fov_note\": \"%s\",\n"
            "  \"operator\": \"%s\",\n"
            "  \"max_person\": %d,\n"
            "  \"start_epoch\": %ld,\n"
            "  \"end_epoch\": %ld,\n"
            "  \"collector_version\": \"2.0-local\"\n"
            "}\n",
            cfg->board_id, cfg->site, cfg->height_cm, cfg->fov_note,
            cfg->operator_name, cfg->max_person,
            start_epoch, end_epoch);
    int rc = fflush(f);
    if (rc == 0) rc = fsync(fileno(f));
    fclose(f);
    return rc ? -1 : 0;
}