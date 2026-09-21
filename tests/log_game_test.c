/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/log.h"

/* 游戏输出落盘通道(include/sxcl/log.h 第 5 节)的夹具测试:
 *   1) **攒批**:32 行一批 / 显式 flush / shutdown 冲刷 —— 断言"什么时候真的落到文件里";
 *   2) **打码**:带令牌的游戏输出不许把明文写进日志;
 *   3) **开关与级别**:log.game=0 / 级别调到 WARN 之后不再收;
 *   4) **截断**:超长单行被截断并计数(不会写出一条几 MB 的日志行);
 *   5) 统计计数(game_lines / game_flushes / game_dropped / game_truncated)与文件内容对得上。
 * 夹具建在构建目录下(ctest 的 WORKING_DIRECTORY),仓库里不留产物。 */

#define LOG_DIR "_log_game_fixture"

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

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

/** 读回当前日志文件的全部内容(malloc,调用方 free)。 */
static char *read_log_file(size_t *len_out)
{
    const char *path = sxcl_log_file_path();
    FILE *f = NULL;
    char *buf = NULL;
    size_t cap = 0;
    size_t got = 0;
    if (path == NULL || *path == '\0') {
        return NULL;
    }
    f = sxcl_fs_fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    cap = (size_t)ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(cap + 1u);
    if (buf == NULL) {
        (void)fclose(f);
        return NULL;
    }
    got = fread(buf, 1, cap, f);
    (void)fclose(f);
    buf[got] = '\0';
    if (len_out != NULL) {
        *len_out = got;
    }
    return buf;
}

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    if (hay == NULL || needle == NULL || *needle == '\0') {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        ++n;
        p += strlen(needle);
    }
    return n;
}

