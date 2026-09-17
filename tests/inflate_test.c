/* SXCL-C DEFLATE 解压测试:任何失败都会让 main 返回非 0,ctest 判定失败。
 *
 * 覆盖内容:
 *   1) 手写 raw deflate 样本(测试里自带一个独立的位写入器 + 定长/动态块编码器,
 *      字节序列完全由本文件推导,不依赖任何外部压缩工具或 zip 文件):
 *        - stored 块(含空块、多块连续、LEN/NLEN 校验);
 *        - fixed Huffman 单符号(只输出一个 'A' 再 EOB);
 *        - fixed Huffman 带重叠匹配(dist < len);
 *        - dynamic Huffman(码长全 1 的两符号树、单符号距离树、15 位码、16/17/18 重复);
 *        - 跨 32 KiB 窗口的距离引用(dist = 32768,含环形回绕);
 *   2) 流式接口:1 字节一段地喂,结果必须与一次性解压逐字节相同;
 *   3) 损坏数据:截断、非法 BTYPE、非法 HLIT/HDIST、LEN/NLEN 不互补、距离超界、
 *      非法 Huffman 码 —— 必须返回错误而不是崩溃/越界;
 *   4) 接口边界:输出缓冲不够(-2 且 out_len 写出真实长度)、sink 主动中止(-3);
 *   5) 大输出(100 KB 字面量)压过"窗口未交付水位"的交付阈值与环形回绕。
 *
 * 手写样本的字节序列另用 Python zlib(独立实现)离线交叉验证过,见报告。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/inflate.h"

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

/* ================================================================== */
/* 测试自带的 DEFLATE 编码器(位写入器,与被测解码器完全独立)            */
/* ================================================================== */

#define BW_CAP (1u << 20)   /* 1 MiB:测试里最大的流是 100 KB 字面量,足够了 */

typedef struct bitw {
    unsigned char buf[BW_CAP];
    size_t len;    /* 已经写满的字节数 */
    unsigned bit;  /* 当前字节里已写了几位(0..7) */
    int over;      /* 越界标记 */
} bitw;

static void bw_init(bitw *w)
{
    memset(w, 0, sizeof *w);
}

/* 写 n 位,低比特在前(块头、HLIT/HDIST/HCLEN、附加位这些"普通字段")。 */
static void bw_put(bitw *w, unsigned value, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; ++i) {
        if (((value >> i) & 1u) != 0u) {
            if (w->len >= (size_t)BW_CAP) {
                w->over = 1;
                return;
            }
            w->buf[w->len] |= (unsigned char)(1u << w->bit);
        }
        w->bit += 1u;
        if (w->bit == 8u) {
            w->bit = 0u;
            w->len += 1u;
            if (w->len < (size_t)BW_CAP) {
                w->buf[w->len] = 0u;
            }
        }
    }
}

/* 写 Huffman 码,高比特在前(RFC 1951:码从最高位开始进流)。 */
static void bw_code(bitw *w, unsigned code, unsigned n)
{
    unsigned i;
    for (i = n; i > 0u; --i) {
        bw_put(w, (code >> (i - 1u)) & 1u, 1u);
    }
}

/* 当前流的总字节数(末尾半个字节按 1 字节算)。 */
static size_t bw_size(const bitw *w)
{
    return w->len + ((w->bit != 0u) ? 1u : 0u);
}

/* 字节对齐(剩余位补 0)。 */
static void bw_align(bitw *w)
{
    if (w->bit != 0u) {
        w->bit = 0u;
        w->len += 1u;
        if (w->len < (size_t)BW_CAP) {
            w->buf[w->len] = 0u;
        }
    }
}

static void bw_mark(bitw *w, int bfinal, unsigned btype)
{
    bw_put(w, (unsigned)bfinal, 1u);
    bw_put(w, btype, 2u);
}

/* stored 块(BFINAL 由调用方给)。 */
static void bw_stored(bitw *w, int bfinal, const unsigned char *data, unsigned len)
{
    unsigned i;
    bw_mark(w, bfinal, 0u);
    bw_align(w);
    bw_put(w, len & 0xFFu, 8u);
    bw_put(w, (len >> 8) & 0xFFu, 8u);
    bw_put(w, (~len) & 0xFFu, 8u);
    bw_put(w, ((~len) >> 8) & 0xFFu, 8u);
    for (i = 0u; i < len; ++i) {
        bw_put(w, data[i], 8u);
    }
}

/* fixed Huffman 的字面量/长度码(RFC 1951 3.2.6)。 */
static void bw_fixed_sym(bitw *w, unsigned sym)
{
    if (sym < 144u) {
        bw_code(w, 0x30u + sym, 8u);
    } else if (sym < 256u) {
        bw_code(w, 0x190u + (sym - 144u), 9u);
    } else if (sym < 280u) {
        bw_code(w, sym - 256u, 7u);
    } else {
        bw_code(w, 0xC0u + (sym - 280u), 8u);
    }
}

