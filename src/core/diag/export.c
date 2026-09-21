/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/logexport.h"

#include "diag_internal.h"

#include "sxcl/crash.h"
#include "sxcl/fs.h"
#include "sxcl/log.h"
#include "sxcl/text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 见 include/sxcl/logexport.h。实现要点:
 *   * zip 写侧是本文件自己写的 **store(method 0)打包器** —— 仓库里只有读侧,
 *     而 DEFLATE 编码器是另一件工程;这里如实只做"原样打包",换来的是
 *     "任何解压工具(以及我们自己的 sxcl_zip_open)都能打开"这个确定性;
 *   * 每条内容都过 sxcl_log_mask_text(凭据 + UUID + 用户名 + 路径用户名);
 *   * 全程不联网、不改游戏目录、不删任何东西。 */

#define EXP_MAX_ITEMS 32
#define EXP_NAME_MAX 192
#define EXP_MAX_LAUNCHER_LOGS 8

typedef struct exp_item {
    char src[SXCL_LOGS_EXPORT_PATH_MAX]; /* 源文件全路径 */
    char name[EXP_NAME_MAX];             /* zip 里的条目名(带目录前缀) */
    long long size;                      /* 源文件字节数 */
    int64_t mtime_ns;
} exp_item;

/* ────────────────────────── zip 写侧(store) ────────────────────────── */

typedef struct zip_central {
    char name[EXP_NAME_MAX];
    uint32_t crc;
    uint32_t size; /* 单条上限 4 MiB,这里不可能溢出 32 位 */
    long long offset;
} zip_central;

typedef struct zip_writer {
    FILE *f;
    long long offset;
    unsigned int count;
    zip_central entries[EXP_MAX_ITEMS + 2];
    int ok;
    unsigned int dostime;
    unsigned int dosdate;
} zip_writer;

static void zw_bytes(zip_writer *w, const void *data, size_t len)
{
    if (!w->ok || len == 0u) {
        return;
    }
    if (fwrite(data, 1, len, w->f) != len) {
        w->ok = 0;
        return;
    }
    w->offset += (long long)len;
}

static void zw_u16(zip_writer *w, unsigned int value)
{
    unsigned char b[2];
    b[0] = (unsigned char)(value & 0xFFu);
    b[1] = (unsigned char)((value >> 8) & 0xFFu);
    zw_bytes(w, b, 2u);
}

static void zw_u32(zip_writer *w, uint32_t value)
{
    unsigned char b[4];
    b[0] = (unsigned char)(value & 0xFFu);
    b[1] = (unsigned char)((value >> 8) & 0xFFu);
    b[2] = (unsigned char)((value >> 16) & 0xFFu);
    b[3] = (unsigned char)((value >> 24) & 0xFFu);
    zw_bytes(w, b, 4u);
}

