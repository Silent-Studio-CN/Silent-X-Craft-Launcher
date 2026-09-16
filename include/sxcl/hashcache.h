/* SXCL-C 哈希缓存:路径|大小|mtime_ns -> 摘要 的磁盘缓存(纯 C11,零第三方依赖)。
 *
 * 用途:下载引擎每次启动都要校验几千个文件(客户端 jar + 74 个库 + 5147 个资源对象,
 * 461MB),完整重算 SHA-1 太慢;命中缓存即可直接跳过重算。
 *
 * 语义(与 Python 版 src/core/download/verify.py 的 HashCache 对齐):
 *   - 键 = "路径|大小|mtime_ns",竖线分隔;同一路径但 size/mtime_ns 变了就是另一条
 *     记录(旧记录保留不影响正确性,只是占地方,可用 prune_missing 清理)。
 *   - 磁盘格式:第一行是版本标记,之后每行
 *         hex<TAB>size<TAB>mtime_ns<TAB>path
 *     版本标记不认识的整份文件当作空缓存;单行格式不对的只跳过该行(坏行忽略,
 *     绝不因为缓存文件损坏而失败)。写入方保证路径里不含 TAB/换行。
 *   - 落盘是原子的:先写 "<path>.tmp" 再改名覆盖 <path>,不会留下半截文件。
 *   - 线程安全:内部一把互斥锁,下列所有函数都可以直接在多线程里调用。
 *   - get 返回的是缓存内部持有的字符串,调用方不得释放;在别的线程对本缓存做写操作
 *     (put/remove/prune/clear/close)之前有效。
 *
 * 上限保护(与 Python 版"防无限增长"的意图一致):
 *   条目数上限 20000。写入新条目时若已有条目数已达到上限,先调用 prune_missing
 *   清掉"文件已不在"的条目;若仍然达到上限,则整体清空缓存,然后再写入本次条目。
 *   于是 count 永远不会超过 20000,且清空之后本次写入必然保留。
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
