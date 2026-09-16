/* SXCL-C 文件校验 —— 凡是 Mojang 给了 SHA-1 的资源一律强校验。
 *
 * 校验矩阵(全部来自官方元数据,不允许跳过):
 *   版本清单 / 版本 JSON  version_manifest_v2.json 里每个版本的 sha1
 *   客户端/服务端 jar    downloads.client.sha1 / server.sha1
 *   依赖库              downloads.artifact.sha1 与 classifiers.*.sha1
 *   资源索引            assetIndex.sha1
 *   资源对象            文件名即 sha1(objects[*].hash)
 *   官方 JRE 文件        files[*].downloads.raw.sha1
 *
 * 语义对齐 Python 版 src/core/download/verify.py:
 *   - expected_hex 为空 => 只校验大小(size 为 0 表示连大小也不校验);
 *   - 先比大小再算哈希(大小不符时不算哈希,省时且可区分故障);
 *   - 哈希比较大小写不敏感。
 */
#ifndef SXCL_VERIFY_H
#define SXCL_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/hash.h"

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

#ifdef __cplusplus
}
#endif
#endif /* SXCL_VERIFY_H */