static uint32_t exp_crc32(const unsigned char *data, size_t len)
{
    uint32_t table[256];
    uint32_t crc = 0xFFFFFFFFu;
    size_t i = 0;
    unsigned int k = 0;
    for (i = 0; i < 256u; ++i) {
        uint32_t c = (uint32_t)i;
        for (k = 0; k < 8u; ++k) {
            c = ((c & 1u) != 0u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    for (i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/** 追加一条(store:不压缩)。名字按 UTF-8 走(zip 的 0x0800 标志位)。 */
static void zw_add(zip_writer *w, const char *name, const unsigned char *data, size_t len)
{
    const size_t name_len = strlen(name);
    uint32_t crc = 0;
    if (!w->ok || w->count >= (unsigned int)(EXP_MAX_ITEMS + 2) || name_len == 0u ||
        name_len > 0xFFFFu || len > 0xFFFFFFFFu || data == NULL) {
        w->ok = 0;
        return;
    }
    crc = exp_crc32(data, len);
    zw_u32(w, 0x04034b50u); /* local file header */
    zw_u16(w, 20u);         /* version needed */
    zw_u16(w, 0x0800u);     /* flags: UTF-8 名字 */
    zw_u16(w, 0u);          /* method: store */
    zw_u16(w, w->dostime);
    zw_u16(w, w->dosdate);
    zw_u32(w, crc);
    zw_u32(w, (uint32_t)len);
    zw_u32(w, (uint32_t)len);
    zw_u16(w, (unsigned int)name_len);
    zw_u16(w, 0u); /* extra len */
    zw_bytes(w, name, name_len);
    zw_bytes(w, data, len);
    (void)snprintf(w->entries[w->count].name, sizeof(w->entries[w->count].name), "%s", name);
    w->entries[w->count].crc = crc;
    w->entries[w->count].size = (uint32_t)len;
    w->entries[w->count].offset = w->offset - (long long)len - (long long)name_len - 30;
    w->count += 1u;
}

static void zw_finish(zip_writer *w)
{
    const long long cd_start = w->offset;
    unsigned int i = 0;
    if (!w->ok) {
        return;
    }
    for (i = 0; i < w->count; ++i) {
        const zip_central *e = &w->entries[i];
        const size_t name_len = strlen(e->name);
        zw_u32(w, 0x02014b50u); /* central directory header */
        zw_u16(w, 20u);         /* version made by */
        zw_u16(w, 20u);         /* version needed */
        zw_u16(w, 0x0800u);
        zw_u16(w, 0u);
        zw_u16(w, w->dostime);
        zw_u16(w, w->dosdate);
        zw_u32(w, e->crc);
        zw_u32(w, e->size);
        zw_u32(w, e->size);
        zw_u16(w, (unsigned int)name_len);
        zw_u16(w, 0u); /* extra */
        zw_u16(w, 0u); /* comment */
        zw_u16(w, 0u); /* disk */
        zw_u16(w, 0u); /* internal attrs */
        zw_u32(w, 0u); /* external attrs */
        zw_u32(w, (uint32_t)e->offset);
        zw_bytes(w, e->name, name_len);
    }
    zw_u32(w, 0x06054b50u); /* end of central directory */
    zw_u16(w, 0u);
    zw_u16(w, 0u);
    zw_u16(w, w->count);
    zw_u16(w, w->count);
    zw_u32(w, (uint32_t)(w->offset - cd_start));
    zw_u32(w, (uint32_t)cd_start);
    zw_u16(w, 0u);
}

/* ────────────────────────── 内容打码 ────────────────────────── */

/** 逐行打码(行结构原样保留)。返回 malloc 的 UTF-8 文本,长度写进 *out_len。 */
static char *mask_text_lines(const char *text, size_t len, size_t *out_len)
{
    char line[SXCL_LOG_LINE_MAX];
    char masked[SXCL_LOG_LINE_MAX * 2];
    char *out = NULL;
    const size_t cap = len * 3u + 512u;
    size_t w = 0;
    const char *cursor = text;
    out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    while (sxcl_text_next_line(&cursor, line, sizeof(line))) {
        const int n = sxcl_log_mask_text(line, masked, sizeof(masked));
        const size_t m = (n > 0) ? (size_t)n : 0u;
        if (w + m + 2u > cap) {
            break; /* 保守:宁可少一点,也不写半个字符 */
        }
        memcpy(out + w, masked, m);
        w += m;
        out[w++] = '\n';
    }
    out[w] = '\0';
    if (out_len != NULL) {
        *out_len = w;
    }
    return out;
}

/** 读文件 -> 按探测到的编码转 UTF-8 -> 逐行打码。失败/空文件返回 NULL。 */
static char *load_masked(const exp_item *item, size_t max_bytes, size_t *len, int *truncated)
{
    char *raw = NULL;
    char *masked = NULL;
    const char *enc = "empty";
    long long got = 0;
    size_t raw_cap = 0;

    if (item->size <= 0) {
        return NULL;
    }
    raw_cap = max_bytes * 3u + 128u;
    raw = (char *)malloc(raw_cap);
    if (raw == NULL) {
        return NULL;
    }
    got = sxcl_crash_read_text(item->src, raw, raw_cap, max_bytes, &enc, truncated, NULL, 0);
    if (got <= 0) {
        free(raw);
        return NULL;
    }
    masked = mask_text_lines(raw, (size_t)got, len);
    free(raw);
    return masked;
}

/* ────────────────────────── 收集条目 ────────────────────────── */

static void push_item(exp_item *items, int *count, const char *src, const char *name)
{
    int64_t size = 0;
    int64_t mtime = 0;
    if (*count >= EXP_MAX_ITEMS) {
        return;
    }
    if (src == NULL || name == NULL || sxcl_fs_stat(src, &size, &mtime) != 0 || size <= 0) {
        return;
    }
    (void)snprintf(items[*count].src, sizeof(items[*count].src), "%s", src);
    (void)snprintf(items[*count].name, sizeof(items[*count].name), "%s", name);
    items[*count].size = size;
    items[*count].mtime_ns = mtime;
    *count += 1;
}

/** 收集启动器自己的 sxcl-*.log,按修改时间从新到旧最多 max_logs 份。 */
static int collect_launcher_logs(const char *log_dir, int max_logs, exp_item *out)
{
    sxcl_diag_dir list;
    exp_item picked[EXP_MAX_LAUNCHER_LOGS];
    int picked_n = 0;
    size_t k = 0;
    if (log_dir == NULL || *log_dir == '\0' || max_logs <= 0) {
        return 0;
    }
    if (max_logs > EXP_MAX_LAUNCHER_LOGS) {
        max_logs = EXP_MAX_LAUNCHER_LOGS;
    }
    if (sxcl_diag_dir_list(log_dir, &list) != 0) {
        return 0;
    }
    for (k = 0; k < list.count; ++k) {
        char full[SXCL_LOGS_EXPORT_PATH_MAX];
        char name[EXP_NAME_MAX];
        int64_t size = 0;
        int64_t mtime = 0;
        int at = 0;
        if (!sxcl_diag_ends_with_ci(list.names[k], ".log") ||
            strncmp(list.names[k], "sxcl-", 5) != 0) {
            continue; /* 只收我们自己的 sxcl-*.log */
        }
        if (sxcl_diag_join(full, sizeof(full), log_dir, list.names[k]) != 0) {
            continue;
        }
        if (sxcl_fs_stat(full, &size, &mtime) != 0 || size <= 0) {
            continue;
        }
        (void)snprintf(name, sizeof(name), "%s/%s", SXCL_LOGS_EXPORT_DIR_LAUNCHER, list.names[k]);
        at = picked_n;
        while (at > 0 && picked[at - 1].mtime_ns < mtime) {
            if (at < max_logs) {
                picked[at] = picked[at - 1];
            }
            --at;
        }
        if (at < max_logs) {
            if (picked_n < max_logs) {
                ++picked_n;
            }
            (void)snprintf(picked[at].src, sizeof(picked[at].src), "%s", full);
            (void)snprintf(picked[at].name, sizeof(picked[at].name), "%s", name);
            picked[at].size = size;
            picked[at].mtime_ns = mtime;
        }
    }
    for (k = 0; k < (size_t)picked_n; ++k) {
        out[k] = picked[k];
    }
    return picked_n;
}

/* ────────────────────────── 主入口 ────────────────────────── */

int sxcl_logs_export(const sxcl_logs_export_request *req, sxcl_logs_export_result *out, char *err,
                     size_t err_len)
{
    exp_item items[EXP_MAX_ITEMS];
    int count = 0;
    int skipped = 0;
    int i = 0;
    int launcher_logs = 0;
    int crash_reports = 0;
    const char *log_dir = NULL;
    size_t max_file = 0;
    int max_launcher = 0;
    int max_reports = 0;
    zip_writer zw;
    FILE *file = NULL;
    long long raw_bytes = 0;
    time_t now = 0;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (err != NULL && err_len > 0u) {
        err[0] = '\0';
    }
    if (req == NULL || req->out_zip == NULL || *req->out_zip == '\0') {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "导出日志:缺少输出路径");
        }
        return -1;
    }
    /* 先把游戏输出的缓冲冲刷掉:否则最后几十行(往往正是崩因)还不在文件里 */
    (void)sxcl_log_game_flush();

    log_dir = (req->log_dir != NULL && *req->log_dir != '\0') ? req->log_dir : sxcl_log_dir();
    max_file = (req->max_file_bytes > 0u) ? req->max_file_bytes
                                          : (size_t)SXCL_LOGS_EXPORT_MAX_FILE_BYTES;
    max_launcher = (req->max_launcher_logs > 0) ? req->max_launcher_logs : 3;
    max_reports = (req->max_crash_reports > 0) ? req->max_crash_reports : 3;
    if (max_reports > SXCL_CRASH_MAX_REPORTS) {
        max_reports = SXCL_CRASH_MAX_REPORTS;
    }

    /* ── 1) 启动器自己的日志(最新的在前)── */
    if (count < EXP_MAX_ITEMS) {
        exp_item picked[EXP_MAX_LAUNCHER_LOGS];
        const int n = collect_launcher_logs(log_dir, max_launcher, picked);
        for (i = 0; i < n && count < EXP_MAX_ITEMS; ++i) {
            items[count++] = picked[i];
            ++launcher_logs;
        }
    }

    /* ── 2) 游戏侧:latest.log / debug.log + 最近的 crash-report ── */
    if (req->game_dir != NULL && *req->game_dir != '\0') {
        char logs_dir[SXCL_LOGS_EXPORT_PATH_MAX];
        char path[SXCL_LOGS_EXPORT_PATH_MAX];
        char name[EXP_NAME_MAX];
        if (sxcl_diag_join(logs_dir, sizeof(logs_dir), req->game_dir, "logs") == 0) {
            if (sxcl_diag_join(path, sizeof(path), logs_dir, "latest.log") == 0) {
                (void)snprintf(name, sizeof(name), "%s/logs/latest.log", SXCL_LOGS_EXPORT_DIR_GAME);
                push_item(items, &count, path, name);
            }
            if (req->include_debug_log && sxcl_diag_join(path, sizeof(path), logs_dir, "debug.log") == 0) {
                (void)snprintf(name, sizeof(name), "%s/logs/debug.log", SXCL_LOGS_EXPORT_DIR_GAME);
                push_item(items, &count, path, name);
            }
        }
        {
            sxcl_crash_file reports[SXCL_CRASH_MAX_REPORTS];
            const size_t total = sxcl_crash_find_reports(req->game_dir, reports, (size_t)max_reports);
            size_t k = 0;
            for (k = 0; k < total && k < (size_t)max_reports; ++k) {
                const char *leaf = strrchr(reports[k].path, '\\');
                if (leaf == NULL) {
                    leaf = strrchr(reports[k].path, '/');
                }
                leaf = (leaf != NULL) ? leaf + 1 : reports[k].path;
                (void)snprintf(name, sizeof(name), "%s/crash-reports/%s", SXCL_LOGS_EXPORT_DIR_GAME,
                               leaf);
                push_item(items, &count, reports[k].path, name);
                ++crash_reports;
            }
        }
    }

    /* ── 3) 写 zip ── */
    if (sxcl_fs_mkdirs_for_file(req->out_zip) != 0) {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "导出日志:建不了输出目录(%s)", req->out_zip);
        }
        return -1;
    }
    file = sxcl_fs_fopen(req->out_zip, "wb");
    if (file == NULL) {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "导出日志:写不了 %s(权限/被占用?)", req->out_zip);
        }
        return -1;
    }
    memset(&zw, 0, sizeof(zw));
    zw.f = file;
    zw.ok = 1;
    now = time(NULL);
    {
        const struct tm *lt = localtime(&now);
        if (lt != NULL) {
            const unsigned int year = (unsigned int)(lt->tm_year + 1900);
            zw.dosdate = (unsigned int)(((year > 1980u ? year - 1980u : 0u) << 9) |
                                        ((unsigned int)(lt->tm_mon + 1) << 5) |
                                        (unsigned int)lt->tm_mday);
            zw.dostime = (unsigned int)(((unsigned int)lt->tm_hour << 11) |
                                        ((unsigned int)lt->tm_min << 5) |
                                        ((unsigned int)lt->tm_sec / 2u));
        }
    }

    /* 3a) 逐条:读 -> 转码 -> 打码(先算好,说明文件里要写实际字节数) */
    for (i = 0; i < count; ++i) {
        size_t masked_len = 0;
        int truncated = 0;
        char *masked = load_masked(&items[i], max_file, &masked_len, &truncated);
        if (masked == NULL) {
            ++skipped;
            continue;
        }
        if (truncated) {
            char note[256];
            const int n = snprintf(note, sizeof(note),
                                   "(本文件超过 %llu 字节,只导出了末尾一段;打码已生效)\n",
                                   (unsigned long long)max_file);
            char *merged = NULL;
            if (n > 0) {
                merged = (char *)malloc((size_t)n + masked_len + 1u);
                if (merged != NULL) {
                    memcpy(merged, note, (size_t)n);
                    memcpy(merged + (size_t)n, masked, masked_len);
                    zw_add(&zw, items[i].name, (const unsigned char *)merged,
                           (size_t)n + masked_len);
                    free(merged);
                }
            }
            if (merged == NULL) {
                zw_add(&zw, items[i].name, (const unsigned char *)masked, masked_len);
            }
        } else {
            zw_add(&zw, items[i].name, (const unsigned char *)masked, masked_len);
        }
        raw_bytes += (long long)masked_len;
        free(masked);
        if (!zw.ok) {
            break;
        }
    }

    /* 3b) 说明文件(路径也要打码:Windows 用户名常出现在路径里) */
    {
        char when[64];
        char safe_out[SXCL_LOGS_EXPORT_PATH_MAX * 2];
        char body[8192];
        size_t w = 0;
        const struct tm *lt = localtime(&now);
        when[0] = '\0';
        if (lt != NULL) {
            (void)strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", lt);
        }
        (void)sxcl_log_mask_text(req->out_zip, safe_out, sizeof(safe_out));
        w = (size_t)snprintf(body, sizeof(body),
                             "Silent X Craft Launcher 日志导出包\n"
                             "导出时间: %s\n"
                             "导出文件: %d 个(共 %lld 字节,打包方式 store 不压缩)\n"
                             "打码: 包内所有文本都经过打码(token / UUID / 玩家名 / "
                             "路径里的用户名 -> ***)\n"
                             "编码: 游戏侧文件按探测到的编码转成 UTF-8\n"
                             "输出: %s\n\n条目:\n",
                             when[0] != '\0' ? when : "(未知)", count - skipped, raw_bytes,
                             safe_out[0] != '\0' ? safe_out : "(已打码)");
        for (i = 0; i < count && w + 320u < sizeof(body); ++i) {
            char safe_src[SXCL_LOGS_EXPORT_PATH_MAX * 2];
            char safe_name[EXP_NAME_MAX * 2];
            (void)sxcl_log_mask_text(items[i].src, safe_src, sizeof(safe_src));
            (void)sxcl_log_mask_text(items[i].name, safe_name, sizeof(safe_name));
            w += (size_t)snprintf(body + w, sizeof(body) - w, "  %s  (%lld 字节, 源 %s)\n",
                                  safe_name, items[i].size, safe_src);
        }
        if (count - skipped == 0) {
            w += (size_t)snprintf(body + w, sizeof(body) - w,
                                  "  (没有找到可打包的日志:启动器日志目录为空,游戏目录也没有 "
                                  "logs/latest.log 或 crash-reports/*.txt)\n");
        }
        zw_add(&zw, SXCL_LOGS_EXPORT_MANIFEST, (const unsigned char *)body, (size_t)w);
    }

    zw_finish(&zw);
    (void)fclose(file);

    if (zw.ok) {
        if (out != NULL) {
            out->files = count - skipped;
            out->entries = (int)zw.count;
            out->skipped = skipped;
            out->raw_bytes = raw_bytes;
            out->zip_bytes = zw.offset;
            (void)snprintf(out->out_path, sizeof(out->out_path), "%s", req->out_zip);
            (void)snprintf(out->manifest_path, sizeof(out->manifest_path), "%s",
                           SXCL_LOGS_EXPORT_MANIFEST);
            if (launcher_logs == 0 && crash_reports == 0) {
                (void)snprintf(out->note, sizeof(out->note),
                               "没有找到启动器日志与崩溃报告(游戏目录为空或还没跑过?)");
            } else if (crash_reports == 0) {
                (void)snprintf(out->note, sizeof(out->note),
                               "crash-reports 里没有非空报告;包里有启动器日志 %d 份", launcher_logs);
            }
        }
        return 0;
    }

    if (err != NULL && err_len > 0u) {
        (void)snprintf(err, err_len, "导出日志:写 %s 失败(磁盘满/被占用?)", req->out_zip);
    }
    return -1;
}

const char *sxcl_logs_export_status_text(int rc)
{
    return (rc == 0) ? "导出成功" : "导出失败";
}
