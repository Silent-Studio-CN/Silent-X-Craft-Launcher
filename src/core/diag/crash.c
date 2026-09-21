/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/crash.h"

#include "diag_internal.h"

#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 见 include/sxcl/crash.h 的说明。本文件的取舍:
 *   * **只读**游戏目录里的 crash-reports/ 与 logs/,不写、不改、不删;
 *   * 只取最近的一份非空报告:一次崩溃最多也就一份有用,把历史全读进来只会拖慢退出;
 *   * 大文件读**尾部**(崩因在最后),但行数按整个文件数(报告里要说清楚"这个文件多少行");
 *   * 编码先探后转(GBK/UTF-16 都认),见 core/diag/text.c。 */

#define SXCL_CRASH_LINE_MAX 4096

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *trim_left(const char *text)
{
    if (text == NULL) {
        return "";
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    return text;
}

/** 忽略大小写的前缀比较(先跳前导空白)。 */
static int starts_with_ci(const char *text, const char *prefix)
{
    size_t i = 0;
    const char *p = trim_left(text);
    if (prefix == NULL || *prefix == '\0') {
        return 0;
    }
    for (i = 0; prefix[i] != '\0'; ++i) {
        char a = p[i];
        char b = prefix[i];
        if (a == '\0') {
            return 0;
        }
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* ────────────────────────── 行数 / 读取 ────────────────────────── */

long long sxcl_crash_count_lines(const char *path)
{
    FILE *f = NULL;
    char buf[64 * 1024];
    size_t got = 0;
    size_t i = 0;
    long long lines = 0;
    int last = -1;
    long long total = 0;
    if (path == NULL || *path == '\0') {
        return -1;
    }
    f = sxcl_fs_fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (i = 0; i < got; ++i) {
            if (buf[i] == '\n') {
                ++lines;
            }
        }
        last = (unsigned char)buf[got - 1u];
        total += (long long)got;
    }
    (void)fclose(f);
    if (total > 0 && last != '\n') {
        ++lines; /* 最后一行没有换行符也算一行(与 sxcl_text_count_lines 同口径) */
    }
    return lines;
}

/** 跳到大文件的尾部读时用 64 位偏移:Windows 的 long 是 32 位,
 *  一个 3GB 的 latest.log 会让 (long)start 溢出成负数(fseek 直接失败)。 */
static int seek_abs(FILE *f, long long offset)
{
#if defined(_WIN32)
    return _fseeki64(f, offset, SEEK_SET);
#else
    return fseeko(f, (off_t)offset, SEEK_SET);
#endif
}

/** 编码探测只看文件头:读大文件的尾部时,尾部没有 BOM,拿尾部去猜会猜错。 */
static void detect_head_encoding(const char *path, const char **encoding_out)
{
    unsigned char head[4096];
    FILE *f = NULL;
    size_t got = 0;
    if (encoding_out != NULL) {
        *encoding_out = "empty";
    }
    f = sxcl_fs_fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    got = fread(head, 1, sizeof(head), f);
    (void)fclose(f);
    if (encoding_out != NULL && got > 0u) {
        *encoding_out = sxcl_text_detect_encoding(head, got);
    }
}

long long sxcl_crash_read_text(const char *path, char *out, size_t out_cap, size_t max_bytes,
                               const char **encoding_out, int *truncated, char *err, size_t err_len)
{
    int64_t size = 0;
    int64_t start = 0;
    size_t want = 0;
    size_t got = 0;
    size_t skip = 0;
    size_t w = 0;
    int cut = 0;
    int cut2 = 0;
    const char *enc = "empty";
    char *raw = NULL;
    FILE *f = NULL;

    if (truncated != NULL) {
        *truncated = 0;
    }
    if (encoding_out != NULL) {
        *encoding_out = "empty";
    }
    if (out != NULL && out_cap > 0u) {
        out[0] = '\0';
    }
    if (path == NULL || *path == '\0' || out == NULL || out_cap == 0u) {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "读文本:参数不合法");
        }
        return -1;
    }
    if (sxcl_fs_stat(path, &size, NULL) != 0) {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "找不到文件:%s", path);
        }
        return -1;
    }
    if (size <= 0) {
        return 0;
    }
    if (max_bytes > 0u && (int64_t)max_bytes < size) {
        start = size - (int64_t)max_bytes;
        cut = 1;
    }
    want = (size_t)(size - start);
    raw = (char *)malloc(want + 8u);
    if (raw == NULL) {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "内存不足:要读 %llu 字节", (unsigned long long)want);
        }
        return -1;
    }
    detect_head_encoding(path, &enc);
    f = sxcl_fs_fopen(path, "rb");
    if (f == NULL) {
        free(raw);
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "打不开文件:%s", path);
        }
        return -1;
    }
    if (start > 0) {
        if (seek_abs(f, (long long)start) != 0) {
            start = 0;
            cut = 0;
            want = (size_t)size;
        }
    }
    got = fread(raw, 1, want, f);
    (void)fclose(f);
    raw[got] = '\0';
    if (got == 0u) {
        free(raw);
        return 0;
    }
    if (start > 0) {
        /* 从半个文件中间开始读:丢掉第一个不完整的行,别把半行当成日志首行 */
        skip = 0;
        while (skip < got && raw[skip] != '\n') {
            ++skip;
        }
        if (skip < got) {
            ++skip;
        }
        if (skip > 0u) {
            memmove(raw, raw + skip, got - skip);
            got -= skip;
            raw[got] = '\0';
        }
    }
    w = sxcl_diag_text_to_utf8_as(enc, raw, got, out, out_cap, &cut2);
    free(raw);
    if (encoding_out != NULL) {
        *encoding_out = enc;
    }
    if (truncated != NULL && (cut || cut2)) {
        *truncated = 1;
    }
    return (long long)w;
}

