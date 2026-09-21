/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "sxcl/crash.h"
#include "sxcl/fs.h"

/* 崩溃取证的夹具(全部建在**构建目录**下,仓库里不留产物):
 *   _crash_fixture/game/crash-reports/
 *       crash-2026-01-01_00.00.00-client.txt   旧的、非空(要选它)
 *       crash-2026-02-02_00.00.00-client.txt   新的、**0 字节空壳**(必须跳过)
 *       crash-2026-02-03_00.00.00-client.txt   更新的、只有空白(必须跳过)
 *       notes.md                               非 .txt(必须跳过)
 *   _crash_fixture/game/logs/latest.log        GBK 编码(中文 Windows 的真实现状)
 *   _crash_fixture/game2/logs/debug.log        没有 latest.log,只有 debug.log(兜底)
 * 断言的是"选了哪一份、什么编码、多少行、Description 是什么"这些**事实**,
 * 原因键的判定在 launch 侧的用例里(那里有 logscan)。 */

#define FIX_DIR   "_crash_fixture"
#define GAME_DIR  FIX_DIR "/game"
#define GAME2_DIR FIX_DIR "/game2"
#define GAME3_DIR FIX_DIR "/game3" /* 只有老版本的 logs/client-*.log(没有 latest.log) */

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static int write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = sxcl_fs_fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    if (len > 0u && fwrite(data, 1, len, f) != len) {
        (void)fclose(f);
        return -1;
    }
    (void)fclose(f);
    return 0;
}

static int write_text(const char *path, const char *text)
{
    return write_bytes(path, text, strlen(text));
}

/* 一份"真形状"的 crash-report:---- Minecraft Crash Report ---- / Description: / Caused by: */
static const char *kReport =
    "---- Minecraft Crash Report ----\n"
    "// Oh dear.\n"
    "\n"
    "Time: 2026-01-01 00:00:00\n"
    "Description: Rendering overlay\n"
    "\n"
    "java.lang.OutOfMemoryError: Java heap space\n"
    "\tat net.minecraft.client.renderer.LevelRenderer.render(LevelRenderer.java:123)\n"
    "\n"
    "A detailed walkthrough of the error, its code path and all known details is as follows:\n"
    "---------------------------------------------------------------------------------------\n"
    "\n"
    "-- System Details --\n"
    "Details:\n"
    "\tMinecraft Version: 1.0\n"
    "\tJava Version: 26.0.2, Oracle Corporation\n"
    "Caused by: java.lang.OutOfMemoryError: Java heap space\n"
    "\tat com.example.mod.Main.tick(Main.java:42)\n";

/* latest.log 的 GBK 正文(中文 Windows 上真实的样子):ASCII 行 + 一行 GBK 中文 */
static const unsigned char kLatestHead[] =
    "[00:00:00] [main/INFO]: Loading Minecraft 1.0\n"
    "[00:00:01] [main/WARN]: ";
/* "内存不足"(GBK) */
static const unsigned char kLatestGbk[] = {0xC4, 0xDA, 0xB4, 0xE6, 0xB2, 0xBB, 0xD7, 0xE3};
static const unsigned char kLatestTail[] = "\n[00:00:02] [main/ERROR]: java.lang.OutOfMemoryError\n";

typedef struct collected {
    char lines[8192];
    size_t len;
    int report_lines;
    int latest_lines;
    int stop_after;
} collected;

