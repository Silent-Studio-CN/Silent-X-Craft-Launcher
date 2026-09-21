/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LOGEXPORT_H
#define SXCL_LOGEXPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一键导出日志包:把**我们自己的** logs/sxcl-*.log + 游戏侧的 logs/latest.log 与
 * crash-reports/*.txt 打成一个 zip,交给用户/工单。
 *
 * 三条硬约定:
 *   1. **必须打码**:token / UUID / 玩家名 / 路径里的用户名(见 sxcl_log_mask_text)在
 *      写进压缩包之前就被换成 ***。导出包是要发给别人的,身份信息不能跟着走。
 *   2. **编码统一**:游戏侧文件先探编码再转 UTF-8(GBK 的 latest.log 不会变成乱码字节)。
 *   3. **不压缩,只打包**:zip 条目用 store(method 0)。仓库里只有 zip 的**读**侧,
 *      写侧为了不留压缩 bug 只做"原样打包";文件名与目录结构是标准 zip,
 *      任何解压工具(以及我们自己的 sxcl_zip_open)都能读。
 *
 * 体积:每条最多 SXCL_LOGS_EXPORT_MAX_FILE_BYTES(默认 4 MiB,超出只取末尾),
 * 默认最多 3 份启动器日志 + 3 份 crash-report;包本身有上限,不会把用户磁盘打满。
 */

/** 单条最多导出多少字节(默认值;超出只导出末尾并加一行说明)。 */
#define SXCL_LOGS_EXPORT_MAX_FILE_BYTES (4u * 1024u * 1024u)
/** 条目名前缀(zip 里的目录结构)。 */
#define SXCL_LOGS_EXPORT_DIR_LAUNCHER "sxcl-logs"
#define SXCL_LOGS_EXPORT_DIR_GAME     "game"
/** 包内的说明文件条目名(能看到"导出了什么、哪一份是什么编码")。 */
#define SXCL_LOGS_EXPORT_MANIFEST     "EXPORT-INFO.txt"
/** 路径与错误缓冲长度。 */
#define SXCL_LOGS_EXPORT_PATH_MAX 640
#define SXCL_LOGS_EXPORT_ERROR_MAX 256

/** 一次导出的请求。字符串一律 UTF-8,生命周期由调用方保证。 */
typedef struct sxcl_logs_export_request {
    const char *game_dir;         /**< 可空:游戏目录(取它的 logs/ 与 crash-reports/) */
    const char *log_dir;          /**< 可空 = sxcl_log_dir()(启动器自己的日志目录) */
    const char *out_zip;          /**< **必填**:输出 zip 全路径(父目录会自动建) */
    int max_launcher_logs;        /**< <=0 = 3:最多打包几份 sxcl-*.log(最新的优先) */
    int max_crash_reports;        /**< <=0 = 3:最多打包几份 crash-report(最新的优先) */
    int include_debug_log;        /**< 1 = 连 <game>/logs/debug.log 一起打包 */
    size_t max_file_bytes;        /**< 0 = SXCL_LOGS_EXPORT_MAX_FILE_BYTES */
} sxcl_logs_export_request;

/** 一次导出的结果(全部字段都会填,失败时是 0/空串)。 */
typedef struct sxcl_logs_export_result {
    int files;                    /**< 打进包里的**源文件**数(不含说明文件) */
    int entries;                  /**< zip 条目总数(源文件 + 说明文件) */
    int skipped;                  /**< 因为空/读不了被跳过的文件数 */
    long long raw_bytes;          /**< 打包前读进来的总字节数 */
    long long zip_bytes;          /**< 产出的 zip 文件大小 */
    char out_path[SXCL_LOGS_EXPORT_PATH_MAX];
    char manifest_path[SXCL_LOGS_EXPORT_PATH_MAX]; /**< 用户能看到的那份说明文件在包里的条目名 */
    char note[192];               /**< 人话补充:"没找到启动器日志" 之类(可空) */
} sxcl_logs_export_result;

/** 导出。返回 0 成功;-1 = 参数不合法/写不了 out_zip(原因写进 err)。
 *  "一个日志都没找到"**不算失败**:包里仍会有说明文件,note 里写清楚。 */
int sxcl_logs_export(const sxcl_logs_export_request *request, sxcl_logs_export_result *out,
                     char *err, size_t err_len);

/** 稳定英文键 -> 人话(给界面直接用):"ok"/"no_input"… */
const char *sxcl_logs_export_status_text(int rc);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LOGEXPORT_H */