int main(void)
{
    sxcl_log_stats st;
    char err[SXCL_LOG_ERROR_MAX];
    char *text = NULL;
    char line[256];
    int i = 0;

    printf("游戏输出落盘通道测试\n");
    /* 环境变量优先级高于设置文件:钉死"开着 + info",免得用户配置把用例带跑 */
#if defined(_WIN32)
    (void)_putenv_s("SXCL_LOG_GAME", "1");
    (void)_putenv_s("SXCL_LOG_LEVEL", "info");
#else
    (void)setenv("SXCL_LOG_GAME", "1", 1);
    (void)setenv("SXCL_LOG_LEVEL", "info", 1);
#endif
    err[0] = '\0';
    (void)sxcl_log_init(LOG_DIR, err, sizeof(err));
    if (sxcl_log_file_path() == NULL || sxcl_log_file_path()[0] == '\0') {
        printf("日志文件打不开(%s):用例无法继续\n", err);
        return 1;
    }

    printf("[1] 攒批:不足一批时只在缓冲里\n");
    check_int(sxcl_log_game_enabled(), 1, "游戏输出通道默认开");
    (void)sxcl_log_game_flush();
    for (i = 0; i < 5; ++i) {
        (void)snprintf(line, sizeof(line), "[00:00:%02d] [main/INFO]: game line %d", i, i);
        sxcl_log_game_line(line);
    }
    check_int((long)sxcl_log_game_pending(), 5, "5 行还在缓冲里");
    text = read_log_file(NULL);
    check_int(count_occurrences(text, "game line"), 0, "没到一批之前文件里还没有它们");
    free(text);

    printf("[2] 显式 flush:整批落盘,行格式与其它模块一致\n");
    check_int((long)sxcl_log_game_flush(), 5, "flush 落盘 5 行");
    check_int((long)sxcl_log_game_pending(), 0, "缓冲清空");
    text = read_log_file(NULL);
    check_int(count_occurrences(text, "game line"), 5, "文件里有 5 行游戏输出");
    check_int(count_occurrences(text, "INFO  game | "), 5, "模块名固定是 game");
    check(count_occurrences(text, "[main/INFO]: game line 0") == 1, "原始行内容原样保留");
    free(text);

    printf("[3] 攒够一批自动落盘(不用等调用方)\n");
    for (i = 0; i < SXCL_LOG_GAME_BLOCK_LINES; ++i) {
        (void)snprintf(line, sizeof(line), "[00:01:%02d] [main/INFO]: batch line %d", i, i);
        sxcl_log_game_line(line);
    }
    check_int((long)sxcl_log_game_pending(), 0, "满一批后自动落盘");
    text = read_log_file(NULL);
    check_int(count_occurrences(text, "batch line"), SXCL_LOG_GAME_BLOCK_LINES,
              "一批的行数都对");
    free(text);

    printf("[4] 打码:令牌明文不许进日志\n");
    sxcl_log_game_line("[main/INFO]: auth --accessToken SUPERSECRETVALUE url=https://x/y?token=ABCDEF");
    (void)sxcl_log_game_flush();
    text = read_log_file(NULL);
    check(!strstr(text, "SUPERSECRETVALUE"), "accessToken 的值不在日志里");
    check(!strstr(text, "ABCDEF"), "URL 里的 token 被打码");
    check(strstr(text, "***") != NULL, "打码标记在");
    free(text);

    printf("[5] 超长行截断(不会写出一条几 MB 的行)\n");
    {
        char *big = (char *)malloc(8192);
        memset(big, 'x', 8191);
        big[8191] = '\0';
        big[0] = 'L';
        sxcl_log_game_line(big);
        (void)sxcl_log_game_flush();
        sxcl_log_get_stats(&st);
        check(st.game_truncated >= 1u, "截断计数 +1");
        text = read_log_file(NULL);
        {
            /* 文件里最长的一行不能超过格式化上限 */
            const char *p = text;
            size_t longest = 0;
            size_t cur = 0;
            while (p != NULL && *p != '\0') {
                if (*p == '\n') {
                    if (cur > longest) {
                        longest = cur;
                    }
                    cur = 0;
                } else {
                    ++cur;
                }
                ++p;
            }
            check(longest <= (size_t)SXCL_LOG_LINE_MAX, "最长的一行不超过 SXCL_LOG_LINE_MAX");
        }
        free(text);
        free(big);
    }

    printf("[6] 开关与级别\n");
    sxcl_log_game_set_enabled(0);
    sxcl_log_game_line("[main/INFO]: blocked while off");
    check_int((long)sxcl_log_game_pending(), 0, "关掉之后不再收");
    check_int(sxcl_log_game_enabled(), 0, "开关状态可读");
    sxcl_log_game_set_enabled(1);
    sxcl_log_game_line("[main/INFO]: accepted after on");
    (void)sxcl_log_game_flush();
    text = read_log_file(NULL);
    check(strstr(text, "accepted after on") != NULL && strstr(text, "blocked while off") == NULL,
          "关的时候丢掉、开了之后照收");
    free(text);
    /* 级别:调到 WARN 后 INFO 级别的游戏输出不再进日志(与其它日志同一口径) */
    (void)sxcl_log_set_level(SXCL_LOG_WARN);
    sxcl_log_game_line("[main/INFO]: info level line");
    (void)sxcl_log_game_flush();
    text = read_log_file(NULL);
    check(strstr(text, "info level line") == NULL, "级别调到 WARN 后游戏输出不再落盘");
    free(text);
    (void)sxcl_log_set_level(SXCL_LOG_INFO);

    printf("[7] shutdown 冲刷 + 统计\n");
    sxcl_log_game_line("[main/INFO]: last line before exit");
    check_int((long)sxcl_log_game_pending(), 1, "退出前还有 1 行没落盘");
    sxcl_log_shutdown();
    text = read_log_file(NULL);
    check(strstr(text, "last line before exit") != NULL, "shutdown 把缓冲冲干净了");
    check(count_occurrences(text, "INFO  game | ") > 0,
          "行格式是 '时间 级别 模块 | 内容'(模块名固定 game)");
    free(text);
    sxcl_log_get_stats(&st);
    check(st.game_lines >= 40u, "统计里的 game_lines 与写出的行数一致");
    check(st.game_flushes >= 4u, "统计里的批次 >= 4");
    check(st.game_dropped >= 2u, "被丢掉的(关掉/级别过滤)有计数");

    printf("log_game 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
