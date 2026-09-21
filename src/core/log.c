/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/log.h"

#include "sxcl/fs.h"
#include "sxcl/settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <pthread.h>
#endif

#if defined(__ANDROID__)
#  include <android/log.h>
#endif

/* 见 include/sxcl/log.h 的设计说明。本文件只做四件事:
 *   1) 决定"写到哪"(配置目录/logs)、"写多细"(级别)、"写多少"(上限/份数);
 *   2) 组一行:时间戳 + 级别 + 模块 + 消息,一次 fwrite 写出(多线程下不会互相穿插);
 *   3) 写满就换新文件,并按份数删最旧的;
 *   4) 任何一步失败都**静默降级**(只丢日志),绝不把错误抛回业务。 */

#if defined(_WIN32)
#  define SXCL_LOG_SEP "\\"
#else
#  define SXCL_LOG_SEP "/"
#endif

#define SXCL_LOG_MODULE_MAX 16
#define SXCL_LOG_MSG_MAX    1536
#define SXCL_LOG_NAME_MAX   64

typedef struct sxcl_log_state {
    int ready;                 /* 1 = 已初始化 */
    int level;                 /* 当前级别(未初始化时按 INFO 算) */
    int to_stderr;             /* 桌面默认 1、安卓默认 0(安卓走 logcat) */
    int file_failed;           /* 文件通道不可用(静默降级,不再重试) */
    FILE *file;                /* 当前文件;NULL = 没有 */
    size_t file_bytes;         /* 当前文件已写字节数 */
    size_t max_bytes;          /* 单文件上限(0 = 不限) */
    int keep_files;            /* 保留份数(含当前这一份) */
    char dir[768];
    char path[1024];           /* 当前文件全路径 */
    char name[SXCL_LOG_NAME_MAX]; /* 当前文件名(修剪时不能把自己删掉) */
    sxcl_log_stats stats;
    /* ── 游戏输出缓冲通道(见 log.h 第 5 节)──
     * 攒批落盘:游戏一秒几百行,逐行 fflush 会把磁盘刷爆。缓冲是**静态**的
     * (不 malloc):日志模块在内存不足时必须还能工作。 */
    int game_on;               /* 1 = 收游戏输出(默认 1;设置/环境变量可关) */
    unsigned long long game_ms; /* 上次落盘的时刻(单调毫秒) */
    size_t game_lines;         /* 缓冲里压着几行 */
    size_t game_used;          /* 缓冲已用字节 */
    char game_buf[SXCL_LOG_GAME_BLOCK_LINES * SXCL_LOG_LINE_MAX];
#if defined(_WIN32)
    CRITICAL_SECTION lock;
    int lock_ready;
#else
    pthread_mutex_t lock;
#endif
} sxcl_log_state;

/* 定义在文件末尾(第 5 节),这里先声明:init/shutdown 都要用它们。 */
static size_t log_game_flush_locked(void);
static unsigned long long log_now_ms(void);

static sxcl_log_state g_log = {
#if !defined(_WIN32)
    .lock = PTHREAD_MUTEX_INITIALIZER,
#endif
/* 终端通道的默认值:**安卓关**(日志走 logcat,免得同一条打印两遍),桌面开。
 * 写成初始化式而不是函数调用:未初始化就写日志时(g_config 还没跑)也要拿到正确默认。 */
#if defined(__ANDROID__)
    .to_stderr = 0,
#else
    .to_stderr = 1,
#endif
    .level = SXCL_LOG_INFO,
};

/* ── 锁 ─────────────────────────────────────────────────────────────── */

#if defined(_WIN32)
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK log_lock_prepare(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_log.lock);
    g_log.lock_ready = 1;
    return TRUE;
}

static void log_lock(void)
{
    (void)InitOnceExecuteOnce(&g_lock_once, log_lock_prepare, NULL, NULL);
    EnterCriticalSection(&g_log.lock);
}
static void log_unlock(void) { LeaveCriticalSection(&g_log.lock); }
#else
static void log_lock(void) { (void)pthread_mutex_lock(&g_log.lock); }
static void log_unlock(void) { (void)pthread_mutex_unlock(&g_log.lock); }
#endif

/* ── 环境变量 ───────────────────────────────────────────────────────── */

/* UTF-8 环境变量(Windows 走 _wgetenv + CP_UTF8:中文用户名/目录不会被代码页毁掉)。
 * 找到返回 1 并把值拷进 out;没有返回 0。 */
static int log_env_utf8(const char *name, char *out, size_t out_len)
{
    if (name == NULL || *name == '\0' || out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
#if defined(_WIN32)
    {
        wchar_t name_w[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, name_w, 64) <= 0) {
            return 0;
        }
        const wchar_t *value = _wgetenv(name_w);
        if (value == NULL || *value == L'\0') {
            return 0;
        }
        if (WideCharToMultiByte(CP_UTF8, 0, value, -1, out, (int)out_len, NULL, NULL) <= 0) {
            return 0;
        }
        return 1;
    }
#else
    {
        const char *value = getenv(name);
        if (value == NULL || *value == '\0') {
            return 0;
        }
        (void)snprintf(out, out_len, "%s", value);
        return 1;
    }
#endif
}

/* "0"/"false"/"no"/"off" = 关;其余 = 开。 */
static int log_text_is_off(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    return (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 || strcmp(text, "no") == 0 ||
            strcmp(text, "off") == 0);
}

static int log_default_stderr(void)
{
#if defined(__ANDROID__)
    return 0; /* 安卓走 logcat;再打 stderr 会被打包层的转发算成第二条 */
#else
    return 1;
#endif
}

/* ── 小工具 ─────────────────────────────────────────────────────────── */

/* dir + 分隔符 + leaf;装不下返回 -1。dir 末尾已有分隔符就不重复加。 */
static int log_join(char *out, size_t out_len, const char *dir, const char *leaf)
{
    if (out == NULL || dir == NULL || leaf == NULL || out_len == 0) {
        return -1;
    }
    const size_t dlen = strlen(dir);
    const size_t llen = strlen(leaf);
    const int need_sep = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) ? 0 : 1;
    if (dlen + (size_t)need_sep + llen + 1u > out_len) {
        return -1;
    }
    memcpy(out, dir, dlen);
    size_t n = dlen;
    if (need_sep) {
        memcpy(out + n, SXCL_LOG_SEP, strlen(SXCL_LOG_SEP));
        n += strlen(SXCL_LOG_SEP);
    }
    memcpy(out + n, leaf, llen + 1u);
    return 0;
}