static int collect_line(void *ud, int source, const char *line)
{
    collected *c = (collected *)ud;
    const size_t n = strlen(line);
    if (source == SXCL_CRASH_SRC_REPORT) {
        ++c->report_lines;
    } else {
        ++c->latest_lines;
    }
    if (c->len + n + 2u < sizeof(c->lines)) {
        memcpy(c->lines + c->len, line, n);
        c->len += n;
        c->lines[c->len++] = '\n';
        c->lines[c->len] = '\0';
    }
    if (c->stop_after > 0 && c->report_lines + c->latest_lines >= c->stop_after) {
        return 1;
    }
    return 0;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static int setup(void)
{
    if (sxcl_fs_mkdirs(GAME_DIR "/crash-reports") != 0 ||
        sxcl_fs_mkdirs(GAME_DIR "/logs") != 0 || sxcl_fs_mkdirs(GAME2_DIR "/logs") != 0) {
        return -1;
    }
    if (write_text(GAME_DIR "/crash-reports/crash-2026-01-01_00.00.00-client.txt", kReport) != 0) {
        return -1;
    }
    /* 新的两份"空壳":一份 0 字节,一份只有空白 —— "最近"必须跳过它们 */
    if (write_bytes(GAME_DIR "/crash-reports/crash-2026-02-02_00.00.00-client.txt", "", 0) != 0) {
        return -1;
    }
    if (write_text(GAME_DIR "/crash-reports/crash-2026-02-03_00.00.00-client.txt", "  \n\t\n") != 0) {
        return -1;
    }
    if (write_text(GAME_DIR "/crash-reports/notes.md", kReport) != 0) {
        return -1;
    }
    {
        FILE *f = sxcl_fs_fopen(GAME_DIR "/logs/latest.log", "wb");
        if (f == NULL) {
            return -1;
        }
        (void)fwrite(kLatestHead, 1, sizeof(kLatestHead) - 1u, f);
        (void)fwrite(kLatestGbk, 1, sizeof(kLatestGbk), f);
        (void)fwrite(kLatestTail, 1, sizeof(kLatestTail) - 1u, f);
        (void)fclose(f);
    }
    if (write_text(GAME2_DIR "/logs/debug.log", "[00:00:00] [main/ERROR]: boom\n") != 0) {
        return -1;
    }
    /* 老版本(1.7 之前)的命名:logs/client-YYYY-MM-DD_N.log —— 没有 latest.log */
    if (sxcl_fs_mkdirs(GAME3_DIR "/logs") != 0) {
        return -1;
    }
    if (write_text(GAME3_DIR "/logs/client-2026-01-01_1.log",
                   "[00:00:00] [main/ERROR]: java.lang.OutOfMemoryError: Java heap space\n") != 0) {
        return -1;
    }
    return 0;
}

static void test_find_reports(void)
{
    sxcl_crash_file files[SXCL_CRASH_MAX_REPORTS];
    size_t total = 0;
    memset(files, 0, sizeof(files));
    total = sxcl_crash_find_reports(GAME_DIR, files, SXCL_CRASH_MAX_REPORTS);
    printf("[1] 找报告\n");
    check_int((long)total, 1, "只有 1 份非空 .txt(0 字节与纯空白被跳过,.md 不算)");
    check(total == 1 && contains(files[0].path, "crash-2026-01-01"), "选中的是那份非空报告");
    check(files[0].size > 100, "报告大小 > 100 字节");
    check_int((long)sxcl_crash_find_reports(GAME_DIR "/nope", NULL, 0), 0, "没有目录 = 0 份(不报错)");
    check_int((long)sxcl_crash_find_reports(NULL, NULL, 0), 0, "game_dir=NULL 不炸");
}

static void test_scan(void)
{
    collected c;
    sxcl_crash_evidence ev;
    long long fed = 0;
    memset(&c, 0, sizeof(c));
    memset(&ev, 0, sizeof(ev));

    printf("[2] 扫游戏目录(报告 + latest.log)\n");
    fed = sxcl_crash_scan(GAME_DIR, SXCL_CRASH_SCAN_ALL, 0, collect_line, &c, &ev, NULL, 0);
    check(fed > 0, "喂出来的行数 > 0");
    check_int((long)ev.reports_total, 1, "证据里报告总数 = 1");
    check(contains(ev.report_path, "crash-2026-01-01"), "证据里是那份非空报告的路径");
    check_str(ev.report_encoding, "ascii", "报告编码 = ascii(纯英文报告)");
    check_int((long)ev.report_lines, 18, "报告行数 = 夹具行数(18 行)");
    check_str(ev.report_description, "Rendering overlay", "Description 抽出来了");
    check(contains(ev.latest_log_path, "latest.log"), "证据里有 latest.log 路径");
    check_int((long)ev.latest_log_lines, 3, "latest.log 一共 3 行(按整个文件数)");
    check(ev.latest_log_encoding[0] != '\0', "latest.log 编码有值");
    check(strcmp(ev.latest_log_encoding, "gbk") == 0 || strcmp(ev.latest_log_encoding, "ansi") == 0,
          "latest.log 认成 gbk/ansi(不是 UTF-8)");
    check_int((long)ev.latest_log_fed, 3, "latest.log 3 行都喂进去了");
    check_int(ev.used_debug_log, 0, "latest.log 有内容时不读 debug.log");
    check(contains(c.lines, "Minecraft Crash Report"), "回调收到了报告头");
    check(contains(c.lines, "Caused by: java.lang.OutOfMemoryError"), "回调收到了 Caused by");
    check(contains(c.lines, "Minecraft Version: 1.0"), "回调收到了系统详情");
    check(contains(c.lines, "OutOfMemoryError"), "回调收到了 latest.log 的 OOM 行");
    check_int(c.report_lines + c.latest_lines, (int)fed, "回调计数与返回行数一致");
}

static void test_debug_fallback(void)
{
    collected c;
    sxcl_crash_evidence ev;
    memset(&c, 0, sizeof(c));
    memset(&ev, 0, sizeof(ev));

    printf("[3] latest.log 缺失 -> 退到 debug.log\n");
    (void)sxcl_crash_scan(GAME2_DIR, SXCL_CRASH_SCAN_ALL, 0, collect_line, &c, &ev, NULL, 0);
    check_int(ev.used_debug_log, 1, "真的读了 debug.log");
    check(contains(ev.debug_log_path, "debug.log"), "证据里点名 debug.log");
    check_int((long)ev.debug_log_lines, 1, "debug.log 1 行");
    check(contains(c.lines, "boom"), "debug.log 的内容喂进来了");
}

static void test_legacy_log(void)
{
    collected c;
    sxcl_crash_evidence ev;
    memset(&c, 0, sizeof(c));
    memset(&ev, 0, sizeof(ev));

    printf("[3b] 老版本:没有 latest.log,只有 logs/client-*.log\n");
    (void)sxcl_crash_scan(GAME3_DIR, SXCL_CRASH_SCAN_ALL, 0, collect_line, &c, &ev, NULL, 0);
    check_int(ev.used_legacy_log, 1, "退到了 client-*.log");
    check(contains(ev.legacy_log_path, "client-2026-01-01_1.log"), "证据里点名那个老式日志");
    check_int((long)ev.legacy_log_lines, 1, "老式日志 1 行");
    check(contains(c.lines, "OutOfMemoryError"), "老式日志的内容喂进来了");
}

static void test_truncation_and_errors(void)
{
    sxcl_crash_evidence ev;
    collected c;
    char path[SXCL_CRASH_PATH_MAX];
    char buf[2048];
    int truncated = 0;
    const char *enc = NULL;

    printf("[4] 边界:大文件只读尾部 / 参数错误\n");
    memset(&ev, 0, sizeof(ev));
    memset(&c, 0, sizeof(c));
    (void)sxcl_crash_scan(GAME_DIR, SXCL_CRASH_SCAN_LATEST, 40u, collect_line, &c, &ev, NULL, 0);
    check_int(ev.latest_log_truncated, 1, "只给 40 字节的窗口 -> 标记截断");
    check_int((long)ev.latest_log_lines, 3, "行数仍然是整个文件的 3 行");

    (void)snprintf(path, sizeof(path), "%s/crash-reports/crash-2026-01-01_00.00.00-client.txt",
                   GAME_DIR);
    check(sxcl_crash_read_text(path, buf, sizeof(buf), 0, &enc, &truncated, NULL, 0) > 0,
          "读报告成功");
    check_str(enc, "ascii", "已知文件的编码键");
    check_int(truncated, 0, "窗口够大时不截断");
    check(sxcl_crash_read_text(GAME_DIR "/nope.txt", buf, sizeof(buf), 0, NULL, NULL, NULL, 0) < 0,
          "读不存在的文件返回负数");
    check_int((long)sxcl_crash_scan(NULL, SXCL_CRASH_SCAN_ALL, 0, NULL, NULL, NULL, NULL, 0), -1,
              "缺游戏目录返回 -1");
    check_int((long)sxcl_crash_scan(FIX_DIR "/empty_dir", SXCL_CRASH_SCAN_ALL, 0, NULL, NULL, &ev,
                                    NULL, 0),
              0, "目录里什么都没有:0 行(scanned=1)");
    check_int(ev.scanned, 1, "空目录也算扫过");
    check_int((long)sxcl_crash_count_lines(GAME_DIR "/logs/latest.log"), 3, "流式数行 = 3");
    check_int((long)sxcl_crash_count_lines(GAME_DIR "/nope.log"), -1, "数不存在的文件 = -1");
}

static void test_structure(void)
{
    char out[128];
    printf("[5] crash-report 结构识别\n");
    check_int(sxcl_crash_report_marker("---- Minecraft Crash Report ----"), 1, "报告头是结构行");
    check_int(sxcl_crash_report_marker("Description: Ticking entity"), 1, "Description 是结构行");
    check_int(sxcl_crash_report_marker("Caused by: java.lang.NullPointerException"), 1,
              "Caused by 是结构行");
    check_int(sxcl_crash_report_marker("\tat com.example.Main.main(Main.java:1)"), 0,
              "普通堆栈行不是结构行");
    check_int(sxcl_crash_report_description(kReport, out, sizeof(out)), 1, "抽取 Description");
    check_str(out, "Rendering overlay", "Description 内容");
    check_int(sxcl_crash_report_description("no description here", out, sizeof(out)), 0,
              "没有 Description 时返回 0");
    check_str(sxcl_crash_encoding_name("gbk"), "GBK(中文 Windows 的 ANSI)", "编码键的人话");
    check_str(sxcl_crash_encoding_name("utf-8"), "UTF-8", "UTF-8 的人话");
}

int main(void)
{
    printf("崩溃取证模块测试\n");
    if (setup() != 0) {
        printf("夹具准备失败(写不了 %s?)\n", FIX_DIR);
        return 1;
    }
    (void)sxcl_fs_mkdirs(FIX_DIR "/empty_dir");
    test_find_reports();
    test_scan();
    test_debug_fallback();
    test_legacy_log();
    test_truncation_and_errors();
    test_structure();
    printf("crash 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