static const unsigned short tst_len_base[29] = {
    3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 15u, 17u, 19u, 23u, 27u,
    31u, 35u, 43u, 51u, 59u, 67u, 83u, 99u, 115u, 131u, 163u, 195u, 227u, 258u
};
static const unsigned char tst_len_extra[29] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u, 2u,
    2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u
};
static const unsigned short tst_dist_base[30] = {
    1u, 2u, 3u, 4u, 5u, 7u, 9u, 13u, 17u, 25u, 33u, 49u, 65u, 97u, 129u,
    193u, 257u, 385u, 513u, 769u, 1025u, 1537u, 2049u, 3073u, 4097u,
    6145u, 8193u, 12289u, 16385u, 24577u
};
static const unsigned char tst_dist_extra[30] = {
    0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u,
    6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u
};

/* fixed Huffman 的一个匹配(长度码 + 距离码 + 附加位)。 */
static int bw_fixed_match(bitw *w, unsigned length, unsigned dist)
{
    unsigned i;
    unsigned li = 29u;
    unsigned di = 30u;

    for (i = 0u; i < 29u; ++i) {
        const unsigned hi = (i + 1u < 29u) ? (unsigned)tst_len_base[i + 1u] : 259u;
        if (length >= (unsigned)tst_len_base[i] && length < hi) {
            li = i;
            break;
        }
    }
    for (i = 0u; i < 30u; ++i) {
        const unsigned hi = (i + 1u < 30u) ? (unsigned)tst_dist_base[i + 1u] : 32769u;
        if (dist >= (unsigned)tst_dist_base[i] && dist < hi) {
            di = i;
            break;
        }
    }
    if (li == 29u || di == 30u) {
        return -1;   /* 长度/距离超出 DEFLATE 能表达的范围 */
    }
    bw_fixed_sym(w, 257u + li);
    bw_put(w, length - (unsigned)tst_len_base[li], tst_len_extra[li]);
    bw_code(w, di, 5u);
    bw_put(w, dist - (unsigned)tst_dist_base[di], tst_dist_extra[di]);
    return 0;
}

/* 规范 Huffman 码分配(独立实现,和被测解码器的算法无关)。 */
static void tst_canon(const unsigned char *lens, unsigned n, unsigned *codes)
{
    unsigned count[16];
    unsigned next[16];
    unsigned code = 0u;
    unsigned i;
    unsigned len;

    memset(count, 0, sizeof count);
    for (i = 0u; i < n; ++i) {
        count[lens[i]] += 1u;
    }
    count[0] = 0u;
    for (len = 1u; len <= 15u; ++len) {
        code = (code + count[len - 1u]) << 1;
        next[len] = code;
    }
    for (i = 0u; i < n; ++i) {
        codes[i] = (lens[i] != 0u) ? next[lens[i]]++ : 0u;
    }
}

/* 动态块头:码长码用"值 0..15 各 4 位"的最朴素写法(完整树,正好 16 个码)。 */
static void bw_dyn_header(bitw *w, int bfinal,
                          const unsigned char *lit_lens, unsigned nlit,
                          const unsigned char *dist_lens, unsigned ndist)
{
    static const unsigned char order[19] = {
        16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u
    };
    unsigned i;

    bw_mark(w, bfinal, 2u);
    bw_put(w, nlit - 257u, 5u);      /* HLIT  */
    bw_put(w, ndist - 1u, 5u);       /* HDIST */
    bw_put(w, 15u, 4u);              /* HCLEN = 19 个码长码 */
    for (i = 0u; i < 19u; ++i) {
        /* 只有 0..15 这 16 个值有码(码长 4),16/17/18 不用 -> 长度 0 */
        bw_put(w, (order[i] < 16u) ? 4u : 0u, 3u);
    }
    for (i = 0u; i < nlit; ++i) {
        bw_code(w, (unsigned)lit_lens[i], 4u);      /* 码长 4 的规范码就是值本身 */
    }
    for (i = 0u; i < ndist; ++i) {
        bw_code(w, (unsigned)dist_lens[i], 4u);
    }
}

/* 动态块头(自定义码长码表版本):cl_sym_len 按"码长码符号 0..18"给码长,
 * HCLEN 固定写 18(覆盖 order 的前 18 位,够到符号 1 所在的第 17 位)。 */
static void bw_dyn_header_cl(bitw *w, int bfinal,
                             const unsigned char *cl_sym_len,
                             unsigned nlit, unsigned ndist)
{
    static const unsigned char order[19] = {
        16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u
    };
    unsigned i;

    bw_mark(w, bfinal, 2u);
    bw_put(w, nlit - 257u, 5u);
    bw_put(w, ndist - 1u, 5u);
    bw_put(w, 18u - 4u, 4u);
    for (i = 0u; i < 18u; ++i) {
        bw_put(w, (unsigned)cl_sym_len[order[i]], 3u);
    }
}

/* ================================================================== */
/* 解压辅助                                                            */
/* ================================================================== */

#define OUT_CAP (1u << 18)

typedef struct outsink {
    unsigned char *buf;
    size_t cap;
    size_t len;
    int abort_at;       /* >= 0 时:累计到这么多字节就让 sink 返回非 0 */
} outsink;

static int outsink_write(void *ud, const void *data, size_t len)
{
    outsink *s = (outsink *)ud;
    if (s->abort_at >= 0 && s->len >= (size_t)s->abort_at) {
        return 1;
    }
    if (s->buf != NULL && s->len < s->cap) {
        const size_t room = s->cap - s->len;
        memcpy(s->buf + s->len, data, (len < room) ? len : room);
    }
    s->len += len;
    return 0;
}

