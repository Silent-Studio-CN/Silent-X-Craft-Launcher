/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* xz.c - .xz 容器 + LZMA2 解码(**本项目自己实现**,见 include/sxcl/xz.h 的说明)。
 *
 * 代码结构(从下往上):
 *   1) CRC32 / CRC64 表驱动实现(XZ 用的 CRC64 是反射 ECMA-182,多项式 0xC96C5795D7870F42);
 *   2) 线性输出窗口:写指针顺序前进,腾地方时把"不再需要回溯"的前缀吐给 sink,
 *      再把最后 dict_size 字节 memmove 回窗口开头 —— 回溯永远在窗口内;
 *   3) 区间解码器 + LZMA 符号解码(字面量/匹配/重复距离/长度/距离槽),按 LZMA 规范实现;
 *   4) LZMA2 块循环(未压缩块 / LZMA 块 / 结束标记);
 *   5) XZ 容器(流头 -> 块* -> 索引 -> 流尾,支持多流串联与 4 字节对齐填充)。
 *
 * 两条实现口径(都是拿真流核对过的,不是照某个实现抄):
 *   * LZMA2 的属性字节只在 control >= 0xC0 时出现;XZ 容器里**没有**块内 4 字节字典字段
 *     (字典来自块头的 LZMA2 过滤器属性);
 *   * 区间解码器读越界时补 0(与 LZMA SDK 一致):正确性由块级 CRC 兜底,任何一处错都会
 *     被 CRC32/CRC64 抓住,不会静默解出垃圾。
 * 不认识的过滤器 / 校验类型一律**报错**,不猜。
 */

#include "sxcl/xz.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XZ_IN_BUF      65536u                 /* 输入缓冲 */
#define XZ_CHUNK_MAX   65536u                 /* LZMA2 压缩块上限(2 字节 + 1) */
#define XZ_WIN_SLACK   (2u * XZ_IN_BUF + 4096u)
#define XZ_MAX_BLOCK_HEADER 1024u
#define XZ_DEFAULT_DICT_LIMIT ((int64_t)64 * 1024 * 1024)

/* LZMA 常量 */
#define LZMA_NUM_STATES        12
#define LZMA_NUM_LIT_STATES     7
#define LZMA_NUM_POS_SLOT_BITS  6
#define LZMA_NUM_LEN_TO_POS     4
#define LZMA_NUM_ALIGN_BITS     4
#define LZMA_END_POS_MODEL     14
#define LZMA_NUM_FULL_DIST    128
#define LZMA_SPEC_POS_SIZE    (1 + LZMA_NUM_FULL_DIST - LZMA_END_POS_MODEL) /* 115 */
#define LZMA_MATCH_MIN_LEN      2
#define LZMA_PROB_INIT       1024u

typedef struct xz_range {
    uint32_t range;
    uint32_t code;
    const unsigned char *in;
    size_t size;
    size_t pos;
} xz_range;

typedef struct xz_len_dec {
    uint16_t choice;
    uint16_t choice2;
    uint16_t low[16][8];
    uint16_t mid[16][8];
    uint16_t high[256];
} xz_len_dec;

typedef struct xz_block_rec {
    uint64_t unpadded;
    uint64_t uncompressed;
} xz_block_rec;

struct sxcl_xz {
    sxcl_xz_source src;
    int (*sink)(void *ud, const void *data, size_t len);
    void *sink_ud;
    sxcl_xz_opts opts;

    unsigned char in[XZ_IN_BUF];
    size_t in_len;
    size_t in_pos;

    unsigned char *out;      /* 线性输出窗口 */
    size_t out_cap;
    size_t out_pos;
    size_t win_avail;        /* 可回溯字节数(<= dict_size) */
    uint32_t dict_size;
    int64_t total_out;

    int check_type;          /* 0 none / 1 CRC32 / 4 CRC64 */
    uint32_t crc32_tab[256];
    uint64_t crc64_tab[256];
    uint32_t blk_crc32;
    uint64_t blk_crc64;

    int lzma_ready;
    uint32_t lc, lp, pb;
    uint32_t lit_pos_mask;
    uint32_t pos_mask;
    uint16_t *lit;
    size_t lit_cap;
    uint16_t is_match[LZMA_NUM_STATES * 16];
    uint16_t is_rep[LZMA_NUM_STATES];
    uint16_t is_rep_g0[LZMA_NUM_STATES];
    uint16_t is_rep_g1[LZMA_NUM_STATES];
    uint16_t is_rep_g2[LZMA_NUM_STATES];
    uint16_t is_rep0_long[LZMA_NUM_STATES * 16];
    uint16_t pos_slot[LZMA_NUM_LEN_TO_POS * 64];
    uint16_t spec_pos[LZMA_SPEC_POS_SIZE];
    uint16_t align[LZMA_NUM_ALIGN_BITS * 16];
    xz_len_dec len_coder;
    xz_len_dec rep_len_coder;
    uint32_t state;
    uint32_t rep0, rep1, rep2, rep3;
    uint32_t lzma_pos;

    unsigned char chunk[XZ_CHUNK_MAX];  /* 当前 LZMA2 块的压缩数据 */
    size_t chunk_size;
    size_t chunk_pos;

    xz_block_rec *blocks;    /* 索引核对用 */
    size_t block_count;
    size_t block_cap;

    int err_code;
    char err_text[256];
    int finished;
    int result;
};

/* ══════════════════════ 1) CRC ══════════════════════ */

static void crc_tables_build(sxcl_xz *xz)
{
    uint32_t i;
    int k;
    for (i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        xz->crc32_tab[i] = c;
    }
    for (i = 0; i < 256; ++i) {
        uint64_t c = (uint64_t)i;
        for (k = 0; k < 8; ++k) {
            c = (c & 1ull) ? (0xC96C5795D7870F42ull ^ (c >> 1)) : (c >> 1);
        }
        xz->crc64_tab[i] = c;
    }
}

