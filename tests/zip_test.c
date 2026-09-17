/* SXCL-C ZIP 读取测试:任何失败都会让 main 返回非 0,ctest 判定失败。
 *
 * 覆盖内容:
 *   1) 手工拼出来的 zip(测试里自带一个最小 ZIP 写入器,字节完全可控):
 *         - stored / deflate(手写 fixed Huffman 流)两种方法;
 *         - 名字编码:纯 ASCII、CP437(0x82 = é)、UTF-8 标志位置位的非 ASCII;
 *         - "本地头大小字段为 0 + 数据描述符"的条目 —— 必须按中央目录取值;
 *         - 目录条目、不支持的方法(12 = bzip2)、加密位、ZIP64 大小字段;
 *         - 损坏数据:不是 zip、空文件、尾部被截断、本地头签名损坏、
 *           压缩长度超出文件、中央目录偏移超出文件 —— 必须报错不崩;
 *   2) 真实 zip 夹具:测试里用 system() 调 PowerShell 的 Compress-Archive
 *      把一棵已知内容的临时目录压成 zip,再用 sxcl_zip_open 打开它,
 *      校验条目名 / 大小 / 方法,并把每条解出来的内容与原始字节逐字节比对;
 *   3) 提取到文件:自动建父目录、内容正确、原子落位后 .tmp 不残留。
 *
 * 夹具生成方式说明:不用 add_test 之前的外部步骤(那会改动 CMakeLists 的结构),
 * 而是测试自己写一个 .ps1 再 system() 调用,生成物落在 ctest 的工作目录(build/),
 * 测试结束把 zip 删掉,仓库里不留任何 zip 二进制。
 */
#define _CRT_SECURE_NO_WARNINGS 1   /* 测试里用 fopen/fwrite 造夹具,MSVC 会标记弃用 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/zip.h"

static int g_checks = 0;
static int g_failures = 0;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

/* 临时目录都放在 ctest 的工作目录(build/)下,不进仓库 */
#define TMP_DIR  "_zip_test"
#define HAND_ZIP TMP_DIR "/handmade.zip"
#define FIX_SRC  TMP_DIR "/src"
#define FIX_ZIP  TMP_DIR "/fixture.zip"
#define FIX_OUT  TMP_DIR "/out"
#define FIX_PS1  TMP_DIR "/fixture.ps1"

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    if (len > 0u && fwrite(data, 1u, len, f) != len) {
        fclose(f);
        return -1;
    }
    return (fclose(f) == 0) ? 0 : -1;
}

/* ================================================================== */
/* 最小 ZIP 写入器(手工拼字节,和被测读取器完全独立)                    */
/* ================================================================== */

typedef struct zbuf {
    unsigned char *p;
    size_t len;
    size_t cap;
    int over;
} zbuf;

static void zb_u16(zbuf *b, unsigned v)
{
    if (b->len + 2u > b->cap) {
        b->over = 1;
        return;
    }
    b->p[b->len] = (unsigned char)(v & 0xFFu);
    b->p[b->len + 1u] = (unsigned char)((v >> 8) & 0xFFu);
    b->len += 2u;
}

static void zb_u32(zbuf *b, unsigned long v)
{
    unsigned i;
    if (b->len + 4u > b->cap) {
        b->over = 1;
        return;
    }
    for (i = 0u; i < 4u; ++i) {
        b->p[b->len + i] = (unsigned char)((v >> (8u * i)) & 0xFFu);
    }
    b->len += 4u;
}

static void zb_raw(zbuf *b, const void *d, size_t n)
{
    if (b->len + n > b->cap) {
        b->over = 1;
        return;
    }
    if (n > 0u) {
        memcpy(b->p + b->len, d, n);
    }
    b->len += n;
}

/* 一条待写入的条目。 */
typedef struct zspec {
    const unsigned char *name;   /* 原始名字字节(可能是 CP437 或 UTF-8) */
    size_t               name_len;
    unsigned             flags;
    unsigned             method;
    const unsigned char *cdata;  /* 数据区字节(deflate 时是压缩数据) */
    size_t               csize;
    unsigned long        usize;  /* 解压后大小 */
    unsigned long        cd_comp;      /* 中央目录里写的 comp(默认 = csize) */
    unsigned long        cd_uncomp;    /* 中央目录里写的 uncomp(默认 = usize) */
    int                  local_zero;   /* 本地头大小写 0(数据描述符风格) */
    int                  descriptor;   /* 数据后面跟 16 字节数据描述符 */
    int                  break_local;  /* 故意破坏本地头签名 */
    long                 local_off_override;  /* >= 0 时覆盖中央目录里的本地头偏移 */
} zspec;