/* 按 chunk 字节分段喂,返回 0/1 同 feed,-1 出错。 */
static int inflate_chunked(const unsigned char *in, size_t in_len, size_t chunk,
                           unsigned char *out, size_t out_cap, size_t *out_len)
{
    sxcl_inflate *inf = sxcl_inflate_open();
    outsink s;
    size_t off = 0u;
    int rc = 0;

    if (inf == NULL) {
        return -1;
    }
    s.buf = out;
    s.cap = out_cap;
    s.len = 0u;
    s.abort_at = -1;

    while (off < in_len) {
        size_t n = in_len - off;
        if (n > chunk) {
            n = chunk;
        }
        rc = sxcl_inflate_feed(inf, in + off, n, outsink_write, &s);
        if (rc < 0) {
            break;
        }
        off += n;
        if (rc == 1) {
            break;      /* 流已经结束 */
        }
    }
    if (rc >= 0 && sxcl_inflate_finish(inf) != 0) {
        rc = -1;
    }
    if (out_len != NULL) {
        *out_len = s.len;
    }
    sxcl_inflate_close(inf);
    return (rc < 0) ? -1 : rc;
}

/* 一次性解压并与期望逐字节比较。 */
static void expect_bytes(const unsigned char *in, size_t in_len,
                         const unsigned char *want, size_t want_len, const char *what)
{
    static unsigned char out[OUT_CAP];
    size_t got = 0u;
    const int rc = sxcl_inflate_raw(in, in_len, out, sizeof out, &got);

    ++g_checks;
    if (rc == 0 && got == want_len && (want_len == 0u || memcmp(out, want, want_len) == 0)) {
        printf("ok   %s (%u 字节)\n", what, (unsigned)want_len);
        return;
    }
    ++g_failures;
    fprintf(stderr, "FAIL %s: rc=%d got=%u want=%u\n", what, rc, (unsigned)got, (unsigned)want_len);
    if (rc == 0 && got == want_len && want_len > 0u) {
        size_t i;
        for (i = 0u; i < want_len; ++i) {
            if (out[i] != want[i]) {
                fprintf(stderr, "     首个不同在 %u: got=0x%02X want=0x%02X\n",
                        (unsigned)i, out[i], want[i]);
                break;
            }
        }
    }
}

/* 期望解压失败(损坏 / 截断)。 */
static void expect_bad(const unsigned char *in, size_t in_len, const char *what)
{
    static unsigned char out[4096];
    size_t got = 0u;
    const int rc = sxcl_inflate_raw(in, in_len, out, sizeof out, &got);

    ++g_checks;
    if (rc != 0) {
        printf("ok   %s(按要求报错 rc=%d)\n", what, rc);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 竟然解成功,输出 %u 字节\n", what, (unsigned)got);
    }
}

/* ================================================================== */
/* 1) stored 块                                                        */
/* ================================================================== */

static void test_stored(void)
{
    static const char *text = "stored block: Hello, DEFLATE! 0123456789";
    static bitw w;
    unsigned char data[64];
    unsigned i;

    printf("-- stored 块 --\n");

    for (i = 0u; i < sizeof data; ++i) {
        data[i] = (unsigned char)(i * 7u + 1u);
    }

    /* 单块,一次性 */
    bw_init(&w);
    bw_stored(&w, 1, (const unsigned char *)text, (unsigned)strlen(text));
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)text, strlen(text), "stored 单块(BFINAL=1)");

    /* 空 stored 块 + 有数据的 stored 块,两块连续 */
    bw_init(&w);
    bw_stored(&w, 0, data, 0u);
    bw_stored(&w, 1, data, 32u);
    expect_bytes(w.buf, bw_size(&w), data, 32u, "空 stored 块 + 32 字节块");

    /* 三块连续,只有最后一块 BFINAL=1 */
    bw_init(&w);
    bw_stored(&w, 0, data, 5u);
    bw_stored(&w, 0, data + 5, 0u);
    bw_stored(&w, 1, data + 5, 59u);
    {
        unsigned char want[64];
        memcpy(want, data, 64u);
        expect_bytes(w.buf, bw_size(&w), want, 64u, "stored 三块连续(含空块)");
    }

    /* 65535 字节的大 stored 块(压过窗口与交付阈值) */
    {
        static unsigned char big[65535];
        static bitw w2;
        for (i = 0u; i < 65535u; ++i) {
            big[i] = (unsigned char)((i * 31u + 7u) & 0xFFu);
        }
        bw_init(&w2);
        bw_stored(&w2, 1, big, 65535u);
        expect_bytes(w2.buf, bw_size(&w2), big, 65535u, "stored 65535 字节(窗口回绕 x2)");
    }

    /* LEN/NLEN 不互补 */
    bw_init(&w);
    bw_mark(&w, 1, 0u);
    bw_align(&w);
    bw_put(&w, 5u, 8u);
    bw_put(&w, 0u, 8u);       /* 正确应为 0xFA */
    bw_put(&w, 0xFAu, 8u);
    bw_put(&w, 0xFFu, 8u);
    bw_put(&w, 'x', 8u);
    expect_bad(w.buf, bw_size(&w), "stored 块 LEN/NLEN 不互补");

    /* 截断:声明 10 字节只给 3 字节 */
    bw_init(&w);
    bw_stored(&w, 1, data, 10u);
    expect_bad(w.buf, bw_size(&w) - 7u, "stored 块数据被截断");
}