static uint32_t crc32_run(const sxcl_xz *xz, uint32_t crc, const unsigned char *p, size_t n)
{
    while (n-- > 0) {
        crc = xz->crc32_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

static uint64_t crc64_run(const sxcl_xz *xz, uint64_t crc, const unsigned char *p, size_t n)
{
    while (n-- > 0) {
        crc = xz->crc64_tab[(unsigned)((crc ^ *p++) & 0xFFu)] ^ (crc >> 8);
    }
    return crc;
}

static uint32_t crc32_of(const sxcl_xz *xz, const unsigned char *p, size_t n)
{
    return crc32_run(xz, 0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

/* ══════════════════════ 2) 输入与错误 ══════════════════════ */

static void xz_fail(sxcl_xz *xz, int code, const char *text)
{
    if (xz->err_code == 0) {
        xz->err_code = code;
        snprintf(xz->err_text, sizeof(xz->err_text), "%s", text != NULL ? text : "");
        xz->err_text[sizeof(xz->err_text) - 1] = '\0';
    }
}

static void xz_failf(sxcl_xz *xz, int code, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    if (xz->err_code != 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    xz_fail(xz, code, buf);
}

static int xz_read_exact(sxcl_xz *xz, void *dst, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n > 0) {
        size_t take;
        if (xz->in_pos >= xz->in_len) {
            const int64_t got = xz->src.read(xz->src.ud, xz->in, sizeof(xz->in));
            xz->in_pos = 0;
            xz->in_len = 0;
            if (got < 0) {
                xz_fail(xz, SXCL_XZ_ERR_IO, "读取输入失败(来源报错)");
                return SXCL_XZ_ERR_IO;
            }
            if (got == 0) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "输入提前结束(流被截断)");
                return SXCL_XZ_ERR_DATA;
            }
            xz->in_len = (size_t)got;
        }
        take = xz->in_len - xz->in_pos;
        if (take > n) {
            take = n;
        }
        memcpy(p, xz->in + xz->in_pos, take);
        xz->in_pos += take;
        p += take;
        n -= take;
    }
    return SXCL_XZ_OK;
}

/** 读一个字节:0 = 成功(写进 *out);1 = 流已经到底(不是错误);负 = 出错。 */
static int xz_try_getc(sxcl_xz *xz, int *out)
{
    if (xz->in_pos >= xz->in_len) {
        const int64_t got = xz->src.read(xz->src.ud, xz->in, sizeof(xz->in));
        xz->in_pos = 0;
        xz->in_len = 0;
        if (got < 0) {
            xz_fail(xz, SXCL_XZ_ERR_IO, "读取输入失败(来源报错)");
            return SXCL_XZ_ERR_IO;
        }
        if (got == 0) {
            return 1;
        }
        xz->in_len = (size_t)got;
    }
    *out = xz->in[xz->in_pos++];
    return 0;
}

static int xz_getc(sxcl_xz *xz, int *out)
{
    const int rc = xz_try_getc(xz, out);
    if (rc == 1) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "输入提前结束(流被截断)");
        return SXCL_XZ_ERR_DATA;
    }
    return rc;
}

/* 说明:块头与索引里的变长整数都是**就地解析**的(索引那份还要边读边算 CRC32),
 * 所以这里没有独立的 varint 读函数 —— 硬拆一个出来只会多一份没人走的死代码。 */

/* ══════════════════════ 3) 输出窗口 ══════════════════════ */

static int win_flush(sxcl_xz *xz, size_t upto)
{
    if (upto == 0) {
        return SXCL_XZ_OK;
    }
    if (xz->sink != NULL && xz->sink(xz->sink_ud, xz->out, upto) != 0) {
        xz_fail(xz, SXCL_XZ_ERR_ABORT, "sink 主动中止");
        return SXCL_XZ_ERR_ABORT;
    }
    return SXCL_XZ_OK;
}

static int win_reserve(sxcl_xz *xz, size_t need)
{
    size_t keep, drop;
    if (xz->out_pos + need <= xz->out_cap) {
        return SXCL_XZ_OK;
    }
    keep = xz->win_avail > xz->dict_size ? xz->dict_size : xz->win_avail;
    drop = xz->out_pos - keep;
    {
        const int rc = win_flush(xz, drop);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
    }
    if (drop > 0) {
        memmove(xz->out, xz->out + drop, keep);
    }
    xz->out_pos = keep;
    if (xz->out_pos + need > xz->out_cap) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "内部错误:输出窗口不够(不该发生)");
        return SXCL_XZ_ERR_DATA;
    }
    return SXCL_XZ_OK;
}

static int win_account(sxcl_xz *xz, size_t start, size_t n)
{
    if (xz->opts.out_limit > 0 && xz->total_out + (int64_t)n > xz->opts.out_limit) {
        xz_failf(xz, SXCL_XZ_ERR_LIMIT, "解压总量超过上限(%lld 字节)",
                 (long long)xz->opts.out_limit);
        return SXCL_XZ_ERR_LIMIT;
    }
    if (xz->check_type == 1) {
        xz->blk_crc32 = crc32_run(xz, xz->blk_crc32, xz->out + start, n);
    } else if (xz->check_type == 4) {
        xz->blk_crc64 = crc64_run(xz, xz->blk_crc64, xz->out + start, n);
    }
    xz->total_out += (int64_t)n;
    xz->lzma_pos += (uint32_t)n;
    xz->win_avail += n;
    if (xz->win_avail > xz->dict_size) {
        xz->win_avail = xz->dict_size;
    }
    return SXCL_XZ_OK;
}

static int win_emit(sxcl_xz *xz, const unsigned char *src, size_t n)
{
    int rc;
    size_t start;
    if (n == 0) {
        return SXCL_XZ_OK;
    }
    rc = win_reserve(xz, n);
    if (rc != SXCL_XZ_OK) {
        return rc;
    }
    start = xz->out_pos;
    memcpy(xz->out + start, src, n);
    xz->out_pos += n;
    return win_account(xz, start, n);
}

/** 回溯拷贝(dist = 真实距离 >= 1;允许 dist < len 的重叠展开)。 */
static int win_emit_match(sxcl_xz *xz, uint32_t dist, uint32_t len)
{
    int rc;
    size_t start;
    size_t produced;
    unsigned char *dst;
    const unsigned char *src;
    uint32_t left;
    if (dist == 0) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "匹配距离为 0");
        return SXCL_XZ_ERR_DATA;
    }
    if (dist > xz->win_avail) {
        xz_failf(xz, SXCL_XZ_ERR_DATA, "匹配距离 %u 超过已产出的 %u 字节",
                 (unsigned)dist, (unsigned)xz->win_avail);
        return SXCL_XZ_ERR_DATA;
    }
    rc = win_reserve(xz, len);
    if (rc != SXCL_XZ_OK) {
        return rc;
    }
    start = xz->out_pos;
    dst = xz->out + start;
    src = dst - dist;
    left = len;
    while (left > 0) {
        const size_t n = (dist < left) ? (size_t)dist : (size_t)left;
        memcpy(dst, src, n);
        dst += n;
        src += n;
        left -= (uint32_t)n;
    }
    produced = (size_t)(dst - (xz->out + start));
    xz->out_pos += produced;
    return win_account(xz, start, produced);
}

static void win_reset(sxcl_xz *xz)
{
    xz->out_pos = 0;
    xz->win_avail = 0;
}

/* ══════════════════════ 4) 区间解码器 + LZMA ══════════════════════ */

static void rc_init(xz_range *r, const unsigned char *in, size_t size)
{
    int i;
    r->in = in;
    r->size = size;
    r->pos = 0;
    r->range = 0xFFFFFFFFu;
    r->code = 0;
    for (i = 0; i < 5; ++i) {
        const unsigned char b = (r->pos < r->size) ? r->in[r->pos] : 0;
        ++r->pos;
        r->code = (r->code << 8) | b;
    }
}

static unsigned char rc_byte(xz_range *r)
{
    const unsigned char b = (r->pos < r->size) ? r->in[r->pos] : 0;
    ++r->pos;
    return b;
}

static unsigned rc_bit(xz_range *r, uint16_t *prob)
{
    uint32_t bound = (r->range >> 11) * (uint32_t)(*prob);
    unsigned bit;
    if (r->code < bound) {
        r->range = bound;
        *prob = (uint16_t)(*prob + (((1u << 11) - *prob) >> 5));
        bit = 0;
    } else {
        r->range -= bound;
        r->code -= bound;
        *prob = (uint16_t)(*prob - (*prob >> 5));
        bit = 1;
    }
    while (r->range < (1u << 24)) {
        r->range <<= 8;
        r->code = (r->code << 8) | rc_byte(r);
    }
    return bit;
}

