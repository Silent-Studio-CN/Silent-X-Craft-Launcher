/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* xz_test.c - .xz(LZMA2)解码器的夹具测试。
 *
 * 夹具是**签入仓库的真 .xz 文件**(用 Python 的 lzma 生成,见 docs/19 的复现命令),
 * 所以这个用例不联网、不依赖 Python、不依赖本机装了 xz:
 *   * jre-tiny-*.xz    同一份 tar 用 CRC64/CRC32/无校验/不压缩四种口径压出来的;
 *   * repeat.xz        高度重复的数据(长匹配 + 重叠展开,dist < len 的那条路);
 *   * rand-uncomp.xz   不可压缩 -> xz 会写"未压缩块"(control 0x01/0x02 那条路);
 *   * multiblock.xz    7.2 MB 输出 -> 多块 + 索引记录核对;
 *   * bigdict.xz       preset 9(64 MiB 字典)-> 字典上限那道闸;
 *   * concat.xz        两条流串联 + 中间 4 字节对齐填充;
 *   * bcj-x86.xz       x86 BCJ 过滤器 -> **必须**报 unsupported;
 *   * corrupt.xz       压缩数据里翻一个字节 -> **必须**被 CRC 抓住;
 *   * truncated.xz     截断 -> **必须**报数据错。
 *
 * 期望值:输出字节数 + 输出的 SHA-256(在生成夹具时算好,写死在这里)。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/hash.h"
#include "sxcl/xz.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

/* ── 夹具路径 ── */

static char g_dir[1024];

static int fixture_path(char *out, size_t out_len, const char *name)
{
    if (g_dir[0] == '\0') {
        const char *env = getenv("SXCL_XZ_FIXTURES");
        snprintf(g_dir, sizeof(g_dir), "%s", env != NULL ? env : "tests/fixtures/xz");
    }
    return snprintf(out, out_len, "%s/%s", g_dir, name) < (int)out_len ? 0 : -1;
}

static unsigned char *read_file(const char *name, size_t *len_out)
{
    char path[1200];
    FILE *fp;
    long size;
    unsigned char *buf;
    if (fixture_path(path, sizeof(path), name) != 0) {
        return NULL;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  [!!] 打不开夹具 %s\n", path);
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        (void)fclose(fp);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        (void)fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        (void)fclose(fp);
        return NULL;
    }
    (void)fclose(fp);
    buf[size] = 0;
    *len_out = (size_t)size;
    return buf;
}

/* ── 内存输入源 ── */

typedef struct mem_src {
    const unsigned char *data;
    size_t len;
    size_t pos;
    size_t step;   /* >0 = 每次最多给 step 字节(测"短读"这条路) */
} mem_src;

static int64_t mem_read(void *ud, void *buf, size_t len)
{
    mem_src *m = (mem_src *)ud;
    size_t n = m->len - m->pos;
    if (m->step > 0 && n > m->step) {
        n = m->step;
    }
    if (n > len) {
        n = len;
    }
    if (n > 0) {
        memcpy(buf, m->data + m->pos, n);
        m->pos += n;
    }
    return (int64_t)n;
}

/* ── 记账 sink(只算 sha256 与字节数,不攒内存) ── */

typedef struct probe {
    sxcl_hash_ctx h;
    int64_t bytes;
    int fail_at;      /* >0 时:收到第 fail_at 个字节后主动中止(测 sink 中止这条路) */
} probe;

static int probe_sink(void *ud, const void *data, size_t len)
{
    probe *p = (probe *)ud;
    p->bytes += (int64_t)len;
    sxcl_hash_update(&p->h, data, len);
    if (p->fail_at > 0 && p->bytes >= p->fail_at) {
        return 1;
    }
    return 0;
}