static void log_stamp(char *human, size_t human_len, char *stamp, size_t stamp_len)
{
    if (human == NULL || human_len == 0 || stamp == NULL || stamp_len == 0) {
        return;
    }
#if defined(_WIN32)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        (void)snprintf(human, human_len, "%04d-%02d-%02d %02d:%02d:%02d.%03d", (int)st.wYear,
                       (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute,
                       (int)st.wSecond, (int)st.wMilliseconds);
        (void)snprintf(stamp, stamp_len, "%04d%02d%02d-%02d%02d%02d", (int)st.wYear, (int)st.wMonth,
                       (int)st.wDay, (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    }
#else
    {
        struct timespec ts;
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
        }
        if (localtime_r(&ts.tv_sec, &tmv) == NULL) {
            memset(&tmv, 0, sizeof(tmv));
        }
        (void)snprintf(human, human_len, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tmv.tm_year + 1900,
                       tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                       (int)(ts.tv_nsec / 1000000));
        (void)snprintf(stamp, stamp_len, "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900,
                       tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }
#endif
}

/* 只认我们自己的文件:sxcl-*.log(轮转/重名都落在这个模式里)。 */
static int log_name_matches(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    const size_t n = strlen(name);
    if (n < 10u || n >= SXCL_LOG_NAME_MAX) {
        return 0;
    }
    if (strncmp(name, "sxcl-", 5) != 0) {
        return 0;
    }
    return strcmp(name + n - 4u, ".log") == 0;
}

#if defined(_WIN32)
/* 定长宽字符拷贝(自带截断)。不用 wcsncpy_s/_s 系列:那套只在 MSVC 上有,
 * 换成自己的循环,MinGW/其它 Windows 工具链也一样能编。 */
static void log_wide_copy(wchar_t *dst, size_t dst_count, const wchar_t *src)
{
    size_t i = 0;
    if (dst == NULL || dst_count == 0) {
        return;
    }
    if (src != NULL) {
        for (; i + 1u < dst_count && src[i] != L'\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = L'\0';
}

static int log_utf8_to_wide(const char *text, wchar_t *out, size_t out_count)
{
    if (text == NULL || out == NULL || out_count == 0) {
        return 0;
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (need <= 0 || (size_t)need > out_count) {
        return 0;
    }
    return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, need) == need ? 1 : 0;
}
#endif

/* 目录里的日志文件数(目录不存在 = 0)。 */
static int log_dir_count(const char *dir)
{
    if (dir == NULL || *dir == '\0') {
        return 0;
    }
#if defined(_WIN32)
    {
        wchar_t wdir[1024];
        wchar_t pattern[1100];
        WIN32_FIND_DATAW found;
        if (!log_utf8_to_wide(dir, wdir, sizeof(wdir) / sizeof(wdir[0]))) {
            return 0;
        }
        (void)swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\sxcl-*.log", wdir);
        HANDLE h = FindFirstFileW(pattern, &found);
        if (h == INVALID_HANDLE_VALUE) {
            return 0;
        }
        int count = 0;
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                int matched = 0;
                const size_t len = wcslen(found.cFileName);
                if (len >= 10u && wcsncmp(found.cFileName, L"sxcl-", 5) == 0) {
                    matched = (wcscmp(found.cFileName + len - 4u, L".log") == 0) ? 1 : 0;
                }
                if (matched) {
                    ++count;
                }
            }
        } while (FindNextFileW(h, &found));
        FindClose(h);
        return count;
    }
#else
    {
        DIR *d = opendir(dir);
        if (d == NULL) {
            return 0;
        }
        int count = 0;
        struct dirent *entry = NULL;
        while ((entry = readdir(d)) != NULL) {
            if (log_name_matches(entry->d_name)) {
                ++count;
            }
        }
        closedir(d);
        return count;
    }
#endif
}

/* 删掉目录里**名字最小**(= 最旧)的一个日志文件,删掉的类名写进 deleted。
 * 不碰当前打开的那一份。返回 0 成功,非 0 = 没删成。 */
static int log_dir_delete_oldest(const char *dir, char *deleted, size_t deleted_len)
{
    if (deleted != NULL && deleted_len > 0) {
        deleted[0] = '\0';
    }
    if (dir == NULL || *dir == '\0') {
        return -1;
    }
#if defined(_WIN32)
    {
        wchar_t wdir[1024];
        wchar_t pattern[1100];
        WIN32_FIND_DATAW found;
        wchar_t best_w[SXCL_LOG_NAME_MAX];
        char best[SXCL_LOG_NAME_MAX];
        int have = 0;
        if (!log_utf8_to_wide(dir, wdir, sizeof(wdir) / sizeof(wdir[0]))) {
            return -1;
        }
        (void)swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\sxcl-*.log", wdir);
        HANDLE h = FindFirstFileW(pattern, &found);
        if (h == INVALID_HANDLE_VALUE) {
            return -1;
        }
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            char name[SXCL_LOG_NAME_MAX];
            if (WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, (int)sizeof(name), NULL,
                                    NULL) <= 0) {
                continue;
            }
            if (!log_name_matches(name)) {
                continue;
            }
            if (g_log.name[0] != '\0' && strcmp(name, g_log.name) == 0) {
                continue; /* 正在写的那一份 */
            }
            if (!have || wcscmp(found.cFileName, best_w) < 0) {
                log_wide_copy(best_w, sizeof(best_w) / sizeof(best_w[0]), found.cFileName);
                (void)snprintf(best, sizeof(best), "%s", name);
                have = 1;
            }
        } while (FindNextFileW(h, &found));
        FindClose(h);
        if (!have) {
            return -1;
        }
        wchar_t full[1100];
        (void)swprintf(full, sizeof(full) / sizeof(full[0]), L"%ls\\%ls", wdir, best_w);
        if (!DeleteFileW(full)) {
            return -1;
        }
        if (deleted != NULL && deleted_len > 0) {
            (void)snprintf(deleted, deleted_len, "%s", best);
        }
        return 0;
    }