/* ────────────────────────── crash-report 结构 ────────────────────────── */

int sxcl_crash_report_description(const char *text, char *out, size_t out_cap)
{
    const char *cursor = text;
    char line[512];
    int seen = 0;
    if (out != NULL && out_cap > 0u) {
        out[0] = '\0';
    }
    if (text == NULL || out == NULL || out_cap == 0u) {
        return 0;
    }
    while (seen < 40 && sxcl_text_next_line(&cursor, line, sizeof(line))) {
        ++seen;
        if (starts_with_ci(line, "description:")) {
            const char *p = trim_left(line) + 12; /* "description:" = 12 字符 */
            copy_str(out, out_cap, trim_left(p));
            return out[0] != '\0' ? 1 : 0;
        }
    }
    return 0;
}

int sxcl_crash_report_marker(const char *line)
{
    static const char *const kMarkers[] = {
        "---- minecraft crash report ----", "description:",  "caused by:",      "time:",
        "stacktrace:",                      "-- head --",     "-- system details --", "details:",
        "a detailed walkthrough",           "//",             NULL};
    size_t i = 0;
    for (i = 0; kMarkers[i] != NULL; ++i) {
        if (starts_with_ci(line, kMarkers[i])) {
            return 1;
        }
    }
    return 0;
}

/* ────────────────────────── 找报告 ────────────────────────── */