static uint32_t rc_bittree(xz_range *r, uint16_t *probs, unsigned nbits)
{
    uint32_t m = 1;
    unsigned i;
    for (i = 0; i < nbits; ++i) {
        m = (m << 1) | rc_bit(r, &probs[m]);
    }
    return m - (1u << nbits);
}

static uint32_t rc_bittree_rev(xz_range *r, uint16_t *probs, unsigned nbits)
{
    uint32_t m = 1;
    uint32_t sym = 0;
    unsigned i;
    for (i = 0; i < nbits; ++i) {
        const uint32_t b = rc_bit(r, &probs[m]);
        m = (m << 1) | b;
        sym |= b << i;
    }
    return sym;
}

static uint32_t rc_direct(xz_range *r, unsigned nbits)
{
    uint32_t result = 0;
    while (nbits-- > 0) {
        uint32_t t;
        r->range >>= 1;
        r->code -= r->range;
        t = 0u - (r->code >> 31);
        r->code += r->range & t;
        result = (result << 1) + (t + 1u);
        while (r->range < (1u << 24)) {
            r->range <<= 8;
            r->code = (r->code << 8) | rc_byte(r);
        }
    }
    return result;
}

static void prob_reset(uint16_t *p, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        p[i] = (uint16_t)LZMA_PROB_INIT;
    }
}

static void lzma_len_reset(xz_len_dec *d)
{
    prob_reset((uint16_t *)d, sizeof(*d) / sizeof(uint16_t));
}

static void lzma_reset_state(sxcl_xz *xz)
{
    prob_reset(xz->is_match, LZMA_NUM_STATES * 16u);
    prob_reset(xz->is_rep, LZMA_NUM_STATES);
    prob_reset(xz->is_rep_g0, LZMA_NUM_STATES);
    prob_reset(xz->is_rep_g1, LZMA_NUM_STATES);
    prob_reset(xz->is_rep_g2, LZMA_NUM_STATES);
    prob_reset(xz->is_rep0_long, LZMA_NUM_STATES * 16u);
    prob_reset(xz->pos_slot, LZMA_NUM_LEN_TO_POS * 64u);
    prob_reset(xz->spec_pos, LZMA_SPEC_POS_SIZE);
    prob_reset(xz->align, LZMA_NUM_ALIGN_BITS * 16u);
    lzma_len_reset(&xz->len_coder);
    lzma_len_reset(&xz->rep_len_coder);
    if (xz->lit != NULL) {
        prob_reset(xz->lit, xz->lit_cap);
    }
    xz->state = 0;
    xz->rep0 = xz->rep1 = xz->rep2 = xz->rep3 = 0;
    /* 注意:**不重置 lzma_pos**。processedPos 只在"字典重置"时归零(mode 3 与未压缩块的
     * ctl=0x01);mode 1/2(状态重置 / 换属性)不重置它 —— 否则 pos_state 会从这里开始错位。 */
}

static int lzma_set_props(sxcl_xz *xz, unsigned char props)
{
    uint32_t lc, lp, pb;
    size_t need;
    if (props >= (9u * 5u * 5u)) {
        xz_failf(xz, SXCL_XZ_ERR_DATA, "LZMA 属性字节 %u 越界(应 < 225)", (unsigned)props);
        return SXCL_XZ_ERR_DATA;
    }
    lc = props % 9u;
    props = (unsigned char)(props / 9u);
    lp = props % 5u;
    pb = props / 5u;
    if (lc + lp > 4u) {
        xz_failf(xz, SXCL_XZ_ERR_UNSUPPORTED, "LZMA 的 lc+lp = %u 超过 4(LZMA2 不允许)",
                 (unsigned)(lc + lp));
        return SXCL_XZ_ERR_UNSUPPORTED;
    }
    need = ((size_t)0x300) << (lc + lp);
    if (need > xz->lit_cap) {
        uint16_t *nlit = (uint16_t *)realloc(xz->lit, need * sizeof(uint16_t));
        if (nlit == NULL) {
            xz_fail(xz, SXCL_XZ_ERR_NOMEM, "字面量概率表分配失败");
            return SXCL_XZ_ERR_NOMEM;
        }
        xz->lit = nlit;
        xz->lit_cap = need;
    }
    xz->lc = lc;
    xz->lp = lp;
    xz->pb = pb;
    xz->lit_pos_mask = (lp == 0u) ? 0u : ((1u << lp) - 1u);
    xz->pos_mask = (pb == 0u) ? 0u : ((1u << pb) - 1u);
    xz->lzma_ready = 1;
    return SXCL_XZ_OK;
}

/** 取第 dist(1 起)个字节之前的那个字节(dist=1 就是上一个字节)。 */
static unsigned char win_back(const sxcl_xz *xz, uint32_t dist)
{
    return xz->out[xz->out_pos - dist];
}

/** 解一个字面量。prev_state 是**更新之前**的 state —— 走不走"匹配字面量"那条路只由它决定
 *  (踩过的坑:先改 state 再判 >= 7,那条路永远不会被走到,解到第一个匹配之后就开始发散)。 */
static int lzma_literal(sxcl_xz *xz, xz_range *r, uint32_t prev_state)
{
    const unsigned char prev = (xz->win_avail > 0) ? xz->out[xz->out_pos - 1] : 0u;
    const uint32_t low = (uint32_t)prev >> (8u - xz->lc);
    const uint32_t high = (xz->lzma_pos & xz->lit_pos_mask) << xz->lc;
    uint16_t *probs = xz->lit + ((size_t)0x300) * (size_t)(low + high);
    uint32_t symbol = 1;
    unsigned char out;
    if (prev_state >= LZMA_NUM_LIT_STATES && xz->win_avail > (size_t)xz->rep0) {
        unsigned match_byte = win_back(xz, xz->rep0 + 1u);
        while (symbol < 0x100u) {
            const unsigned match_bit = (match_byte >> 7) & 1u;
            const unsigned bit = rc_bit(r, &probs[0x100u + (match_bit << 8) + symbol]);
            match_byte = (match_byte << 1) & 0xFFu;
            symbol = (symbol << 1) | bit;
            if (bit != match_bit) {
                while (symbol < 0x100u) {
                    symbol = (symbol << 1) | rc_bit(r, &probs[symbol]);
                }
                break;
            }
        }
    } else {
        while (symbol < 0x100u) {
            symbol = (symbol << 1) | rc_bit(r, &probs[symbol]);
        }
    }
    out = (unsigned char)(symbol & 0xFFu);
    return win_emit(xz, &out, 1);
}

static uint32_t lzma_len(xz_range *r, xz_len_dec *d, uint32_t pos_state)
{
    if (rc_bit(r, &d->choice) == 0) {
        return rc_bittree(r, d->low[pos_state], 3);
    }
    if (rc_bit(r, &d->choice2) == 0) {
        return 8u + rc_bittree(r, d->mid[pos_state], 3);
    }
    return 16u + rc_bittree(r, d->high, 8);
}

/** 解一个 LZMA "符号"(一次字面量或一次匹配),把结果写进输出窗口。
 *  amount: 还剩多少字节要解(用来判断"最后一个符号不再产出"的边界)。 */