static size_t build_zip(unsigned char *out, size_t cap, const zspec *es, size_t n)
{
    zbuf b;
    size_t i;
    size_t cd_start;
    size_t cd_size;

    b.p = out;
    b.len = 0u;
    b.cap = cap;
    b.over = 0;

    /* ── 本地头 + 数据 ── */
    for (i = 0u; i < n; ++i) {
        const zspec *e = &es[i];
        zb_u32(&b, e->break_local ? 0x04034B51u : 0x04034B50u);
        zb_u16(&b, 20u);                                  /* version needed */
        zb_u16(&b, e->flags);
        zb_u16(&b, e->method);
        zb_u16(&b, 0u);                                   /* time */
        zb_u16(&b, 0x21u);                                /* date */
        zb_u32(&b, 0u);                                   /* crc(读取端不校验) */
        zb_u32(&b, e->local_zero ? 0u : (unsigned long)e->csize);
        zb_u32(&b, e->local_zero ? 0u : e->usize);
        zb_u16(&b, (unsigned)e->name_len);
        zb_u16(&b, 0u);                                   /* extra len */
        zb_raw(&b, e->name, e->name_len);
        zb_raw(&b, e->cdata, e->csize);
        if (e->descriptor != 0) {
            zb_u32(&b, 0x08074B50u);                      /* 数据描述符签名 */
            zb_u32(&b, 0u);
            zb_u32(&b, (unsigned long)e->csize);
            zb_u32(&b, e->usize);
        }
    }

    /* ── 中央目录 ── */
    cd_start = b.len;
    for (i = 0u; i < n; ++i) {
        const zspec *e = &es[i];
        zb_u32(&b, 0x02014B50u);
        zb_u16(&b, 20u);                                  /* version made by */
        zb_u16(&b, 20u);                                  /* version needed */
        zb_u16(&b, e->flags);
        zb_u16(&b, e->method);
        zb_u16(&b, 0u);
        zb_u16(&b, 0x21u);
        zb_u32(&b, 0u);
        zb_u32(&b, (e->cd_comp != 0u) ? e->cd_comp : (unsigned long)e->csize);
        zb_u32(&b, (e->cd_uncomp != 0u) ? e->cd_uncomp : e->usize);
        zb_u16(&b, (unsigned)e->name_len);
        zb_u16(&b, 0u);                                   /* extra */
        zb_u16(&b, 0u);                                   /* comment */
        zb_u16(&b, 0u);                                   /* disk */
        zb_u16(&b, 0u);                                   /* internal attr */
        zb_u32(&b, 0u);                                   /* external attr */
        {
            const size_t local_at = (e->local_off_override >= 0)
                                        ? (size_t)e->local_off_override
                                        : (e->local_zero ? (size_t)0 : (size_t)0);
            /* 本地头偏移:正常情况下要从"本地头起点"算,这里用一个简表推 */
            (void)local_at;
        }
        zb_u32(&b, 0u);                                   /* 本地头偏移:下面统一回填 */
        zb_raw(&b, e->name, e->name_len);
    }
    cd_size = b.len - cd_start;

    /* 回填每条的本地头偏移(本地头是连续写的:30 + 名字 + 数据 + 可选描述符)。 */
    {
        size_t off = 0u;
        for (i = 0u; i < n; ++i) {
            const zspec *e = &es[i];
            size_t rec_off = cd_start;
            size_t j;
            unsigned long lo;

            for (j = 0u; j < i; ++j) {
                rec_off += 46u + es[j].name_len;
            }
            lo = (e->local_off_override >= 0) ? (unsigned long)e->local_off_override
                                              : (unsigned long)off;
            out[rec_off + 42u] = (unsigned char)(lo & 0xFFu);
            out[rec_off + 43u] = (unsigned char)((lo >> 8) & 0xFFu);
            out[rec_off + 44u] = (unsigned char)((lo >> 16) & 0xFFu);
            out[rec_off + 45u] = (unsigned char)((lo >> 24) & 0xFFu);

            off += 30u + e->name_len + e->csize + ((e->descriptor != 0) ? 16u : 0u);
        }
    }

    /* ── EOCD ── */
    zb_u32(&b, 0x06054B50u);
    zb_u16(&b, 0u);
    zb_u16(&b, 0u);
    zb_u16(&b, (unsigned)n);
    zb_u16(&b, (unsigned)n);
    zb_u32(&b, (unsigned long)cd_size);
    zb_u32(&b, (unsigned long)cd_start);
    zb_u16(&b, 0u);

    return b.over ? 0u : b.len;
}

/* 极简 fixed Huffman 编码器:只发字面量 + EOB,给 deflate 条目造压缩数据。 */
typedef struct bitw2 {
    unsigned char *p;
    size_t len;
    unsigned bit;
} bitw2;

static void bw2_put(bitw2 *w, unsigned value, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; ++i) {
        if (((value >> i) & 1u) != 0u) {
            w->p[w->len] |= (unsigned char)(1u << w->bit);
        }
        w->bit += 1u;
        if (w->bit == 8u) {
            w->bit = 0u;
            w->len += 1u;
        }
    }
}

static void bw2_code(bitw2 *w, unsigned code, unsigned n)
{
    unsigned i;
    for (i = n; i > 0u; --i) {
        bw2_put(w, (code >> (i - 1u)) & 1u, 1u);
    }
}

