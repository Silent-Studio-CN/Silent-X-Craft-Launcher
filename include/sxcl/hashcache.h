/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_HASHCACHE_H
#define SXCL_HASHCACHE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sxcl_hash_cache sxcl_hash_cache;
/** 打开缓存。path 为空表示纯内存缓存;文件不存在或内容损坏都被当作空缓存(不许失败)。 */
sxcl_hash_cache *sxcl_hash_cache_open(const char *path);
/** 原子落盘(先写 <path>.tmp 再改名覆盖)。path 为空或没有改动时直接返回 0。 */
int sxcl_hash_cache_save(sxcl_hash_cache *cache);
/** 落盘并释放。传 NULL 安全。 */
void sxcl_hash_cache_close(sxcl_hash_cache *cache);
/** 查:命中返回内部持有的十六进制串(调用方不得释放),未命中返回 NULL。
 *  cache 或 path 为空、键不存在都返回 NULL。 */
const char *sxcl_hash_cache_get(sxcl_hash_cache *cache, const char *path, int64_t size, int64_t mtime_ns);
/** 写:同一 path 但 size/mtime 变化视为另一条记录(旧记录保留不影响正确性,可用 prune 清理)。
 *  cache/path/hex 任一为空则什么都不做。 */
void sxcl_hash_cache_put(sxcl_hash_cache *cache, const char *path, int64_t size, int64_t mtime_ns, const char *hex);
/** 删除某条精确记录(不存在的键不算错误)。返回 0 表示成功或本来就没有;-1 表示参数非法。 */
int sxcl_hash_cache_remove(sxcl_hash_cache *cache, const char *path, int64_t size, int64_t mtime_ns);
/** 当前条目数。 */
size_t sxcl_hash_cache_count(const sxcl_hash_cache *cache);
/** 删除"文件已不在"的条目(路径不存在),返回删除条数。 */
size_t sxcl_hash_cache_prune_missing(sxcl_hash_cache *cache);
#ifdef __cplusplus
}
#endif
#endif /* SXCL_HASHCACHE_H */