#else
    {
        DIR *d = opendir(dir);
        if (d == NULL) {
            return -1;
        }
        char best[SXCL_LOG_NAME_MAX];
        best[0] = '\0';
        struct dirent *entry = NULL;
        while ((entry = readdir(d)) != NULL) {
            const char *name = entry->d_name;
            if (!log_name_matches(name)) {
                continue;
            }
            if (g_log.name[0] != '\0' && strcmp(name, g_log.name) == 0) {
                continue;
            }
            if (best[0] == '\0' || strcmp(name, best) < 0) {
                (void)snprintf(best, sizeof(best), "%s", name);
            }
        }
        closedir(d);
        if (best[0] == '\0') {
            return -1;
        }
        char full[1200];
        if (log_join(full, sizeof(full), dir, best) != 0) {
            return -1;
        }
        if (remove(full) != 0) {
            return -1;
        }
        if (deleted != NULL && deleted_len > 0) {
            (void)snprintf(deleted, deleted_len, "%s", best);
        }
        return 0;
    }
#endif
}

/* 保留份数:多了就把最旧的删掉。keep <= 0 = 不限。 */
static void log_prune_locked(void)
{
    if (g_log.keep_files <= 0) {
        return;
    }
    int count = log_dir_count(g_log.dir);
    while (count > g_log.keep_files) {
        char deleted[SXCL_LOG_NAME_MAX];
        if (log_dir_delete_oldest(g_log.dir, deleted, sizeof(deleted)) != 0) {
            break;
        }
        g_log.stats.pruned += 1u;
        count -= 1;
    }
}

/* 开一个新文件(名字带时间戳;同一秒内已经有同名的就加 -N)。失败 = file 保持 NULL。 */
static void log_open_locked(void)
{
    char human[32];
    char stamp[32];
    char leaf[SXCL_LOG_NAME_MAX];
    char full[1024];
    log_stamp(human, sizeof(human), stamp, sizeof(stamp));
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (attempt == 0) {
            (void)snprintf(leaf, sizeof(leaf), "sxcl-%s.log", stamp);
        } else {
            (void)snprintf(leaf, sizeof(leaf), "sxcl-%s-%d.log", stamp, attempt);
        }
        if (log_join(full, sizeof(full), g_log.dir, leaf) != 0) {
            break;
        }
        if (sxcl_fs_exists(full)) {
            continue;
        }
        FILE *handle = sxcl_fs_fopen(full, "wb");
        if (handle == NULL) {
            continue;
        }
        g_log.file = handle;
        g_log.file_bytes = 0;
        (void)snprintf(g_log.path, sizeof(g_log.path), "%s", full);
        (void)snprintf(g_log.name, sizeof(g_log.name), "%s", leaf);
        return;
    }
    g_log.file = NULL;
    g_log.file_bytes = 0;
    g_log.path[0] = '\0';
    g_log.name[0] = '\0';
}

/* 换文件:关掉旧的 -> 开新的 -> 修剪份数。count_rotation=0 表示这是首次打开。 */
static void log_rotate_locked(int count_rotation)
{
    if (g_log.file != NULL) {
        (void)fclose(g_log.file);
        g_log.file = NULL;
    }
    if (count_rotation) {
        g_log.stats.rotations += 1u;
    }
    log_open_locked();
    log_prune_locked();
}

/* 真正的落盘:满了先换文件。调用方必须持锁。
 * do_flush = 0 时**不刷盘** —— 游戏输出的攒批通道用一条 fflush 收尾一整批。 */
static int log_sink_line_locked(const char *line, size_t len, int do_flush)
{
    if (!g_log.ready || g_log.file_failed) {
        /* 还没 init(或文件通道已经判死):不落盘。**绝不**在没定目录时凭空造文件。 */
        g_log.stats.dropped += 1u;
        return 0;
    }
    if (g_log.file != NULL && g_log.max_bytes > 0 && g_log.file_bytes > 0 &&
        g_log.file_bytes + len > g_log.max_bytes) {
        log_rotate_locked(1);
    }
    if (g_log.file == NULL) {
        /* 还没开成过(init 时失败)或刚换文件也失败 -> 再试一次,仍是失败就静默降级 */
        log_open_locked();
        if (g_log.file == NULL) {
            g_log.file_failed = 1;
            g_log.stats.dropped += 1u;
            return 0;
        }
        log_prune_locked();
    }
    const size_t wrote = fwrite(line, 1, len, g_log.file);
    if (wrote != len) {
        g_log.stats.dropped += 1u;
        g_log.file_failed = 1;
        return 0;
    }
    if (do_flush) {
        (void)fflush(g_log.file);
    }
    g_log.file_bytes += wrote;
    g_log.stats.lines += 1u;
    g_log.stats.bytes += (unsigned long long)wrote;
    return 1;
}

static void log_sink_file_locked(const char *line, size_t len)
{
    (void)log_sink_line_locked(line, len, 1);
}

#if defined(__ANDROID__)
static int log_android_prio(int level)
{
    switch (level) {
    case SXCL_LOG_ERROR:
        return ANDROID_LOG_ERROR;
    case SXCL_LOG_WARN:
        return ANDROID_LOG_WARN;
    case SXCL_LOG_DEBUG:
        return ANDROID_LOG_DEBUG;
    default:
        return ANDROID_LOG_INFO;
    }
}
#endif

/* ── 对外 API ───────────────────────────────────────────────────────── */

const char *sxcl_log_level_name(int level)
{
    switch (level) {
    case SXCL_LOG_ERROR:
        return "ERROR";
    case SXCL_LOG_WARN:
        return "WARN";
    case SXCL_LOG_INFO:
        return "INFO";
    case SXCL_LOG_DEBUG:
        return "DEBUG";
    case SXCL_LOG_OFF:
        return "OFF";
    default:
        return "?";
    }
}