static size_t make_fixed_deflate(const unsigned char *text, size_t len, unsigned char *out)
{
    bitw2 w;
    size_t i;

    w.p = out;
    w.len = 0u;
    w.bit = 0u;
    bw2_put(&w, 1u, 1u);      /* BFINAL */
    bw2_put(&w, 1u, 2u);      /* BTYPE = 01 固定 Huffman */
    for (i = 0u; i < len; ++i) {
        const unsigned c = text[i];
        if (c < 144u) {
            bw2_code(&w, 0x30u + c, 8u);
        } else if (c < 256u) {
            bw2_code(&w, 0x190u + (c - 144u), 9u);
        } else {
            bw2_code(&w, 0xC0u + (c - 280u), 8u);
        }
    }
    bw2_code(&w, 0u, 7u);     /* EOB(256)的固定码 = 0,7 位 */
    return w.len + ((w.bit != 0u) ? 1u : 0u);
}

/* ================================================================== */
/* 手工 zip 的内容与断言                                                */
/* ================================================================== */

static const unsigned char PLAIN_STORED[] = "stored entry: hello from SXCL-C\n";
static const unsigned char PLAIN_DEFLATE[] =
    "deflate entry: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA BBBBBBBBBBBBBBBBBBBB\n";

#define NAME_CP437  "caf\x82.txt"          /* 0x82 在 CP437 里是 e-acute */
#define NAME_CP437_UTF8 "caf\xC3\xA9.txt"  /* 期望读出来的 UTF-8 */
#define NAME_UTF8   "\xE4\xB8\xAD\xE6\x96\x87.txt"   /* "中文.txt" */

