/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_HASH_H
#define SXCL_HASH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** 支持的哈希算法。枚举值 0/1 是稳定契约，勿改动。 */
typedef enum sxcl_hash_algo { SXCL_HASH_SHA1 = 0, SXCL_HASH_SHA256 = 1 } sxcl_hash_algo;

/** SHA-1 增量上下文：h[5] 为状态字，total_len 为已吃进的字节数（64 位）。 */
typedef struct sxcl_sha1_ctx   { uint32_t h[5]; uint64_t total_len; unsigned char buf[64]; size_t buf_len; } sxcl_sha1_ctx;
/** SHA-256 增量上下文：h[8] 为状态字，total_len 为已吃进的字节数（64 位）。 */
typedef struct sxcl_sha256_ctx { uint32_t h[8]; uint64_t total_len; unsigned char buf[64]; size_t buf_len; } sxcl_sha256_ctx;

/** 算法无关的哈希上下文。结构体公开，允许栈上分配，无需任何 init/销毁配套函数。 */
typedef struct sxcl_hash_ctx {
    sxcl_hash_algo algo;
    union { sxcl_sha1_ctx sha1; sxcl_sha256_ctx sha256; } u;
} sxcl_hash_ctx;

/** 初始化上下文。ctx 为 NULL 时静默返回；未知算法会使后续 final 失败。 */
void   sxcl_hash_init(sxcl_hash_ctx *ctx, sxcl_hash_algo algo);
/** 增量吃数据，可任意分块（含 0 长度）。内部不使用堆，不做隐式分配。 */
void   sxcl_hash_update(sxcl_hash_ctx *ctx, const void *data, size_t len);
/** 收尾并写出十六进制小写字符串 + NUL。out_len 至少 41/65；成功返回 0。
 *  失败（ctx/out 为 NULL、未知算法、out_len 不足）返回 -1，并在 out_len > 0 时把 out 置为空串。
 *  收尾会改动 ctx，若要复用请重新 sxcl_hash_init。 */
int    sxcl_hash_final_hex(sxcl_hash_ctx *ctx, char *out, size_t out_len); /* out_len 至少 41/65,写十六进制小写 + NUL;成功返回 0 */
/** 返回十六进制摘要长度（不含 NUL）：SHA-1 为 40，SHA-256 为 64，未知算法为 0。 */
size_t sxcl_hash_hex_len(sxcl_hash_algo algo);        /* 40 / 64 */
/** 返回算法小写名："sha1" / "sha256"，未知算法返回 "unknown"。返回静态字符串，勿释放。 */
const char *sxcl_hash_algo_name(sxcl_hash_algo algo); /* "sha1" / "sha256" */
/** 一次性哈希。成功返回 0，失败返回 -1（语义同 final_hex）。 */
int    sxcl_hash_digest(sxcl_hash_algo algo, const void *data, size_t len, char *out, size_t out_len); /* 一次性,成功返回 0 */
/** 十六进制摘要比较：大小写不敏感。长度不等、含非十六进制字符、任一为 NULL 时返回 0。
 *  注：两个空串长度相等且不含非法字符，按契约返回 1；实际使用中摘要串不会为空。 */
int    sxcl_hash_hex_equal(const char *a, const char *b); /* 大小写不敏感;长度不等或非十六进制返回 0 */

#ifdef __cplusplus
}
#endif
#endif