size_t sxcl_crash_find_reports(const char *game_dir, sxcl_crash_file *out, size_t cap)
{
    char dir[SXCL_CRASH_PATH_MAX];
    sxcl_diag_dir list;
    sxcl_crash_file best[SXCL_CRASH_MAX_REPORTS];
    size_t n = 0;
    size_t total = 0;
    size_t i = 0;
    if (game_dir == NULL || *game_dir == '\0') {
        return 0;
    }
    if (cap > SXCL_CRASH_MAX_REPORTS) {
        cap = SXCL_CRASH_MAX_REPORTS;
    }
    if (sxcl_diag_join(dir, sizeof(dir), game_dir, "crash-reports") != 0) {
        return 0;
    }
    if (sxcl_diag_dir_list(dir, &list) != 0) {
        return 0; /* 没有 crash-reports 目录 = 没有任何报告(不是错误) */
    }
    for (i = 0; i < list.count; ++i) {
        char full[SXCL_CRASH_PATH_MAX];
        int64_t size = 0;
        int64_t mtime = 0;
        sxcl_crash_file item;
        size_t at = 0;
        if (!sxcl_diag_ends_with_ci(list.names[i], ".txt")) {
            continue;
        }
        if (sxcl_diag_join(full, sizeof(full), dir, list.names[i]) != 0) {
            continue;
        }
        if (sxcl_fs_stat(full, &size, &mtime) != 0 || size <= 0) {
            continue;
        }
        if (!sxcl_diag_has_visible_text(full)) {
            continue; /* 0 字节/只有空白的报告:跳过,别让"最近的一份"是空壳 */
        }
        ++total;
        if (cap == 0u) {
            continue;
        }
        memset(&item, 0, sizeof(item));
        copy_str(item.path, sizeof(item.path), full);
        item.size = size;
        item.mtime_ns = mtime;
        /* 按修改时间从新到旧插进 best(元素最多 SXCL_CRASH_MAX_REPORTS 个,插入排序足够) */
        at = n;
        while (at > 0u && best[at - 1u].mtime_ns < item.mtime_ns) {
            if (at < cap) {
                best[at] = best[at - 1u];
            }
            --at;
        }
        if (at < cap) {
            if (n < cap) {
                ++n;
            }
            best[at] = item;
        }
    }
    if (out != NULL) {
        for (i = 0; i < n && i < cap; ++i) {
            out[i] = best[i];
        }
    }
    return total;
}


/** 老版本兜底:logs/ 里最新的 client-*.log(1.7 之前 MC 的命名,没有 latest.log)。
 *  找不到 client-* 时退回"除 latest/debug 之外最新的一份 .log"。返回 1 = 找到。 */
static int find_legacy_log(const char *game_dir, char *out, size_t out_cap)
{
    char dir[SXCL_CRASH_PATH_MAX];
    sxcl_diag_dir list;
    size_t i = 0;
    int64_t best_time = 0;
    int found = 0;
    if (sxcl_diag_join(dir, sizeof(dir), game_dir, "logs") != 0 ||
        sxcl_diag_dir_list(dir, &list) != 0) {
        return 0;
    }
    out[0] = '\0';
    for (i = 0; i < list.count; ++i) {
        char full[SXCL_CRASH_PATH_MAX];
        int64_t size = 0;
        int64_t mtime = 0;
        if (!sxcl_diag_ends_with_ci(list.names[i], ".log")) {
            continue;
        }
        if (sxcl_diag_ends_with_ci(list.names[i], "latest.log") ||
            sxcl_diag_ends_with_ci(list.names[i], "debug.log")) {
            continue;
        }
        if (sxcl_diag_join(full, sizeof(full), dir, list.names[i]) != 0) {
            continue;
        }
        if (sxcl_fs_stat(full, &size, &mtime) != 0 || size <= 0) {
            continue;
        }
        if (!found || mtime > best_time) {
            best_time = mtime;
            copy_str(out, out_cap, full);
            found = 1;
        }
    }
    return found;
}

/* ────────────────────────── 主入口 ────────────────────────── */

typedef struct scan_ctx {
    sxcl_crash_line_fn fn;
    void *ud;
    int stop;
    long long lines;
} scan_ctx;

static void feed_text(const char *text, int source, scan_ctx *ctx)
{
    const char *cursor = text;
    char line[SXCL_CRASH_LINE_MAX];
    if (ctx->stop) {
        return;
    }
    while (sxcl_text_next_line(&cursor, line, sizeof(line))) {
        ++ctx->lines;
        if (ctx->fn != NULL && ctx->fn(ctx->ud, source, line) != 0) {
            ctx->stop = 1;
            return;
        }
    }
}

