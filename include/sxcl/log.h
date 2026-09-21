/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LOG_H
#define SXCL_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SXCL-C 运行日志:**从进程启动那一刻起**的一条时间线,纯 C11、零第三方依赖。
 *
 * 为什么要有它(现状是实测出来的,不是洁癖):
 *   * 排查全靠 fprintf(stderr),而**安卓上 stderr 不进 logcat**
 *     —— adb logcat -s sxcl-ui 是空的,真机只能靠"清单缓存字节数/控件几何"间接读数;
 *   * 现有的 uiTrace() 只在 SXCL_UI_TRACE=1 时输出,没有级别、没有文件、没有时间戳规范、
 *     没有轮转:用户报障时我们手上什么都没有,而且开一整天会刷爆控制台。
 * 所以这里给一条**可留档、可限制、可分级**的通道:文件(必须)+ stderr(桌面)+ logcat(安卓)。
 *
 * ── 级别 ──
 *   error < warn < info < debug(数值越大越啰嗦;默认 info)。
 *   debug 由环境变量 SXCL_LOG_LEVEL=debug 或设置文件 log.level=debug 打开。
 *   SXCL_LOG_LEVEL=off 关掉文件与终端输出(断言/压测时用)。
 *
 * ── 输出目标 ──
 *   * 文件(**必须**):<配置目录>/logs/sxcl-YYYYMMDD-HHMMSS.log
 *     单文件上限默认 8 MiB、保留份数默认 10 —— 写满就换新文件并把最旧的删掉,
 *     绝不让日志把用户磁盘吃满。
 *   * stderr(桌面默认开;SXCL_LOG_STDERR=0/1 可改):
 *     与现有 fprintf 走同一个流,顺序可对照。
 *   * logcat(安卓):tag 固定 "sxcl",所以 adb logcat -s sxcl 能拿到全部日志
 *     (打包层另有 stderr->logcat 的转发,现有 fprintf 也一并可见)。
 *
 * ── 行格式(稳定,便于 grep/统计) ──
 *   YYYY-MM-DD HH:MM:SS.mmm LEVEL module | message
 *   例:2026-09-16 13:12:20.123 INFO  startup | SXCL 启动 版本=0.1.0 ...
 *   module 是**稳定短键**:startup / paths / net / install / launch / ui / error …(<=16 字符)。
 *
 * ── 线程安全与降级 ──
 *   多工作线程可同时写:每行一次 fwrite(整行原子),内部有锁。
 *   **失败绝不影响主流程**:目录建不出来/文件打不开/磁盘满 -> 静默降级(只丢日志,
 *   不返回错误给业务、不抛异常、不卡主线程);丢弃条数在 sxcl_log_stats.dropped 里。
 *
 * ── 环境变量(全部可选) ──
 *   SXCL_LOG_LEVEL       error|warn|info|debug|off(默认 info)
 *   SXCL_LOG_DIR         日志目录(默认 <配置目录>/logs)
 *   SXCL_LOG_MAX_BYTES   单文件上限字节数(默认 8388608)
 *   SXCL_LOG_KEEP        保留份数(默认 10)
 *   SXCL_LOG_STDERR      0/1(桌面默认 1,安卓默认 0 —— 安卓走 logcat,免得每条打印两遍)
 *   设置文件里的 log.level 在环境变量缺席时生效(设置开关,不改环境也能开 debug)。
 *   配置目录口径与 sxcl_settings_default_dir 同一份(SXCL_CONFIG_DIR 覆盖);
 *   SXCL_UI_SETTINGS 也认(界面层钉死设置文件时,log.level 从同一个文件读)。
 */

/* ── 返回码 ── */
#define SXCL_LOG_OK        0
#define SXCL_LOG_ERR_ARG (-1)
#define SXCL_LOG_ERR_IO  (-2)   /**< 目录/文件打不开(调用方可忽略:日志已静默降级) */

/** 人话错误缓冲长度。 */
#define SXCL_LOG_ERROR_MAX 256

/* ── 级别 ── */
#define SXCL_LOG_ERROR 0
#define SXCL_LOG_WARN  1
#define SXCL_LOG_INFO  2
#define SXCL_LOG_DEBUG 3
#define SXCL_LOG_OFF   4   /**< 关(不入文件、不打终端) */

/** 单文件上限默认值(8 MiB)与保留份数默认值(10)。 */
#define SXCL_LOG_DEFAULT_MAX_BYTES (8u * 1024u * 1024u)
#define SXCL_LOG_DEFAULT_KEEP      10