int sxcl_log_level_from_name(const char *text)
{
    if (text == NULL || *text == '\0') {
        return -1;
    }
    char buf[16];
    size_t n = 0;
    for (; text[n] != '\0' && n + 1u < sizeof(buf); ++n) {
        char c = text[n];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        buf[n] = c;
    }
    buf[n] = '\0';
    if (strcmp(buf, "error") == 0 || strcmp(buf, "err") == 0 || strcmp(buf, "e") == 0 ||
        strcmp(buf, "0") == 0) {
        return SXCL_LOG_ERROR;
    }
    if (strcmp(buf, "warn") == 0 || strcmp(buf, "warning") == 0 || strcmp(buf, "w") == 0 ||
        strcmp(buf, "1") == 0) {
        return SXCL_LOG_WARN;
    }
    if (strcmp(buf, "info") == 0 || strcmp(buf, "i") == 0 || strcmp(buf, "2") == 0) {
        return SXCL_LOG_INFO;
    }
    if (strcmp(buf, "debug") == 0 || strcmp(buf, "d") == 0 || strcmp(buf, "3") == 0 ||
        strcmp(buf, "trace") == 0) {
        return SXCL_LOG_DEBUG;
    }
    if (strcmp(buf, "off") == 0 || strcmp(buf, "none") == 0 || strcmp(buf, "-1") == 0) {
        return SXCL_LOG_OFF;
    }
    return -1;
}

int sxcl_log_level(void)
{
    return g_log.ready ? g_log.level : SXCL_LOG_INFO;
}

int sxcl_log_set_level(int level)
{
    if (level < SXCL_LOG_ERROR || level > SXCL_LOG_OFF) {
        return -1;
    }
    const int previous = g_log.level;
    g_log.level = level;
    return previous;
}

int sxcl_log_enabled(int level)
{
    if (level < SXCL_LOG_ERROR || level > SXCL_LOG_DEBUG) {
        return 0;
    }
    return level <= sxcl_log_level() ? 1 : 0;
}

size_t sxcl_log_max_bytes(void) { return g_log.max_bytes; }
int sxcl_log_keep_files(void) { return g_log.keep_files; }
const char *sxcl_log_file_path(void) { return g_log.path; }
const char *sxcl_log_dir(void) { return g_log.dir; }

void sxcl_log_set_limits(size_t max_bytes, int keep_files)
{
    if (max_bytes > 0) {
        g_log.max_bytes = max_bytes;
    }
    if (keep_files > 0) {
        g_log.keep_files = keep_files > 100 ? 100 : keep_files;
    }
}

void sxcl_log_get_stats(sxcl_log_stats *out)
{
    if (out == NULL) {
        return;
    }
    log_lock();
    *out = g_log.stats;
    log_unlock();
}

void sxcl_log_flush(void)
{
    log_lock();
    if (g_log.file != NULL) {
        (void)fflush(g_log.file);
    }
    log_unlock();
    (void)fflush(stderr);
}

int sxcl_log_rotate_now(void)
{
    log_lock();
    if (!g_log.ready) {
        log_unlock();
        return SXCL_LOG_ERR_IO;
    }
    log_rotate_locked(1);
    const int ok = (g_log.file != NULL) ? SXCL_LOG_OK : SXCL_LOG_ERR_IO;
    log_unlock();
    return ok;
}

/* 从设置文件读 log.level / log.max_bytes / log.keep(环境变量优先,见 init)。 */
static void log_apply_settings(void)
{
    char path[1024];
    char err[SXCL_LOG_ERROR_MAX];
    char pinned[1024];
    path[0] = '\0';
    err[0] = '\0';
    /* 界面层可能钉死了设置文件(SXCL_UI_SETTINGS):那就读同一份,
     * 免得"设置里开了 debug"和界面看到的不是同一个文件。 */
    if (log_env_utf8("SXCL_UI_SETTINGS", pinned, sizeof(pinned))) {
        (void)snprintf(path, sizeof(path), "%s", pinned);
    } else if (sxcl_settings_default_path(path, sizeof(path), err, sizeof(err)) != SXCL_SETTINGS_OK) {
        return;
    }
    if (path[0] == '\0') {
        return;
    }
    sxcl_settings *settings = sxcl_settings_open(path);
    if (settings == NULL) {
        return;
    }
    const char *level = sxcl_settings_get(settings, "log.level", "");
    if (level != NULL && *level != '\0') {
        const int parsed = sxcl_log_level_from_name(level);
        if (parsed >= 0) {
            g_log.level = parsed;
        }
    }
    const int64_t max_bytes = sxcl_settings_get_int(settings, "log.max_bytes", 0);
    if (max_bytes > 0) {
        g_log.max_bytes = (size_t)max_bytes;
    }
    const int64_t keep = sxcl_settings_get_int(settings, "log.keep", 0);
    if (keep > 0) {
        g_log.keep_files = keep > 100 ? 100 : (int)keep;
    }
    /* log.game:游戏输出落盘开关(默认开)。用户抱怨日志占地/刷屏时可以关掉它,
     * 而**不**影响启动器自己的日志。 */
    const char *game = sxcl_settings_get(settings, "log.game", "");
    if (game != NULL && *game != '\0') {
        g_log.game_on = log_text_is_off(game) ? 0 : 1;
    }
    sxcl_settings_free(settings);
}

