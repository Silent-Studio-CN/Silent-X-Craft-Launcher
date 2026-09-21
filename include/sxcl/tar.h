/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_TAR_H
#define SXCL_TAR_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/xz.h"   /* sxcl_xz_source(一步到位解 .tar.xz 用) */

#ifdef __cplusplus
extern "C" {
#endif

/* ── tar 解包(ustar / POSIX pax / GNU 长名):本项目自己实现 ──
 *
 * 用途:JRE 自托管包是 \`universal.tar.xz\` + \`bin-<abi>.tar.xz\`(见 docs/19),
 * 所以除了 XZ 还要能解 tar。仓库里没有 tar 能力,这里按 POSIX ustar + pax 规范自己写。
 *
 * 支持:
 *   * 头:512 字节 ustar(name 100 + prefix 155 拼接)、GNU 'L'/'K' 长名、POSIX pax 'x'/'g'
 *     (path / linkpath / size 三类记录);
 *   * 类型:'0' 与 '\0' 普通文件、'5' 目录、'2' 符号链接、'1' 硬链接;
 *   * mode:'0o111 位 -> 补可执行位(POSIX;Windows 没这个概念,静默跳过);目录自动建;
 *   * 安全:拒绝绝对路径 / 盘符 / 反斜杠 / ".." 分量 / 控制字符 / 超长路径,
 *     一律返回 SXCL_TAR_ERR_UNSAFE 并点名 —— 绝不让归档里的路径写到 dest 之外;
 *   * 收尾:认两个全 0 块为归档结束;其后只允许 0(填充),多一个非 0 就报数据错。
 *
 * **不支持**(如实报 SXCL_TAR_ERR_UNSUPPORTED,不假装解过):
 *   * 设备节点 / FIFO('3'/'4'/'6')、GNU sparse 文件、'S' 稀疏标记。
 *
 * 用法:open(dest) -> feed(数据块)* -> finish()。数据块从 sxcl/xz.h 的 sink 里来,
 * 或者直接用 sxcl_tar_extract_file() 一步到位(内部就是 xz -> tar)。
 */

#define SXCL_TAR_OK               0
#define SXCL_TAR_ERR_DATA       (-1) /**< tar 结构不对 / 截断 / 校验和不符 */
#define SXCL_TAR_ERR_IO         (-2) /**< 建目录/写文件失败 */
#define SXCL_TAR_ERR_UNSUPPORTED (-3)/**< 用到了不支持的条目类型 */
#define SXCL_TAR_ERR_UNSAFE     (-4) /**< 归档里的路径不安全(绝对/".."/盘符/反斜杠) */
#define SXCL_TAR_ERR_NOMEM      (-5) /**< 内存不足 */
#define SXCL_TAR_ERR_ARG        (-6) /**< 参数不合法 */
#define SXCL_TAR_ERR_ABORT      (-7) /**< on_entry / is_cancelled 主动中止 */
#define SXCL_TAR_ERR_LIMIT      (-8) /**< 解出来的总字节超过 max_total */

/** 返回码的稳定名字。 */
const char *sxcl_tar_code_name(int code);

typedef enum sxcl_tar_type {
    SXCL_TAR_FILE = 0,
    SXCL_TAR_DIR = 1,
    SXCL_TAR_SYMLINK = 2,
    SXCL_TAR_HARDLINK = 3
} sxcl_tar_type;

/** 一个条目(回调里"看一眼"用,指针在回调返回后失效)。 */
typedef struct sxcl_tar_entry {
    const char *path;   /**< 归档里的相对路径(已去掉前导 "./") */
    const char *dest;   /**< 落到磁盘上的全路径(dest_dir + path) */
    int64_t size;
    int mode;           /**< tar 里的 mode(低 12 位) */
    sxcl_tar_type type;
    const char *link;   /**< 链接目标(可空串) */
} sxcl_tar_entry;

typedef struct sxcl_tar_opts {
    const char *dest_dir;   /**< 解到哪个目录(必需;不存在会被建出来) */
    /** 每个条目**写出之后**报一次(目录/文件/链接都报);返回非 0 中止解包(ERR_ABORT)。 */
    int (*on_entry)(void *ud, const sxcl_tar_entry *entry);
    void *ud;
    /** 用户取消探针(可空):非 0 = 中止(ERR_ABORT)。 */
    int (*is_cancelled)(void *ud);
    /** 解出来的文件总字节上限(>0 时生效);这是防解压炸弹的第二道闸。 */
    int64_t max_total;
    /** 1 = 允许把符号链接当目录前缀(默认 0:链接就是链接,不会跟着它往下写)。 */
    int follow_symlink_dirs;
} sxcl_tar_opts;

typedef struct sxcl_tar sxcl_tar;

/** 建解包器。失败返回 NULL 并写 err。 */
sxcl_tar *sxcl_tar_open(const sxcl_tar_opts *opts, char *err, size_t err_len);
/** 喂一块数据(顺序)。返回 SXCL_TAR_OK;出错返回负错误码(状态是"粘"的)。 */
int sxcl_tar_feed(sxcl_tar *tar, const void *data, size_t len);
/** 收尾:核对"归档结束"。成功返回 SXCL_TAR_OK。 */
int sxcl_tar_finish(sxcl_tar *tar, char *err, size_t err_len);
/** 释放(允许 NULL;没写完的文件会被删掉,不留半个文件)。 */
void sxcl_tar_close(sxcl_tar *tar);

/** 统计(给人看的进度/取证):写出的文件字节数、文件数、目录数、链接数。 */
int64_t sxcl_tar_bytes_out(const sxcl_tar *tar);
size_t sxcl_tar_files_written(const sxcl_tar *tar);
size_t sxcl_tar_dirs_created(const sxcl_tar *tar);
size_t sxcl_tar_links_created(const sxcl_tar *tar);
/** 已经看到归档结束标记(两个全 0 块)了没。 */
int sxcl_tar_is_done(const sxcl_tar *tar);

/** 一步到位:.tar.xz(数据从 sxcl_xz_source 拉)。
 *  err 里写人话;progress 回调从本函数的调用线程触发(不额外开线程)。 */
int sxcl_tar_extract_xz(const sxcl_xz_source *source, const sxcl_tar_opts *opts,
                        char *err, size_t err_len);

/** 一步到位:本地 .tar.xz 文件。 */
int sxcl_tar_extract_file(const char *archive_path, const sxcl_tar_opts *opts,
                          char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_TAR_H */
