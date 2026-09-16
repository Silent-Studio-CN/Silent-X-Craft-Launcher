/* SXCL-C 文件系统小工具(只做启动器真正需要的几件事,不追求完整 POSIX 兼容)。
 * 路径一律 UTF-8;Windows 侧内部转宽字符,避免代码页问题。 */
#ifndef SXCL_FS_H
#define SXCL_FS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 取文件大小与修改时间(纳秒)。成功后 size/mtime_ns 可传 NULL 表示不关心。
 *  返回 0 成功;文件不存在返回 -1;不可访问返回 -2;参数非法返回 -3。 */
int sxcl_fs_stat(const char *path, int64_t *size, int64_t *mtime_ns);

/** 是否存在(且是文件)。1 是,0 否。 */
int sxcl_fs_exists(const char *path);

/** 是否目录。1 是,0 否。 */
int sxcl_fs_is_dir(const char *path);

/** 递归建目录(已存在视为成功)。返回 0 成功。 */
int sxcl_fs_mkdirs(const char *path);

/** 原子替换:把 src 改名到 dst(覆盖已存在的 dst)。返回 0 成功。
 *  下载落盘用:先写 .part,校验通过后再原子改名,避免半成品被当成正式文件。 */
int sxcl_fs_rename_replace(const char *src, const char *dst);

/** 删除文件(不存在视为成功)。返回 0 成功。 */
int sxcl_fs_remove(const char *path);

/** 保证父目录存在(用于建文件前)。返回 0 成功。 */
int sxcl_fs_mkdirs_for_file(const char *path);

/* ── 下载落盘用的文件句柄(支持按绝对偏移写入,分片之间互不干扰) ── */

typedef struct sxcl_file sxcl_file;

/** 打开(创建/截断)并按 final_size 预分配。final_size < 0 表示不预分配。
 *  失败返回 NULL。 */
sxcl_file *sxcl_file_open_write(const char *path, int64_t final_size);

/** 在 offset 处写入 len 字节(必须整块写完才算成功)。返回写入字节数,<0 表示出错。 */
int64_t sxcl_file_write_at(sxcl_file *file, const void *data, size_t len, int64_t offset);

/** 把已写入的内容刷到磁盘。返回 0 成功。 */
int sxcl_file_flush(sxcl_file *file);

/** 关闭句柄。返回 0 成功。 */
int sxcl_file_close(sxcl_file *file);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_FS_H */
