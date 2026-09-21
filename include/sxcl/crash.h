/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_CRASH_H
#define SXCL_CRASH_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 崩溃取证:进程退出后去**游戏自己写的那两份文件**里找死因。
 *
 * 为什么非做不可(现状):我们的归类只吃 stdout/stderr 一路,而 Java 进程一崩,
 * stdout 早就断了 —— 真正的死因写在两处:
 *   1) <game>/crash-reports/crash-YYYY-MM-DD_HH.MM.SS-client.txt
 *      带 "---- Minecraft Crash Report ----" 头、"Description:" 与 "Caused by:" 链;
 *   2) <game>/logs/latest.log
 *      崩之前的最后几百行(模组列表、Java 版本、GL 上下文、最后一条 mod 日志)。
 * 只读 stdout 就等于把这两份证据全丢了。所以本模块负责:
 *   * 找到**最近的一份非空** crash-report(不把整个历史目录读进来);
 *   * 按探测到的编码读成 UTF-8(中文 Windows 的日志是 GBK,直接按 UTF-8 读会乱码);
 *   * 逐行喂给调用方的回调(启动层把它接进 logscan —— 与 stdout 同一套分析)。
 *
 * 本模块**只读文件、不起进程、不联网、不改游戏目录里的任何东西**。
 */

/** 路径缓冲长度(含 NUL)。 */
#define SXCL_CRASH_PATH_MAX 640
/** 一次最多看几份 crash-report(非空、按修改时间从新到旧)。 */
#define SXCL_CRASH_MAX_REPORTS 8
/** 单个文件最多读多少字节(默认 4 MiB,超过就只读**尾部** —— 崩因在最后)。 */
#define SXCL_CRASH_DEFAULT_MAX_BYTES (4u * 1024u * 1024u)
/** 人话错误缓冲长度。 */
#define SXCL_CRASH_ERROR_MAX 256

/** crash-reports 里的一个候选文件。 */
typedef struct sxcl_crash_file {
    char path[SXCL_CRASH_PATH_MAX];
    int64_t size;     /**< 字节数 */
    int64_t mtime_ns; /**< 修改时间(纳秒;排序用) */
} sxcl_crash_file;

/** 一次取证的**事实**(不带判断,判断在 logscan 的原因表里):
 *  读了哪个文件、多少行、什么编码、描述是什么。界面"查看详情"直接显示这些。 */
typedef struct sxcl_crash_evidence {
    char report_path[SXCL_CRASH_PATH_MAX]; /**< 实际读的那份报告(没读到就是空串) */
    char report_encoding[SXCL_TEXT_ENCODING_MAX];
    char report_description[192];          /**< crash-report 的 "Description: xxx" */
    long long report_bytes;                /**< 文件大小(不是读进来的字节数) */
    long long report_lines;                /**< 喂进分析的行数 */
    int reports_total;                     /**< crash-reports 里非空报告的**总**份数 */
    char latest_log_path[SXCL_CRASH_PATH_MAX];
    char latest_log_encoding[SXCL_TEXT_ENCODING_MAX];
    long long latest_log_bytes;
    long long latest_log_lines;            /**< latest.log 的**总**行数(含没读进来的部分) */
    int latest_log_truncated;              /**< 1 = 文件太大,只读了尾部 */
    long long latest_log_fed;              /**< 实际喂进分析的行数 */
    char debug_log_path[SXCL_CRASH_PATH_MAX];
    char debug_log_encoding[SXCL_TEXT_ENCODING_MAX];
    long long debug_log_lines;
    int used_debug_log;                    /**< 1 = latest.log 缺失/为空,退到了 debug.log */
    /* 老版本兜底:1.7 之前的 MC 写的是 logs/client-YYYY-MM-DD_N.log(没有 latest.log)。
     * 实测:MC 1.0 就是这种 —— 不认它的话,取证在"老版本"这一类上直接是空的。 */
    char legacy_log_path[SXCL_CRASH_PATH_MAX];
    char legacy_log_encoding[SXCL_TEXT_ENCODING_MAX];
    long long legacy_log_lines;
    int used_legacy_log;                   /**< 1 = 用的是 logs/client-*.log */
    int scanned;                           /**< 1 = 真的扫过游戏目录(哪怕一个文件都没有) */
} sxcl_crash_evidence;

/* 行来源(喂给回调的 source 参数)。 */
#define SXCL_CRASH_SRC_REPORT     0
#define SXCL_CRASH_SRC_LATEST_LOG 1
#define SXCL_CRASH_SRC_DEBUG_LOG  2

/** 逐行回调:source 见上面三个宏。返回非 0 = 请求停止(scan 会提前结束,已读的行数照常返回)。 */
typedef int (*sxcl_crash_line_fn)(void *userdata, int source, const char *line);

/* 扫什么(位掩码)。 */
#define SXCL_CRASH_SCAN_REPORTS 0x1u /**< 最近的非空 crash-report */
#define SXCL_CRASH_SCAN_LATEST  0x2u /**< <game>/logs/latest.log */
#define SXCL_CRASH_SCAN_DEBUG   0x4u /**< <game>/logs/debug.log(只在 latest.log 缺失/为空时才读) */
#define SXCL_CRASH_SCAN_ALL     0x7u

/** 找 <game>/crash-reports 下**非空**的 *.txt,按修改时间从新到旧。
 *  out 可为 NULL(cap=0 时只数个数)。返回非空报告的总份数;
 *  写进 out 的最多 cap 条(最新的在前)。目录不存在返回 0。 */
size_t sxcl_crash_find_reports(const char *game_dir, sxcl_crash_file *out, size_t cap);

/** 数一个文件的行数(流式,不把文件读进内存)。打不开返回 -1。 */
long long sxcl_crash_count_lines(const char *path);

/** 按探测到的编码把文件读成 UTF-8。max_bytes > 0 且文件更大时只读**尾部**
 *  (并对齐到行首);truncated(可空)置 1。encoding_out(可空)给稳定编码键。
 *  返回写进 out 的字节数;-1 = 打不开(out 置空串,err 写原因)。 */
long long sxcl_crash_read_text(const char *path, char *out, size_t out_cap, size_t max_bytes,
                               const char **encoding_out, int *truncated, char *err, size_t err_len);

/** 从整篇 crash-report 文本里抽 "Description: xxx"(行首、允许前导空白)。
 *  找到返回 1 并写进 out;没有返回 0。 */
int sxcl_crash_report_description(const char *text, char *out, size_t out_cap);

/** 这一行是不是 crash-report 的结构行(---- Minecraft Crash Report ---- / Description: /
 *  Caused by: / Time: / Stacktrace: / -- Head -- …)。给界面高亮与测试用。 */
int sxcl_crash_report_marker(const char *line);

/** 主入口:扫 crash-report + latest.log(必要时 debug.log),逐行交给 on_line。
 *  on_line 可为 NULL(只取事实)。
 *  max_bytes = 0 用 SXCL_CRASH_DEFAULT_MAX_BYTES。
 *  out(可空)拿事实;err(可空)拿人话原因。
 *  返回喂出的**总行数**;-1 = 参数不合法(缺游戏目录)。 */
long long sxcl_crash_scan(const char *game_dir, unsigned flags, size_t max_bytes,
                          sxcl_crash_line_fn on_line, void *userdata, sxcl_crash_evidence *out,
                          char *err, size_t err_len);

/** 编码键 -> 人话("UTF-8"/"UTF-8(带 BOM)"/"UTF-16LE"/"GBK(中文 Windows 的 ANSI)"/…)。 */
const char *sxcl_crash_encoding_name(const char *encoding);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_CRASH_H */