static void test_handmade(void)
{
    static unsigned char zipbuf[64u * 1024u];
    static unsigned char deflate_payload[4096];
    static const unsigned char cp437_name[] = { 'c', 'a', 'f', 0x82u, '.', 't', 'x', 't' };
    static const unsigned char utf8_name[] = { 0xE4u, 0xB8u, 0xADu, 0xE6u, 0x96u, 0x87u, '.', 't', 'x', 't' };
    static const unsigned char desc_name[] = { 'd', 'e', 's', 'c', '.', 't', 'x', 't' };
    static const unsigned char dir_name[] = { 'd', 'i', 'r', '/' };
    static const unsigned char bad_name[] = { 'b', 'z', '2', '.', 'b', 'i', 'n' };
    static const unsigned char enc_name[] = { 's', 'e', 'c', 'r', 'e', 't', '.', 't', 'x', 't' };
    static const unsigned char z64_name[] = { 'b', 'i', 'g', '6', '4', '.', 'b', 'i', 'n' };
    zspec es[8];
    size_t n = 0u;
    size_t total;
    size_t dlen;

    sxcl_zip *zip;
    unsigned char out[256];
    size_t got;
    int rc;

    printf("-- 手工拼的 zip --\n");

    dlen = make_fixed_deflate(PLAIN_DEFLATE, strlen((const char *)PLAIN_DEFLATE), deflate_payload);

    memset(es, 0, sizeof es);

    /* 0:stored */
    es[n].name = (const unsigned char *)"hello.txt";
    es[n].name_len = 9u;
    es[n].method = 0u;
    es[n].cdata = PLAIN_STORED;
    es[n].csize = strlen((const char *)PLAIN_STORED);
    es[n].usize = (unsigned long)es[n].csize;
    es[n].local_off_override = -1;
    ++n;

    /* 1:deflate(手写 fixed Huffman 流) */
    es[n].name = (const unsigned char *)"deflate.txt";
    es[n].name_len = 11u;
    es[n].method = 8u;
    es[n].cdata = deflate_payload;
    es[n].csize = dlen;
    es[n].usize = (unsigned long)strlen((const char *)PLAIN_DEFLATE);
    es[n].local_off_override = -1;
    ++n;

    /* 2:目录条目 */
    es[n].name = dir_name;
    es[n].name_len = sizeof dir_name;
    es[n].method = 0u;
    es[n].cdata = (const unsigned char *)"";
    es[n].csize = 0u;
    es[n].usize = 0u;
    es[n].local_off_override = -1;
    ++n;

    /* 3:CP437 名字(没置 UTF-8 位) */
    es[n].name = cp437_name;
    es[n].name_len = sizeof cp437_name;
    es[n].method = 0u;
    es[n].cdata = PLAIN_STORED;
    es[n].csize = strlen((const char *)PLAIN_STORED);
    es[n].usize = (unsigned long)es[n].csize;
    es[n].local_off_override = -1;
    ++n;

    /* 4:UTF-8 名字(置了 bit 11) */
    es[n].name = utf8_name;
    es[n].name_len = sizeof utf8_name;
    es[n].flags = 0x0800u;
    es[n].method = 0u;
    es[n].cdata = PLAIN_STORED;
    es[n].csize = strlen((const char *)PLAIN_STORED);
    es[n].usize = (unsigned long)es[n].csize;
    es[n].local_off_override = -1;
    ++n;

    /* 5:不支持的方法(12 = bzip2) */
    es[n].name = bad_name;
    es[n].name_len = sizeof bad_name;
    es[n].method = 12u;
    es[n].cdata = PLAIN_STORED;
    es[n].csize = strlen((const char *)PLAIN_STORED);
    es[n].usize = (unsigned long)es[n].csize;
    es[n].local_off_override = -1;
    ++n;

    /* 6:加密位 */
    es[n].name = enc_name;
    es[n].name_len = sizeof enc_name;
    es[n].flags = 0x0001u;
    es[n].method = 8u;
    es[n].cdata = deflate_payload;
    es[n].csize = dlen;
    es[n].usize = (unsigned long)strlen((const char *)PLAIN_DEFLATE);
    es[n].local_off_override = -1;
    ++n;

    /* 7:ZIP64 大小字段(0xFFFFFFFF) + 数据描述符风格 + 本地头大小写 0 */
    es[n].name = z64_name;
    es[n].name_len = sizeof z64_name;
    es[n].flags = 0x0008u;               /* bit 3:有数据描述符 */
    es[n].method = 8u;
    es[n].cdata = deflate_payload;
    es[n].csize = dlen;
    es[n].usize = (unsigned long)strlen((const char *)PLAIN_DEFLATE);
    es[n].local_zero = 1;
    es[n].descriptor = 1;
    es[n].cd_comp = 0xFFFFFFFFul;        /* 中央目录里的 ZIP64 标记 */
    es[n].cd_uncomp = 0xFFFFFFFFul;
    es[n].local_off_override = -1;
    ++n;
    total = build_zip(zipbuf, sizeof zipbuf, es, n);
    check(total > 0u, "手工 zip 生成成功");
    check(write_file(HAND_ZIP, zipbuf, total) == 0, "手工 zip 落盘");

    zip = sxcl_zip_open(HAND_ZIP);
    if (zip == NULL) {
        ++g_failures;
        fprintf(stderr, "FAIL 打不开手工 zip\n");
        return;
    }
    check(sxcl_zip_count(zip) == n, "条目数正确");

    check(strcmp(sxcl_zip_name_at(zip, 0u), "hello.txt") == 0, "名字 0 = hello.txt");
    check(strcmp(sxcl_zip_name_at(zip, 1u), "deflate.txt") == 0, "名字 1 = deflate.txt");
    check(strcmp(sxcl_zip_name_at(zip, 2u), "dir/") == 0, "名字 2 = dir/");
    check(strcmp(sxcl_zip_name_at(zip, 3u), NAME_CP437_UTF8) == 0, "CP437 名字转成了 UTF-8(café.txt)");
    check(strcmp(sxcl_zip_name_at(zip, 4u), NAME_UTF8) == 0, "UTF-8 标志位的名字原样读出(中文.txt)");
    check(sxcl_zip_name_at(zip, n) == NULL, "越界名字返回 NULL");

    check(sxcl_zip_method_at(zip, 0u) == 0, "方法 0 = stored");
    check(sxcl_zip_method_at(zip, 1u) == 8, "方法 1 = deflate");
    check(sxcl_zip_method_at(zip, 5u) == 12, "方法 5 = 12(不支持,但列得出来)");
    check(sxcl_zip_method_at(zip, n) == -1, "越界方法返回 -1");
    check(sxcl_zip_size_at(zip, 1u) == (int64_t)strlen((const char *)PLAIN_DEFLATE), "条目 1 的解压后大小");
    check(sxcl_zip_size_at(zip, n) == -1, "越界大小返回 -1");

    check(sxcl_zip_find(zip, "deflate.txt") == 1, "find 命中");
    check(sxcl_zip_find(zip, "deflate.TXT") == -1, "find 大小写敏感");
    check(sxcl_zip_find(zip, "nope.txt") == -1, "find 未命中返回 -1");
    check(sxcl_zip_find(zip, NAME_CP437_UTF8) == 3, "find 也能按转码后的名字找");

    /* stored 条目:解到内存 */
    memset(out, 0, sizeof out);
    got = 0u;
    rc = sxcl_zip_extract_memory(zip, "hello.txt", out, sizeof out, &got);
    check(rc == 0 && got == strlen((const char *)PLAIN_STORED) &&
          memcmp(out, PLAIN_STORED, got) == 0, "stored 条目解到内存内容正确");

    /* deflate 条目:解到内存 */
    memset(out, 0, sizeof out);
    got = 0u;
    rc = sxcl_zip_extract_memory(zip, "deflate.txt", out, sizeof out, &got);
    check(rc == 0 && got == strlen((const char *)PLAIN_DEFLATE) &&
          memcmp(out, PLAIN_DEFLATE, got) == 0, "deflate 条目解到内存内容正确");

    /* 缓冲不够 */
    got = 0u;
    rc = sxcl_zip_extract_memory(zip, "deflate.txt", out, 4u, &got);
    check(rc == -2 && got == strlen((const char *)PLAIN_DEFLATE), "缓冲不够 -> -2 且 out_len = 真实长度");

    /* 各种不支持 / 找不到 */
    check(sxcl_zip_extract_memory(zip, "nope.txt", out, sizeof out, &got) == -1, "找不到的条目 -> -1");
    check(sxcl_zip_extract_memory(zip, "dir/", out, sizeof out, &got) == -3, "目录条目 -> -3");
    check(sxcl_zip_extract_memory(zip, "bz2.bin", out, sizeof out, &got) == -3, "不支持的方法 -> -3");
    check(sxcl_zip_extract_memory(zip, "secret.txt", out, sizeof out, &got) == -3, "加密条目 -> -3");
    check(sxcl_zip_extract_memory(zip, "big64.bin", out, sizeof out, &got) == -3, "ZIP64 条目 -> -3");
    (void)desc_name;
    (void)NAME_CP437;
    sxcl_zip_close(zip);
}

