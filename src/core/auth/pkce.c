/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include "sxcl/hash.h"

#include <string.h>

#define SXCL_PKCE_VERIFIER_CHARS 64u

int sxcl_auth_pkce_challenge(const char *verifier, char *challenge, size_t challenge_len)
{
    if (verifier == NULL || challenge == NULL || challenge_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    const size_t vlen = strlen(verifier);
    if (vlen < 43u || vlen > 128u) {
        return SXCL_AUTH_ERR_ARG; /* RFC 7636 §4.1 的长度约束,自己先挡住 */
    }
    /* sha1/sha256 只有"输出十六进制"的接口(见 hash.h),这里再转回字节:
     * challenge 要的是摘要的原始 32 字节做 base64url,不是摘要的十六进制串。 */
    sxcl_hash_ctx ctx;
    sxcl_hash_init(&ctx, SXCL_HASH_SHA256);
    sxcl_hash_update(&ctx, verifier, vlen);
    char hex[65];
    if (sxcl_hash_final_hex(&ctx, hex, sizeof(hex)) != 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    unsigned char digest[32];
    size_t digest_len = 0;
    if (sxcl_auth_hex_decode(hex, digest, sizeof(digest), &digest_len) != 0 || digest_len != 32u) {
        return SXCL_AUTH_ERR_ARG;
    }
    const int written = sxcl_auth_base64url(digest, digest_len, challenge, challenge_len);
    sxcl_auth_secure_zero(digest, sizeof(digest));
    if (written < 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_pkce_from_entropy(const unsigned char *entropy, size_t entropy_len,
                                char *verifier, size_t verifier_len,
                                char *challenge, size_t challenge_len)
{
    if (entropy == NULL || entropy_len == 0 || verifier == NULL || challenge == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    /* base64url 的字母表 {A-Za-z0-9-_} 正好是 RFC 7636 的 unreserved 子集,可以直接当 verifier 用 */
    char encoded[256];
    if (sxcl_auth_base64url(entropy, entropy_len, encoded, sizeof(encoded)) < 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    const size_t elen = strlen(encoded);
    const size_t take = (elen < SXCL_PKCE_VERIFIER_CHARS) ? elen : SXCL_PKCE_VERIFIER_CHARS;
    if (take < 43u) {
        return SXCL_AUTH_ERR_ARG; /* 熵不够凑出 43 字符的 verifier */
    }
    char v[SXCL_AUTH_PKCE_VERIFIER_MAX];
    memcpy(v, encoded, take);
    v[take] = '\0';
    if (sxcl_auth_copy(verifier, verifier_len, v) < 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    return sxcl_auth_pkce_challenge(v, challenge, challenge_len);
}

int sxcl_auth_pkce_generate(char *verifier, size_t verifier_len,
                            char *challenge, size_t challenge_len)
{
    unsigned char entropy[48]; /* 384 bit:生成 64 个字符绰绰有余 */
    if (sxcl_auth_random(entropy, sizeof(entropy)) != 0) {
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    const int rc = sxcl_auth_pkce_from_entropy(entropy, sizeof(entropy), verifier, verifier_len,
                                               challenge, challenge_len);
    sxcl_auth_secure_zero(entropy, sizeof(entropy));
    return rc;
}
