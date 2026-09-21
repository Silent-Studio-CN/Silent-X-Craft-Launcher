/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* fopen/fread 在 MSVC 下默认被标记弃用 */
#endif

#include "sxcl/verify.h"

#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 与 Python 版 HASH_CHUNK 一致:4MiB */
#define SXCL_HASH_CHUNK ((size_t)4 * 1024 * 1024)

const char *sxcl_verify_status_name(sxcl_verify_status status)
{
    switch (status) {
    case SXCL_VERIFY_OK:
        return "ok";
    case SXCL_VERIFY_MISSING:
        return "missing";
    case SXCL_VERIFY_SIZE:
        return "size-mismatch";
    case SXCL_VERIFY_HASH:
        return "hash-mismatch";
    case SXCL_VERIFY_IO:
        return "io-error";
    case SXCL_VERIFY_BAD_ARG:
        return "bad-argument";
    }
    return "unknown";
}

int sxcl_hash_file(const char *path, sxcl_hash_algo algo, char *out_hex, size_t out_len)
{
    if (!path || !out_hex || out_len == 0) {
        return -3;
    }
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return -2;
    }
    unsigned char *buf = (unsigned char *)malloc(SXCL_HASH_CHUNK);
    if (!buf) {
        fclose(fh);
        return -2;
    }

    sxcl_hash_ctx ctx;
    sxcl_hash_init(&ctx, algo);
    int rc = 0;
    for (;;) {
        const size_t got = fread(buf, 1, SXCL_HASH_CHUNK, fh);
        if (got > 0) {
            sxcl_hash_update(&ctx, buf, got);
        }
        if (got < SXCL_HASH_CHUNK) {
            if (ferror(fh) != 0) {
                rc = -2;
            }
            break;
        }
    }
    free(buf);
    fclose(fh);
    if (rc != 0) {
        return rc;
    }
    return sxcl_hash_final_hex(&ctx, out_hex, out_len);
}

sxcl_verify_status sxcl_verify_file_cached(const char *path, int64_t expected_size,
                                           const char *expected_hex, sxcl_hash_algo algo,
                                           sxcl_hash_cache *cache, sxcl_verify_result *out)
{
    if (out) {
        out->from_cache = 0;
    }
    if (!cache) {
        return sxcl_verify_file(path, expected_size, expected_hex, algo, out);
    }
    if (!path) {
        if (out) {
            out->status = SXCL_VERIFY_BAD_ARG;
        }
        return SXCL_VERIFY_BAD_ARG;
    }

    int64_t size = 0;
    int64_t mtime_ns = 0;
    const int st = sxcl_fs_stat(path, &size, &mtime_ns);
    if (out) {
        out->size = (st == 0) ? size : -1;
    }
    if (st == -1) {
        if (out) {
            out->status = SXCL_VERIFY_MISSING;
        }
        return SXCL_VERIFY_MISSING;
    }
    if (st != 0) {
        if (out) {
            out->status = SXCL_VERIFY_IO;
        }
        return SXCL_VERIFY_IO;
    }
    if (expected_size > 0 && size != expected_size) {
        if (out) {
            out->status = SXCL_VERIFY_SIZE;
        }
        return SXCL_VERIFY_SIZE;
    }
    if (!expected_hex || expected_hex[0] == '\0') {
        return SXCL_VERIFY_OK; /* 只校验大小 */
    }

    /* 缓存命中:路径 + 大小 + 修改时间三者一致就复用摘要(不重读文件) */
    const char *cached = sxcl_hash_cache_get(cache, path, size, mtime_ns);
    if (cached) {
        if (out) {
            snprintf(out->actual_hex, sizeof(out->actual_hex), "%s", cached);
            out->from_cache = 1;
        }
        if (sxcl_hash_hex_equal(cached, expected_hex) == 0) {
            if (out) {
                out->status = SXCL_VERIFY_HASH;
            }
            return SXCL_VERIFY_HASH;
        }
        return SXCL_VERIFY_OK;
    }

    const size_t hex_len = sxcl_hash_hex_len(algo);
    char actual[65];
    if (hex_len + 1 > sizeof(actual)) {
        if (out) {
            out->status = SXCL_VERIFY_BAD_ARG;
        }
        return SXCL_VERIFY_BAD_ARG;
    }
    if (sxcl_hash_file(path, algo, actual, sizeof(actual)) != 0) {
        if (out) {
            out->status = SXCL_VERIFY_IO;
        }
        return SXCL_VERIFY_IO;
    }
    sxcl_hash_cache_put(cache, path, size, mtime_ns, actual);
    if (out) {
        memcpy(out->actual_hex, actual, hex_len + 1);
    }
    if (sxcl_hash_hex_equal(actual, expected_hex) == 0) {
        if (out) {
            out->status = SXCL_VERIFY_HASH;
        }
        return SXCL_VERIFY_HASH;
    }
    return SXCL_VERIFY_OK;
}

sxcl_verify_status sxcl_verify_file(const char *path, int64_t expected_size,
                                    const char *expected_hex, sxcl_hash_algo algo,
                                    sxcl_verify_result *out)
{
    if (out) {
        out->status = SXCL_VERIFY_OK;
        out->size = -1;
        out->actual_hex[0] = '\0';
    }
    if (!path) {
        if (out) {
            out->status = SXCL_VERIFY_BAD_ARG;
        }
        return SXCL_VERIFY_BAD_ARG;
    }

    int64_t size = 0;
    const int st = sxcl_fs_stat(path, &size, NULL);
    if (st == -1) {
        if (out) {
            out->status = SXCL_VERIFY_MISSING;
        }
        return SXCL_VERIFY_MISSING;
    }
    if (st != 0) {
        if (out) {
            out->status = SXCL_VERIFY_IO;
        }
        return SXCL_VERIFY_IO;
    }
    if (out) {
        out->size = size;
    }
    if (expected_size > 0 && size != expected_size) {
        if (out) {
            out->status = SXCL_VERIFY_SIZE;
        }
        return SXCL_VERIFY_SIZE;
    }
    if (!expected_hex || expected_hex[0] == '\0') {
        return SXCL_VERIFY_OK; /* 只校验大小 */
    }

    const size_t hex_len = sxcl_hash_hex_len(algo);
    char actual[65];
    if (hex_len + 1 > sizeof(actual)) {
        if (out) {
            out->status = SXCL_VERIFY_BAD_ARG;
        }
        return SXCL_VERIFY_BAD_ARG;
    }
    if (sxcl_hash_file(path, algo, actual, sizeof(actual)) != 0) {
        if (out) {
            out->status = SXCL_VERIFY_IO;
        }
        return SXCL_VERIFY_IO;
    }
    if (out) {
        memcpy(out->actual_hex, actual, hex_len + 1);
    }
    if (sxcl_hash_hex_equal(actual, expected_hex) == 0) {
        if (out) {
            out->status = SXCL_VERIFY_HASH;
        }
        return SXCL_VERIFY_HASH;
    }
    return SXCL_VERIFY_OK;
}