/* ================================================================== */
/* 2) fixed Huffman                                                    */
/* ================================================================== */

static void test_fixed(void)
{
    static bitw w;

    printf("-- fixed Huffman --\n");

    /* 单符号:一个 'A' + EOB。手算:块头 3 位(1,1,0)+ 'A' 的固定码 0x71(8 位,
     * MSB 在前 = 0,1,1,1,0,0,0,1)+ EOB 的 7 位 0,共 18 位 = 3 字节 73 04 00。 */
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    bw_fixed_sym(&w, 'A');
    bw_fixed_sym(&w, 256u);
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"A", 1u, "fixed 单符号 'A'");
    check(bw_size(&w) == 3u && w.buf[0] == 0x73u && w.buf[1] == 0x04u && w.buf[2] == 0x00u,
          "fixed 单符号 'A' 的字节就是手算的 73 04 00");

    /* 空输出:只有 EOB */
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    bw_fixed_sym(&w, 256u);
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"", 0u, "fixed 只有 EOB(0 字节输出)");

    /* 一小段字面量 + 重叠匹配(dist=1 < len=8) */
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    bw_fixed_sym(&w, 'x');
    if (bw_fixed_match(&w, 8u, 1u) != 0) {
        ++g_failures;
        fprintf(stderr, "FAIL 编码 dist=1 len=8 匹配\n");
    }
    bw_fixed_sym(&w, 256u);
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"xxxxxxxxx", 9u, "fixed 重叠匹配 dist=1 len=8");

    /* 匹配 + 跨块(前一块 provided 字典,后一块用距离引用前面块的数据) */
    {
        unsigned char dict[64];
        unsigned i;
        for (i = 0u; i < 64u; ++i) {
            dict[i] = (unsigned char)('a' + (i % 26u));
        }
        bw_init(&w);
        bw_stored(&w, 0, dict, 64u);
        bw_mark(&w, 1, 1u);
        if (bw_fixed_match(&w, 20u, 64u) != 0) {
            ++g_failures;
            fprintf(stderr, "FAIL 编码 dist=64 len=20 匹配\n");
        }
        bw_fixed_sym(&w, 256u);
        {
            unsigned char want[84];
            memcpy(want, dict, 64u);
            memcpy(want + 64u, dict, 20u);
            expect_bytes(w.buf, bw_size(&w), want, 84u, "跨块距离引用(stored 块 + fixed 块)");
        }
    }

    /* 非法长度码 286(fixed 表里有这个码,但没有定义) */
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    bw_fixed_sym(&w, 286u);
    bw_fixed_sym(&w, 256u);
    expect_bad(w.buf, bw_size(&w), "fixed 长度码 286 无定义");

    /* 距离超出已输出长度:第一个符号就是 dist=1 的匹配 */
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    if (bw_fixed_match(&w, 3u, 1u) != 0) {
        ++g_failures;
        fprintf(stderr, "FAIL 编码 dist=1 len=3 匹配\n");
    }
    bw_fixed_sym(&w, 256u);
    expect_bad(w.buf, bw_size(&w), "fixed 距离超出已输出长度(空历史 dist=1)");

    /* BTYPE = 11 保留值 */
    {
        static const unsigned char bad[2] = { 0x07u, 0x00u };  /* BFINAL=1, BTYPE=11 */
        expect_bad(bad, sizeof bad, "BTYPE=11 保留值");
    }
}

/* ================================================================== */
/* 3) dynamic Huffman                                                  */
/* ================================================================== */

