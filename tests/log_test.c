/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* 运行日志模块(include/sxcl/log.h + src/core/log.c)的夹具。
 *
 * 六条都是"真跑真读文件",不是查 API 存不存在:
 *   1) URL 打码:敏感查询参数的值变 ***,其它参数一个字不动;
 *   2) 级别过滤:级别是 debug 时 debug 行进文件,切成 info 之后同一行**进不去**;
 *   3) 行格式:时间戳(YYYY-MM-DD HH:MM:SS.mmm)+ 级别 + 模块 + " | " + 消息,逐行可解析;
 *   4) 轮转与保留份数:上限调小后连续写 -> 目录里剩下的文件数 <= 保留份数,且真的换过文件;
 *   5) 多线程:4 条线程各写 200 行 -> 800 行一条不少、每行格式都完整(不会互相穿插);
 *   6) 静默降级:目录建不出来(路径中间那一段是个普通文件)时 init 返回 IO 错误、
 *      写入不崩、丢弃计数增加 —— 日志坏了绝不影响主流程。
 *
 * 临时目录用相对路径 build/_log_tmp(与 options_test 的 build/_options_tmp 同口径)。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/log.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

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

static void set_env(const char *key, const char *value)
{
#if defined(_WIN32)
    (void)_putenv_s(key, value);
#else
    (void)setenv(key, value, 1);
#endif
}

/* ── 文件小工具 ─────────────────────────────────────────────────────── */

#define SLURP_MAX (4u * 1024u * 1024u)
static char g_slurp[SLURP_MAX];

static size_t slurp(const char *path)
{
    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        g_slurp[0] = '\0';
        return 0;
    }
    const size_t got = fread(g_slurp, 1, SLURP_MAX - 1u, handle);
    g_slurp[got] = '\0';
    fclose(handle);
    return got;
}

static int count_lines(const char *text, size_t len)
{
    int lines = 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            ++lines;
        }
    }
    return lines;
}

static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static int digits_at(const char *s, int offset, int count)
{
    for (int i = 0; i < count; ++i) {
        const char c = s[offset + i];
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    return 1;
}

/* "YYYY-MM-DD HH:MM:SS.mmm LEVEL module | message" */
static int line_format_ok(const char *line, size_t len)
{
    static const char *const kLevels[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
    if (len < 32u) {
        return 0;
    }
    if (!digits_at(line, 0, 4) || line[4] != '-' || !digits_at(line, 5, 2) || line[7] != '-' ||
        !digits_at(line, 8, 2) || line[10] != ' ' || !digits_at(line, 11, 2) || line[13] != ':' ||
        !digits_at(line, 14, 2) || line[16] != ':' || !digits_at(line, 17, 2) || line[19] != '.' ||
        !digits_at(line, 20, 3) || line[23] != ' ') {
        return 0;
    }
    int level_ok = 0;
    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); ++i) {
        if (strncmp(line + 24, kLevels[i], 5) == 0) {
            level_ok = 1;
        }
    }
    if (!level_ok || line[29] != ' ') {
        return 0;
    }
    const char *bar = strstr(line + 30, " | ");
    return (bar != NULL && bar > line + 30) ? 1 : 0;
}

/* 逐行检查格式;返回坏行数,坏行的第一条内容写进 first_bad。 */
static int format_violations(const char *text, size_t len, char *first_bad, size_t bad_len)
{
    int bad = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || text[i] == '\n') {
            const size_t line_len = i - start;
            if (line_len > 0) {
                char line[4096];
                const size_t copy = line_len < sizeof(line) - 1u ? line_len : sizeof(line) - 1u;
                memcpy(line, text + start, copy);
                line[copy] = '\0';
                if (!line_format_ok(line, copy)) {
                    if (bad == 0 && first_bad != NULL) {
                        (void)snprintf(first_bad, bad_len, "%s", line);
                    }
                    ++bad;
                }
            }
            start = i + 1u;
        }
    }
    return bad;
}