/** 一行最多多少字节(超出截断并标注)。 */
#define SXCL_LOG_LINE_MAX 2048
/** URL 打码时最多看多少字节(超出截断并标注总长度)。 */
#define SXCL_LOG_URL_MAX 512

/** 初始化(幂等:第二次调用是空操作,返回 SXCL_LOG_OK)。
 *  dir 为 NULL = 用 SXCL_LOG_DIR,再退回 <配置目录>/logs。
 *  失败(建不了目录/打不开文件)返回 SXCL_LOG_ERR_IO 并在 err 里写原因 ——
 *  但**日志通道仍然可用**(stderr/logcat),业务照常继续。 */
int sxcl_log_init(const char *dir, char *err, size_t err_len);

/** 收尾:刷盘 + 关文件(进程退出前调用;之后可以再 init,测试用得上)。 */
void sxcl_log_shutdown(void);

/** 设置级别(返回设置前的级别)。写入不用加锁也没关系:级别是单个 int。 */
int sxcl_log_set_level(int level);
/** 当前级别(未初始化时按默认 info 算)。 */
int sxcl_log_level(void);
/** 级别名("ERROR"/"WARN"/"INFO"/"DEBUG"/"OFF");认不出返回 "?"。 */
const char *sxcl_log_level_name(int level);
/** 名字/数字 -> 级别;认不出返回 -1(error 是 0,不能用 0 表示失败)。 */
int sxcl_log_level_from_name(const char *text);
/** 这个级别现在会不会被记(调用方可以拿它挡掉昂贵的格式化)。 */
int sxcl_log_enabled(int level);

/** 写一行。module 为空按 "-" 处理;fmt 里的 % 由实现转义,调用方给真实格式串。
 *  未初始化也能调:只走终端通道(启动最初的几行不会丢)。 */
void sxcl_log_write(int level, const char *module, const char *fmt, ...);

/* 便捷宏(名字带下划线,避免与各平台/打包层已有的 SXCL_LOGI 之类撞车)。 */
#define SXCL_LOG_E(module, ...) sxcl_log_write(SXCL_LOG_ERROR, (module), __VA_ARGS__)
#define SXCL_LOG_W(module, ...) sxcl_log_write(SXCL_LOG_WARN, (module), __VA_ARGS__)
#define SXCL_LOG_I(module, ...) sxcl_log_write(SXCL_LOG_INFO, (module), __VA_ARGS__)
#define SXCL_LOG_D(module, ...) sxcl_log_write(SXCL_LOG_DEBUG, (module), __VA_ARGS__)

/** 运行期改上限/保留份数(验收"把上限调小跑一次"用它;0 = 保持原值)。 */
void sxcl_log_set_limits(size_t max_bytes, int keep_files);
size_t sxcl_log_max_bytes(void);
int sxcl_log_keep_files(void);

/** 当前日志文件全路径(未开文件时是空串);指针归模块所有,别 free。 */
const char *sxcl_log_file_path(void);
/** 日志目录(未初始化时是空串)。 */
const char *sxcl_log_dir(void);

/** 立刻换一个新文件(测试/验收用;正常由写满自动触发)。 */
int sxcl_log_rotate_now(void);
/** 刷盘(崩溃前想留证据时用)。 */
void sxcl_log_flush(void);

/** 运行统计:写进文件的行数/字节数、被丢弃的行数、轮转与删除次数。
 *  验收要的"一次运行里日志行数"就取这里,不用去数文件。 */
typedef struct sxcl_log_stats {
    unsigned long long lines;     /**< 真正写进文件的行数 */
    unsigned long long bytes;     /**< 写进文件的字节数 */
    unsigned long long dropped;   /**< 静默降级丢掉的行数(文件不可写/写失败) */
    unsigned int rotations;       /**< 换过几次文件(不含首次打开) */
    unsigned int pruned;          /**< 删掉过几个过期文件 */
} sxcl_log_stats;
void sxcl_log_get_stats(sxcl_log_stats *out);

/** URL 打码:把敏感查询参数的值换成 ***,过长的截断并标注总长度。
 *  与界面错误报告同一口径 —— 日志里**绝不出现** token/授权码明文的第二份。
 *  典型:.../auth?code=abc&x=1 -> .../auth?code=***&x=1
 *  返回写入字节数(不含 NUL);参数不合法返回 SXCL_LOG_ERR_ARG。 */
int sxcl_log_mask_url(const char *url, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LOG_H */