static void test_dynamic(void)
{
    unsigned char lit_lens[288];
    unsigned char dist_lens[32];
    unsigned lit_codes[288];
    unsigned dist_codes[32];
    static bitw w;

    printf("-- dynamic Huffman --\n");

    /* A) 码长全 1 的两符号树(A 与 EOB 各 1 位),距离树只有 1 个码长 0 的符号 */
    memset(lit_lens, 0, sizeof lit_lens);
    lit_lens[65] = 1u;
    lit_lens[256] = 1u;
    memset(dist_lens, 0, sizeof dist_lens);
    bw_init(&w);
    bw_dyn_header(&w, 1, lit_lens, 257u, dist_lens, 1u);
    tst_canon(lit_lens, 257u, lit_codes);
    tst_canon(dist_lens, 1u, dist_codes);
    bw_code(&w, lit_codes[65], lit_lens[65]);
    bw_code(&w, lit_codes[256], lit_lens[256]);
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"A", 1u,
                 "dynamic 码长全 1 的两符号树");

    /* B) 单符号距离树(只有 1 个距离码,码长 1)+ 重叠匹配 dist=1 len=3 */
    memset(lit_lens, 0, sizeof lit_lens);
    lit_lens[65] = 2u;
    lit_lens[256] = 1u;
    lit_lens[257] = 2u;
    memset(dist_lens, 0, sizeof dist_lens);
    dist_lens[0] = 1u;
    bw_init(&w);
    bw_dyn_header(&w, 1, lit_lens, 258u, dist_lens, 1u);
    tst_canon(lit_lens, 258u, lit_codes);
    tst_canon(dist_lens, 1u, dist_codes);
    bw_code(&w, lit_codes[65], lit_lens[65]);
    bw_code(&w, lit_codes[257], lit_lens[257]);   /* 长度码 257 = 3 字节 */
    bw_code(&w, dist_codes[0], dist_lens[0]);     /* 距离码 0 = 距离 1 */
    bw_code(&w, lit_codes[256], lit_lens[256]);
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"AAAA", 4u,
                 "dynamic 单符号距离树 + 重叠匹配");

    /* C) 最长 15 位码:lit/len 树的码长取 1,2,...,14 各一个 + 两个 15 位
     *    (Kraft 和 = (1 - 2^-14) + 2 * 2^-15 = 1,刚好完整);
     *    距离树用 16 个 4 位码(Kraft 和 = 16/16 = 1,完整树;HDIST 上限 30 个码)。 */
    memset(lit_lens, 0, sizeof lit_lens);
    lit_lens[256] = 1u;
    {
        unsigned k;
        for (k = 0u; k < 13u; ++k) {
            lit_lens[65u + k] = (unsigned char)(2u + k);   /* 65..77 -> 2..14 */
        }
        lit_lens[78] = 15u;
        lit_lens[79] = 15u;
    }
    memset(dist_lens, 0, sizeof dist_lens);
    {
        unsigned k;
        for (k = 0u; k < 16u; ++k) {
            dist_lens[k] = 4u;
        }
    }
    bw_init(&w);
    bw_dyn_header(&w, 1, lit_lens, 286u, dist_lens, 16u);
    tst_canon(lit_lens, 286u, lit_codes);
    bw_code(&w, lit_codes[78], lit_lens[78]);     /* 'N',15 位码 */
    bw_code(&w, lit_codes[256], lit_lens[256]);
    tst_canon(dist_lens, 16u, dist_codes);
    check(lit_lens[78] == 15u && lit_codes[78] == 32766u, "15 位码的规范码 = 32766");
    check(dist_lens[0] == 4u && dist_codes[15] == 15u, "距离树 16 个 4 位码(完整树)");
    expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"N", 1u, "dynamic 15 位码");

    /* D) 码长表用 16/17/18 重复编码
     *    码长码树(按符号):18 -> 1 位,0/1/16/17 -> 3 位,Kraft = 1/2 + 4/8 = 1 完整;
     *    规范码:18 -> 0,0 -> 4,1 -> 5,16 -> 6,17 -> 7。
     *    码长序列(共 258 个 = HLIT 257 + HDIST 1):
     *      符号0=0;16 重复 6 次 -> 符号 1..6;18 重复 58 次 -> 符号 7..64;
     *      符号65=1;18 重复 138 次 -> 66..203;17 重复 10 次 x5 -> 204..253;
     *      符号254=0;符号255=0;符号256=1;距离符号0=0。 */
    {
        unsigned char cl_sym_len[19];
        unsigned cl_sym_codes[19];
        unsigned i;

        memset(cl_sym_len, 0, sizeof cl_sym_len);
        cl_sym_len[0] = 3u;
        cl_sym_len[1] = 3u;
        cl_sym_len[16] = 3u;
        cl_sym_len[17] = 3u;
        cl_sym_len[18] = 1u;
        tst_canon(cl_sym_len, 19u, cl_sym_codes);
        check(cl_sym_codes[18] == 0u && cl_sym_codes[0] == 4u && cl_sym_codes[1] == 5u &&
              cl_sym_codes[16] == 6u && cl_sym_codes[17] == 7u, "码长码树规范码符合手算");

        memset(lit_lens, 0, sizeof lit_lens);
        lit_lens[65] = 1u;
        lit_lens[256] = 1u;
        memset(dist_lens, 0, sizeof dist_lens);

        bw_init(&w);
        bw_dyn_header_cl(&w, 1, cl_sym_len, 257u, 1u);
        bw_code(&w, cl_sym_codes[0], cl_sym_len[0]);          /* 符号 0 的码长 = 0 */
        bw_code(&w, cl_sym_codes[16], cl_sym_len[16]);        /* 16:重复上一个码长 */
        bw_put(&w, 6u - 3u, 2u);
        bw_code(&w, cl_sym_codes[18], cl_sym_len[18]);        /* 18:58 个 0 */
        bw_put(&w, 58u - 11u, 7u);
        bw_code(&w, cl_sym_codes[1], cl_sym_len[1]);          /* 符号 65 的码长 = 1 */
        bw_code(&w, cl_sym_codes[18], cl_sym_len[18]);        /* 18:138 个 0 */
        bw_put(&w, 138u - 11u, 7u);
        for (i = 0u; i < 5u; ++i) {                           /* 17 x5:每次 10 个 0 */
            bw_code(&w, cl_sym_codes[17], cl_sym_len[17]);
            bw_put(&w, 10u - 3u, 3u);
        }
        bw_code(&w, cl_sym_codes[0], cl_sym_len[0]);          /* 符号 254 的码长 = 0 */
        bw_code(&w, cl_sym_codes[0], cl_sym_len[0]);          /* 符号 255 的码长 = 0 */
        bw_code(&w, cl_sym_codes[1], cl_sym_len[1]);          /* 符号 256 的码长 = 1 */
        bw_code(&w, cl_sym_codes[0], cl_sym_len[0]);          /* 距离符号 0 的码长 = 0 */
        tst_canon(lit_lens, 257u, lit_codes);
        bw_code(&w, lit_codes[65], lit_lens[65]);
        bw_code(&w, lit_codes[256], lit_lens[256]);
        expect_bytes(w.buf, bw_size(&w), (const unsigned char *)"A", 1u,
                     "dynamic 码长表用 16/17/18 重复");
    }

    /* E) 非法 HLIT(字段 30 -> 287) */
    bw_init(&w);
    bw_mark(&w, 1, 2u);
    bw_put(&w, 30u, 5u);
    bw_put(&w, 0u, 5u);
    bw_put(&w, 0u, 4u);
    expect_bad(w.buf, bw_size(&w), "dynamic 非法 HLIT(=287)");

    /* F) 非法 HDIST(字段 30 -> 31) */
    bw_init(&w);
    bw_mark(&w, 1, 2u);
    bw_put(&w, 0u, 5u);
    bw_put(&w, 30u, 5u);
    bw_put(&w, 0u, 4u);
    expect_bad(w.buf, bw_size(&w), "dynamic 非法 HDIST(=31)");

    /* G) 码长码超额:18 个符号全给 1 位码 -> Kraft 和 = 9 > 1 */
    {
        unsigned char cl_sym_len[19];
        unsigned i;
        memset(cl_sym_len, 0, sizeof cl_sym_len);
        for (i = 0u; i < 19u; ++i) {
            cl_sym_len[i] = 1u;
        }
        bw_init(&w);
        bw_dyn_header_cl(&w, 1, cl_sym_len, 257u, 1u);
        for (i = 0u; i < 600u; ++i) {
            bw_put(&w, 0u, 1u);
        }
        expect_bad(w.buf, bw_size(&w), "dynamic 码长码超额(18 个 1 位码)");
    }

    /* H) 码长重复次数越过表尾:码长码树 {18:1, 0:2, 1:2}(完整),
     *    连写两个"18 号重复 138 个 0" = 276 > 258 个码长 -> 必须报错 */
    {
        unsigned char cl_sym_len[19];
        unsigned cl_sym_codes[19];
        memset(cl_sym_len, 0, sizeof cl_sym_len);
        cl_sym_len[18] = 1u;
        cl_sym_len[0] = 2u;
        cl_sym_len[1] = 2u;
        tst_canon(cl_sym_len, 19u, cl_sym_codes);
        bw_init(&w);
        bw_dyn_header_cl(&w, 1, cl_sym_len, 257u, 1u);
        bw_code(&w, cl_sym_codes[18], cl_sym_len[18]);
        bw_put(&w, 138u - 11u, 7u);
        bw_code(&w, cl_sym_codes[18], cl_sym_len[18]);
        bw_put(&w, 138u - 11u, 7u);
        expect_bad(w.buf, bw_size(&w), "dynamic 码长重复越过表尾(276 > 258)");
    }

    /* I) 第一个码长就用 16 号"重复上一个" -> 没有上一个,必须报错 */
    {
        unsigned char cl_sym_len[19];
        unsigned cl_sym_codes[19];
        memset(cl_sym_len, 0, sizeof cl_sym_len);
        cl_sym_len[16] = 1u;
        cl_sym_len[0] = 2u;
        cl_sym_len[1] = 2u;
        tst_canon(cl_sym_len, 19u, cl_sym_codes);
        bw_init(&w);
        bw_dyn_header_cl(&w, 1, cl_sym_len, 257u, 1u);
        bw_code(&w, cl_sym_codes[16], cl_sym_len[16]);
        bw_put(&w, 6u - 3u, 2u);
        expect_bad(w.buf, bw_size(&w), "dynamic 首个码长就是 16 号重复");
    }
}