/* 数据描述符 + 本地头大小字段为 0:必须按中央目录解出正确内容 */
static void test_descriptor_entry(void)
{
    static unsigned char zipbuf[8u * 1024u];
    static unsigned char payload[2048];
    static const unsigned char nm[] = { 'd', 'e', 's', 'c', '.', 't', 'x', 't' };
    static const unsigned char text[] = "data descriptor entry: the local header says 0\n";
    zspec e;
    size_t total;
    size_t dlen;
    sxcl_zip *zip;
    unsigned char out[128];
    size_t got = 0u;
    int rc;

    printf("-- 数据描述符条目 --\n");

    dlen = make_fixed_deflate(text, strlen((const char *)text), payload);
    memset(&e, 0, sizeof e);
    e.name = nm;
    e.name_len = sizeof nm;
    e.flags = 0x0008u;      /* bit 3:大小在数据描述符里,本地头写 0 */
    e.method = 8u;
    e.cdata = payload;
    e.csize = dlen;
    e.usize = (unsigned long)strlen((const char *)text);
    e.local_zero = 1;
    e.descriptor = 1;
    e.local_off_override = -1;

    total = build_zip(zipbuf, sizeof zipbuf, &e, 1u);
    check(total > 0u && write_file(HAND_ZIP, zipbuf, total) == 0, "数据描述符 zip 落盘");

    zip = sxcl_zip_open(HAND_ZIP);
    if (zip == NULL) {
        ++g_failures;
        fprintf(stderr, "FAIL 打不开数据描述符 zip\n");
        return;
    }
    check(sxcl_zip_count(zip) == 1u, "数据描述符 zip 条目数");
    rc = sxcl_zip_extract_memory(zip, "desc.txt", out, sizeof out, &got);
    check(rc == 0 && got == strlen((const char *)text) && memcmp(out, text, got) == 0,
          "本地头大小字段为 0 时按中央目录解出正确内容");
    sxcl_zip_close(zip);
}