static int lzma_symbol(sxcl_xz *xz, xz_range *r)
{
    const uint32_t pos_state = xz->lzma_pos & xz->pos_mask;
    if (rc_bit(r, &xz->is_match[xz->state * 16u + pos_state]) == 0) {
        const uint32_t prev_state = xz->state;
        xz->state = (prev_state < 4u) ? 0u
                                      : (prev_state < 10u ? prev_state - 3u : prev_state - 6u);
        return lzma_literal(xz, r, prev_state);
    }
    {
        uint32_t len;
        uint32_t dist;
        if (rc_bit(r, &xz->is_rep[xz->state]) != 0) {
            if (rc_bit(r, &xz->is_rep_g0[xz->state]) == 0) {
                if (rc_bit(r, &xz->is_rep0_long[xz->state * 16u + pos_state]) == 0) {
                    /* 短重复:只吐一个字节 */
                    xz->state = (xz->state < LZMA_NUM_LIT_STATES) ? 9u : 11u;
                    return win_emit_match(xz, xz->rep0 + 1u, 1u);
                }
            } else {
                uint32_t d;
                if (rc_bit(r, &xz->is_rep_g1[xz->state]) == 0) {
                    d = xz->rep1;
                } else {
                    if (rc_bit(r, &xz->is_rep_g2[xz->state]) == 0) {
                        d = xz->rep2;
                    } else {
                        d = xz->rep3;
                        xz->rep3 = xz->rep2;
                    }
                    xz->rep2 = xz->rep1;
                }
                xz->rep1 = xz->rep0;
                xz->rep0 = d;
            }
            len = lzma_len(r, &xz->rep_len_coder, pos_state) + LZMA_MATCH_MIN_LEN;
            xz->state = (xz->state < LZMA_NUM_LIT_STATES) ? 8u : 11u;
        } else {
            uint32_t pos_slot;
            xz->rep3 = xz->rep2;
            xz->rep2 = xz->rep1;
            xz->rep1 = xz->rep0;
            len = lzma_len(r, &xz->len_coder, pos_state) + LZMA_MATCH_MIN_LEN;
            xz->state = (xz->state < LZMA_NUM_LIT_STATES) ? 7u : 10u;
            pos_slot = rc_bittree(r, xz->pos_slot + 64u * ((len - LZMA_MATCH_MIN_LEN < 4u)
                                                               ? (len - LZMA_MATCH_MIN_LEN)
                                                               : 3u),
                                  LZMA_NUM_POS_SLOT_BITS);
            if (pos_slot < 4u) {
                xz->rep0 = pos_slot;
            } else {
                const unsigned nbits = (unsigned)(pos_slot >> 1) - 1u;
                uint32_t d = (2u | (pos_slot & 1u)) << nbits;
                if (pos_slot < LZMA_END_POS_MODEL) {
                    uint16_t *probs = xz->spec_pos + d - pos_slot - 1u;
                    uint32_t m = 1;
                    uint32_t sym = 0;
                    unsigned i;
                    for (i = 0; i < nbits; ++i) {
                        const uint32_t b = rc_bit(r, &probs[m]);
                        m = (m << 1) | b;
                        sym |= b << i;
                    }
                    d += sym;
                } else {
                    d += rc_direct(r, nbits - LZMA_NUM_ALIGN_BITS) << LZMA_NUM_ALIGN_BITS;
                    d += rc_bittree_rev(r, xz->align, LZMA_NUM_ALIGN_BITS);
                }
                if (d == 0xFFFFFFFFu) {
                    xz_fail(xz, SXCL_XZ_ERR_UNSUPPORTED,
                            "LZMA 结束标记(end marker)不在 LZMA2 里使用");
                    return SXCL_XZ_ERR_UNSUPPORTED;
                }
                xz->rep0 = d;
            }
        }
        dist = xz->rep0 + 1u;
        return win_emit_match(xz, dist, len);
    }
}

/** 解一个 LZMA2 的 LZMA 块:把 chunk 里 uncompressed 字节解出来。 */
static int lzma2_decode_chunk(sxcl_xz *xz, uint32_t uncompressed)
{
    xz_range r;
    uint32_t produced = 0;
    rc_init(&r, xz->chunk, xz->chunk_size);
    while (produced < uncompressed) {
        const int64_t before = xz->total_out;
        const int rc = lzma_symbol(xz, &r);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
        {
            const int64_t delta = xz->total_out - before;
            if (delta <= 0) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "LZMA 解码没有推进(数据损坏)");
                return SXCL_XZ_ERR_DATA;
            }
            if ((uint64_t)delta > (uint64_t)(uncompressed - produced)) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "LZMA 块解出的字节数超过块头声明");
                return SXCL_XZ_ERR_DATA;
            }
            produced += (uint32_t)delta;
        }
    }
    return SXCL_XZ_OK;
}

/* ══════════════════════ 5) LZMA2 块循环 ══════════════════════ */

/** 解一个数据块的 LZMA2 数据(到 control == 0x00 为止)。返回用掉的字节数(含结束标记)。 */
static int lzma2_run(sxcl_xz *xz, uint64_t *bytes_used)
{
    uint64_t used = 0;
    for (;;) {
        int ctl = 0;
        int rc = xz_getc(xz, &ctl);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
        ++used;
        if (ctl == 0x00) {
            break; /* LZMA2 数据结束 */
        }
        if (ctl <= 0x02) {
            int hi = 0, lo = 0;
            size_t size;
            rc = xz_getc(xz, &hi);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            rc = xz_getc(xz, &lo);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            used += 2;
            size = (size_t)(((unsigned)hi << 8) | (unsigned)lo) + 1u;
            if (ctl == 0x01) {
                /* 字典重置:窗口里之前的东西一律不再引用 */
                const int drc = win_flush(xz, xz->out_pos);
                if (drc != SXCL_XZ_OK) {
                    return drc;
                }
                win_reset(xz);
                xz->lzma_pos = 0;
            }
            {
                /* 未压缩块最大 64 KiB:读满一段就吐一段 */
                unsigned char buf[4096];
                size_t left = size;
                while (left > 0) {
                    const size_t take = left < sizeof(buf) ? left : sizeof(buf);
                    rc = xz_read_exact(xz, buf, take);
                    if (rc != SXCL_XZ_OK) {
                        return rc;
                    }
                    rc = win_emit(xz, buf, take);
                    if (rc != SXCL_XZ_OK) {
                        return rc;
                    }
                    left -= take;
                }
                used += size;
            }
            continue;
        }
        if (ctl >= 0x80) {
            int b1 = 0, b2 = 0, b3 = 0, b4 = 0;
            uint32_t uncompressed, csize;
            unsigned mode = ((unsigned)ctl >> 5) & 0x3u;
            rc = xz_getc(xz, &b1);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            rc = xz_getc(xz, &b2);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            rc = xz_getc(xz, &b3);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            rc = xz_getc(xz, &b4);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            used += 4;
            uncompressed = (((uint32_t)(ctl & 0x1F) << 16) | ((uint32_t)b1 << 8) |
                            (uint32_t)b2) + 1u;
            csize = (((uint32_t)b3 << 8) | (uint32_t)b4) + 1u;
            if (ctl >= 0xC0) {
                int props = 0;
                rc = xz_getc(xz, &props);
                if (rc != SXCL_XZ_OK) {
                    return rc;
                }
                ++used;
                rc = lzma_set_props(xz, (unsigned char)props);
                if (rc != SXCL_XZ_OK) {
                    return rc;
                }
            } else if (!xz->lzma_ready) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "第一个 LZMA 块没有给属性字节");
                return SXCL_XZ_ERR_DATA;
            }
            if (ctl >= 0xE0) {
                /* 字典重置(属性已经给了,状态/概率在下面按 mode 处理) */
                const int drc = win_flush(xz, xz->out_pos);
                if (drc != SXCL_XZ_OK) {
                    return drc;
                }
                win_reset(xz);
                xz->lzma_pos = 0;
            }
            if (mode >= 2) {
                lzma_reset_state(xz);
            } else if (mode == 1) {
                prob_reset(xz->is_match, LZMA_NUM_STATES * 16u);
                prob_reset(xz->is_rep, LZMA_NUM_STATES);
                prob_reset(xz->is_rep_g0, LZMA_NUM_STATES);
                prob_reset(xz->is_rep_g1, LZMA_NUM_STATES);
                prob_reset(xz->is_rep_g2, LZMA_NUM_STATES);
                prob_reset(xz->is_rep0_long, LZMA_NUM_STATES * 16u);
                prob_reset(xz->pos_slot, LZMA_NUM_LEN_TO_POS * 64u);
                prob_reset(xz->spec_pos, LZMA_SPEC_POS_SIZE);
                prob_reset(xz->align, LZMA_NUM_ALIGN_BITS * 16u);
                lzma_len_reset(&xz->len_coder);
                lzma_len_reset(&xz->rep_len_coder);
                if (xz->lit != NULL) {
                    prob_reset(xz->lit, xz->lit_cap);
                }
                xz->state = 0;
                xz->rep0 = xz->rep1 = xz->rep2 = xz->rep3 = 0;
            }
            if (csize > sizeof(xz->chunk)) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "LZMA2 压缩块长度越界");
                return SXCL_XZ_ERR_DATA;
            }
            rc = xz_read_exact(xz, xz->chunk, csize);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            used += csize;
            xz->chunk_size = csize;
            rc = lzma2_decode_chunk(xz, uncompressed);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            continue;
        }
        xz_failf(xz, SXCL_XZ_ERR_DATA, "LZMA2 控制字节 %#x 非法(0x03..0x7F 保留)",
                 (unsigned)ctl);
        return SXCL_XZ_ERR_DATA;
    }
    *bytes_used = used;
    return SXCL_XZ_OK;
}