static int join3(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    char tmp[SXCL_CRASH_PATH_MAX];
    if (sxcl_diag_join(tmp, sizeof(tmp), a, b) != 0) {
        return -1;
    }
    return sxcl_diag_join(out, cap, tmp, c);
}

long long sxcl_crash_scan(const char *game_dir, unsigned flags, size_t max_bytes,
                          sxcl_crash_line_fn on_line, void *userdata, sxcl_crash_evidence *out,
                          char *err, size_t err_len)
{
    sxcl_crash_evidence ev;
    scan_ctx ctx;
    char *buf = NULL;
    size_t buf_cap = 0;
    int rc = 0;

    if (err != NULL && err_len > 0u) {
        err[0] = '\0';
    }
    memset(&ev, 0, sizeof(ev));
    memset(&ctx, 0, sizeof(ctx));
    ctx.fn = on_line;
    ctx.ud = userdata;
    if (game_dir == NULL || *game_dir == '\0') {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "崩溃取证:缺少游戏目录");
        }
        if (out != NULL) {
            *out = ev;
        }
        return -1;
    }
    if (flags == 0u) {
        flags = SXCL_CRASH_SCAN_ALL;
    }
    if (max_bytes == 0u) {
        max_bytes = SXCL_CRASH_DEFAULT_MAX_BYTES;
    }
    if (max_bytes > 64u * 1024u * 1024u) {
        max_bytes = 64u * 1024u * 1024u;
    }
    /* GBK 双字节 -> UTF-8 三字节,最坏要 1.5 倍;Latin-1 是 2 倍。留 3 倍再加头。
     * 中文注释一多,这个余量就是"会不会读到半行乱码"的分界线。 */
    buf_cap = max_bytes * 3u + 64u;
    buf = (char *)malloc(buf_cap);
    if (buf == NULL) {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "崩溃取证:内存不足(%llu 字节)",
                           (unsigned long long)buf_cap);
        }
        if (out != NULL) {
            *out = ev;
        }
        return -1;
    }

    /* 1) 最近的一份非空 crash-report */
    if ((flags & SXCL_CRASH_SCAN_REPORTS) != 0u) {
        sxcl_crash_file files[SXCL_CRASH_MAX_REPORTS];
        const size_t total = sxcl_crash_find_reports(game_dir, files, SXCL_CRASH_MAX_REPORTS);
        ev.reports_total = (int)total;
        if (total > 0u) {
            const char *enc = "empty";
            int cut = 0;
            const long long got = sxcl_crash_read_text(files[0].path, buf, buf_cap, max_bytes, &enc,
                                                       &cut, NULL, 0);
            if (got > 0) {
                copy_str(ev.report_path, sizeof(ev.report_path), files[0].path);
                copy_str(ev.report_encoding, sizeof(ev.report_encoding), enc);
                ev.report_bytes = files[0].size;
                feed_text(buf, SXCL_CRASH_SRC_REPORT, &ctx);
                ev.report_lines = ctx.lines;
                (void)sxcl_crash_report_description(buf, ev.report_description,
                                                    sizeof(ev.report_description));
            }
        }
    }

    /* 2) latest.log */
    if ((flags & SXCL_CRASH_SCAN_LATEST) != 0u && !ctx.stop) {
        char path[SXCL_CRASH_PATH_MAX];
        if (join3(path, sizeof(path), game_dir, "logs", "latest.log") == 0 &&
            sxcl_fs_exists(path)) {
            const char *enc = "empty";
            int cut = 0;
            const long long got = sxcl_crash_read_text(path, buf, buf_cap, max_bytes, &enc, &cut,
                                                       NULL, 0);
            copy_str(ev.latest_log_path, sizeof(ev.latest_log_path), path);
            copy_str(ev.latest_log_encoding, sizeof(ev.latest_log_encoding), enc);
            (void)sxcl_fs_stat(path, &ev.latest_log_bytes, NULL);
            ev.latest_log_lines = sxcl_crash_count_lines(path);
            ev.latest_log_truncated = cut;
            if (got > 0) {
                const long long before = ctx.lines;
                feed_text(buf, SXCL_CRASH_SRC_LATEST_LOG, &ctx);
                ev.latest_log_fed = ctx.lines - before;
            }
        }
    }

    /* 3) debug.log:只有 latest.log 缺失/为空时才读(用户没点名要它) */
    if ((flags & SXCL_CRASH_SCAN_DEBUG) != 0u && !ctx.stop) {
        const int latest_missing = (ev.latest_log_path[0] == '\0') || (ev.latest_log_bytes <= 0) ||
                                   (ev.latest_log_fed <= 0);
        if (latest_missing) {
            char path[SXCL_CRASH_PATH_MAX];
            if (join3(path, sizeof(path), game_dir, "logs", "debug.log") == 0 &&
                sxcl_fs_exists(path)) {
                const char *enc = "empty";
                const long long got = sxcl_crash_read_text(path, buf, buf_cap, max_bytes, &enc, NULL,
                                                           NULL, 0);
                copy_str(ev.debug_log_path, sizeof(ev.debug_log_path), path);
                copy_str(ev.debug_log_encoding, sizeof(ev.debug_log_encoding), enc);
                ev.debug_log_lines = sxcl_crash_count_lines(path);
                if (got > 0) {
                    feed_text(buf, SXCL_CRASH_SRC_DEBUG_LOG, &ctx);
                    ev.used_debug_log = 1;
                }
            }
        }
    }

    /* 4) 老版本兜底:1.7 之前写的是 logs/client-*.log(没有 latest.log/debug.log)。
     *    只有在前面两条都没喂进任何东西时才用,免得把无关的历史日志当成本次证据。 */
    if (!ctx.stop && ctx.lines == 0 && (flags & SXCL_CRASH_SCAN_DEBUG) != 0u) {
        char legacy[SXCL_CRASH_PATH_MAX];
        if (find_legacy_log(game_dir, legacy, sizeof(legacy))) {
            const char *enc = "empty";
            const long long got = sxcl_crash_read_text(legacy, buf, buf_cap, max_bytes, &enc, NULL,
                                                       NULL, 0);
            copy_str(ev.legacy_log_path, sizeof(ev.legacy_log_path), legacy);
            copy_str(ev.legacy_log_encoding, sizeof(ev.legacy_log_encoding), enc);
            ev.legacy_log_lines = sxcl_crash_count_lines(legacy);
            if (got > 0) {
                feed_text(buf, SXCL_CRASH_SRC_DEBUG_LOG, &ctx);
                ev.used_legacy_log = 1;
            }
        }
    }

    free(buf);
    ev.scanned = 1;
    if (out != NULL) {
        *out = ev;
    }
    (void)rc;
    return ctx.lines;
}

const char *sxcl_crash_encoding_name(const char *encoding)
{
    if (encoding == NULL) {
        return "未知";
    }
    if (strcmp(encoding, "empty") == 0) {
        return "空文件";
    }
    if (strcmp(encoding, "ascii") == 0) {
        return "纯 ASCII";
    }
    if (strcmp(encoding, "utf-8") == 0) {
        return "UTF-8";
    }
    if (strcmp(encoding, "utf-8-bom") == 0) {
        return "UTF-8(带 BOM)";
    }
    if (strcmp(encoding, "utf-16le") == 0) {
        return "UTF-16LE";
    }
    if (strcmp(encoding, "utf-16be") == 0) {
        return "UTF-16BE";
    }
    if (strcmp(encoding, "gbk") == 0) {
        return "GBK(中文 Windows 的 ANSI)";
    }
    if (strcmp(encoding, "ansi") == 0) {
        return "系统 ANSI 代码页";
    }
    if (strcmp(encoding, "binary") == 0) {
        return "二进制(不是文本)";
    }
    return encoding;
}