/* 损坏数据必须报错而不是崩 */
static void test_broken(void)
{
    static unsigned char zipbuf[8u * 1024u];
    static const unsigned char nm[] = { 'a', '.', 't', 'x', 't' };
    static const unsigned char text[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
    zspec e;
    size_t total;
    sxcl_zip *zip;
    unsigned char out[128];
    size_t got = 0u;
    size_t i;

    printf("-- 损坏数据 --\n");

    /* 不是 zip */
    {
        unsigned char junk[128];
        for (i = 0u; i < sizeof junk; ++i) {
            junk[i] = (unsigned char)(i * 7u);
        }
        check(write_file(HAND_ZIP, junk, sizeof junk) == 0, "写垃圾文件");
        check(sxcl_zip_open(HAND_ZIP) == NULL, "乱码文件打不开(返回 NULL)");
    }
    /* 空文件 */
    check(write_file(HAND_ZIP, "", 0u) == 0, "写空文件");
    check(sxcl_zip_open(HAND_ZIP) == NULL, "空文件打不开(返回 NULL)");
    check(sxcl_zip_open(TMP_DIR "/no_such_file.zip") == NULL, "不存在的文件打不开");
    check(sxcl_zip_open(NULL) == NULL, "path = NULL 打不开");

    /* 正常 zip,但尾部被截断 */
    memset(&e, 0, sizeof e);
    e.name = nm;
    e.name_len = sizeof nm;
    e.method = 0u;
    e.cdata = text;
    e.csize = strlen((const char *)text);
    e.usize = (unsigned long)strlen((const char *)text);
    e.local_off_override = -1;
    total = build_zip(zipbuf, sizeof zipbuf, &e, 1u);
    check(total > 22u && write_file(HAND_ZIP, zipbuf, total - 10u) == 0, "写尾部被截断的 zip");
    check(sxcl_zip_open(HAND_ZIP) == NULL, "尾部截断 -> 打不开");

    /* 本地头签名被破坏 */
    {
        zspec e2 = e;
        e2.break_local = 1;
        total = build_zip(zipbuf, sizeof zipbuf, &e2, 1u);
        check(total > 0u && write_file(HAND_ZIP, zipbuf, total) == 0, "写本地头签名损坏的 zip");
        zip = sxcl_zip_open(HAND_ZIP);
        check(zip != NULL, "本地头坏了仍然能打开(中央目录是好的)");
        if (zip != NULL) {
            check(sxcl_zip_extract_memory(zip, "a.txt", out, sizeof out, &got) == -1,
                  "本地头签名不对 -> 解压报错");
            check(sxcl_zip_count(zip) == 1u, "本地头坏了条目数照样是 1");
            sxcl_zip_close(zip);
        }
    }

    /* 中央目录声明的压缩长度超出文件 */
    {
        zspec e2 = e;
        e2.cd_comp = 0x7FFFFFF0ul;
        total = build_zip(zipbuf, sizeof zipbuf, &e2, 1u);
        check(total > 0u && write_file(HAND_ZIP, zipbuf, total) == 0, "写压缩长度超界的 zip");
        zip = sxcl_zip_open(HAND_ZIP);
        check(zip != NULL, "长度超界的 zip 仍能打开");
        if (zip != NULL) {
            check(sxcl_zip_extract_memory(zip, "a.txt", out, sizeof out, &got) == -1,
                  "压缩长度超出文件 -> 解压报错");
            sxcl_zip_close(zip);
        }
    }

    /* 本地头偏移指到文件外 */
    {
        zspec e2 = e;
        e2.local_off_override = 100000L;
        total = build_zip(zipbuf, sizeof zipbuf, &e2, 1u);
        check(total > 0u && write_file(HAND_ZIP, zipbuf, total) == 0, "写本地头偏移越界的 zip");
        zip = sxcl_zip_open(HAND_ZIP);
        check(zip != NULL, "本地头偏移越界的 zip 仍能打开");
        if (zip != NULL) {
            check(sxcl_zip_extract_memory(zip, "a.txt", out, sizeof out, &got) == -1,
                  "本地头偏移越界 -> 解压报错");
            sxcl_zip_close(zip);
        }
    }

    /* 只留一个 EOCD 的空 zip:合法,count = 0 */
    {
        static unsigned char empty_zip[22];
        size_t k = 0u;
        memset(empty_zip, 0, sizeof empty_zip);
        empty_zip[k++] = 'P'; empty_zip[k++] = 'K'; empty_zip[k++] = 5u; empty_zip[k++] = 6u;
        /* 其余字段全 0:0 个条目、中央目录大小 0、偏移 0 */
        check(write_file(HAND_ZIP, empty_zip, sizeof empty_zip) == 0, "写空 zip");
        zip = sxcl_zip_open(HAND_ZIP);
        check(zip != NULL && sxcl_zip_count(zip) == 0u, "空 zip 能打开且条目数为 0");
        sxcl_zip_close(zip);
    }

    /* 句柄边界 */
    check(sxcl_zip_count(NULL) == 0u, "count(NULL) = 0");
    check(sxcl_zip_name_at(NULL, 0u) == NULL, "name_at(NULL) = NULL");
    check(sxcl_zip_size_at(NULL, 0u) == -1, "size_at(NULL) = -1");
    check(sxcl_zip_method_at(NULL, 0u) == -1, "method_at(NULL) = -1");
    check(sxcl_zip_find(NULL, "x") == -1, "find(NULL) = -1");
    check(sxcl_zip_extract_memory(NULL, "x", out, sizeof out, &got) == -1, "extract_memory(NULL) = -1");
    check(sxcl_zip_extract_file(NULL, "x", TMP_DIR "/out/x") == -1, "extract_file(NULL) = -1");
    check(sxcl_zip_extract_stream(NULL, "x", NULL, NULL) == -1, "extract_stream(NULL) = -1");
    sxcl_zip_close(NULL);
}

/* ================================================================== */
/* 真实 zip 夹具(Compress-Archive)                                    */
/* ================================================================== */

#define BIG_LEN 200000u
static unsigned char g_big[BIG_LEN];

static void gen_big_text(unsigned char *out, size_t len)
{
    static const char *words[8] = {
        "alpha ", "beta ", "gamma ", "delta ", "epsilon ", "zeta ", "eta ", "theta "
    };
    unsigned k = 20250101u;
    size_t i = 0u;

    while (i < len) {
        size_t wl;
        const char *w;
        k = k * 1103515245u + 12345u;
        w = words[(k >> 16) % 8u];
        wl = strlen(w);
        if (i + wl > len) {
            wl = len - i;
        }
        memcpy(out + i, w, wl);
        i += wl;
        if (((k >> 8) & 0x1Fu) == 0u && i < len) {
            out[i] = (unsigned char)'\n';
            ++i;
        }
    }
}

static void gen_bin(unsigned char *out, size_t len)
{
    size_t i;
    for (i = 0u; i < len; ++i) {
        out[i] = (unsigned char)((i * 37u + (i >> 3)) & 0xFFu);
    }
}

typedef struct stream_cmp {
    const unsigned char *want;
    size_t               want_len;
    size_t               off;
    size_t               calls;
    int                  mismatch;
} stream_cmp;

static int stream_cmp_write(void *ud, const void *data, size_t len)
{
    stream_cmp *s = (stream_cmp *)ud;

    ++s->calls;
    if (s->off + len > s->want_len ||
        memcmp(s->want + s->off, data, len) != 0) {
        s->mismatch = 1;
        return 1;   /* 立刻中止 */
    }
    s->off += len;
    return 0;
}

static void check_extract_memory_eq(sxcl_zip *zip, const char *name,
                                    const unsigned char *want, size_t want_len, const char *what)
{
    static unsigned char buf[1u << 20];
    size_t got = 0u;
    const int rc = sxcl_zip_extract_memory(zip, name, buf, sizeof buf, &got);

    if (rc == 0 && got == want_len && (want_len == 0u || memcmp(buf, want, want_len) == 0)) {
        check(1, what);
    } else {
        ++g_checks;
        ++g_failures;
        fprintf(stderr, "FAIL %s: rc=%d got=%u want=%u\n", what, rc, (unsigned)got, (unsigned)want_len);
    }
}

static void test_real_fixture(void)
{
    static const char hello[] = "Hello from SXCL-C zip test!\nsecond line\n";
    static unsigned char bin_data[1200];
    static const char ps1[] =
        "$ErrorActionPreference = 'Stop'\n"
        "$root = $PSScriptRoot\n"
        "$src = Join-Path $root 'src'\n"
        "$dst = Join-Path $root 'fixture.zip'\n"
        "if (Test-Path $dst) { Remove-Item $dst -Force }\n"
        "Compress-Archive -Path (Join-Path $src '*') -DestinationPath $dst -Force\n"
        "if (-not (Test-Path $dst)) { exit 3 }\n"
        "exit 0\n";

    sxcl_zip *zip;
    int idx;
    int rc_cmd;
    int method;
    int64_t size;
    size_t i;

    printf("-- 真实 zip 夹具(Compress-Archive) --\n");

    check(sxcl_fs_mkdirs(TMP_DIR) == 0, "建临时目录");
    check(sxcl_fs_mkdirs(FIX_SRC) == 0, "建夹具源目录");
    check(sxcl_fs_mkdirs(FIX_SRC "/nested/deep") == 0, "建夹具嵌套目录");
    check(sxcl_fs_mkdirs(FIX_OUT) == 0, "建输出目录");

    gen_bin(bin_data, sizeof bin_data);
    gen_big_text(g_big, BIG_LEN);

    check(write_file(FIX_SRC "/hello.txt", hello, strlen(hello)) == 0, "写 hello.txt");
    check(write_file(FIX_SRC "/nested/deep/data.bin", bin_data, sizeof bin_data) == 0, "写 data.bin");
    check(write_file(FIX_SRC "/big.txt", g_big, BIG_LEN) == 0, "写 big.txt(200 KB)");
    check(write_file(FIX_PS1, ps1, strlen(ps1)) == 0, "写夹具生成脚本");

    (void)sxcl_fs_remove(FIX_ZIP);
    rc_cmd = system("powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " FIX_PS1);
    if (rc_cmd != 0) {
        rc_cmd = system("pwsh -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " FIX_PS1);
    }
    if (rc_cmd != 0 || !sxcl_fs_exists(FIX_ZIP)) {
#if defined(_WIN32)
        check(0, "Compress-Archive 生成夹具 zip(Windows 上必须有 PowerShell,失败即测试失败)");
#else
        printf("skip 本机没有可用的 PowerShell/Compress-Archive,跳过真实 zip 夹具\n");
#endif
        return;
    }
    check(1, "Compress-Archive 生成夹具 zip");

    zip = sxcl_zip_open(FIX_ZIP);
    if (zip == NULL) {
        ++g_checks;
        ++g_failures;
        fprintf(stderr, "FAIL 打不开 Compress-Archive 生成的 zip\n");
        return;
    }

    printf("     条目共 %u 个:\n", (unsigned)sxcl_zip_count(zip));
    for (i = 0u; i < sxcl_zip_count(zip); ++i) {
        printf("       [%u] %-28s method=%d size=%lld\n", (unsigned)i,
               sxcl_zip_name_at(zip, i), sxcl_zip_method_at(zip, i),
               (long long)sxcl_zip_size_at(zip, i));
    }

    idx = sxcl_zip_find(zip, "hello.txt");
    check(idx >= 0, "夹具里有 hello.txt");
    idx = sxcl_zip_find(zip, "nested/deep/data.bin");
    check(idx >= 0, "夹具里有 nested/deep/data.bin(正斜杠路径)");
    idx = sxcl_zip_find(zip, "big.txt");
    check(idx >= 0, "夹具里有 big.txt");

    size = sxcl_zip_size_at(zip, (size_t)sxcl_zip_find(zip, "hello.txt"));
    check(size == (int64_t)strlen(hello), "hello.txt 的解压后大小与源文件一致");
    size = sxcl_zip_size_at(zip, (size_t)sxcl_zip_find(zip, "big.txt"));
    check(size == (int64_t)BIG_LEN, "big.txt 的解压后大小 = 200000");

    /* 内容逐字节比对 */
    check_extract_memory_eq(zip, "hello.txt", (const unsigned char *)hello, strlen(hello),
                            "hello.txt 解出来逐字节相等");
    check_extract_memory_eq(zip, "nested/deep/data.bin", bin_data, sizeof bin_data,
                            "nested/deep/data.bin 解出来逐字节相等");
    check_extract_memory_eq(zip, "big.txt", g_big, BIG_LEN, "big.txt(200 KB)解出来逐字节相等");

    {
        int any_deflate = 0;
        for (i = 0u; i < sxcl_zip_count(zip); ++i) {
            method = sxcl_zip_method_at(zip, i);
            check(method == 0 || method == 8, "夹具条目的方法只可能是 stored/deflate");
            if (method == 8 && sxcl_zip_size_at(zip, i) > 1000) {
                any_deflate = 1;
            }
        }
        check(any_deflate != 0, "至少有一个大条目真的是 deflate(真实动态 Huffman 数据)");
    }

    /* 逐块解出(大条目路径) */
    {
        stream_cmp sc;
        sc.want = g_big;
        sc.want_len = BIG_LEN;
        sc.off = 0u;
        sc.calls = 0u;
        sc.mismatch = 0;
        check(sxcl_zip_extract_stream(zip, "big.txt", stream_cmp_write, &sc) == 0 &&
              sc.off == BIG_LEN && sc.mismatch == 0, "big.txt 逐块解出内容一致");
        printf("     big.txt 分成 %u 块交给 sink\n", (unsigned)sc.calls);
        check(sc.calls > 1u, "大条目确实分了多块(sink 被多次调用)");

        sc.off = BIG_LEN - 10u;   /* 故意错位,验证 sink 中止路径 */
        sc.calls = 0u;
        sc.mismatch = 0;
        check(sxcl_zip_extract_stream(zip, "big.txt", stream_cmp_write, &sc) == -4,
              "sink 中止 -> -4");
    }

    /* 提取到文件:自动建父目录 + 原子落位 + 不留 .tmp */
    {
        static unsigned char back[1u << 20];
        FILE *f;
        size_t got = 0u;

        check(sxcl_zip_extract_file(zip, "nested/deep/data.bin", FIX_OUT "/deep/out.bin") == 0,
              "extract_file 到多层新目录成功");
        check(sxcl_fs_exists(FIX_OUT "/deep/out.bin") == 1, "落位后的文件存在");
        check(sxcl_fs_exists(FIX_OUT "/deep/out.bin.tmp") == 0, "不残留 .tmp");
        f = fopen(FIX_OUT "/deep/out.bin", "rb");
        if (f == NULL) {
            ++g_checks;
            ++g_failures;
            fprintf(stderr, "FAIL 读回落位的文件\n");
        } else {
            got = fread(back, 1u, sizeof back, f);
            fclose(f);
            check(got == sizeof bin_data && memcmp(back, bin_data, sizeof bin_data) == 0,
                  "落位文件内容逐字节相等");
        }

        check(sxcl_zip_extract_file(zip, "big.txt", FIX_OUT "/big_copy.txt") == 0,
              "extract_file 大条目成功");
        check(sxcl_fs_exists(FIX_OUT "/big_copy.txt.tmp") == 0, "大条目也不残留 .tmp");
        check(sxcl_fs_stat(FIX_OUT "/big_copy.txt", &size, NULL) == 0 && size == (int64_t)BIG_LEN,
              "落位的大文件大小 = 200000");

        check(sxcl_zip_extract_file(zip, "nope.txt", FIX_OUT "/nope.txt") == -1,
              "提取不存在的条目 -> -1");
        check(sxcl_fs_exists(FIX_OUT "/nope.txt.tmp") == 0, "失败也不留 .tmp");
    }

    sxcl_zip_close(zip);
    /* 仓库里不留 zip 二进制:夹具用完就删(源目录留在 build/ 里便于排查) */
    (void)sxcl_fs_remove(FIX_ZIP);
    (void)sxcl_fs_remove(HAND_ZIP);
    check(sxcl_fs_exists(FIX_ZIP) == 0 && sxcl_fs_exists(HAND_ZIP) == 0,
          "测试结束删掉所有临时 zip,仓库里不留二进制");
}

/* ================================================================== */

int main(void)
{
    printf("== sxcl zip test ==\n");

    /* 所有手工夹具都写在 build/_zip_test/ 下 */
    if (sxcl_fs_mkdirs(TMP_DIR) != 0) {
        fprintf(stderr, "FAIL 建不了临时目录 %s\n", TMP_DIR);
        return 1;
    }

    test_handmade();
    test_descriptor_entry();
    test_broken();
    test_real_fixture();

    printf("== %d 项断言, %d 项失败 ==\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