/** 解一份夹具,返回返回码,并把输出摘要写进 digest_hex。 */
static int decode_fixture(const char *name, char *digest_hex, size_t digest_len,
                          int64_t *bytes_out, size_t step, int64_t out_limit,
                          char *err, size_t err_len)
{
    size_t in_len = 0;
    unsigned char *in = read_file(name, &in_len);
    mem_src src;
    probe p;
    sxcl_xz_source source;
    sxcl_xz *xz;
    int rc;
    if (in == NULL) {
        return -1000;
    }
    memset(&src, 0, sizeof(src));
    src.data = in;
    src.len = in_len;
    src.step = step;
    source.read = mem_read;
    source.ud = &src;
    memset(&p, 0, sizeof(p));
    sxcl_hash_init(&p.h, SXCL_HASH_SHA256);
    xz = sxcl_xz_open(&source, NULL, probe_sink, &p, err, err_len);
    if (xz == NULL) {
        free(in);
        return -1001;
    }
    if (out_limit > 0) {
        /* 直接改句柄里的配置没有公开入口:用 opts 重新开一个更干净 */
        sxcl_xz_close(xz);
        memset(&p, 0, sizeof(p));
        sxcl_hash_init(&p.h, SXCL_HASH_SHA256);
        memset(&src, 0, sizeof(src));
        src.data = in;
        src.len = in_len;
        src.step = step;
        {
            sxcl_xz_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.out_limit = out_limit;
            xz = sxcl_xz_open(&source, &opts, probe_sink, &p, err, err_len);
        }
        if (xz == NULL) {
            free(in);
            return -1001;
        }
    }
    rc = sxcl_xz_run(xz, err, err_len);
    if (rc != SXCL_XZ_OK) {
        printf("      [err] %s -> %s: %s\n", name, sxcl_xz_code_name(rc), err);
    }
    if (digest_hex != NULL && digest_len > 0) {
        (void)sxcl_hash_final_hex(&p.h, digest_hex, digest_len);
    }
    if (bytes_out != NULL) {
        *bytes_out = p.bytes;
    }
    sxcl_xz_close(xz);
    free(in);
    return rc;
}

/* ── 用例 ── */

static const char *kTarSha256 =
    "0e6e9a8f6e9aebcadbe21121cc310e57a313de1234ba425bba1bc87713bbc3f6";
static const char *kTarSha256Short =
    "0e6e9a8f6e9aebcadbe21121cc310e57a313de1234ba425bba1bc87713bbc3f6";

static void test_tiny_variants(void)
{
    static const char *names[] = {"jre-tiny-crc64.xz", "jre-tiny-crc32.xz", "jre-tiny-none.xz",
                                  "jre-tiny-uncomp.xz", "bigdict.xz"};
    size_t i;
    printf("== 同一条 tar 的四种校验/压缩口径 + preset9 大字典\n");
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char digest[65];
        char err[256];
        int64_t bytes = 0;
        const int rc = decode_fixture(names[i], digest, sizeof(digest), &bytes, 0, 0, err,
                                      sizeof(err));
        check_int(rc, 0, names[i]);
        check_int((long)bytes, 102400, names[i]);
        check_str(digest, kTarSha256, names[i]);
    }
}

static void report_first_mismatch(const char *xz_name, const char *raw_name);

static void test_hash_matches_tar(void)
{
    /* 解出来的东西必须与签入的 .tar 字节级一致(不只是 sha256 字面量对得上) */
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    size_t len = 0;
    unsigned char *tar;
    char tar_digest[65];
    const int rc = decode_fixture("jre-tiny-crc64.xz", digest, sizeof(digest), &bytes, 0, 0, err,
                                  sizeof(err));
    check_int(rc, 0, "解 jre-tiny-crc64.xz");
    tar = read_file("jre-tiny.tar", &len);
    check(tar != NULL, "读到 jre-tiny.tar");
    if (tar != NULL) {
        sxcl_hash_digest(SXCL_HASH_SHA256, tar, len, tar_digest, sizeof(tar_digest));
        check_str(tar_digest, digest, "解出来的 sha256 == .tar 的 sha256");
        check_int((long)len, 102400, "jre-tiny.tar 长度");
        free(tar);
    }
    (void)kTarSha256Short;
    report_first_mismatch("jre-tiny-crc64.xz", "jre-tiny.tar");
}

/* 解到一块预分配缓冲里,和期望字节逐字节比,报**第一个不一致的位置** —— 定位发散点用。 */
typedef struct cmp_sink {
    unsigned char *buf;
    size_t cap;
    size_t len;
} cmp_sink;