static int count_log_files(const char *dir)
{
    int count = 0;
#if defined(_WIN32)
    char pattern[1024];
    WIN32_FIND_DATAA found;
    (void)snprintf(pattern, sizeof(pattern), "%s\\sxcl-*.log", dir);
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            ++count;
        }
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *d = opendir(dir);
    struct dirent *entry = NULL;
    if (d == NULL) {
        return 0;
    }
    while ((entry = readdir(d)) != NULL) {
        const size_t n = strlen(entry->d_name);
        if (n > 9u && strncmp(entry->d_name, "sxcl-", 5) == 0 &&
            strcmp(entry->d_name + n - 4u, ".log") == 0) {
            ++count;
        }
    }
    closedir(d);
#endif
    return count;
}

/* ── 多线程写 ───────────────────────────────────────────────────────── */

typedef struct thread_arg {
    int id;
    int lines;
} thread_arg;

static void write_lines(const thread_arg *arg)
{
    for (int i = 0; i < arg->lines; ++i) {
        SXCL_LOG_I("test", "thread=%d seq=%d payload=%s", arg->id, i,
                   "0123456789012345678901234567890123456789");
    }
}

#if defined(_WIN32)
static DWORD WINAPI thread_main(LPVOID param)
{
    write_lines((const thread_arg *)param);
    return 0;
}
#else
static void *thread_main(void *param)
{
    write_lines((const thread_arg *)param);
    return NULL;
}
#endif

/* ── 用例 ───────────────────────────────────────────────────────────── */

static void test_mask_url(void)
{
    char out[1024];
    /* 1) 敏感参数被打码,普通参数不动 */
    (void)sxcl_log_mask_url(
        "https://login.microsoftonline.com/common/oauth2/v2.0/token?code=SECRETCODE&x=1", out,
        sizeof(out));
    check(strcmp(out, "https://login.microsoftonline.com/common/oauth2/v2.0/token?code=***&x=1") == 0,
          "code= 的值被打码,其余原样");
    check(!contains(out, "SECRETCODE"), "URL 打码:明文不出来");

    (void)sxcl_log_mask_url("https://x/y?a=1&access_token=ABCDEF&b=2", out, sizeof(out));
    check(strcmp(out, "https://x/y?a=1&access_token=***&b=2") == 0, "access_token 打码且不影响后面参数");

    (void)sxcl_log_mask_url("https://piston-meta.mojang.com/v1/packages/abc.json", out, sizeof(out));
    check(strcmp(out, "https://piston-meta.mojang.com/v1/packages/abc.json") == 0, "无查询串时原样");

    /* 2) 过长截断:总长度要标出来 */
    {
        char big[4096];
        for (size_t i = 0; i < sizeof(big) - 1u; ++i) {
            big[i] = 'u';
        }
        big[sizeof(big) - 1u] = '\0';
        (void)sxcl_log_mask_url(big, out, sizeof(out));
        check(strlen(out) <= SXCL_LOG_URL_MAX + 32u, "超长 URL 被截断");
        check(contains(out, "共 4095 字节"), "截断时标出原始长度");
    }
}

