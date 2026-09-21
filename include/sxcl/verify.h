/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_VERIFY_H
#define SXCL_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/hash.h"
#include "sxcl/hashcache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxcl_verify_status {
    SXCL_VERIFY_OK = 0,      /**< 通过 */
    SXCL_VERIFY_MISSING = 1, /**< 文件不存在 */
    SXCL_VERIFY_SIZE = 2,    /**< 大小不符 */
    SXCL_VERIFY_HASH = 3,    /**< 哈希不符 */
    SXCL_VERIFY_IO = 4,      /**< 读写失败 */
    SXCL_VERIFY_BAD_ARG = 5  /**< 参数非法 */
} sxcl_verify_status;

typedef struct sxcl_verify_result {
    sxcl_verify_status status;
    int64_t size;       /**< 实际大小(取不到时为 -1) */
    int from_cache;     /**< 1 = 摘要是哈希缓存命中来的,没有重算 */
    char actual_hex[65];/**< 实际十六进制摘要(SHA-1 40 / SHA-256 64 字符);未计算时为空串 */
} sxcl_verify_result;

/** 状态名(日志用)。 */
const char *sxcl_verify_status_name(sxcl_verify_status status);

/** 流式计算文件摘要(4MiB 分块,内存占用与文件大小无关)。返回 0 成功,-2 读写失败,-3 参数非法。 */
int sxcl_hash_file(const char *path, sxcl_hash_algo algo, char *out_hex, size_t out_len);

/** 校验单个文件。out 可为 NULL。 */
sxcl_verify_status sxcl_verify_file(const char *path, int64_t expected_size,
                                    const char *expected_hex, sxcl_hash_algo algo,
                                    sxcl_verify_result *out);

/** 同上,但带哈希缓存:命中缓存则跳过重算(5000+ 资源文件重复校验的关键路径)。
 *  cache 可为 NULL(等价于不带缓存)。校验通过时会把结果写进缓存。 */
sxcl_verify_status sxcl_verify_file_cached(const char *path, int64_t expected_size,
                                           const char *expected_hex, sxcl_hash_algo algo,
                                           sxcl_hash_cache *cache, sxcl_verify_result *out);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_VERIFY_H */