/* ══════════════════════ 6) XZ 容器 ══════════════════════ */

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t le64(const unsigned char *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int xz_read_stream_header(sxcl_xz *xz, int first)
{
    static const unsigned char magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
    unsigned char hdr[12];
    int rc;
    if (first) {
        rc = xz_read_exact(xz, hdr, 12);
    } else {
        /* 已经吃掉了前 4 个字节(调用方判定过不是填充) */
        memcpy(hdr, "\xFD" "7zX", 4);
        rc = xz_read_exact(xz, hdr + 4, 8);
    }
    if (rc != SXCL_XZ_OK) {
        return rc;
    }
    if (memcmp(hdr, magic, 6) != 0) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "不是 .xz 流(魔数不符)");
        return SXCL_XZ_ERR_DATA;
    }
    if (hdr[6] != 0x00) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "流头保留字节非 0");
        return SXCL_XZ_ERR_DATA;
    }
    if (hdr[7] == 0x00 || hdr[7] == 0x01 || hdr[7] == 0x04) {
        xz->check_type = (int)hdr[7];
    } else {
        xz_failf(xz, SXCL_XZ_ERR_UNSUPPORTED, "校验类型 %u 不支持(只支持 0/1/4)",
                 (unsigned)hdr[7]);
        return SXCL_XZ_ERR_UNSUPPORTED;
    }
    if (crc32_of(xz, hdr + 6, 2) != le32(hdr + 8)) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "流头 CRC32 不符");
        return SXCL_XZ_ERR_DATA;
    }
    return SXCL_XZ_OK;
}

static int block_rec_push(sxcl_xz *xz, uint64_t unpadded, uint64_t uncompressed)
{
    if (xz->block_count == xz->block_cap) {
        const size_t cap = xz->block_cap == 0 ? 16u : xz->block_cap * 2u;
        xz_block_rec *p = (xz_block_rec *)realloc(xz->blocks, cap * sizeof(*p));
        if (p == NULL) {
            xz_fail(xz, SXCL_XZ_ERR_NOMEM, "块记录数组分配失败");
            return SXCL_XZ_ERR_NOMEM;
        }
        xz->blocks = p;
        xz->block_cap = cap;
    }
    xz->blocks[xz->block_count].unpadded = unpadded;
    xz->blocks[xz->block_count].uncompressed = uncompressed;
    ++xz->block_count;
    return SXCL_XZ_OK;
}

static int xz_read_block_header(sxcl_xz *xz, uint32_t *dict_size, uint64_t *hdr_size,
                                uint64_t *comp_size, int *has_comp_size, uint64_t *uncomp_size,
                                int *has_uncomp_size)
{
    unsigned char hdr[XZ_MAX_BLOCK_HEADER];
    size_t total;
    size_t pos = 0;
    unsigned char flags;
    uint64_t v = 0;
    int rc;
    {
        int first = 0;
        rc = xz_getc(xz, &first);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
        total = ((size_t)first + 1u) * 4u;
        if (total > sizeof(hdr)) {
            xz_fail(xz, SXCL_XZ_ERR_DATA, "块头长度越界");
            return SXCL_XZ_ERR_DATA;
        }
        hdr[0] = (unsigned char)first;
    }
    rc = xz_read_exact(xz, hdr + 1, total - 1);
    if (rc != SXCL_XZ_OK) {
        return rc;
    }
    if (crc32_of(xz, hdr, total - 4) != le32(hdr + total - 4)) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "块头 CRC32 不符");
        return SXCL_XZ_ERR_DATA;
    }
    flags = hdr[1];
    if ((flags & 0x3Cu) != 0) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "块头保留位非 0");
        return SXCL_XZ_ERR_DATA;
    }
    pos = 2;
    *has_comp_size = (flags & 0x40u) ? 1 : 0;
    *has_uncomp_size = (flags & 0x80u) ? 1 : 0;
    *comp_size = 0;
    *uncomp_size = 0;
