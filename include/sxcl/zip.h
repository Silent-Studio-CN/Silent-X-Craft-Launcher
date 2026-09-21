/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_ZIP_H
#define SXCL_ZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 返回值约定(解压类接口共用)。 */
#define SXCL_ZIP_OK           0    /**< 成功 */
#define SXCL_ZIP_ERR         (-1)  /**< 没有这个条目 / 打不开 / 数据损坏 / 落盘失败 */
#define SXCL_ZIP_ERR_SPACE   (-2)  /**< 缓冲不够(只有 extract_memory 会返回) */
#define SXCL_ZIP_ERR_UNSUP   (-3)  /**< 不支持:压缩方法、加密、ZIP64、目录条目 */
#define SXCL_ZIP_ERR_ABORT   (-4)  /**< sink 返回非 0 主动中止 */

/** 不透明句柄。 */
typedef struct sxcl_zip sxcl_zip;

/** 打开 zip 文件(只读)。打不开 / 不是 zip 返回 NULL。
 *  路径按 UTF-8 处理(Windows 侧内部转 UTF-16,不受代码页影响)。 */
sxcl_zip *sxcl_zip_open(const char *path);

/** 关闭句柄(允许传 NULL)。 */
void sxcl_zip_close(sxcl_zip *zip);

/** 条目总数(含目录条目,按中央目录顺序)。 */
size_t sxcl_zip_count(const sxcl_zip *zip);

/** 第 index 个条目的名字(UTF-8,指向内部存储,不要 free;越界返回 NULL)。 */
const char *sxcl_zip_name_at(const sxcl_zip *zip, size_t index);

/** 第 index 个条目解压后的大小(字节;越界返回 -1)。 */
int64_t sxcl_zip_size_at(const sxcl_zip *zip, size_t index);

/** 第 index 个条目的压缩方法(0 = stored,8 = deflate;越界返回 -1)。 */
int sxcl_zip_method_at(const sxcl_zip *zip, size_t index);

/** 按名字找条目(大小写敏感)。返回下标;-1 = 没有(同时是名字非法/zip 为 NULL 的返回)。 */
int sxcl_zip_find(const sxcl_zip *zip, const char *name);

/** 解到内存。成功返回 0 并把实际字节数写进 *out_len。
 *  缓冲不够返回 -2,*out_len 写入"实际需要多少字节"(条目原始大小)。
 *  其它失败返回 -1 / -3。 */
int sxcl_zip_extract_memory(sxcl_zip *zip, const char *name, void *out, size_t out_cap, size_t *out_len);

/** 解到文件:自动建父目录,先写 <dest>.tmp 再原子改名,失败不留 .tmp。
 *  成功返回 0,失败返回 -1 / -3。 */
int sxcl_zip_extract_file(sxcl_zip *zip, const char *name, const char *dest);

/** 逐块解出(大条目用,内存里只占一个解压窗口 + 一块读缓冲):
 *  每解出一块就调用 sink,sink 返回非 0 中止并让本函数返回 -4。
 *  成功返回 0,失败返回 -1 / -3。 */
int sxcl_zip_extract_stream(sxcl_zip *zip, const char *name,
                            int (*sink)(void *ud, const void *data, size_t len), void *ud);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_ZIP_H */