/* ================================================================== */
/* 4) 跨 32 KiB 窗口的距离引用                                          */
/* ================================================================== */

static void test_cross_window(void)
{
    static unsigned char stored[40000];
    static unsigned char want[40000 + 1 + 258];
    static bitw w;
    unsigned i;

    printf("-- 跨 32 KiB 窗口 --\n");

    for (i = 0u; i < 40000u; ++i) {
        stored[i] = (unsigned char)((i * 7u + 3u) & 0xFFu);
    }

    /* 先用 stored 块铺 40000 字节历史,再用 fixed 块引用距离 32768(窗口上限) */
    bw_init(&w);
    bw_stored(&w, 0, stored, 40000u);
    bw_mark(&w, 1, 1u);
    bw_fixed_sym(&w, 'X');
    if (bw_fixed_match(&w, 258u, 32768u) != 0) {
        ++g_failures;
        fprintf(stderr, "FAIL 编码 dist=32768 len=258 匹配\n");
    }
    bw_fixed_sym(&w, 256u);

    memcpy(want, stored, 40000u);
    want[40000] = (unsigned char)'X';
    /* 距离 32768:从 total_out(=40001)往前 32768 处开始拷贝 258 字节 */
    memcpy(want + 40001u, stored + (40001u - 32768u), 258u);

    expect_bytes(w.buf, bw_size(&w), want, sizeof want, "dist=32768(窗口上限)引用 40 KB 历史");

    /* 同一条流按 7 字节一段喂,结果必须一样 */
    {
        static unsigned char out[OUT_CAP];
        size_t got = 0u;
        const int rc = inflate_chunked(w.buf, bw_size(&w), 7u, out, sizeof out, &got);
        ++g_checks;
        if (rc >= 0 && got == sizeof want && memcmp(out, want, sizeof want) == 0) {
            printf("ok   跨窗口流按 7 字节一段喂结果一致\n");
        } else {
            ++g_failures;
            fprintf(stderr, "FAIL 跨窗口流分片喂: rc=%d got=%u\n", rc, (unsigned)got);
        }
    }

    /* 距离刚好超过已输出长度 1 字节(dist=32769 无法用 5 位码表达,改用小历史) */
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    bw_fixed_sym(&w, 'a');
    if (bw_fixed_match(&w, 3u, 2u) != 0) {
        ++g_failures;
        fprintf(stderr, "FAIL 编码 dist=2 len=3 匹配\n");
    }
    bw_fixed_sym(&w, 256u);
    expect_bad(w.buf, bw_size(&w), "距离 2 > 已输出 1 字节");
}