static void test_level_and_format(void)
{
    const char *dir = "build/_log_tmp/level";
    char err[SXCL_LOG_ERROR_MAX];
    err[0] = '\0';

    check(sxcl_log_init(dir, err, sizeof(err)) == SXCL_LOG_OK, "init 成功");
    check(strcmp(sxcl_log_dir(), dir) == 0, "日志目录就是传进去的那个");
    check(sxcl_log_level() == SXCL_LOG_DEBUG, "级别来自环境变量 SXCL_LOG_LEVEL=debug");

    const char *file = sxcl_log_file_path();
    check(file != NULL && *file != '\0', "拿到了日志文件路径");
    {
        // 目录分隔符在 Windows 上可能是 '\'(核心库按平台选),所以两种都看,取更靠后的那个
        const char *base = strrchr(file, '/');
#if defined(_WIN32)
        const char *back = strrchr(file, '\\');
        if (back != NULL && (base == NULL || back > base)) {
            base = back;
        }
#endif
        base = (base != NULL) ? base + 1 : file;
        /* sxcl-YYYYMMDD-HHMMSS.log = 5 + 8 + 1 + 6 + 4 = 24 */
        check(strncmp(base, "sxcl-", 5) == 0 && strlen(base) >= 24u &&
                  strcmp(base + strlen(base) - 4u, ".log") == 0,
              "文件名是 sxcl-YYYYMMDD-HHMMSS.log");
    }

    SXCL_LOG_D("test", "debug-line-should-appear-at-debug");
    SXCL_LOG_I("test", "info-line");
    (void)sxcl_log_set_level(SXCL_LOG_INFO);
    check(sxcl_log_level() == SXCL_LOG_INFO, "set_level 改成 INFO");
    SXCL_LOG_D("test", "debug-line-must-be-dropped");
    SXCL_LOG_W("test", "warn-line");
    SXCL_LOG_E("test", "error-line");
    sxcl_log_flush();

    const size_t len = slurp(file);
    check(len > 0, "日志文件非空");
    check(count_lines(g_slurp, len) >= 4, "至少 4 行(debug/info/warn/error 各一)");
    check(contains(g_slurp, "debug-line-should-appear-at-debug"), "debug 级别下 debug 行进文件");
    check(!contains(g_slurp, "debug-line-must-be-dropped"), "切成 info 后 debug 行**不**进文件");
    check(contains(g_slurp, "info-line") && contains(g_slurp, "warn-line") &&
              contains(g_slurp, "error-line"),
          "info/warn/error 都进文件");
    check(contains(g_slurp, " INFO  test | info-line"), "行里有级别与模块");

    char bad[512];
    bad[0] = '\0';
    const int violations = format_violations(g_slurp, len, bad, sizeof(bad));
    if (violations > 0) {
        printf("  坏行样例: %s\n", bad);
    }
    check(violations == 0, "每一行都是 时间戳+级别+模块+消息 的格式");

    (void)sxcl_log_set_limits(0, 0);
    sxcl_log_shutdown();
}

static void test_rotation(void)
{
    const char *dir = "build/_log_tmp/rotate";
    char err[SXCL_LOG_ERROR_MAX];
    err[0] = '\0';
    check(sxcl_log_init(dir, err, sizeof(err)) == SXCL_LOG_OK, "轮转测试:init");
    /* 上限调到 400 字节 + 只留 3 份:每次换文件都在这个上限上触发 */
    sxcl_log_set_limits(400u, 3);
    check(sxcl_log_max_bytes() == 400u, "上限已改成 400 字节");
    check(sxcl_log_keep_files() == 3, "保留份数已改成 3");

    for (int i = 0; i < 60; ++i) {
        SXCL_LOG_I("test", "rotate-line-%02d-%s", i, "0123456789012345678901234567890123456789");
    }
    sxcl_log_flush();

    sxcl_log_stats stats;
    memset(&stats, 0, sizeof(stats));
    sxcl_log_get_stats(&stats);
    const int files = count_log_files(dir);
    printf("  轮转后目录里剩 %d 个文件,换过 %u 次\n", files, stats.rotations);
    check(stats.rotations >= 2u, "确实换过文件(rotations >= 2)");
    check(stats.pruned >= 1u, "确实删过最旧的(pruned >= 1)");
    check(files > 0 && files <= 3, "剩下的文件数 <= 保留份数 3");

    (void)sxcl_log_set_limits(0, 0);
    sxcl_log_shutdown();
}

