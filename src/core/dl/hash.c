/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/hash.h"

#include <string.h>

#define SXCL_SHA_BLOCK_SIZE  64u
#define SXCL_SHA1_DIGEST_LEN  20u
#define SXCL_SHA256_DIGEST_LEN 32u

/* ============================ 基础工具 ============================ */

/* 循环左移；n 必须落在 1..31，调用点全部是编译期常量，不存在未定义移位。 */
static uint32_t sxcl_rotl32(uint32_t x, unsigned n)
{
    return (uint32_t)((x << n) | (x >> (32u - n)));
}

/* 循环右移；n 必须落在 1..31。 */
static uint32_t sxcl_rotr32(uint32_t x, unsigned n)
{
    return (uint32_t)((x >> n) | (x << (32u - n)));
}

/* 大端读 4 字节：逐字节移位拼装，不做指针强转，规避未对齐访问与别名问题。 */
static uint32_t sxcl_load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

/* 大端写 4 字节。 */
static void sxcl_store_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFFu);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8) & 0xFFu);
    p[3] = (unsigned char)(v & 0xFFu);
}

/* 十六进制字符 -> 半字节值；非法返回 -1。 */
static int sxcl_hex_nibble(unsigned char c)
{
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9') {
        return (int)(c - (unsigned char)'0');
    }
    if (c >= (unsigned char)'a' && c <= (unsigned char)'f') {
        return (int)(c - (unsigned char)'a') + 10;
    }
    if (c >= (unsigned char)'A' && c <= (unsigned char)'F') {
        return (int)(c - (unsigned char)'A') + 10;
    }
    return -1;
}

/* ============================== SHA-1 ============================== */

/* 压缩一个 64 字节块，更新 ctx->h。 */
static void sxcl_sha1_compress(sxcl_sha1_ctx *ctx, const unsigned char *block)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    unsigned i;

    for (i = 0u; i < 16u; ++i) {
        w[i] = sxcl_load_be32(block + (size_t)i * 4u);
    }
    for (i = 16u; i < 80u; ++i) {
        w[i] = sxcl_rotl32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];

    for (i = 0u; i < 80u; ++i) {
        uint32_t f;
        uint32_t k;
        uint32_t tmp;

        if (i < 20u) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        tmp = sxcl_rotl32(a, 5u) + f + e + k + w[i];
        e = d;
        d = c;
        c = sxcl_rotl32(b, 30u);
        b = a;
        a = tmp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sxcl_sha1_update(sxcl_sha1_ctx *ctx, const unsigned char *data, size_t len)
{
    ctx->total_len += (uint64_t)len;

    /* 先补齐并可能冲掉已有缓冲。 */
    if (ctx->buf_len > 0u) {
        size_t need = (size_t)SXCL_SHA_BLOCK_SIZE - ctx->buf_len;
        size_t take = (len < need) ? len : need;

        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;

        if (ctx->buf_len == (size_t)SXCL_SHA_BLOCK_SIZE) {
            sxcl_sha1_compress(ctx, ctx->buf);
            ctx->buf_len = 0u;
        }
    }

    /* 整块直接压缩，零拷贝。 */
    while (len >= (size_t)SXCL_SHA_BLOCK_SIZE) {
        sxcl_sha1_compress(ctx, data);
        data += SXCL_SHA_BLOCK_SIZE;
        len -= (size_t)SXCL_SHA_BLOCK_SIZE;
    }

    /* 尾巴留在缓冲里。 */
    if (len > 0u) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = len;
    }
}

/* 补位（0x80 + 0x00... + 64 位大端比特长度）并输出 20 字节摘要。 */
static void sxcl_sha1_final(sxcl_sha1_ctx *ctx, unsigned char *out)
{
    const uint64_t bits = ctx->total_len << 3; /* 64 位比特长度，> 2^32 不截断 */
    size_t i = ctx->buf_len;
    unsigned j;

    ctx->buf[i] = 0x80u;
    ++i;

    if (i > 56u) { /* 本块放不下长度，先压一块，长度放进下一块 */
        while (i < (size_t)SXCL_SHA_BLOCK_SIZE) {
            ctx->buf[i] = 0u;
            ++i;
        }
        sxcl_sha1_compress(ctx, ctx->buf);
        i = 0u;
    }
    while (i < 56u) {
        ctx->buf[i] = 0u;
        ++i;
    }
    for (j = 0u; j < 8u; ++j) {
        ctx->buf[56u + j] = (unsigned char)((bits >> (8u * (7u - j))) & 0xFFu);
    }
    sxcl_sha1_compress(ctx, ctx->buf);

    for (j = 0u; j < 5u; ++j) {
        sxcl_store_be32(out + (size_t)j * 4u, ctx->h[j]);
    }
}