/* ================================================================== */
/* 5) 流式接口 / 分片                                                  */
/* ================================================================== */

static void test_streaming(void)
{
    static bitw w;
    static unsigned char big[100000];
    unsigned i;

    printf("-- 流式接口 --\n");

    /* 100 KB 字面量:压过 16 KiB 交付阈值,且输出回绕窗口 3 次 */
    for (i = 0u; i < sizeof big; ++i) {
        big[i] = (unsigned char)((i * 13u + 5u) & 0xFFu);
    }
    bw_init(&w);
    bw_mark(&w, 1, 1u);
    for (i = 0u; i < sizeof big; ++i) {
        bw_fixed_sym(&w, big[i]);
    }
    bw_fixed_sym(&w, 256u);
    expect_bytes(w.buf, bw_size(&w), big, sizeof big, "100 KB fixed 字面量");

    /* 1 字节一段喂 */
    {
        static unsigned char out[OUT_CAP];
        size_t got = 0u;
        const int rc = inflate_chunked(w.buf, bw_size(&w), 1u, out, sizeof out, &got);
        ++g_checks;
        if (rc == 1 && got == sizeof big && memcmp(out, big, sizeof big) == 0) {
            printf("ok   100 KB 流按 1 字节一段喂结果一致\n");
        } else {
            ++g_failures;
            fprintf(stderr, "FAIL 100 KB 分片喂: rc=%d got=%u\n", rc, (unsigned)got);
        }
    }

    /* 3 字节一段喂(会有符号跨段) */
    {
        static unsigned char out[OUT_CAP];
        size_t got = 0u;
        const int rc = inflate_chunked(w.buf, bw_size(&w), 3u, out, sizeof out, &got);
        ++g_checks;
        if (rc == 1 && got == sizeof big && memcmp(out, big, sizeof big) == 0) {
            printf("ok   100 KB 流按 3 字节一段喂结果一致\n");
        } else {
            ++g_failures;
            fprintf(stderr, "FAIL 100 KB 分片喂(3B): rc=%d got=%u\n", rc, (unsigned)got);
        }
    }

    /* feed 返回 1 之后:finish 必须成功,再 feed 仍然返回 1 */
    {
        sxcl_inflate *inf = sxcl_inflate_open();
        unsigned char out[16];
        size_t got = 0u;
        static bitw w2;
        bw_init(&w2);
        bw_stored(&w2, 1, (const unsigned char *)"hi", 2u);

        if (inf == NULL) {
            ++g_failures;
            fprintf(stderr, "FAIL sxcl_inflate_open 返回 NULL\n");
        } else {
            outsink s;
            int rc;
            s.buf = out;
            s.cap = sizeof out;
            s.len = 0u;
            s.abort_at = -1;
            rc = sxcl_inflate_feed(inf, w2.buf, bw_size(&w2), outsink_write, &s);
            check(rc == 1 && s.len == 2u, "feed 解完整条流返回 1");
            check(sxcl_inflate_finish(inf) == 0, "finish 返回 0");
            rc = sxcl_inflate_feed(inf, w2.buf, bw_size(&w2), outsink_write, &s);
            check(rc == 1, "结束之后再 feed 仍返回 1");
            sxcl_inflate_close(inf);
        }
        (void)got;
    }

    /* 截断:少喂最后一个字节 -> feed 返回 0,finish 返回 -1 */
    {
        sxcl_inflate *inf = sxcl_inflate_open();
        static bitw w2;
        outsink s;
        int rc;

        bw_init(&w2);
        bw_mark(&w2, 1, 1u);
        bw_fixed_sym(&w2, 'A');
        bw_fixed_sym(&w2, 256u);     /* 共 2 字节 */

        s.buf = NULL;
        s.cap = 0u;
        s.len = 0u;
        s.abort_at = -1;
        if (inf == NULL) {
            ++g_failures;
            fprintf(stderr, "FAIL sxcl_inflate_open 返回 NULL\n");
        } else {
            rc = sxcl_inflate_feed(inf, w2.buf, 1u, outsink_write, &s);
            check(rc == 0, "只喂 1 字节时 feed 返回 0(需要更多数据)");
            check(sxcl_inflate_finish(inf) == -1, "截断数据 finish 返回 -1");
            sxcl_inflate_close(inf);
        }
    }

    /* sink 主动中止 -> feed 返回 -3 且状态粘住 */
    {
        sxcl_inflate *inf = sxcl_inflate_open();
        static bitw w2;
        static unsigned char out[64];
        outsink s;
        int rc;

        bw_init(&w2);
        bw_stored(&w2, 1, (const unsigned char *)"0123456789", 10u);

        s.buf = out;
        s.cap = sizeof out;
        s.len = 0u;
        s.abort_at = 0;
        if (inf == NULL) {
            ++g_failures;
            fprintf(stderr, "FAIL sxcl_inflate_open 返回 NULL\n");
        } else {
            rc = sxcl_inflate_feed(inf, w2.buf, bw_size(&w2), outsink_write, &s);
            check(rc == SXCL_INFLATE_ERR_ABORT, "sink 中止 -> feed 返回 -3");
            rc = sxcl_inflate_feed(inf, w2.buf, bw_size(&w2), outsink_write, &s);
            check(rc == SXCL_INFLATE_ERR_ABORT, "中止后状态粘住(-3)");
            check(sxcl_inflate_finish(inf) == SXCL_INFLATE_ERR_ABORT, "中止后 finish 也报错");
            sxcl_inflate_close(inf);
        }
    }

    /* NULL 句柄 / NULL 输入 */
    check(sxcl_inflate_finish(NULL) == -1, "finish(NULL) 返回 -1");
    check(sxcl_inflate_feed(NULL, "x", 1u, NULL, NULL) == -1, "feed(NULL) 返回 -1");
    sxcl_inflate_close(NULL);
    {
        sxcl_inflate *inf = sxcl_inflate_open();
        static bitw w2;
        bw_init(&w2);
        bw_stored(&w2, 1, (const unsigned char *)"x", 1u);
        check(sxcl_inflate_feed(inf, NULL, 5u, NULL, NULL) == -1, "feed(in=NULL,len>0) 返回 -1");
        sxcl_inflate_close(inf);
    }
}