static void test_threads(void)
{
    const char *dir = "build/_log_tmp/threads";
    char err[SXCL_LOG_ERROR_MAX];
    err[0] = '\0';
    check(sxcl_log_init(dir, err, sizeof(err)) == SXCL_LOG_OK, "多线程测试:init");
    sxcl_log_set_limits(4u * 1024u * 1024u, 4);

    const int per_thread = 200;
    const int thread_count = 4;
    thread_arg args[4];
#if defined(_WIN32)
    HANDLE handles[4];
    for (int i = 0; i < thread_count; ++i) {
        args[i].id = i;
        args[i].lines = per_thread;
        handles[i] = CreateThread(NULL, 0, thread_main, &args[i], 0, NULL);
        check(handles[i] != NULL, "线程起来了");
    }
    for (int i = 0; i < thread_count; ++i) {
        if (handles[i] != NULL) {
            WaitForSingleObject(handles[i], INFINITE);
            CloseHandle(handles[i]);
        }
    }
#else
    pthread_t threads[4];
    for (int i = 0; i < thread_count; ++i) {
        args[i].id = i;
        args[i].lines = per_thread;
        check(pthread_create(&threads[i], NULL, thread_main, &args[i]) == 0, "线程起来了");
    }
    for (int i = 0; i < thread_count; ++i) {
        (void)pthread_join(threads[i], NULL);
    }
#endif
    sxcl_log_flush();

    sxcl_log_stats stats;
    memset(&stats, 0, sizeof(stats));
    sxcl_log_get_stats(&stats);
    check(stats.lines == (unsigned long long)(per_thread * thread_count),
          "统计行数 = 线程数 x 每线程行数(一条不多一条不少)");
    check(stats.dropped == 0u, "没有丢弃");

    const char *file = sxcl_log_file_path();
    const size_t len = slurp(file);
    check(count_lines(g_slurp, len) == per_thread * thread_count, "文件里就是 800 行");
    char bad[512];
    bad[0] = '\0';
    const int violations = format_violations(g_slurp, len, bad, sizeof(bad));
    if (violations > 0) {
        printf("  坏行样例: %s\n", bad);
    }
    check(violations == 0, "多线程下没有互相穿插的坏行");

    sxcl_log_shutdown();
}

static void test_degrade(void)
{
    const char *root = "build/_log_tmp/degrade";
    const char *blocker = "build/_log_tmp/degrade/blocker";
    check(sxcl_fs_mkdirs(root) == 0, "降级测试:建目录");
    {
        FILE *handle = fopen(blocker, "wb");
        check(handle != NULL, "放一个与目录同名的普通文件");
        if (handle != NULL) {
            fputs("not a directory", handle);
            fclose(handle);
        }
    }
    char err[SXCL_LOG_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_log_init("build/_log_tmp/degrade/blocker/logs", err, sizeof(err));
    check(rc == SXCL_LOG_ERR_IO, "目录建不出来时 init 返回 IO 错误(不抛异常)");
    check(err[0] != '\0', "错误信息有人话");

    sxcl_log_stats before;
    memset(&before, 0, sizeof(before));
    sxcl_log_get_stats(&before);
    for (int i = 0; i < 5; ++i) {
        SXCL_LOG_E("test", "degrade-line-%d", i);
    }
    sxcl_log_stats after;
    memset(&after, 0, sizeof(after));
    sxcl_log_get_stats(&after);
    check(after.dropped >= before.dropped + 5u, "写不进去时按丢弃计数(静默降级)");
    check(after.lines == before.lines, "没有假装写成功");
    sxcl_log_shutdown();
}

int main(void)
{
    /* 环境变量口径:debug 级别 + 不打 stderr(单测输出保持干净) */
    set_env("SXCL_LOG_LEVEL", "debug");
    set_env("SXCL_LOG_STDERR", "0");
    (void)sxcl_fs_remove_tree("build/_log_tmp");

    test_mask_url();
    test_level_and_format();
    test_rotation();
    test_threads();
    test_degrade();

    printf("log_test: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