static int cmp_write(void *ud, const void *data, size_t len)
{
    cmp_sink *s = (cmp_sink *)ud;
    if (s->len + len > s->cap) {
        return 1;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    return 0;
}

static void report_first_mismatch(const char *xz_name, const char *raw_name)
{
    size_t xz_len = 0, raw_len = 0;
    unsigned char *xz = read_file(xz_name, &xz_len);
    unsigned char *raw = read_file(raw_name, &raw_len);
    cmp_sink sink;
    mem_src src;
    sxcl_xz_source source;
    sxcl_xz *h;
    char err[256];
    size_t i;
    int rc;
    if (xz == NULL || raw == NULL) {
        free(xz);
        free(raw);
        return;
    }
    memset(&sink, 0, sizeof(sink));
    sink.buf = (unsigned char *)malloc(raw_len + 4096);
    sink.cap = raw_len + 4096;
    memset(&src, 0, sizeof(src));
    src.data = xz;
    src.len = xz_len;
    source.read = mem_read;
    source.ud = &src;
    h = sxcl_xz_open(&source, NULL, cmp_write, &sink, err, sizeof(err));
    if (h != NULL) {
        rc = sxcl_xz_run(h, err, sizeof(err));
        printf("      [trace] %s: rc=%d out=%lu 期望=%lu err=%s\n", xz_name, rc,
               (unsigned long)sink.len, (unsigned long)raw_len, err);
        for (i = 0; i < sink.len && i < raw_len; ++i) {
            if (sink.buf[i] != raw[i]) {
                size_t k;
                printf("      [trace] 第一个不一致在偏移 %lu:\n        got :", (unsigned long)i);
                for (k = i; k < i + 12 && k < sink.len; ++k) {
                    printf(" %02x", sink.buf[k]);
                }
                printf("\n        want:");
                for (k = i; k < i + 12 && k < raw_len; ++k) {
                    printf(" %02x", raw[k]);
                }
                printf("\n");
                break;
            }
        }
        if (i == raw_len && sink.len >= raw_len) {
            printf("      [trace] 前缀完全一致\n");
        }
        sxcl_xz_close(h);
    }
    free(sink.buf);
    free(xz);
    free(raw);
}

static void test_repeat_and_rand(void)
{
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    int rc;

    printf("== 长匹配 / 重叠展开(repeat.xz)\n");
    rc = decode_fixture("repeat.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, 0, "repeat.xz rc");
    check_int((long)bytes, 300000, "repeat.xz 输出字节数");
    check_str(digest, "7015b3562f80b5784275ef6f3a8f073cafdc623739e7df1d40b582aed7d88e30",
              "repeat.xz sha256");

    printf("== 不可压缩 -> 未压缩块(rand-uncomp.xz)\n");
    rc = decode_fixture("rand-uncomp.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, 0, "rand-uncomp.xz rc");
    check_int((long)bytes, 100000, "rand-uncomp.xz 输出字节数");
    check_str(digest, "d060a877edc4837c1b0a5cd174176fc6332d1b86e486d8bd2b30d13878653b47",
              "rand-uncomp.xz sha256");
}

static void test_multiblock(void)
{
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    int rc;
    printf("== 7.2 MB 多块 + 索引记录核对(multiblock.xz)\n");
    rc = decode_fixture("multiblock.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, 0, "multiblock.xz rc");
    check_int((long)bytes, 7200000, "multiblock.xz 输出字节数");
    check_str(digest, "269bbd3185a6d43b346c41b7df24e7d771dbde3ac64fc9fa3c85c4d51d92626b",
              "multiblock.xz sha256");
}

static void test_short_read(void)
{
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    const int rc = decode_fixture("jre-tiny-crc64.xz", digest, sizeof(digest), &bytes, 7, 0, err,
                                  sizeof(err));
    printf("== 短读(每次最多 7 字节喂进来)\n");
    check_int(rc, 0, "短读 rc");
    check_int((long)bytes, 102400, "短读输出字节数");
    check_str(digest, kTarSha256, "短读 sha256");
}

static void test_concat(void)
{
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    const int rc = decode_fixture("concat.xz", digest, sizeof(digest), &bytes, 0, 0, err,
                                  sizeof(err));
    printf("== 两条流串联 + 填充(concat.xz)\n");
    check_int(rc, 0, "concat.xz rc");
    check_int((long)bytes, 152400, "concat.xz 输出字节数");
    check_str(digest, "72e6a0d837c7186f38c99652b6b9b3ee5fb103f577ad6df70857ec2a5681619c",
              "concat.xz sha256");
}

static void test_negative(void)
{
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    int rc;

    printf("== 负例:不支持的过滤器 / 坏数据 / 截断 / 非 xz\n");
    rc = decode_fixture("bcj-x86.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, SXCL_XZ_ERR_UNSUPPORTED, "bcj-x86.xz 必须报 unsupported");
    check(strstr(err, "过滤器") != NULL, "bcj 的错误信息点到过滤器");

    /* 压缩数据里翻一个字节:算术编码流会立刻发散,可能先撞上"距离越界"而不是 CRC —— 
     * 两条都是 data,只要**不静默解出垃圾**就算过。 */
    rc = decode_fixture("corrupt.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, SXCL_XZ_ERR_DATA, "corrupt.xz 必须报 data(压缩数据坏了)");
    check(err[0] != '\0', "corrupt 有人话错误");

    /* 只翻块校验字段里的一个字节:这一次必须是被 CRC 抓住的那条路 */
    rc = decode_fixture("badcheck.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, SXCL_XZ_ERR_DATA, "badcheck.xz 必须报 data(校验字段被改)");
    check(strstr(err, "CRC") != NULL, "badcheck 的错误信息点到 CRC");

    rc = decode_fixture("truncated.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, SXCL_XZ_ERR_DATA, "truncated.xz 必须报 data");

    rc = decode_fixture("jre-tiny.tar", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, SXCL_XZ_ERR_DATA, "拿 tar 当 xz 解必须报 data");

    rc = decode_fixture("jre-tiny-crc64.xz", digest, sizeof(digest), &bytes, 0, 1000, err,
                        sizeof(err));
    check_int(rc, SXCL_XZ_ERR_LIMIT, "out_limit 必须拦住 multiblock/repeat 之外的大输出");

    /* 边界:0 字节的输入(空文件)——必须报 data,不许崩、不许当成"空流"成功 */
    rc = decode_fixture("empty.xz", digest, sizeof(digest), &bytes, 0, 0, err, sizeof(err));
    check_int(rc, SXCL_XZ_ERR_DATA, "0 字节输入必须报 data");
    check_int((long)bytes, 0, "0 字节输入解出 0 字节");
}

static void test_dict_limit(void)
{
    char err[256];
    char digest[65];
    int64_t bytes = 0;
    size_t in_len = 0;
    unsigned char *in = read_file("bigdict.xz", &in_len);
    mem_src src;
    sxcl_xz_source source;
    sxcl_xz_opts opts;
    sxcl_xz *xz;
    int rc;

    printf("== 字典上限(防解压炸弹)\n");
    check(in != NULL, "读到 bigdict.xz");
    if (in == NULL) {
        return;
    }
    memset(&src, 0, sizeof(src));
    src.data = in;
    src.len = in_len;
    source.read = mem_read;
    source.ud = &src;
    memset(&opts, 0, sizeof(opts));
    opts.dict_limit = 1 * 1024 * 1024; /* 夹具是 preset 9(64 MiB 字典) */
    xz = sxcl_xz_open(&source, &opts, NULL, NULL, err, sizeof(err));
    check(xz != NULL, "开句柄");
    if (xz != NULL) {
        rc = sxcl_xz_run(xz, err, sizeof(err));
        check_int(rc, SXCL_XZ_ERR_MEMLIMIT, "1 MiB 上限必须拦住 64 MiB 字典");
        check(strstr(err, "字典") != NULL, "错误信息点到字典");
        sxcl_xz_close(xz);
    }
    free(in);
    (void)digest;
    (void)bytes;
}

static void test_decode_memory(void)
{
    char err[256];
    size_t in_len = 0;
    size_t tar_len = 0;
    unsigned char *in = read_file("jre-tiny-crc64.xz", &in_len);
    unsigned char *tar = read_file("jre-tiny.tar", &tar_len);
    unsigned char *out;
    size_t out_len = 0;
    int rc;

    printf("== 便捷入口(内存 -> 内存)\n");
    check(in != NULL && tar != NULL, "读到夹具");
    if (in == NULL || tar == NULL) {
        free(in);
        free(tar);
        return;
    }
    out = (unsigned char *)malloc(tar_len);
    check(out != NULL, "分配输出");
    if (out != NULL) {
        rc = sxcl_xz_decode_memory(in, in_len, out, tar_len, &out_len, err, sizeof(err));
        check_int(rc, 0, "decode_memory rc");
        check_int((long)out_len, (long)tar_len, "decode_memory 长度");
        check(memcmp(out, tar, tar_len) == 0, "decode_memory 字节级一致");
        /* 缓冲故意少一个字节 */
        rc = sxcl_xz_decode_memory(in, in_len, out, tar_len - 1, &out_len, err, sizeof(err));
        check_int(rc, SXCL_XZ_ERR_LIMIT, "缓冲不够要单独报(不是 data)");
        check_int((long)out_len, (long)tar_len, "缓冲不够时报出需要的字节数");
        free(out);
    }
    {
        unsigned char *heap_out = NULL;
        size_t heap_len = 0;
        rc = sxcl_xz_decode_alloc(in, in_len, &heap_out, &heap_len, err, sizeof(err));
        check_int(rc, 0, "decode_alloc rc");
        check_int((long)heap_len, (long)tar_len, "decode_alloc 长度");
        check(heap_out != NULL && memcmp(heap_out, tar, tar_len) == 0, "decode_alloc 字节级一致");
        free(heap_out);
    }
    free(in);
    free(tar);
}

static void test_sink_abort(void)
{
    size_t in_len = 0;
    unsigned char *in = read_file("multiblock.xz", &in_len);
    mem_src src;
    sxcl_xz_source source;
    probe p;
    sxcl_xz *xz;
    int rc;
    char err[256];

    printf("== sink 主动中止(取消)\n");
    check(in != NULL, "读到 multiblock.xz");
    if (in == NULL) {
        return;
    }
    memset(&src, 0, sizeof(src));
    src.data = in;
    src.len = in_len;
    source.read = mem_read;
    source.ud = &src;
    memset(&p, 0, sizeof(p));
    sxcl_hash_init(&p.h, SXCL_HASH_SHA256);
    p.fail_at = 100000;
    xz = sxcl_xz_open(&source, NULL, probe_sink, &p, err, sizeof(err));
    check(xz != NULL, "开句柄");
    if (xz != NULL) {
        rc = sxcl_xz_run(xz, err, sizeof(err));
        check_int(rc, SXCL_XZ_ERR_ABORT, "sink 返回非 0 -> abort");
        check(p.bytes < 7200000, "确实提前停了");
        /* 粘性:再跑一次拿到同一个结果 */
        check_int(sxcl_xz_run(xz, err, sizeof(err)), SXCL_XZ_ERR_ABORT, "结果粘性");
        sxcl_xz_close(xz);
    }
    free(in);
}

static void test_names(void)
{
    printf("== 返回码名字\n");
    check_str(sxcl_xz_code_name(SXCL_XZ_OK), "ok", "ok");
    check_str(sxcl_xz_code_name(SXCL_XZ_ERR_DATA), "data", "data");
    check_str(sxcl_xz_code_name(SXCL_XZ_ERR_UNSUPPORTED), "unsupported", "unsupported");
    check_str(sxcl_xz_code_name(SXCL_XZ_ERR_MEMLIMIT), "memlimit", "memlimit");
    check_str(sxcl_xz_code_name(SXCL_XZ_ERR_ABORT), "abort", "abort");
    check_str(sxcl_xz_code_name(-999), "?", "未知");
    check(sxcl_xz_open(NULL, NULL, NULL, NULL, NULL, 0) == NULL, "source 为空 -> NULL");
}

/* ── 诊断模式:sxcl_xz_test <file.xz> [expected.raw] ──
 * 解一个**任意** .xz(不必是仓库夹具),把解出来的字节留在内存里,打印
 * 返回码/字节数/摘要/人话错误;给了 expected.raw 就逐字节比并报**第一个不一致的位置**。
 * 定位解码发散点用(前缀二分就是靠它)。 */
typedef struct cli_sink {
    unsigned char *buf;
    size_t cap;
    size_t len;
    sxcl_hash_ctx h;
} cli_sink;

static int cli_write(void *ud, const void *data, size_t len)
{
    cli_sink *s = (cli_sink *)ud;
    if (s->len + len > s->cap) {
        return 1;
    }
    memcpy(s->buf + s->len, data, len);
    sxcl_hash_update(&s->h, data, len);
    s->len += len;
    return 0;
}

static int decode_cli(const char *path, const char *expect_path)
{
    FILE *fp = fopen(path, "rb");
    long size;
    unsigned char *in;
    char err[256];
    char digest[65];
    cli_sink sink;
    mem_src src;
    sxcl_xz_source source;
    sxcl_xz *xz;
    int rc;
    if (fp == NULL) {
        printf("打不开 %s\n", path);
        return 2;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    in = (unsigned char *)malloc((size_t)size + 1u);
    if (in == NULL || fread(in, 1, (size_t)size, fp) != (size_t)size) {
        (void)fclose(fp);
        free(in);
        return 2;
    }
    (void)fclose(fp);
    memset(&src, 0, sizeof(src));
    src.data = in;
    src.len = (size_t)size;
    source.read = mem_read;
    source.ud = &src;
    memset(&sink, 0, sizeof(sink));
    sink.cap = 512u * 1024u * 1024u;
    sink.buf = (unsigned char *)malloc(sink.cap);
    sxcl_hash_init(&sink.h, SXCL_HASH_SHA256);
    err[0] = '\0';
    xz = (sink.buf == NULL)
             ? NULL
             : sxcl_xz_open(&source, NULL, cli_write, &sink, err, sizeof(err));
    rc = (xz == NULL) ? -1000 : sxcl_xz_run(xz, err, sizeof(err));
    (void)sxcl_hash_final_hex(&sink.h, digest, sizeof(digest));
    printf("rc=%d(%s) out=%lu sha256=%s err=%s\n", rc, sxcl_xz_code_name(rc),
           (unsigned long)sink.len, digest, err);
    sxcl_xz_close(xz);
    if (expect_path != NULL && sink.buf != NULL) {
        FILE *ef = fopen(expect_path, "rb");
        if (ef != NULL) {
            unsigned char *want;
            long wsize;
            (void)fseek(ef, 0, SEEK_END);
            wsize = ftell(ef);
            (void)fseek(ef, 0, SEEK_SET);
            want = (unsigned char *)malloc((size_t)(wsize > 0 ? wsize : 0) + 1u);
            if (want != NULL && wsize > 0 &&
                fread(want, 1, (size_t)wsize, ef) == (size_t)wsize) {
                size_t i;
                size_t lim = sink.len < (size_t)wsize ? sink.len : (size_t)wsize;
                for (i = 0; i < lim; ++i) {
                    if (sink.buf[i] != want[i]) {
                        break;
                    }
                }
                if (i == lim && sink.len >= (size_t)wsize) {
                    printf("trace: 解出来的前缀与期望完全一致(%lu 字节)\n",
                           (unsigned long)lim);
                } else {
                    size_t k;
                    printf("trace: 第一个不一致在偏移 %lu / 期望 %ld 字节\n", (unsigned long)i,
                           wsize);
                    printf("  got :");
                    for (k = i; k < i + 12 && k < sink.len; ++k) {
                        printf(" %02x", sink.buf[k]);
                    }
                    printf("\n  want:");
                    for (k = i; k < i + 12 && k < (size_t)wsize; ++k) {
                        printf(" %02x", want[k]);
                    }
                    printf("\n");
                }
            }
            free(want);
            (void)fclose(ef);
        }
    }
    free(sink.buf);
    free(in);
    return rc == 0 ? 0 : 1;
}


int main(int argc, char **argv)
{
    if (argc >= 2) {
        return decode_cli(argv[1], argc >= 3 ? argv[2] : NULL);
    }
    test_tiny_variants();
    test_hash_matches_tar();
    test_repeat_and_rand();
    test_multiblock();
    test_short_read();
    test_concat();
    test_negative();
    test_dict_limit();
    test_decode_memory();
    test_sink_abort();
    test_names();
    printf("\nxz_test: 通过 %d,失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