/* ============================= SHA-256 ============================= */

static const uint32_t sxcl_sha256_k[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

static void sxcl_sha256_compress(sxcl_sha256_ctx *ctx, const unsigned char *block)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0u; i < 16u; ++i) {
        w[i] = sxcl_load_be32(block + (size_t)i * 4u);
    }
    for (i = 16u; i < 64u; ++i) {
        const uint32_t x = w[i - 15u];
        const uint32_t y = w[i - 2u];
        const uint32_t s0 = sxcl_rotr32(x, 7u) ^ sxcl_rotr32(x, 18u) ^ (x >> 3);
        const uint32_t s1 = sxcl_rotr32(y, 17u) ^ sxcl_rotr32(y, 19u) ^ (y >> 10);

        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];
    f = ctx->h[5];
    g = ctx->h[6];
    h = ctx->h[7];

    for (i = 0u; i < 64u; ++i) {
        const uint32_t big_s1 = sxcl_rotr32(e, 6u) ^ sxcl_rotr32(e, 11u) ^ sxcl_rotr32(e, 25u);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + big_s1 + ch + sxcl_sha256_k[i] + w[i];
        const uint32_t big_s0 = sxcl_rotr32(a, 2u) ^ sxcl_rotr32(a, 13u) ^ sxcl_rotr32(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big_s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

static void sxcl_sha256_update(sxcl_sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    ctx->total_len += (uint64_t)len;

    if (ctx->buf_len > 0u) {
        size_t need = (size_t)SXCL_SHA_BLOCK_SIZE - ctx->buf_len;
        size_t take = (len < need) ? len : need;

        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;

        if (ctx->buf_len == (size_t)SXCL_SHA_BLOCK_SIZE) {
            sxcl_sha256_compress(ctx, ctx->buf);
            ctx->buf_len = 0u;
        }
    }

    while (len >= (size_t)SXCL_SHA_BLOCK_SIZE) {
        sxcl_sha256_compress(ctx, data);
        data += SXCL_SHA_BLOCK_SIZE;
        len -= (size_t)SXCL_SHA_BLOCK_SIZE;
    }

    if (len > 0u) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = len;
    }
}

static void sxcl_sha256_final(sxcl_sha256_ctx *ctx, unsigned char *out)
{
    const uint64_t bits = ctx->total_len << 3;
    size_t i = ctx->buf_len;
    unsigned j;

    ctx->buf[i] = 0x80u;
    ++i;

    if (i > 56u) {
        while (i < (size_t)SXCL_SHA_BLOCK_SIZE) {
            ctx->buf[i] = 0u;
            ++i;
        }
        sxcl_sha256_compress(ctx, ctx->buf);
        i = 0u;
    }
    while (i < 56u) {
        ctx->buf[i] = 0u;
        ++i;
    }
    for (j = 0u; j < 8u; ++j) {
        ctx->buf[56u + j] = (unsigned char)((bits >> (8u * (7u - j))) & 0xFFu);
    }
    sxcl_sha256_compress(ctx, ctx->buf);

    for (j = 0u; j < 8u; ++j) {
        sxcl_store_be32(out + (size_t)j * 4u, ctx->h[j]);
    }
}

/* ============================ 对外接口 ============================ */

void sxcl_hash_init(sxcl_hash_ctx *ctx, sxcl_hash_algo algo)
{
    if (ctx == NULL) {
        return;
    }

    ctx->algo = algo;

    if (algo == SXCL_HASH_SHA1) {
        ctx->u.sha1.h[0] = 0x67452301u;
        ctx->u.sha1.h[1] = 0xEFCDAB89u;
        ctx->u.sha1.h[2] = 0x98BADCFEu;
        ctx->u.sha1.h[3] = 0x10325476u;
        ctx->u.sha1.h[4] = 0xC3D2E1F0u;
        ctx->u.sha1.total_len = 0u;
        ctx->u.sha1.buf_len = 0u;
    } else if (algo == SXCL_HASH_SHA256) {
        ctx->u.sha256.h[0] = 0x6A09E667u;
        ctx->u.sha256.h[1] = 0xBB67AE85u;
        ctx->u.sha256.h[2] = 0x3C6EF372u;
        ctx->u.sha256.h[3] = 0xA54FF53Au;
        ctx->u.sha256.h[4] = 0x510E527Fu;
        ctx->u.sha256.h[5] = 0x9B05688Cu;
        ctx->u.sha256.h[6] = 0x1F83D9ABu;
        ctx->u.sha256.h[7] = 0x5BE0CD19u;
        ctx->u.sha256.total_len = 0u;
        ctx->u.sha256.buf_len = 0u;
    } else {
        /* 未知算法：保持可查询状态，final 会明确失败。 */
        memset(&ctx->u, 0, sizeof ctx->u);
    }
}

void sxcl_hash_update(sxcl_hash_ctx *ctx, const void *data, size_t len)
{
    if (ctx == NULL || len == 0u || data == NULL) {
        return; /* 0 长度更新是合法空操作；契约外的 NULL 也安全忽略 */
    }

    if (ctx->algo == SXCL_HASH_SHA1) {
        sxcl_sha1_update(&ctx->u.sha1, (const unsigned char *)data, len);
    } else if (ctx->algo == SXCL_HASH_SHA256) {
        sxcl_sha256_update(&ctx->u.sha256, (const unsigned char *)data, len);
    }
}

int sxcl_hash_final_hex(sxcl_hash_ctx *ctx, char *out, size_t out_len)
{
    static const char hexdigits[] = "0123456789abcdef";
    unsigned char digest[SXCL_SHA256_DIGEST_LEN];
    size_t digest_len;
    size_t need;
    size_t i;

    if (ctx == NULL || out == NULL) {
        return -1;
    }

    if (ctx->algo == SXCL_HASH_SHA1) {
        sxcl_sha1_final(&ctx->u.sha1, digest);
        digest_len = (size_t)SXCL_SHA1_DIGEST_LEN;
    } else if (ctx->algo == SXCL_HASH_SHA256) {
        sxcl_sha256_final(&ctx->u.sha256, digest);
        digest_len = (size_t)SXCL_SHA256_DIGEST_LEN;
    } else {
        if (out_len > 0u) {
            out[0] = '\0';
        }
        return -1;
    }

    need = digest_len * 2u + 1u;
    if (out_len < need) {
        if (out_len > 0u) {
            out[0] = '\0';
        }
        return -1;
    }

    for (i = 0u; i < digest_len; ++i) {
        out[i * 2u] = hexdigits[digest[i] >> 4];
        out[i * 2u + 1u] = hexdigits[digest[i] & 0x0Fu];
    }
    out[need - 1u] = '\0';
    return 0;
}

size_t sxcl_hash_hex_len(sxcl_hash_algo algo)
{
    if (algo == SXCL_HASH_SHA1) {
        return (size_t)SXCL_SHA1_DIGEST_LEN * 2u;
    }
    if (algo == SXCL_HASH_SHA256) {
        return (size_t)SXCL_SHA256_DIGEST_LEN * 2u;
    }
    return 0u;
}

const char *sxcl_hash_algo_name(sxcl_hash_algo algo)
{
    if (algo == SXCL_HASH_SHA1) {
        return "sha1";
    }
    if (algo == SXCL_HASH_SHA256) {
        return "sha256";
    }
    return "unknown";
}

int sxcl_hash_digest(sxcl_hash_algo algo, const void *data, size_t len, char *out, size_t out_len)
{
    sxcl_hash_ctx ctx;

    if (out == NULL) {
        return -1;
    }
    if (data == NULL && len != 0u) {
        if (out_len > 0u) {
            out[0] = '\0';
        }
        return -1;
    }

    sxcl_hash_init(&ctx, algo);
    sxcl_hash_update(&ctx, data, len);
    return sxcl_hash_final_hex(&ctx, out, out_len);
}

int sxcl_hash_hex_equal(const char *a, const char *b)
{
    size_t i = 0u;

    if (a == NULL || b == NULL) {
        return 0;
    }

    for (;;) {
        const unsigned char ca = (unsigned char)a[i];
        const unsigned char cb = (unsigned char)b[i];
        int na;
        int nb;

        if (ca == (unsigned char)'\0' || cb == (unsigned char)'\0') {
            /* 同时结束才算长度相等（两个空串按契约视为相等）。 */
            return (ca == (unsigned char)'\0' && cb == (unsigned char)'\0') ? 1 : 0;
        }

        na = sxcl_hex_nibble(ca);
        nb = sxcl_hex_nibble(cb);
        if (na < 0 || nb < 0 || na != nb) {
            return 0;
        }
        ++i;
    }
}
