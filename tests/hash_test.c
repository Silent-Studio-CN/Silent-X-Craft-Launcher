/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sxcl/hash.h"

/* 摘要字符串缓冲：64 + NUL 足够两种算法。 */
#define HEXBUF 65u

static int g_checks = 0;
static int g_failures = 0;

static void check_true(int cond, const char *what)
{
    ++g_checks;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

static void check_hex(const char *what, const char *got, const char *want)
{
    ++g_checks;
    if (strcmp(got, want) == 0) {
        printf("ok   %s = %s\n", what, got);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s\n     got  = %s\n     want = %s\n", what, got, want);
    }
}

/* 一次性哈希的小包装：失败时写入 "?" 以便断言报错可读。 */
static void digest_hex(sxcl_hash_algo algo, const void *data, size_t len, char *out)
{
    if (sxcl_hash_digest(algo, data, len, out, HEXBUF) != 0) {
        out[0] = '?';
        out[1] = '\0';
    }
}

static void expect_hex(sxcl_hash_algo algo, const void *data, size_t len,
                       const char *want, const char *what)
{
    char got[HEXBUF];
    digest_hex(algo, data, len, got);
    check_hex(what, got, want);
}

/* ------------------------------------------------------------------ */
/* 1) 标准向量                                                         */
/* ------------------------------------------------------------------ */

static void test_standard_vectors(void)
{
    /* 56 字节：补位后 0x80 落在下标 56，长度字段放不进同一块，必须多压一块。 */
    static const char msg56[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    /* 112 字节（896 bit）标准向量。 */
    static const char msg112[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

    const char *abc = "abc";
    char *mega = (char *)malloc(1000000u);
    char got[HEXBUF];

    printf("-- 标准向量 --\n");

    /* 空消息：data == NULL 且 len == 0 必须合法。 */
    expect_hex(SXCL_HASH_SHA1, NULL, 0u,
               "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1(\"\")");
    expect_hex(SXCL_HASH_SHA256, NULL, 0u,
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(\"\")");

    expect_hex(SXCL_HASH_SHA1, abc, 3u,
               "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(\"abc\")");
    expect_hex(SXCL_HASH_SHA256, abc, 3u,
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\")");

    expect_hex(SXCL_HASH_SHA1, msg56, strlen(msg56),
               "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "sha1(56B 标准串)");
    expect_hex(SXCL_HASH_SHA256, msg56, strlen(msg56),
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256(56B 标准串)");

    expect_hex(SXCL_HASH_SHA1, msg112, strlen(msg112),
               "a49b2446a02c645bf419f995b67091253a04a259", "sha1(112B 标准串)");
    expect_hex(SXCL_HASH_SHA256, msg112, strlen(msg112),
               "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "sha256(112B 标准串)");

    if (mega == NULL) {
        ++g_failures;
        fprintf(stderr, "FAIL malloc(1000000) 失败，无法测 1e6 个 'a'\n");
        return;
    }
    memset(mega, 'a', 1000000u);

    expect_hex(SXCL_HASH_SHA1, mega, 1000000u,
               "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "sha1(1,000,000 x 'a')");
    expect_hex(SXCL_HASH_SHA256, mega, 1000000u,
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
               "sha256(1,000,000 x 'a')");

    /* 同一百万字节再走一次 1000 字节一块的增量路径（NIST 的经典喂法）。 */
    {
        sxcl_hash_ctx c1;
        sxcl_hash_ctx c2;
        size_t off = 0u;

        sxcl_hash_init(&c1, SXCL_HASH_SHA1);
        sxcl_hash_init(&c2, SXCL_HASH_SHA256);
        while (off < 1000000u) {
            size_t n = 1000000u - off;
            if (n > 1000u) {
                n = 1000u;
            }
            sxcl_hash_update(&c1, mega + off, n);
            sxcl_hash_update(&c2, mega + off, n);
            off += n;
        }
        memset(got, 0, sizeof got);
        check_true(sxcl_hash_final_hex(&c1, got, sizeof got) == 0, "final_hex(sha1 增量) 返回 0");
        check_hex("sha1(1e6 'a', 1000B/块)", got,
                  "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        check_true(sxcl_hash_final_hex(&c2, got, sizeof got) == 0, "final_hex(sha256 增量) 返回 0");
        check_hex("sha256(1e6 'a', 1000B/块)", got,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    free(mega);
}

/* ------------------------------------------------------------------ */
/* 2) 分块一致性                                                       */
/* ------------------------------------------------------------------ */

/* 把 data 按 chunk 字节切片喂给增量接口，比较是否与一次性 digest 相同。 */
static int chunked_matches(sxcl_hash_algo algo, const unsigned char *data,
                           size_t len, size_t chunk)
{
    char want[HEXBUF];
    char got[HEXBUF];
    sxcl_hash_ctx ctx;
    size_t off = 0u;

    digest_hex(algo, data, len, want);

    sxcl_hash_init(&ctx, algo);
    while (off < len) {
        size_t n = len - off;
        if (n > chunk) {
            n = chunk;
        }
        sxcl_hash_update(&ctx, data + off, n);
        off += n;
    }
    if (sxcl_hash_final_hex(&ctx, got, sizeof got) != 0) {
        return 0;
    }
    return strcmp(want, got) == 0;
}

static void test_chunked_update(void)
{
    static const size_t chunks[] = { 1u, 2u, 3u, 7u, 64u, 65u, 127u, 1000u };
    static const size_t boundaries[] = { 0u, 1u, 54u, 55u, 56u, 57u, 63u, 64u, 65u,
                                         119u, 120u, 127u, 128u, 129u, 191u, 192u,
                                         255u, 256u, 257u, 511u, 512u, 513u,
                                         1000u, 1023u, 1024u, 4096u, 4097u };
    static const char msg56[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    unsigned char pattern[4097];
    const sxcl_hash_algo algos[2] = { SXCL_HASH_SHA1, SXCL_HASH_SHA256 };
    size_t ai;
    size_t ci;
    size_t bi;
    size_t i;
    int cases = 0;
    int bad = 0;

    printf("-- 分块一致性 --\n");

    for (i = 0u; i < sizeof pattern; ++i) {
        pattern[i] = (unsigned char)(i & 0xFFu);
    }

    /* 2a) 固定数据 x 指定切片长度（题目要求的 1/2/3/7/64/65/127/1000）。 */
    for (ai = 0u; ai < 2u; ++ai) {
        for (ci = 0u; ci < sizeof chunks / sizeof chunks[0]; ++ci) {
            ++cases;
            if (!chunked_matches(algos[ai], (const unsigned char *)msg56, strlen(msg56), chunks[ci])) {
                ++bad;
                fprintf(stderr, "FAIL 分块不一致: %s, 56B 串, chunk=%u\n",
                        sxcl_hash_algo_name(algos[ai]), (unsigned)chunks[ci]);
            }
            ++cases;
            if (!chunked_matches(algos[ai], pattern, 1000u, chunks[ci])) {
                ++bad;
                fprintf(stderr, "FAIL 分块不一致: %s, 1000B pattern, chunk=%u\n",
                        sxcl_hash_algo_name(algos[ai]), (unsigned)chunks[ci]);
            }
        }
    }

    /* 2b) 长度扫描：0..200 全覆盖 + 块边界大长度, 切片 1/7/64/65。 */
    for (ai = 0u; ai < 2u; ++ai) {
        for (bi = 0u; bi < sizeof boundaries / sizeof boundaries[0]; ++bi) {
            for (ci = 0u; ci < sizeof chunks / sizeof chunks[0]; ++ci) {
                ++cases;
                if (!chunked_matches(algos[ai], pattern, boundaries[bi], chunks[ci])) {
                    ++bad;
                    fprintf(stderr, "FAIL 分块不一致: %s, len=%u, chunk=%u\n",
                            sxcl_hash_algo_name(algos[ai]), (unsigned)boundaries[bi],
                            (unsigned)chunks[ci]);
                }
            }
        }
    }
    for (ai = 0u; ai < 2u; ++ai) {
        size_t len;
        for (len = 0u; len <= 200u; ++len) {
            for (ci = 0u; ci < 4u; ++ci) {
                ++cases;
                if (!chunked_matches(algos[ai], pattern, len, chunks[ci])) {
                    ++bad;
                    fprintf(stderr, "FAIL 分块不一致: %s, len=%u, chunk=%u\n",
                            sxcl_hash_algo_name(algos[ai]), (unsigned)len, (unsigned)chunks[ci]);
                }
            }
        }
    }

    ++g_checks;
    if (bad == 0) {
        printf("ok   分块一致性: %d 个切片用例全部与一次性 digest 相同\n", cases);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL 分块一致性: %d/%d 个用例不一致\n", bad, cases);
    }
}

/* ------------------------------------------------------------------ */
/* 3) 补位边界长度向量（参考值由 .NET IncrementalHash 独立生成）        */
/* ------------------------------------------------------------------ */

static void test_boundary_vectors(void)
{
    /* pattern[i] = i & 0xFF，取前 len 字节。 */
    static const struct {
        size_t len;
        const char *sha1;
        const char *sha256;
    } vec[] = {
        { 55u,  "8ae2d46729cfe68ff927af5eec9c7d1b66d65ac2",
                "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59" },
        { 56u,  "636e2ec698dac903498e648bd2f3af641d3c88cb",
                "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562" },
        { 57u,  "7cb1330f35244b57437539253304ea78a6b7c443",
                "2fe741af801cc238602ac0ec6a7b0c3a8a87c7fc7d7f02a3fe03d1c12eac4d8f" },
        { 63u,  "6d942da0c4392b123528f2905c713a3ce28364bd",
                "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488" },
        { 64u,  "c6138d514ffa2135bfce0ed0b8fac65669917ec7",
                "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108" },
        { 65u,  "69bd728ad6e13cd76ff19751fde427b00e395746",
                "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781" },
        { 119u, "41c89d06001bab4ab78736b44efe7ce18ce6ae08",
                "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6" },
        { 120u, "d3dbd653bd8597b7475321b60a36891278e6a04a",
                "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c" },
        { 127u, "89d7312a903f65cd2b3e34a975e55dbea9033353",
                "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976" }
    };
    unsigned char pattern[128];
    char label[64];
    size_t i;

    printf("-- 补位边界长度向量 --\n");

    for (i = 0u; i < sizeof pattern; ++i) {
        pattern[i] = (unsigned char)(i & 0xFFu);
    }

    for (i = 0u; i < sizeof vec / sizeof vec[0]; ++i) {
        int n = snprintf(label, sizeof label, "sha1(len=%u)", (unsigned)vec[i].len);
        if (n > 0) {
            expect_hex(SXCL_HASH_SHA1, pattern, vec[i].len, vec[i].sha1, label);
        }
        n = snprintf(label, sizeof label, "sha256(len=%u)", (unsigned)vec[i].len);
        if (n > 0) {
            expect_hex(SXCL_HASH_SHA256, pattern, vec[i].len, vec[i].sha256, label);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 4) > 512 MiB：64 位长度字段                                         */
/* ------------------------------------------------------------------ */

static void test_large_stream_64bit_length(void)
{
    /* 513 MiB = 537,919,488 字节 = 4,303,355,904 bit > 2^32：
     * 若长度字段被截成 32 位，摘要一定错。
     * 参考值由 .NET IncrementalHash 独立生成（1 MiB 块 x 513，块内 i & 0xFF）。 */
    const size_t block_size = 1024u * 1024u;
    const unsigned blocks = 513u;
    const uint64_t total = (uint64_t)blocks * (uint64_t)block_size;
    unsigned char *block = (unsigned char *)malloc(block_size);
    sxcl_hash_ctx c1;
    sxcl_hash_ctx c2;
    char got[HEXBUF];
    size_t i;
    unsigned k;

    printf("-- > 512 MiB 64 位长度 --\n");

    if (block == NULL) {
        ++g_failures;
        fprintf(stderr, "FAIL malloc(1 MiB) 失败，无法测 >512MiB 路径\n");
        return;
    }
    for (i = 0u; i < block_size; ++i) {
        block[i] = (unsigned char)(i & 0xFFu);
    }

    sxcl_hash_init(&c1, SXCL_HASH_SHA1);
    sxcl_hash_init(&c2, SXCL_HASH_SHA256);
    for (k = 0u; k < blocks; ++k) {
        sxcl_hash_update(&c1, block, block_size);
        sxcl_hash_update(&c2, block, block_size);
    }

    printf("     (共喂入 %lu 字节)\n", (unsigned long)total);
    check_true(c1.u.sha1.total_len == total, "sha1 total_len 精确记录 513 MiB (>2^32 bit)");
    check_true(c2.u.sha256.total_len == total, "sha256 total_len 精确记录 513 MiB (>2^32 bit)");

    check_true(sxcl_hash_final_hex(&c1, got, sizeof got) == 0, "final_hex(sha1 513MiB) 返回 0");
    check_hex("sha1(513 MiB, 64 位长度)", got,
              "03dab43f4328470c37236be9426868352d2963f0");
    check_true(sxcl_hash_final_hex(&c2, got, sizeof got) == 0, "final_hex(sha256 513MiB) 返回 0");
    check_hex("sha256(513 MiB, 64 位长度)", got,
              "099be0a986c59c84e1f003edab074a30969079f00c8cf557a023b82074e4e8e1");

    free(block);
}

/* ------------------------------------------------------------------ */
/* 5) 十六进制比较                                                     */
/* ------------------------------------------------------------------ */

static void test_hex_equal(void)
{
    const char *lower = "a9993e364706816aba3e25717850c26c9cd0d89d";
    const char *upper = "A9993E364706816ABA3E25717850C26C9CD0D89D";
    const char *mixed = "a9993E364706816aba3E25717850c26c9cd0D89d";
    const char *short_one = "a9993e364706816aba3e25717850c26c9cd0d89";   /* 少一位 */
    const char *long_one = "a9993e364706816aba3e25717850c26c9cd0d89dd";  /* 多一位 */
    const char *bad_char = "a9993e364706816aba3e25717850c26c9cd0d89g";   /* 含 'g' */
    const char *bad_char2 = "a9993e364706816aba3e25717850c26c9cd0d 9d";  /* 含空格 */

    printf("-- sxcl_hash_hex_equal --\n");

    check_true(sxcl_hash_hex_equal(lower, lower) == 1, "hex_equal: 完全相同 -> 1");
    check_true(sxcl_hash_hex_equal(lower, upper) == 1, "hex_equal: 全大写 vs 全小写 -> 1");
    check_true(sxcl_hash_hex_equal(lower, mixed) == 1, "hex_equal: 大小写混合 -> 1");
    check_true(sxcl_hash_hex_equal(upper, mixed) == 1, "hex_equal: 大写 vs 混合 -> 1");
    check_true(sxcl_hash_hex_equal(short_one, lower) == 0, "hex_equal: 长度不等(短) -> 0");
    check_true(sxcl_hash_hex_equal(long_one, lower) == 0, "hex_equal: 长度不等(长) -> 0");
    check_true(sxcl_hash_hex_equal(lower, short_one) == 0, "hex_equal: 长度不等(反序) -> 0");
    check_true(sxcl_hash_hex_equal(bad_char, bad_char) == 0, "hex_equal: 非法字符 'g' -> 0");
    check_true(sxcl_hash_hex_equal(bad_char2, bad_char2) == 0, "hex_equal: 非法字符 空格 -> 0");
    check_true(sxcl_hash_hex_equal(lower, bad_char) == 0, "hex_equal: 一侧非法 -> 0");
    check_true(sxcl_hash_hex_equal(NULL, lower) == 0, "hex_equal: NULL 入参 -> 0");
    check_true(sxcl_hash_hex_equal(lower, NULL) == 0, "hex_equal: NULL 入参(反序) -> 0");
    /* 契约字面含义：长度相等且无非法字符即为相等(空串无字符,故为真)。 */
    check_true(sxcl_hash_hex_equal("", "") == 1, "hex_equal: 两个空串 -> 1 (契约字面语义)");
    check_true(sxcl_hash_hex_equal("", lower) == 0, "hex_equal: 空串 vs 摘要 -> 0");

    /* 与真实摘要联动：大小写不同的期望值应判定相等。 */
    {
        char got[HEXBUF];
        char upper_buf[HEXBUF];
        size_t i;
        digest_hex(SXCL_HASH_SHA256, "abc", 3u, got);
        for (i = 0u; i < 64u; ++i) {
            char c = got[i];
            upper_buf[i] = (char)((c >= 'a' && c <= 'f') ? (c - 'a' + 'A') : c);
        }
        upper_buf[64] = '\0';
        check_true(sxcl_hash_hex_equal(got, upper_buf) == 1, "hex_equal: 真实摘要大小写不敏感");
        upper_buf[10] = (char)((upper_buf[10] == '0') ? '1' : '0');
        check_true(sxcl_hash_hex_equal(got, upper_buf) == 0, "hex_equal: 改一位后不相等");
    }
}

/* ------------------------------------------------------------------ */
/* 6) 接口边界                                                         */
/* ------------------------------------------------------------------ */

static void test_api_edges(void)
{
    char got[HEXBUF];
    sxcl_hash_ctx ctx;
    static const sxcl_hash_algo bad_algo = (sxcl_hash_algo)99;

    printf("-- 接口边界 --\n");

    check_true(sxcl_hash_hex_len(SXCL_HASH_SHA1) == 40u, "hex_len(sha1) == 40");
    check_true(sxcl_hash_hex_len(SXCL_HASH_SHA256) == 64u, "hex_len(sha256) == 64");
    check_true(sxcl_hash_hex_len(bad_algo) == 0u, "hex_len(未知算法) == 0");
    check_true(strcmp(sxcl_hash_algo_name(SXCL_HASH_SHA1), "sha1") == 0, "algo_name(sha1) == \"sha1\"");
    check_true(strcmp(sxcl_hash_algo_name(SXCL_HASH_SHA256), "sha256") == 0, "algo_name(sha256) == \"sha256\"");

    /* out_len 恰好够用。 */
    check_true(sxcl_hash_digest(SXCL_HASH_SHA1, "abc", 3u, got, 41u) == 0, "digest sha1 out_len=41 -> 0");
    check_true(strlen(got) == 40u, "digest sha1 out_len=41 写出 40 字符");
    check_true(sxcl_hash_digest(SXCL_HASH_SHA256, "abc", 3u, got, 65u) == 0, "digest sha256 out_len=65 -> 0");
    check_true(strlen(got) == 64u, "digest sha256 out_len=65 写出 64 字符");

    /* out_len 差一字节：必须失败且不越界写，out 置空串。 */
    memset(got, 'X', sizeof got);
    check_true(sxcl_hash_digest(SXCL_HASH_SHA1, "abc", 3u, got, 40u) == -1, "digest sha1 out_len=40 -> -1");
    check_true(got[0] == '\0', "失败时 out 置为空串");
    check_true(sxcl_hash_digest(SXCL_HASH_SHA256, "abc", 3u, got, 8u) == -1, "digest sha256 out_len=8 -> -1");
    check_true(sxcl_hash_digest(SXCL_HASH_SHA1, "abc", 3u, NULL, 41u) == -1, "digest out == NULL -> -1");
    check_true(sxcl_hash_digest(bad_algo, "abc", 3u, got, HEXBUF) == -1, "digest 未知算法 -> -1");
    check_true(sxcl_hash_digest(SXCL_HASH_SHA1, NULL, 3u, got, HEXBUF) == -1, "digest data==NULL 且 len>0 -> -1");

    /* 未知算法上下文：init/update 不崩，final 明确失败。 */
    sxcl_hash_init(&ctx, bad_algo);
    sxcl_hash_update(&ctx, "abc", 3u);
    check_true(sxcl_hash_final_hex(&ctx, got, sizeof got) == -1, "未知算法 final_hex -> -1");

    /* NULL ctx / 0 长度 update 是安全空操作。 */
    sxcl_hash_init(NULL, SXCL_HASH_SHA1);
    sxcl_hash_update(NULL, "abc", 3u);
    sxcl_hash_init(&ctx, SXCL_HASH_SHA1);
    sxcl_hash_update(&ctx, NULL, 0u);
    sxcl_hash_update(&ctx, "a", 1u);
    sxcl_hash_update(&ctx, "b", 1u);
    sxcl_hash_update(&ctx, "c", 1u);
    check_true(sxcl_hash_final_hex(&ctx, got, sizeof got) == 0, "NULL/0 长度 update 后仍可正常收尾");
    check_hex("零散 update 拼出 abc", got, "a9993e364706816aba3e25717850c26c9cd0d89d");

    /* 复用同一 ctx：init 必须完全重置状态。 */
    sxcl_hash_init(&ctx, SXCL_HASH_SHA256);
    sxcl_hash_update(&ctx, "xxxx", 4u);
    sxcl_hash_init(&ctx, SXCL_HASH_SHA256);
    sxcl_hash_update(&ctx, "abc", 3u);
    check_true(sxcl_hash_final_hex(&ctx, got, sizeof got) == 0, "ctx 复用：重新 init 后收尾成功");
    check_hex("ctx 复用后 sha256(abc)", got,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    /* 一大块 vs 逐字节：极端切片比。 */
    {
        unsigned char big[300];
        char one_shot[HEXBUF];
        size_t i;
        for (i = 0u; i < sizeof big; ++i) {
            big[i] = (unsigned char)(i * 7u + 3u);
        }
        digest_hex(SXCL_HASH_SHA256, big, sizeof big, one_shot);
        sxcl_hash_init(&ctx, SXCL_HASH_SHA256);
        for (i = 0u; i < sizeof big; ++i) {
            sxcl_hash_update(&ctx, &big[i], 1u);
        }
        check_true(sxcl_hash_final_hex(&ctx, got, sizeof got) == 0, "逐字节 update 收尾成功");
        check_hex("300B 逐字节 == 一次性", got, one_shot);
    }
}

/* ------------------------------------------------------------------ */
/* 7) 吞吐（仅打印，不设阈值）                                          */
/* ------------------------------------------------------------------ */

static void test_throughput(void)
{
    const size_t size = 32u * 1024u * 1024u; /* 32 MiB */
    const int passes = 3;
    unsigned char *buf = (unsigned char *)malloc(size);
    char got[HEXBUF];
    clock_t t0;
    clock_t t1;
    double secs;
    double mbps;
    int p;

    printf("-- 吞吐 --\n");

    if (buf == NULL) {
        printf("skip throughput: malloc(32 MiB) 失败\n");
        return;
    }
    memset(buf, 0xA5, size);

    t0 = clock();
    for (p = 0; p < passes; ++p) {
        if (sxcl_hash_digest(SXCL_HASH_SHA256, buf, size, got, sizeof got) != 0) {
            free(buf);
            ++g_failures;
            fprintf(stderr, "FAIL 吞吐测试中 digest 失败\n");
            return;
        }
    }
    t1 = clock();

    secs = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    if (secs <= 0.0) {
        secs = 1e-9;
    }
    mbps = ((double)size * (double)passes) / (1024.0 * 1024.0) / secs;
    printf("throughput: sha256 %.1f MB/s (%.0f MiB x %d passes, %.3f s, digest=%s)\n",
           mbps, (double)size / (1024.0 * 1024.0), passes, secs, got);
    check_true(strlen(got) == 64u, "吞吐测试同时校验摘要长度为 64 字符");
    check_true(sxcl_hash_hex_equal(got, got) == 1, "吞吐测试摘要可自比较");
    free(buf);
}

int main(void)
{
    printf("== sxcl hash test ==\n");

    test_standard_vectors();
    test_chunked_update();
    test_boundary_vectors();
    test_large_stream_64bit_length();
    test_hex_equal();
    test_api_edges();
    test_throughput();

    printf("== %d 项断言, %d 项失败 ==\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