int sxcl_log_init(const char *dir, char *err, size_t err_len)
{
    char work[768];
    char base[768];
    char dir_err[SXCL_LOG_ERROR_MAX];
    int rc = SXCL_LOG_OK;

    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }

    log_lock();
    if (g_log.ready) {
        log_unlock();
        return SXCL_LOG_OK; /* 幂等:安卓打包层与 main.cpp 都会调一次 */
    }

    /* 1) 默认值与设置文件(环境变量随后覆盖) */
    g_log.level = SXCL_LOG_INFO;
    g_log.max_bytes = SXCL_LOG_DEFAULT_MAX_BYTES;
    g_log.keep_files = SXCL_LOG_DEFAULT_KEEP;
    g_log.to_stderr = log_default_stderr();
    g_log.file_failed = 0;
    g_log.file = NULL;
    g_log.file_bytes = 0;
    g_log.path[0] = '\0';
    g_log.name[0] = '\0';
    g_log.game_on = 1; /* 默认收游戏输出;设置文件与环境变量随后可以关 */
    g_log.game_lines = 0;
    g_log.game_used = 0;
    g_log.game_ms = log_now_ms();
    memset(&g_log.stats, 0, sizeof(g_log.stats));
    log_apply_settings();

    /* 2) 环境变量(优先于设置文件) */
    if (log_env_utf8("SXCL_LOG_LEVEL", work, sizeof(work))) {
        const int parsed = sxcl_log_level_from_name(work);
        if (parsed >= 0) {
            g_log.level = parsed;
        }
    }
    if (log_env_utf8("SXCL_LOG_MAX_BYTES", work, sizeof(work))) {
        const long long value = atoll(work);
        if (value > 0) {
            g_log.max_bytes = (size_t)value;
        }
    }
    if (log_env_utf8("SXCL_LOG_KEEP", work, sizeof(work))) {
        const long long value = atoll(work);
        if (value > 0) {
            g_log.keep_files = value > 100 ? 100 : (int)value;
        }
    }
    if (log_env_utf8("SXCL_LOG_STDERR", work, sizeof(work))) {
        g_log.to_stderr = log_text_is_off(work) ? 0 : 1;
    }
    if (log_env_utf8("SXCL_LOG_GAME", work, sizeof(work))) {
        g_log.game_on = log_text_is_off(work) ? 0 : 1;
    }

    /* 3) 目录:参数 > SXCL_LOG_DIR > <配置目录>/logs */
    if (dir != NULL && *dir != '\0') {
        (void)snprintf(g_log.dir, sizeof(g_log.dir), "%s", dir);
    } else if (log_env_utf8("SXCL_LOG_DIR", work, sizeof(work))) {
        (void)snprintf(g_log.dir, sizeof(g_log.dir), "%s", work);
    } else {
        dir_err[0] = '\0';
        if (sxcl_settings_default_dir(base, sizeof(base), dir_err, sizeof(dir_err)) ==
                SXCL_SETTINGS_OK &&
            log_join(g_log.dir, sizeof(g_log.dir), base, "logs") == 0) {
            /* 已填好 */
        } else if (log_env_utf8("TEMP", work, sizeof(work)) &&
                   log_join(g_log.dir, sizeof(g_log.dir), work, "sxcl-logs") == 0) {
            /* 兜底:配置目录都拼不出来(极端受限环境)时也别一条都不留 */
        } else {
            (void)snprintf(g_log.dir, sizeof(g_log.dir), ".");
        }
    }

    /* 4) 开文件。开不出来只降级,不让调用方失败。 */
    if (sxcl_fs_mkdirs(g_log.dir) != 0) {
        g_log.file_failed = 1;
        rc = SXCL_LOG_ERR_IO;
        if (err != NULL && err_len > 0) {
            (void)snprintf(err, err_len, "日志目录建不出来:%s", g_log.dir);
        }
    } else {
        log_open_locked();
        if (g_log.file == NULL) {
            g_log.file_failed = 1;
            rc = SXCL_LOG_ERR_IO;
            if (err != NULL && err_len > 0) {
                (void)snprintf(err, err_len, "日志文件打不开(目录 %s)", g_log.dir);
            }
        } else {
            log_prune_locked();
        }
    }

    g_log.ready = 1;
    log_unlock();
    return rc;
}

void sxcl_log_shutdown(void)
{
    log_lock();
    log_game_flush_locked(); /* 退出前把游戏输出的缓冲冲刷干净(最后几十行也是证据) */
    if (g_log.file != NULL) {
        (void)fflush(g_log.file);
        (void)fclose(g_log.file);
        g_log.file = NULL;
    }
    g_log.ready = 0;
    log_unlock();
}

/* 单调毫秒(攒批通道判断"距上次落盘多久了"用;不受系统时间被改的影响)。 */
static unsigned long long log_now_ms(void)
{
#if defined(_WIN32)
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000L);
#endif
}

/** 组一行:时间戳 + 级别 + 模块 + 消息,一次 snprintf。**收下的那一刻**就定时间戳,
 *  所以攒批不会让整批日志共用一个时间。写入字节数(含 '\n');组不出来返回 0。 */
static size_t log_format_line(char *line, size_t line_cap, int level, const char *module,
                              const char *msg)
{
    char human[32];
    char stamp[32];
    int len = 0;
    size_t used = 0;
    log_stamp(human, sizeof(human), stamp, sizeof(stamp));
    len = snprintf(line, line_cap, "%s %-5s %.16s | %s\n", human, sxcl_log_level_name(level),
                   (module != NULL && *module != '\0') ? module : "-",
                   (msg != NULL) ? msg : "");
    if (len < 0) {
        return 0; /* 组行失败:什么都不做(静默降级) */
    }
    used = (size_t)len;
    if (used >= line_cap) {
        /* 太长:截断,并明确标注(不要让读日志的人以为是完整内容) */
        static const char kCut[] = " …(已截断)";
        const size_t cut_len = sizeof(kCut) - 1u;
        used = line_cap - 1u;
        if (used > cut_len + 1u) {
            memcpy(line + used - cut_len - 1u, kCut, cut_len);
            line[used - 1u] = '\n';
        }
    }
    return used;
}

void sxcl_log_write(int level, const char *module, const char *fmt, ...)
{
    char msg[SXCL_LOG_MSG_MAX];
    char line[SXCL_LOG_LINE_MAX];
    va_list args;

    if (level < SXCL_LOG_ERROR || level > SXCL_LOG_DEBUG) {
        return;
    }
    if (level > sxcl_log_level()) {
        return;
    }

    msg[0] = '\0';
    if (fmt != NULL) {
        va_start(args, fmt);
        (void)vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
    }

    const size_t used = log_format_line(line, sizeof(line), level, module, msg);
    if (used == 0) {
        return;
    }

    log_lock();
    log_sink_file_locked(line, used);
    log_unlock();

    /* 终端通道在锁外:慢 IO 不拖住别的线程;它们各自线程安全(stdout/stderr / logcat)。 */
    if (g_log.to_stderr) {
        (void)fwrite(line, 1, used, stderr);
        (void)fflush(stderr);
    }
#if defined(__ANDROID__)
    if (used > 0 && line[used - 1u] == '\n') {
        line[used - 1u] = '\0';
    }
    __android_log_write(log_android_prio(level), "sxcl", line);
#endif
}

/* ── URL 打码 ───────────────────────────────────────────────────────── */

/* 值必须打码的查询参数(小写比较)。key/secret 这类名字太常见,但出现在**查询串**里
 * 基本就是签名/密钥,一律按敏感处理 —— 宁可多打一个,不可漏一个。 */