#define XZ_HDR_VARINT(dst)                                                          \
    do {                                                                            \
        uint64_t acc = 0;                                                           \
        int shift = 0;                                                              \
        int _xi;                                                                    \
        for (_xi = 0; _xi < 9; ++_xi) {                                             \
            unsigned char c;                                                        \
            if (pos >= total - 4) {                                                 \
                xz_fail(xz, SXCL_XZ_ERR_DATA, "块头里变长整数越界");                \
                return SXCL_XZ_ERR_DATA;                                            \
            }                                                                       \
            c = hdr[pos++];                                                         \
            acc |= (uint64_t)(c & 0x7Fu) << shift;                                  \
            if ((c & 0x80u) == 0) {                                                 \
                break;                                                              \
            }                                                                       \
            shift += 7;                                                             \
        }                                                                           \
        (dst) = acc;                                                                \
    } while (0)
    if (*has_comp_size) {
        XZ_HDR_VARINT(*comp_size);
    }
    if (*has_uncomp_size) {
        XZ_HDR_VARINT(*uncomp_size);
    }
    {
        const unsigned nfilters = (unsigned)(flags & 0x03u) + 1u;
        unsigned i;
        if (nfilters != 1u) {
            xz_failf(xz, SXCL_XZ_ERR_UNSUPPORTED, "块里有 %u 个过滤器(只支持 1 个 LZMA2)",
                     nfilters);
            return SXCL_XZ_ERR_UNSUPPORTED;
        }
        for (i = 0; i < nfilters; ++i) {
            uint64_t fid = 0, psize = 0;
            XZ_HDR_VARINT(fid);
            XZ_HDR_VARINT(psize);
            if (fid != 0x21u) {
                xz_failf(xz, SXCL_XZ_ERR_UNSUPPORTED,
                         "过滤器 ID %#llx 不支持(只支持 LZMA2 = 0x21)", (unsigned long long)fid);
                return SXCL_XZ_ERR_UNSUPPORTED;
            }
            if (psize != 1u || pos + 1u > total - 4) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "LZMA2 过滤器属性长度不是 1");
                return SXCL_XZ_ERR_DATA;
            }
            {
                const unsigned char prop = hdr[pos++];
                if (prop > 40u) {
                    xz_failf(xz, SXCL_XZ_ERR_DATA, "LZMA2 字典大小属性 %u 越界", (unsigned)prop);
                    return SXCL_XZ_ERR_DATA;
                }
                v = (prop == 40u) ? 0xFFFFFFFFu
                                  : (((uint64_t)2u | (prop & 1u)) << (prop / 2u + 11u));
            }
        }
    }
#undef XZ_HDR_VARINT
    /* 过滤器之后是 0 填充,对齐到 4 字节(整块头起点对齐) */
    while (pos < total - 4 && hdr[pos] == 0x00) {
        ++pos;
    }
    if (pos != total - 4) {
        xz_fail(xz, SXCL_XZ_ERR_DATA, "块头里有多余字节(对齐不对)");
        return SXCL_XZ_ERR_DATA;
    }
    *dict_size = (uint32_t)v;
    *hdr_size = total;
    return SXCL_XZ_OK;
}

static int xz_read_index(sxcl_xz *xz, uint64_t *index_size_out)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t count = 0;
    uint64_t pos = 0;
    size_t i;
    {
        int c = 0;
        int rc = xz_getc(xz, &c);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
        if (c != 0x00) {
            xz_fail(xz, SXCL_XZ_ERR_DATA, "索引指示字节不是 0x00");
            return SXCL_XZ_ERR_DATA;
        }
        {
            const unsigned char zb = 0x00;
            crc = crc32_run(xz, crc, &zb, 1);
        }
        pos = 1;
    }
    /* 索引里的变长整数也计入 CRC,所以要边读边算 —— 用一个包装把字节吃进来 */
#define XZ_IDX_GETC(dst)                                                      \
    do {                                                                      \
        int _c = 0;                                                           \
        const int _rc = xz_getc(xz, &_c);                                     \
        if (_rc != SXCL_XZ_OK) {                                              \
            return _rc;                                                       \
        }                                                                     \
        {                                                                     \
            const unsigned char _b = (unsigned char)_c;                        \
            crc = crc32_run(xz, crc, &_b, 1);                                 \
        }                                                                     \
        (dst) = (unsigned char)_c;                                            \
        ++pos;                                                                \
    } while (0)
    {
        uint64_t acc = 0;
        int shift = 0;
        int i2;
        for (i2 = 0; i2 < 9; ++i2) {
            unsigned char c = 0;
            XZ_IDX_GETC(c);
            acc |= (uint64_t)(c & 0x7Fu) << shift;
            if ((c & 0x80u) == 0) {
                break;
            }
            shift += 7;
        }
        count = acc;
    }
    if (count != (uint64_t)xz->block_count) {
        xz_failf(xz, SXCL_XZ_ERR_DATA, "索引里的块数 %llu 与解出来的 %llu 不符",
                 (unsigned long long)count, (unsigned long long)xz->block_count);
        return SXCL_XZ_ERR_DATA;
    }
    for (i = 0; i < (size_t)count; ++i) {
        uint64_t unpadded = 0, uncompressed = 0;
        int pass;
        for (pass = 0; pass < 2; ++pass) {
            uint64_t acc = 0;
            int shift = 0;
            int k;
            for (k = 0; k < 9; ++k) {
                unsigned char c = 0;
                XZ_IDX_GETC(c);
                acc |= (uint64_t)(c & 0x7Fu) << shift;
                if ((c & 0x80u) == 0) {
                    break;
                }
                shift += 7;
            }
            if (pass == 0) {
                unpadded = acc;
            } else {
                uncompressed = acc;
            }
        }
        if (unpadded != xz->blocks[i].unpadded || uncompressed != xz->blocks[i].uncompressed) {
            xz_failf(xz, SXCL_XZ_ERR_DATA,
                     "索引第 %u 条记录(未填充 %llu/解压 %llu)与实际块不符",
                     (unsigned)(i + 1), (unsigned long long)unpadded,
                     (unsigned long long)uncompressed);
            return SXCL_XZ_ERR_DATA;
        }
    }
    while ((pos % 4u) != 0u) {
        unsigned char c = 0;
        XZ_IDX_GETC(c);
        if (c != 0x00) {
            xz_fail(xz, SXCL_XZ_ERR_DATA, "索引填充字节非 0");
            return SXCL_XZ_ERR_DATA;
        }
    }
#undef XZ_IDX_GETC
    crc ^= 0xFFFFFFFFu;
    {
        unsigned char tail[4];
        int rc = xz_read_exact(xz, tail, 4);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
        if (le32(tail) != crc) {
            xz_fail(xz, SXCL_XZ_ERR_DATA, "索引 CRC32 不符");
            return SXCL_XZ_ERR_DATA;
        }
    }
    *index_size_out = pos + 4u;
    return SXCL_XZ_OK;
}