/* ================================================================== */
/* 6) 一次性接口边界                                                   */
/* ================================================================== */

static void test_raw_edges(void)
{
    static unsigned char out[4096];
    size_t got = 0u;
    static bitw w;
    int rc;

    printf("-- 一次性接口边界 --\n");

    /* 输出缓冲差一字节 -> -2,且 out_len 给出真实长度 */
    bw_init(&w);
    bw_stored(&w, 1, (const unsigned char *)"0123456789", 10u);
    memset(out, 0, sizeof out);
    got = 0u;
    rc = sxcl_inflate_raw(w.buf, bw_size(&w), out, 9u, &got);
    check(rc == -2 && got == 10u, "缓冲太小 -> -2 且 out_len = 10");
    check(memcmp(out, "012345678", 9u) == 0, "缓冲太小也把能装的 9 字节写进去");

    /* 刚好够 */
    got = 0u;
    rc = sxcl_inflate_raw(w.buf, bw_size(&w), out, 10u, &got);
    check(rc == 0 && got == 10u && memcmp(out, "0123456789", 10u) == 0, "缓冲刚好够 -> 0");

    /* out = NULL, cap = 0:只问长度 */
    got = 0u;
    rc = sxcl_inflate_raw(w.buf, bw_size(&w), NULL, 0u, &got);
    check(rc == -2 && got == 10u, "out=NULL/cap=0 只问长度 -> -2 且 out_len = 10");

    /* out_len = NULL 也允许 */
    rc = sxcl_inflate_raw(w.buf, bw_size(&w), out, sizeof out, NULL);
    check(rc == 0, "out_len=NULL 时仍然解压成功");

    /* 0 字节输入、NULL 输入 */
    check(sxcl_inflate_raw(NULL, 0u, out, sizeof out, &got) == -1, "in=NULL -> -1");
    check(sxcl_inflate_raw("\x03", 1u, out, sizeof out, &got) == -1, "只有半截块头 -> -1");
    check(sxcl_inflate_raw("", 0u, out, sizeof out, &got) == -1, "空输入 -> -1");

    /* 非法的 BTYPE=10 但没有后续数据 -> 截断 */
    {
        static const unsigned char bad[1] = { 0x05u };  /* BFINAL=1 BTYPE=10,后面没了 */
        expect_bad(bad, 1u, "dynamic 块头截断");
    }
}

/* ================================================================== */

int main(void)
{
    printf("== sxcl inflate test ==\n");

    test_stored();
    test_fixed();
    test_dynamic();
    test_cross_window();
    test_streaming();
    test_raw_edges();

    printf("== %d 项断言, %d 项失败 ==\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