static const char *const kSecretParams[] = {
    "access_token", "refresh_token", "id_token", "identity_token", "xsts_token", "token",
    "code",         "client_secret", "password",     "authorization",  "sig",         "signature",
    "api_key",      "apikey",        "secret",       "key",            "x-amz-signature",
    "x-amz-credential", "x-amz-security-token", "session", "ticket", NULL};

static int log_param_is_secret(const char *key, size_t key_len)
{
    char buf[64];
    if (key_len == 0 || key_len >= sizeof(buf)) {
        return 0;
    }
    for (size_t i = 0; i < key_len; ++i) {
        char c = key[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        buf[i] = c;
    }
    buf[key_len] = '\0';
    for (size_t i = 0; kSecretParams[i] != NULL; ++i) {
        if (strcmp(buf, kSecretParams[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int sxcl_log_mask_url(const char *url, char *out, size_t out_len)
{
    if (url == NULL || out == NULL || out_len == 0) {
        return SXCL_LOG_ERR_ARG;
    }
    const size_t total = strlen(url);
    size_t limit = total;
    int truncated = 0;
    if (limit > SXCL_LOG_URL_MAX) {
        limit = SXCL_LOG_URL_MAX;
        truncated = 1;
    }
    size_t w = 0;
#define SXCL_LOG_PUT(ch)                                                                        \
    do {                                                                                        \
        if (w + 1u < out_len) {                                                                 \
            out[w++] = (char)(ch);                                                              \
        }                                                                                       \
    } while (0)
    size_t i = 0;
    while (i < limit) {
        const char c = url[i];
        if (c != '?' && c != '&') {
            SXCL_LOG_PUT(c);
            ++i;
            continue;
        }
        SXCL_LOG_PUT(c);
        ++i;
        const size_t key_start = i;
        while (i < limit && url[i] != '=' && url[i] != '&') {
            ++i;
        }
        const size_t key_end = i;
        if (i < limit && url[i] == '=') {
            for (size_t k = key_start; k < key_end; ++k) {
                SXCL_LOG_PUT(url[k]);
            }
            SXCL_LOG_PUT('=');
            ++i;
            const size_t value_start = i;
            while (i < limit && url[i] != '&') {
                ++i;
            }
            if (log_param_is_secret(url + key_start, key_end - key_start)) {
                SXCL_LOG_PUT('*');
                SXCL_LOG_PUT('*');
                SXCL_LOG_PUT('*');
            } else {
                for (size_t v = value_start; v < i; ++v) {
                    SXCL_LOG_PUT(url[v]);
                }
            }
        } else {
            /* 没有 '=' 的"裸参数":原样带走(不是键值对,没有明文可漏) */
            for (size_t k = key_start; k < key_end; ++k) {
                SXCL_LOG_PUT(url[k]);
            }
        }
    }
    if (truncated) {
        char tail[64];
        (void)snprintf(tail, sizeof(tail), "...(共 %llu 字节)", (unsigned long long)total);
        for (const char *p = tail; *p != '\0'; ++p) {
            SXCL_LOG_PUT(*p);
        }
    }
#undef SXCL_LOG_PUT
    out[w] = '\0';
    return (int)w;
}

/* ══════════════════════ 5. 游戏输出落盘(缓冲通道) ══════════════════════
 *
 * 设计见 include/sxcl/log.h 第 5 节。这里只重复一条**实测过**的理由:
 * 游戏启动头 10 秒能刷上千行,而 log_sink_line_locked 每行 fflush ——
 * 逐行落盘等于拿日志当性能瓶颈(用户那边表现为"启动变卡、磁盘灯常亮")。
 * 所以按批:32 行一批 / 距上次落盘 300ms / 显式 flush,三选一触发。
 */

/* 调用方必须持锁。返回写进文件的行数(文件通道判死时是 0)。 */
static size_t log_game_flush_locked(void)
{
    size_t written = 0;
    size_t remaining = g_log.game_used;
    const char *p = g_log.game_buf;
    if (remaining == 0) {
        g_log.game_ms = log_now_ms();
        return 0;
    }
    while (remaining > 0) {
        const char *nl = (const char *)memchr(p, '\n', remaining);
        const size_t len = (nl != NULL) ? (size_t)(nl - p) + 1u : remaining;
        if (log_sink_line_locked(p, len, 0)) {
            ++written;
        }
        p += len;
        remaining -= len;
    }
    if (g_log.file != NULL) {
        (void)fflush(g_log.file); /* 一批只刷一次盘 */
    }
    g_log.stats.game_lines += (unsigned long long)written;
    g_log.stats.game_flushes += 1u;
    g_log.stats.game_dropped += (unsigned long long)(g_log.game_lines - written);
    g_log.game_used = 0;
    g_log.game_lines = 0;
    g_log.game_ms = log_now_ms();
    return written;
}

int sxcl_log_game_enabled(void)
{
    return g_log.game_on;
}

void sxcl_log_game_set_enabled(int on)
{
    log_lock();
    if (!on) {
        (void)log_game_flush_locked();
    }
    g_log.game_on = on ? 1 : 0;
    log_unlock();
}

size_t sxcl_log_game_pending(void)
{
    size_t n = 0;
    log_lock();
    n = g_log.game_lines;
    log_unlock();
    return n;
}

size_t sxcl_log_game_flush(void)
{
    size_t n = 0;
    log_lock();
    n = log_game_flush_locked();
    log_unlock();
    return n;
}

void sxcl_log_game_line(const char *line)
{
    char masked[SXCL_LOG_GAME_LINE_MAX * 2];
    char body[SXCL_LOG_GAME_LINE_MAX + 1];
    char text[SXCL_LOG_LINE_MAX];
    size_t len = 0;
    size_t body_len = 0;
    int truncated = 0;

    if (line == NULL) {
        return;
    }
    /* 未初始化也要能收:与 sxcl_log_write 一个口径 —— 头几行不进文件也不该丢 */
    if (!g_log.game_on || SXCL_LOG_INFO > sxcl_log_level()) {
        log_lock();
        g_log.stats.game_dropped += 1u;
        log_unlock();
        return;
    }

    /* 先打码再落盘:令牌在磁盘上不存在第二份(用户名/UUID 留着,排查要用)。
     * 打码优先于截断 —— 宁可少留几个字符,也不能在截断的口子上漏半截令牌。 */
    (void)sxcl_log_mask_secrets(line, masked, sizeof(masked));
    body_len = strlen(masked);
    if (body_len > (size_t)SXCL_LOG_GAME_LINE_MAX) {
        body_len = (size_t)SXCL_LOG_GAME_LINE_MAX;
        truncated = 1;
    }
    memcpy(body, masked, body_len);
    body[body_len] = '\0';

    len = log_format_line(text, sizeof(text), SXCL_LOG_INFO, SXCL_LOG_GAME_MODULE, body);
    if (len == 0) {
        return;
    }

    log_lock();
    if (truncated) {
        g_log.stats.game_truncated += 1u;
    }
    if (g_log.game_used + len > sizeof(g_log.game_buf)) {
        (void)log_game_flush_locked(); /* 缓冲满:先冲刷,再收这一行 */
    }
    if (len > sizeof(g_log.game_buf)) {
        /* 单行比整块还大:直接落盘(正常走不到 —— 上面已按行上限截过) */
        if (log_sink_line_locked(text, len, 1)) {
            g_log.stats.game_lines += 1u;
            g_log.stats.game_flushes += 1u;
        } else {
            g_log.stats.game_dropped += 1u;
        }
    } else {
        memcpy(g_log.game_buf + g_log.game_used, text, len);
        g_log.game_used += len;
        g_log.game_lines += 1u;
    }
    /* 触发落盘:批满 / 缓冲放不下下一行 / 距上次落盘够久 */
    if (g_log.game_lines >= (size_t)SXCL_LOG_GAME_BLOCK_LINES ||
        g_log.game_used + (size_t)SXCL_LOG_LINE_MAX > sizeof(g_log.game_buf) ||
        log_now_ms() - g_log.game_ms >= (unsigned long long)SXCL_LOG_GAME_FLUSH_MS) {
        (void)log_game_flush_locked();
    }
    log_unlock();
}

/* ══════════════════════ 6. 整行文本打码 ══════════════════════
 *
 * 两个口径(见 log.h):
 *   sxcl_log_mask_secrets —— 只打**凭据**:URL 的敏感查询参数、accessToken/token/
 *     password/session 这类键后面的值。游戏输出落盘走它(本地日志,用户名留着)。
 *   sxcl_log_mask_text    —— 凭据 + **身份**:UUID、"Setting user: xxx"、
 *     username=xxx、C:\Users\xxx\ 与 /home/xxx/ 里的那一段。导出包走它。
 *
 * 热路径:先做一次"整行里像不像有敏感词"的粗筛(不做解析),不命中就逐字节拷贝。
 */

static int mask_ci_prefix(const char *text, size_t avail, const char *prefix)
{
    size_t i = 0;
    for (i = 0; prefix[i] != '\0'; ++i) {
        char a = 0;
        if (i >= avail) {
            return 0;
        }
        a = text[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != prefix[i]) {
            return 0;
        }
    }
    return 1;
}

static int mask_contains_ci(const char *text, const char *needle)
{
    const size_t n = strlen(text);
    size_t i = 0;
    for (i = 0; i < n; ++i) {
        if (mask_ci_prefix(text + i, n - i, needle)) {
            return 1;
        }
    }
    return 0;
}

static int mask_is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int mask_is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 值必须打码的键(小写)。与 URL 参数表同一份口径 + argv/日志里的常见写法。 */
static const char *const kSecretKeys[] = {
    "accesstoken",  "access_token",  "refresh_token", "id_token", "identity_token",
    "xsts_token",   "client_secret", "password",      "passwd",   "authorization",
    "api_key",      "apikey",        "secret",        "session",  "ticket",
    "token",        "bearer",        "client_id",     "code",     NULL};

/* 只有"导出/分享"口径才打码的身份键。 */
static const char *const kIdentityKeys[] = {
    "username", "player_name", "auth_player_name", "setting user", "user_name",
    "displayname", "display_name", NULL};

static const char *const kUserPathMarkers[] = {"\\users\\", "/users/", "/home/", NULL};

typedef struct mask_out {
    char *buf;
    size_t cap;
    size_t w;
    int hit;
} mask_out;

static void mask_put(mask_out *o, const char *text, size_t len)
{
    size_t take = len;
    if (o->w + 1u >= o->cap) {
        return;
    }
    if (take > o->cap - o->w - 1u) {
        take = o->cap - o->w - 1u;
    }
    memcpy(o->buf + o->w, text, take);
    o->w += take;
}

static void mask_star(mask_out *o)
{
    mask_put(o, "***", 3);
    o->hit = 1;
}

/** 在 text(长度 avail)开头找敏感键,命中返回键长(> 0);否则 0。
 *  条件:键后面必须跟分隔符,免得把 "tokenizer" 这类词当成 token=。 */
static size_t mask_key_at(const char *text, size_t avail, const char *const *keys)
{
    size_t k = 0;
    for (k = 0; keys[k] != NULL; ++k) {
        const size_t key_len = strlen(keys[k]);
        size_t after = 0;
        if (!mask_ci_prefix(text, avail, keys[k])) {
            continue;
        }
        after = key_len;
        if (after >= avail) {
            continue;
        }
        if (text[after] != ' ' && text[after] != '=' && text[after] != ':' && text[after] != '"' &&
            text[after] != '\'' && text[after] != '\t') {
            continue;
        }
        return key_len;
    }
    return 0;
}

/** 分隔符之后的值的范围(跳过空格/等号/冒号/引号);找不到返回 0。 */
static int mask_value_span(const char *text, size_t avail, size_t after_key, size_t *start,
                           size_t *end)
{
    size_t s = after_key;
    size_t e = 0;
    while (s < avail && (text[s] == ' ' || text[s] == '=' || text[s] == ':' || text[s] == '"' ||
                         text[s] == '\'' || text[s] == '>' || text[s] == '\t')) {
        ++s;
    }
    if (s >= avail || text[s] == '*' || text[s] == '\0') {
        return 0; /* 已经没有值 / 已经打过码 */
    }
    e = s;
    while (e < avail && strchr(" \t\r\n\"'&,;)}]<>", text[e]) == NULL) {
        ++e;
    }
    if (e == s) {
        return 0;
    }
    *start = s;
    *end = e;
    return 1;
}

/** 8-4-4-4-12 的 UUID(前后不能是十六进制字符,免得把更长的串切一半)。 */
static int mask_uuid_at(const char *text, size_t avail, size_t at)
{
    size_t i = 0;
    if (avail - at < 36u) {
        return 0;
    }
    for (i = 0; i < 36u; ++i) {
        const char c = text[at + i];
        if (i == 8u || i == 13u || i == 18u || i == 23u) {
            if (c != '-') {
                return 0;
            }
        } else if (!mask_is_hex(c)) {
            return 0;
        }
    }
    if (at > 0u && mask_is_hex(text[at - 1u])) {
        return 0;
    }
    if (avail - at > 36u && (mask_is_hex(text[at + 36u]) || text[at + 36u] == '-')) {
        return 0;
    }
    return 1;
}

static int mask_apply(const char *text, char *out, size_t out_len, int identity, int *hit)
{
    mask_out o;
    size_t i = 0;
    const size_t n = strlen(text);
    if (out == NULL || out_len == 0u) {
        return -1;
    }
    o.buf = out;
    o.cap = out_len;
    o.w = 0;
    o.hit = 0;
    while (i < n) {
        /* 1) URL:整段交给 sxcl_log_mask_url(敏感查询参数的口径只有一份) */
        if (mask_ci_prefix(text + i, n - i, "http://") ||
            mask_ci_prefix(text + i, n - i, "https://")) {
            char url[SXCL_LOG_URL_MAX + 8];
            char masked[SXCL_LOG_URL_MAX + 64];
            size_t end = i;
            while (end < n && strchr(" \t\r\n\"'<>]", text[end]) == NULL) {
                ++end;
            }
            if (end > i && end - i < sizeof(url)) {
                memcpy(url, text + i, end - i);
                url[end - i] = '\0';
                (void)sxcl_log_mask_url(url, masked, sizeof(masked));
                mask_put(&o, masked, strlen(masked));
                o.hit = 1;
                i = end;
                continue;
            }
        }
        /* 2) 键值:token=xxx / "accessToken":"xxx" / --accessToken xxx */
        {
            size_t key_len = 0;
            size_t vs = 0;
            size_t ve = 0;
            const int at_word_start = (i == 0u || !mask_is_alnum(text[i - 1u]));
            if (at_word_start) {
                key_len = mask_key_at(text + i, n - i, kSecretKeys);
                if (key_len == 0u && identity) {
                    key_len = mask_key_at(text + i, n - i, kIdentityKeys);
                }
            }
            if (key_len > 0u && mask_value_span(text, n, i + key_len, &vs, &ve)) {
                mask_put(&o, text + i, vs - i);
                mask_star(&o);
                i = ve;
                /* "Authorization: Bearer <token>":把后面那个 token 也吃掉 */
                if ((ve - vs) == 6u && mask_ci_prefix(text + vs, n - vs, "bearer")) {
                    size_t skip = ve;
                    size_t vs2 = 0;
                    size_t ve2 = 0;
                    while (skip < n && (text[skip] == ' ' || text[skip] == '\t')) {
                        ++skip;
                    }
                    if (mask_value_span(text, n, skip, &vs2, &ve2)) {
                        mask_put(&o, text + skip, vs2 - skip);
                        mask_star(&o);
                        i = ve2;
                    }
                }
                continue;
            }
        }
        /* 3) 身份:UUID / 路径里的用户名(只在导出/分享口径下打) */
        if (identity) {
            size_t m = 0;
            if (mask_uuid_at(text, n, i)) {
                mask_star(&o);
                i += 36u;
                continue;
            }
            for (m = 0; kUserPathMarkers[m] != NULL; ++m) {
                const size_t marker_len = strlen(kUserPathMarkers[m]);
                if (mask_ci_prefix(text + i, n - i, kUserPathMarkers[m])) {
                    size_t end = i + marker_len;
                    mask_put(&o, text + i, marker_len);
                    while (end < n && text[end] != '\\' && text[end] != '/' && text[end] != '"' &&
                           text[end] != '\'' && text[end] != ' ' && text[end] != ':' &&
                           text[end] != '\r' && text[end] != '\n') {
                        ++end;
                    }
                    if (end > i + marker_len) {
                        mask_star(&o);
                        i = end;
                    } else {
                        i += marker_len;
                    }
                    break;
                }
            }
            if (kUserPathMarkers[m] != NULL) {
                continue;
            }
        }
        mask_put(&o, text + i, 1u);
        ++i;
    }
    out[o.w] = '\0';
    if (hit != NULL) {
        *hit = o.hit;
    }
    return (int)o.w;
}

/** 粗筛:整行里有没有可能触发打码的标记。没有就逐字节拷贝(热路径)。 */
static int mask_maybe(const char *text, int identity)
{
    static const char *const kTriggers[] = {"http",    "token",  "secret", "password",
                                           "passwd",  "session", "ticket", "bearer",
                                           "apikey",  "auth",   NULL};
    static const char *const kIdentityTriggers[] = {"user", "uuid", "home", NULL};
    size_t i = 0;
    for (i = 0; kTriggers[i] != NULL; ++i) {
        if (mask_contains_ci(text, kTriggers[i])) {
            return 1;
        }
    }
    if (!identity) {
        return 0;
    }
    for (i = 0; kIdentityTriggers[i] != NULL; ++i) {
        if (mask_contains_ci(text, kIdentityTriggers[i])) {
            return 1;
        }
    }
    return 0;
}

static int log_mask_common(const char *text, char *out, size_t out_len, int identity)
{
    size_t n = 0;
    int hit = 0;
    if (out == NULL || out_len == 0u) {
        return SXCL_LOG_ERR_ARG;
    }
    if (text == NULL) {
        out[0] = '\0';
        return 0;
    }
    n = strlen(text);
    if (!mask_maybe(text, identity)) {
        /* 快路径:没有任何敏感标记,原样带走(截断以 out_len-1 为界) */
        size_t take = (n < out_len - 1u) ? n : (out_len - 1u);
        memcpy(out, text, take);
        out[take] = '\0';
        return (int)take;
    }
    return mask_apply(text, out, out_len, identity, &hit);
}

int sxcl_log_mask_secrets(const char *text, char *out, size_t out_len)
{
    return log_mask_common(text, out, out_len, 0);
}

int sxcl_log_mask_text(const char *text, char *out, size_t out_len)
{
    return log_mask_common(text, out, out_len, 1);
}