static int xz_run_one_stream(sxcl_xz *xz, int first)
{
    int rc = xz_read_stream_header(xz, first);
    if (rc != SXCL_XZ_OK) {
        return rc;
    }
    for (;;) {
        int peek = 0;
        const int prc = xz_try_getc(xz, &peek);
        if (prc == 1) {
            xz_fail(xz, SXCL_XZ_ERR_DATA, "块流提前结束(没等到索引)");
            return SXCL_XZ_ERR_DATA;
        }
        if (prc != 0) {
            return prc;
        }
        if (peek == 0x00) {
            /* 索引指示字节:这一个字节已经吃掉了,索引读取从它开始 —— 放回去 */
            --xz->in_pos;
            break;
        }
        --xz->in_pos;
        {
            uint32_t dict_size = 0;
            uint64_t hdr_size = 0, comp_size = 0, uncomp_size = 0;
            int has_comp = 0, has_uncomp = 0;
            uint64_t data_used = 0;
            uint64_t block_uncompressed = 0;
            uint64_t unpadded;
            rc = xz_read_block_header(xz, &dict_size, &hdr_size, &comp_size, &has_comp,
                                      &uncomp_size, &has_uncomp);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            if (dict_size == 0) {
                dict_size = (uint32_t)1 << 16; /* 属性 0 → 4 KiB,不当 0 处理 */
            }
            if (xz->opts.dict_limit > 0 && (int64_t)dict_size > xz->opts.dict_limit) {
                xz_failf(xz, SXCL_XZ_ERR_MEMLIMIT, "字典 %llu 字节超过上限 %lld 字节",
                         (unsigned long long)dict_size, (long long)xz->opts.dict_limit);
                return SXCL_XZ_ERR_MEMLIMIT;
            }
            if (xz->out == NULL || (size_t)dict_size > xz->dict_size) {
                unsigned char *nwin =
                    (unsigned char *)realloc(xz->out, (size_t)dict_size + XZ_WIN_SLACK);
                if (nwin == NULL) {
                    xz_fail(xz, SXCL_XZ_ERR_NOMEM, "输出窗口分配失败");
                    return SXCL_XZ_ERR_NOMEM;
                }
                xz->out = nwin;
                xz->out_cap = (size_t)dict_size + XZ_WIN_SLACK;
                xz->dict_size = dict_size;
            }
            xz->blk_crc32 = 0xFFFFFFFFu;
            xz->blk_crc64 = 0xFFFFFFFFFFFFFFFFull;
            {
                const int64_t block_out_start = xz->total_out;
                rc = lzma2_run(xz, &data_used);
                block_uncompressed = (uint64_t)(xz->total_out - block_out_start);
            }
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            /* 块填充:压缩数据补齐到 4 字节 */
            {
                size_t pad = (size_t)((4u - (data_used % 4u)) % 4u);
                size_t i;
                for (i = 0; i < pad; ++i) {
                    int c = 0;
                    rc = xz_getc(xz, &c);
                    if (rc != SXCL_XZ_OK) {
                        return rc;
                    }
                    if (c != 0x00) {
                        xz_fail(xz, SXCL_XZ_ERR_DATA, "块填充字节非 0");
                        return SXCL_XZ_ERR_DATA;
                    }
                }
            }
            /* 块校验 */
            if (xz->check_type == 1) {
                unsigned char tail[4];
                rc = xz_read_exact(xz, tail, 4);
                if (rc != SXCL_XZ_OK) {
                    return rc;
                }
                if (le32(tail) != (xz->blk_crc32 ^ 0xFFFFFFFFu)) {
                    xz_fail(xz, SXCL_XZ_ERR_DATA, "块 CRC32 不符(解出来的数据不对)");
                    return SXCL_XZ_ERR_DATA;
                }
            } else if (xz->check_type == 4) {
                unsigned char tail[8];
                rc = xz_read_exact(xz, tail, 8);
                if (rc != SXCL_XZ_OK) {
                    return rc;
                }
                if (le64(tail) != (xz->blk_crc64 ^ 0xFFFFFFFFFFFFFFFFull)) {
                    xz_fail(xz, SXCL_XZ_ERR_DATA, "块 CRC64 不符(解出来的数据不对)");
                    return SXCL_XZ_ERR_DATA;
                }
            }
            if (has_comp && comp_size != data_used) {
                xz_failf(xz, SXCL_XZ_ERR_DATA, "块头声明的压缩大小 %llu 与实际 %llu 不符",
                         (unsigned long long)comp_size, (unsigned long long)data_used);
                return SXCL_XZ_ERR_DATA;
            }
            unpadded = hdr_size + data_used +
                       (uint64_t)(xz->check_type == 0 ? 0 : (xz->check_type == 1 ? 4 : 8));
            if (has_uncomp && uncomp_size != block_uncompressed) {
                xz_failf(xz, SXCL_XZ_ERR_DATA, "块头声明的解压大小 %llu 与实际 %llu 不符",
                         (unsigned long long)uncomp_size,
                         (unsigned long long)block_uncompressed);
                return SXCL_XZ_ERR_DATA;
            }
            rc = block_rec_push(xz, unpadded, block_uncompressed);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
        }
    }
    {
        uint64_t index_size = 0;
        rc = xz_read_index(xz, &index_size);
        if (rc != SXCL_XZ_OK) {
            return rc;
        }
        {
            unsigned char foot[12];
            rc = xz_read_exact(xz, foot, 12);
            if (rc != SXCL_XZ_OK) {
                return rc;
            }
            if (foot[10] != 'Y' || foot[11] != 'Z') {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "流尾魔数不是 YZ");
                return SXCL_XZ_ERR_DATA;
            }
            /* 流尾的 CRC32 覆盖的是"Backward Size + Stream Flags"这 6 个字节(不含 CRC 自己) */
            if (crc32_of(xz, foot + 4, 6) != le32(foot)) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "流尾 CRC32 不符");
                return SXCL_XZ_ERR_DATA;
            }
            if (((uint64_t)le32(foot + 4) + 1u) * 4u != index_size) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "流尾的索引大小与实际不符");
                return SXCL_XZ_ERR_DATA;
            }
            if (foot[8] != 0x00 || foot[9] != (unsigned char)xz->check_type) {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "流尾的流标志与流头不一致");
                return SXCL_XZ_ERR_DATA;
            }
        }
    }
    return SXCL_XZ_OK;
}

/* 未压缩大小记账:每个块开始时记下 total_out,结束时算出差值 —— 用一个小栈变量挂在
 * sxcl_xz 上比传参更省事(单线程,一个句柄一次只解一个流)。 */

int sxcl_xz_run(sxcl_xz *xz, char *err, size_t err_len)
{
    int rc;
    if (xz == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "句柄为空");
        }
        return SXCL_XZ_ERR_ARG;
    }
    if (xz->finished) {
        rc = xz->result;
    } else {
        xz->block_count = 0;
        rc = xz_run_one_stream(xz, 1);
        while (rc == SXCL_XZ_OK) {
            /* 流之间的 4 字节对齐填充(0 个或多个 0x00 四字节组);后面可能还有一条流 */
            unsigned char quad[4];
            int i;
            int got = 0;
            for (i = 0; i < 4; ++i) {
                int c = 0;
                const int trc = xz_try_getc(xz, &c);
                if (trc == 1) {
                    got = 1;
                    break;
                }
                if (trc != 0) {
                    rc = trc;
                    break;
                }
                quad[i] = (unsigned char)c;
            }
            if (rc != SXCL_XZ_OK || got) {
                break;
            }
            while (quad[0] == 0 && quad[1] == 0 && quad[2] == 0 && quad[3] == 0) {
                int j;
                int done = 0;
                for (j = 0; j < 4; ++j) {
                    int c = 0;
                    const int trc = xz_try_getc(xz, &c);
                    if (trc == 1) {
                        done = 1;
                        break;
                    }
                    if (trc != 0) {
                        rc = trc;
                        break;
                    }
                    quad[j] = (unsigned char)c;
                }
                if (rc != SXCL_XZ_OK || done) {
                    break;
                }
            }
            if (rc != SXCL_XZ_OK) {
                break;
            }
            if (quad[0] != 0xFD || quad[1] != '7' || quad[2] != 'z' || quad[3] != 'X') {
                xz_fail(xz, SXCL_XZ_ERR_DATA, "流尾之后既不是填充也不是新流头");
                rc = SXCL_XZ_ERR_DATA;
                break;
            }
            /* 新的一条流:重新开始块记录 */
            xz->block_count = 0;
            rc = xz_run_one_stream(xz, 0);
        }
        if (rc == SXCL_XZ_OK) {
            rc = win_flush(xz, xz->out_pos);
            xz->out_pos = 0;
        }
        xz->result = rc;
        xz->finished = 1;
    }
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", xz->err_code == 0 ? "" : xz->err_text);
        err[err_len - 1] = '\0';
    }
    return rc;
}

