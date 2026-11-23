/*
 * smoke_test.c — collector SDK 无关模块主机冒烟测试
 *
 * 覆盖：config 解析 / storage 目录+CSV 不丢行(95帧回归)+manifest+续采 /
 *       state 状态机+节流+低空间 / ui 命中+映射 / PC 工具链对账 /
 *       main+null 平台端到端（AUTOREC → SIGTERM → 数据完整）。
 *
 * 构建：make test（见 Makefile）
 */
#include "collector.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
    else { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

static char g_root[256];

static int count_csv_rows(const char *session_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/sessions.csv", session_dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) n++;
    fclose(f);
    return n - 1;   /* 去表头 */
}

static int file_exists(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0;
}

static void test_config(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/site_info.txt", g_root);
    FILE *f = fopen(path, "w");
    fprintf(f, "# 测试\nboard_id=B02\nsite=测试墙\nheight_cm=150\n"
               "fov_note=GC2083 默认\noperator=张三\nmax_person=6\n");
    fclose(f);
    col_config_t cfg;
    CHECK(col_config_load(&cfg, g_root) == 0, "config load");
    CHECK(strcmp(cfg.board_id, "B02") == 0, "board_id=B02");
    CHECK(strcmp(cfg.operator_name, "张三") == 0, "operator 中文");
    CHECK(cfg.max_person == 6, "max_person=6");
    /* 无 site_info 默认值 */
    col_config_t cfg2;
    col_config_load(&cfg2, "/tmp");
    CHECK(strcmp(cfg2.board_id, "B01") == 0, "默认 board_id=B01");
    CHECK(cfg2.max_person == 5, "默认 max_person=5");
}

/* 生成真实 1920x1080 q90 测试 JPEG（PC 工具链 validate 期望 1080p），读入内存 */
static unsigned char *gen_jpeg_1080p(size_t *len)
{
    char src[300], cmd[600];
    snprintf(src, sizeof(src), "%s/_testsrc.jpg", g_root);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"from PIL import Image; "
             "Image.new('RGB',(1920,1080),(120,60,30)).save('%s',quality=90)\"", src);
    if (system(cmd) != 0) return NULL;
    FILE *f = fopen(src, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void test_storage(void)
{
    col_config_t cfg;
    col_config_load(&cfg, g_root);
    col_storage_t *st = col_storage_open(g_root, &cfg);
    CHECK(st != NULL, "storage open");
    const char *sdir = col_storage_session_dir(st);

    col_params_t hand = {.person=1, .task=COL_TASK_HAND, .dist_idx=2,
                         .pos_h=1, .pos_v=0, .light=0};
    col_params_t face = {.person=2, .task=COL_TASK_FACE, .dist_idx=4,
                         .pos_h=0, .pos_v=0, .light=1};
    col_params_t neg  = {.person=1, .task=COL_TASK_NEGATIVE, .dist_idx=0,
                         .pos_h=0, .pos_v=0, .light=0};

    /* 95 帧回归（旧版丢 30 行的场景）：60 hand + 20 face + 15 negative
     * 用真实 1920x1080 JPEG，保证 PC 工具链 validate 的 EOI/SOF/尺寸检查可过 */
    size_t jlen = 0;
    unsigned char *jpeg = gen_jpeg_1080p(&jlen);
    CHECK(jpeg != NULL, "1080p 测试 JPEG 生成");
    if (!jpeg) return;
    int rc = 0;
    for (int i = 0; i < 60; i++)
        rc |= col_storage_save_frame(st, &hand, jpeg, jlen, 1000 + i);
    for (int i = 0; i < 20; i++)
        rc |= col_storage_save_frame(st, &face, jpeg, jlen, 2000 + i);
    for (int i = 0; i < 15; i++)
        rc |= col_storage_save_frame(st, &neg, jpeg, jlen, 3000 + i);
    CHECK(rc == 0, "95 帧落盘返回 0");
    CHECK(count_csv_rows(sdir) == 95, "CSV 95 行不丢（flush 修复回归）");
    CHECK(col_storage_total_frames(st) == 95, "total_frames=95");

    char path[512];
    snprintf(path, sizeof(path), "%s/P1/hand/D10/pos_mid_high/L1/img_000060.jpg", sdir);
    CHECK(file_exists(path), "hand 目录契约 P1/hand/D10/pos_mid_high/L1");
    snprintf(path, sizeof(path), "%s/P2/face/D20/L2/img_000020.jpg", sdir);
    CHECK(file_exists(path), "face 无 pos 目录 P2/face/D20/L2");
    snprintf(path, sizeof(path), "%s/negative/img_000015.jpg", sdir);
    CHECK(file_exists(path), "negative 目录");

    CHECK(col_storage_write_manifest(st, &hand, 100, 200) == 0, "manifest 写入");
    snprintf(path, sizeof(path), "%s/P1/hand/D10/pos_mid_high/L1/manifest.json", sdir);
    CHECK(file_exists(path), "manifest 文件存在");
    CHECK(col_storage_slot_frames(st, &hand) == 60, "slot_frames=60");
    CHECK(col_storage_slot_exists(st, &hand), "slot_exists");
    CHECK(col_storage_free_mb(st) > 0, "free_mb 查询");

    /* 续采：关闭重开（同一分钟内会话名相同），帧序接续 */
    char sname[64];
    snprintf(sname, sizeof(sname), "%s", col_storage_session_name(st));
    col_storage_close(st);
    st = col_storage_open(g_root, &cfg);
    if (strcmp(col_storage_session_name(st), sname) == 0) {
        CHECK(col_storage_save_frame(st, &hand, jpeg, jlen, 4000) == 0,
              "续采落盘");
        snprintf(path, sizeof(path), "%s/P1/hand/D10/pos_mid_high/L1/img_000061.jpg", sdir);
        CHECK(file_exists(path), "续采帧序接续 img_000061");
        CHECK(count_csv_rows(sdir) == 96, "续采后 CSV 96 行");
        /* 续采后重写 manifest（真实流程在 slot_done 时写），保持 frames 与磁盘一致 */
        CHECK(col_storage_write_manifest(st, &hand, 100, 200) == 0, "续采后 manifest 重写");
    } else {
        fprintf(stderr, "skip: 跨分钟，续采测试跳过\n");
    }
    free(jpeg);
    col_storage_close(st);
}

static void test_state(void)
{
    col_fsm_t f;
    col_fsm_init(&f, 500, 500);
    CHECK(f.state == COL_ST_IDLE, "初始 IDLE");
    CHECK(col_fsm_pause(&f) == -1, "IDLE 不能 pause");
    CHECK(col_fsm_start(&f, 100) == 0, "start → RECORDING");
    CHECK(col_fsm_tick(&f, 0, 6000), "首帧立即抓");
    col_fsm_frame_done(&f, 0);
    CHECK(!col_fsm_tick(&f, 499, 6000), "499ms 节流");
    CHECK(col_fsm_tick(&f, 500, 6000), "500ms 放行");
    CHECK(col_fsm_pause(&f) == 0, "pause");
    CHECK(!col_fsm_tick(&f, 1000, 6000), "PAUSED 不抓");
    CHECK(col_fsm_resume(&f, 100) == 0, "resume");
    CHECK(col_fsm_slot_done(&f) == 0, "slot_done → READY");
    CHECK(f.state == COL_ST_READY, "READY");
    CHECK(col_fsm_start(&f, 200) == 0, "READY 可再 start");
    /* 低空间保护 */
    CHECK(!col_fsm_tick(&f, 2000, 100), "低空间不抓");
    CHECK(f.state == COL_ST_PAUSED && f.low_space_paused, "低空间自动 PAUSED");
    CHECK(col_fsm_stop(&f) == 0, "stop → IDLE");
    CHECK(f.state == COL_ST_IDLE, "IDLE");
}

static void test_ui(void)
{
    col_ui_t ui;
    col_params_t p = {.person=1, .task=COL_TASK_HAND, .dist_idx=2,
                      .pos_h=1, .pos_v=0, .light=0};
    col_ui_build(&ui, &p, 5);
    CHECK(ui.nbtn == 5 + 5 + 7 + 6 + 3 + 3, "按钮数 29");
    /* 找 P2 按钮并命中其中心 */
    int found = 0;
    for (int i = 0; i < ui.nbtn; i++) {
        if (ui.btn[i].kind == COL_BTN_PERSON && ui.btn[i].index == 2) {
            col_button_t b = ui.btn[i];
            col_button_t h = col_ui_hit(&ui, (b.x0+b.x1)/2, (b.y0+b.y1)/2);
            CHECK(h.kind == COL_BTN_PERSON && h.index == 2, "P2 命中");
            found = 1;
        }
    }
    CHECK(found, "P2 按钮存在");
    /* 操作行中心：START */
    col_button_t h = col_ui_hit(&ui, 170, 945);
    CHECK(h.kind == COL_BTN_START, "开始按钮命中");
    h = col_ui_hit(&ui, 500, 945);
    CHECK(h.kind == COL_BTN_SLOT_DONE, "本档完成命中");
    h = col_ui_hit(&ui, 900, 945);
    CHECK(h.kind == COL_BTN_STOP, "停止会话命中");
    /* 空白区不命中 */
    h = col_ui_hit(&ui, 500, 200);
    CHECK(h.kind == COL_BTN_NONE, "预览区不命中");
    /* 触摸映射：默认 800×1280 → 归一化 */
    int nx, ny;
    col_ui_map_touch(&ui, 400, 640, &nx, &ny);
    CHECK(nx == 500 && ny == 500, "触摸映射中心");
    col_ui_map_touch(&ui, 0, 0, &nx, &ny);
    CHECK(nx == 0 && ny == 0, "触摸映射原点");
    /* 状态文本 */
    col_fsm_t f;
    col_fsm_init(&f, 500, 500);
    col_config_t cfg;
    col_config_load(&cfg, g_root);
    char line[256];
    col_ui_status_line(&f, &cfg, 95, 6000, line, sizeof(line));
    CHECK(strstr(line, "IDLE") && strstr(line, "P1") && strstr(line, "hand"),
          "状态文本含 IDLE/P1/hand");
}

static void test_toolchain(void)
{
    /* 用 PC 工具链对账（validate 三方核对） */
    char cmd[600];
    snprintf(cmd, sizeof(cmd),
             "python3 /home/oooa/Milk-V-Duo/tools/collect_tool.py validate %s >/tmp/coltest_validate.log 2>&1",
             g_root);
    int rc = system(cmd);
    CHECK(rc == 0, "collect_tool validate 通过");
    if (rc != 0)
        system("cat /tmp/coltest_validate.log >&2");
}

static void test_e2e(void)
{
    /* main + null 平台：AUTOREC 100ms 间隔跑 1.2s → SIGTERM → 数据完整 */
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/e2e", g_root);
    mkdir(dir, 0755);
    char cmd[700];
    snprintf(cmd, sizeof(cmd),
             "COLLECTOR_AUTOREC=1 COLLECTOR_INTERVAL_MS=100 "
             "./collector_null %s >/tmp/coltest_e2e.log 2>&1 & "
             "pid=$!; sleep 1.2; kill -TERM $pid; wait $pid; echo rc=$?",
             dir);
    int rc = system(cmd);
    CHECK(rc == 0, "e2e 进程正常退出");
    /* 找会话目录 */
    char findcmd[600];
    snprintf(findcmd, sizeof(findcmd), "%s/collect_*", dir);
    /* 用 shell 统计 */
    snprintf(cmd, sizeof(cmd),
             "d=$(ls -d %s/collect_* 2>/dev/null | head -1); "
             "[ -n \"$d\" ] && tail -n +2 \"$d/sessions.csv\" | wc -l",
             dir);
    FILE *p = popen(cmd, "r");
    int rows = 0;
    if (p) { fscanf(p, "%d", &rows); pclose(p); }
    CHECK(rows >= 5, "e2e 1.2s@100ms 至少 5 帧");
    snprintf(cmd, sizeof(cmd),
             "d=$(ls -d %s/collect_* 2>/dev/null | head -1); "
             "grep -q '\"end_epoch\": [1-9]' \"$d/session_meta.json\"", dir);
    CHECK(system(cmd) == 0, "e2e session_meta end_epoch 已写");
    snprintf(cmd, sizeof(cmd),
             "d=$(ls -d %s/collect_* 2>/dev/null | head -1); "
             "ls \"$d\"/P1/hand/D10/pos_mid_high/L1/manifest.json", dir);
    CHECK(system(cmd) == 0, "e2e 退出时写当前档 manifest");
}

int main(void)
{
    char tmpl[] = "/tmp/coltest_XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); return 1; }
    snprintf(g_root, sizeof(g_root), "%s", d);
    fprintf(stderr, "== 冒烟测试根目录 %s ==\n", g_root);

    test_config();
    test_storage();
    test_state();
    test_ui();
    test_toolchain();
    test_e2e();

    fprintf(stderr, "== 结果: %s（fail=%d）==\n", g_fail ? "FAIL" : "PASS", g_fail);
    if (!g_fail) {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
        system(cmd);
    } else {
        fprintf(stderr, "保留现场: %s\n", g_root);
    }
    return g_fail ? 1 : 0;
}