/* ══════════════════════ 7) 开/关与便捷入口 ══════════════════════ */

const char *sxcl_xz_code_name(int code)
{
    switch (code) {
    case SXCL_XZ_OK:
        return "ok";
    case SXCL_XZ_ERR_DATA:
        return "data";
    case SXCL_XZ_ERR_UNSUPPORTED:
        return "unsupported";
    case SXCL_XZ_ERR_IO:
        return "io";
    case SXCL_XZ_ERR_ABORT:
        return "abort";
    case SXCL_XZ_ERR_NOMEM:
        return "nomem";
    case SXCL_XZ_ERR_ARG:
        return "arg";
    case SXCL_XZ_ERR_MEMLIMIT:
        return "memlimit";
    case SXCL_XZ_ERR_LIMIT:
        return "limit";
    default:
        return "?";
    }
}

sxcl_xz *sxcl_xz_open(const sxcl_xz_source *source, const sxcl_xz_opts *opts,
                      int (*sink)(void *ud, const void *data, size_t len), void *ud,
                      char *err, size_t err_len)
{
    sxcl_xz *xz;
    if (source == NULL || source->read == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "source 为空(read 没有)");
        }
        return NULL;
    }
    xz = (sxcl_xz *)calloc(1, sizeof(*xz));
    if (xz == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "内存不足");
        }
        return NULL;
    }
    xz->src = *source;
    xz->sink = sink;
    xz->sink_ud = ud;
    if (opts != NULL) {
        xz->opts = *opts;
    }
    if (xz->opts.dict_limit <= 0) {
        xz->opts.dict_limit = XZ_DEFAULT_DICT_LIMIT;
    }
    crc_tables_build(xz);
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return xz;
}

void sxcl_xz_close(sxcl_xz *xz)
{
    if (xz == NULL) {
        return;
    }
    free(xz->out);
    free(xz->lit);
    free(xz->blocks);
    free(xz);
}

int64_t sxcl_xz_out_bytes(const sxcl_xz *xz)
{
    return xz == NULL ? 0 : xz->total_out;
}

int sxcl_xz_check_type(const sxcl_xz *xz)
{
    return xz == NULL ? 0 : xz->check_type;
}

/* ── 内存输入源 ── */

typedef struct xz_mem_src {
    const unsigned char *data;
    size_t len;
    size_t pos;
} xz_mem_src;

static int64_t xz_mem_read(void *ud, void *buf, size_t len)
{
    xz_mem_src *m = (xz_mem_src *)ud;
    size_t n = m->len - m->pos;
    if (n > len) {
        n = len;
    }
    if (n > 0) {
        memcpy(buf, m->data + m->pos, n);
        m->pos += n;
    }
    return (int64_t)n;
}

/* ── 内存 sink ── */

typedef struct xz_mem_sink {
    unsigned char *data;
    size_t cap;
    size_t len;
    int overflow;
    int64_t needed;
} xz_mem_sink;

static int xz_mem_write(void *ud, const void *data, size_t len)
{
    xz_mem_sink *s = (xz_mem_sink *)ud;
    s->needed += (int64_t)len;
    if (s->data != NULL && s->len + len <= s->cap) {
        memcpy(s->data + s->len, data, len);
        s->len += len;
        return 0;
    }
    /* 记账但不收(让上层拿到"差多少"):返回非 0 会中止解码,所以只记第一个越界 */
    if (s->data != NULL) {
        s->overflow = 1;
        return 1;
    }
    s->len += len;
    return 0;
}

int sxcl_xz_decode_memory(const void *in, size_t in_len, void *out, size_t out_cap,
                          size_t *out_len, char *err, size_t err_len)
{
    sxcl_xz_source src;
    xz_mem_src mem;
    xz_mem_sink sink;
    sxcl_xz *xz;
    int rc;
    if (in == NULL || (out == NULL && out_cap != 0)) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "参数不合法");
        }
        return SXCL_XZ_ERR_ARG;
    }
    memset(&mem, 0, sizeof(mem));
    mem.data = (const unsigned char *)in;
    mem.len = in_len;
    src.read = xz_mem_read;
    src.ud = &mem;
    memset(&sink, 0, sizeof(sink));
    sink.data = (unsigned char *)out;
    sink.cap = out_cap;
    xz = sxcl_xz_open(&src, NULL, xz_mem_write, &sink, err, err_len);
    if (xz == NULL) {
        return SXCL_XZ_ERR_NOMEM;
    }
    rc = sxcl_xz_run(xz, err, err_len);
    if (rc == SXCL_XZ_ERR_ABORT && sink.overflow) {
        /* 缓冲不够不是"数据坏了":单独报,并把需要的字节数交出去 */
        rc = SXCL_XZ_ERR_LIMIT;
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "输出缓冲不够(需要 %lld 字节)",
                     (long long)sink.needed);
            err[err_len - 1] = '\0';
        }
    }
    if (out_len != NULL) {
        *out_len = sink.overflow ? (size_t)sink.needed : sink.len;
    }
    sxcl_xz_close(xz);
    return rc;
}

typedef struct xz_grow_sink {
    unsigned char *data;
    size_t len;
    size_t cap;
    int oom;
} xz_grow_sink;

static int xz_grow_write(void *ud, const void *data, size_t len)
{
    xz_grow_sink *s = (xz_grow_sink *)ud;
    if (s->len + len > s->cap) {
        size_t cap = s->cap == 0 ? 65536u : s->cap;
        unsigned char *p;
        while (cap < s->len + len) {
            cap *= 2u;
        }
        p = (unsigned char *)realloc(s->data, cap);
        if (p == NULL) {
            s->oom = 1;
            return 1;
        }
        s->data = p;
        s->cap = cap;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return 0;
}

int sxcl_xz_decode_alloc(const void *in, size_t in_len, unsigned char **out, size_t *out_len,
                         char *err, size_t err_len)
{
    sxcl_xz_source src;
    xz_mem_src mem;
    xz_grow_sink sink;
    sxcl_xz *xz;
    int rc;
    if (in == NULL || out == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "参数不合法");
        }
        return SXCL_XZ_ERR_ARG;
    }
    *out = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }
    memset(&mem, 0, sizeof(mem));
    mem.data = (const unsigned char *)in;
    mem.len = in_len;
    src.read = xz_mem_read;
    src.ud = &mem;
    memset(&sink, 0, sizeof(sink));
    xz = sxcl_xz_open(&src, NULL, xz_grow_write, &sink, err, err_len);
    if (xz == NULL) {
        return SXCL_XZ_ERR_NOMEM;
    }
    rc = sxcl_xz_run(xz, err, err_len);
    sxcl_xz_close(xz);
    if (rc != SXCL_XZ_OK) {
        if (sink.oom && rc == SXCL_XZ_ERR_ABORT) {
            rc = SXCL_XZ_ERR_NOMEM;
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "内存不足");
            }
        }
        free(sink.data);
        return rc;
    }
    *out = sink.data;
    if (out_len != NULL) {
        *out_len = sink.len;
    }
    return SXCL_XZ_OK;
}
