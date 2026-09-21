/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/inflate.h"

#include <stdlib.h>
#include <string.h>

/* ── 常量 ── */

#define INF_WINDOW   ((size_t)32768u)         /* 窗口大小,也是最大匹配距离 */
#define INF_WMASK    ((size_t)32767u)         /* 窗口大小 - 1(2 的幂,回绕用 & ) */
#define INF_MAXBITS  15u                      /* DEFLATE 的码长上限 */
#define INF_LITMAX   288u                     /* 字面量/长度字母表符号数 */
#define INF_DISTMAX  32u                      /* 距离字母表符号数(实际用 30) */
#define INF_CLMAX    19u                      /* 码长字母表符号数 */
#define INF_LENS_MAX (INF_LITMAX + INF_DISTMAX)
#define INF_FLUSH_AT ((size_t)16384u)         /* 未交付水位到多少就先交给 sink */

#define INF_NEED     1                        /* inf_run:需要更多输入 */
#define INF_ERR     (-1)                      /* inf_run:数据损坏 */

/* ── 规范 Huffman 表 ── */

typedef struct inf_huff {
    unsigned counts[INF_MAXBITS + 1];       /* counts[len] = 码长为 len 的符号个数 */
    unsigned first_code[INF_MAXBITS + 1];   /* 该码长的第一个规范码 */
    unsigned first_index[INF_MAXBITS + 1];  /* symbols[] 中该码长的起始下标 */
    unsigned symbols[INF_LITMAX];           /* 按 (码长, 符号值) 排序的符号表 */
} inf_huff;

/* ── 状态机 ── */

enum inf_state {
    INF_ST_BLOCK = 0,   /* 读块头(BFINAL + BTYPE) */
    INF_ST_STORED,      /* stored 块:LEN/NLEN + 原样拷贝 */
    INF_ST_DYN_HDR,     /* 动态块:HLIT/HDIST/HCLEN */
    INF_ST_DYN_CL,      /* 动态块:码长表(码长码 + 16/17/18 重复) */
    INF_ST_SYMBOL,      /* Huffman 数据:字面量/长度符号 */
    INF_ST_LEN_EXTRA,   /* 长度附加位 */
    INF_ST_DIST_SYM,    /* 距离符号 */
    INF_ST_DIST_EXTRA,  /* 距离附加位 */
    INF_ST_DONE         /* 整条流结束 */
};

/* 位读取状态快照:符号解到一半发现输入不够时整体回滚,不丢位 */
typedef struct inf_bits_state {
    size_t in_pos;
    uint32_t bitbuf;
    unsigned bitcnt;
} inf_bits_state;

struct sxcl_inflate {
    /* 位读取器(每段 feed 重置 in/in_len/in_pos,bitbuf/bitcnt 跨段保留) */
    const unsigned char *in;
    size_t in_len;
    size_t in_pos;
    uint32_t bitbuf;
    unsigned bitcnt;

    /* 输出:32 KiB 环形窗口 + 交付水位 */
    unsigned char win[INF_WINDOW];
    size_t win_pos;          /* 下一个写入位置 [0, 32768) */
    uint64_t total_out;      /* 本流累计产出字节数(距离合法性判据) */
    uint64_t flushed;        /* 已经交给 sink 的字节数 */

    int (*sink)(void *ud, const void *data, size_t len);
    void *ud;

    /* Huffman 表 + 码长临时表 */
    inf_huff lit;
    inf_huff dist;
    inf_huff cl;
    unsigned char cl_lens[INF_CLMAX];
    unsigned char lens[INF_LENS_MAX];

    /* 状态机 */
    int state;
    int error;               /* 出错后粘住,后续调用直接返回它 */
    int done;
    int bfinal;

    /* stored 块 */
    unsigned char hdr[4];    /* LEN(2) + NLEN(2),逐字节读,读到一半可中断 */
    unsigned hdr_n;
    int stored_hdr;          /* LEN/NLEN 是否已经读全 */
    unsigned stored_len;
    unsigned stored_left;    /* 还没拷贝的字节数 */

    /* 动态块头 */
    unsigned hlit;
    unsigned hdist;
    unsigned hclen;          /* 码长码的个数(4..19) */
    unsigned total_syms;     /* hlit + hdist,共要读多少个码长 */
    unsigned idx;
    int cl_done;

    /* 长度 / 距离 */
    unsigned len;
    unsigned len_sym;
};

/* ── RFC 1951 定长表 ── */

/* 长度码 257..285 的基准值与附加位数(下标 = 长度码 - 257)。 */
static const unsigned short inf_len_base[29] = {
    3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 15u, 17u, 19u, 23u, 27u,
    31u, 35u, 43u, 51u, 59u, 67u, 83u, 99u, 115u, 131u, 163u, 195u, 227u, 258u
};
static const unsigned char inf_len_extra[29] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u, 2u,
    2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u
};

/* 距离码 0..29 的基准值与附加位数。 */
static const unsigned short inf_dist_base[30] = {
    1u, 2u, 3u, 4u, 5u, 7u, 9u, 13u, 17u, 25u, 33u, 49u, 65u, 97u, 129u,
    193u, 257u, 385u, 513u, 769u, 1025u, 1537u, 2049u, 3073u, 4097u,
    6145u, 8193u, 12289u, 16385u, 24577u
};
static const unsigned char inf_dist_extra[30] = {
    0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u,
    6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u
};

/* 动态块里 19 个码长码的存储顺序(RFC 1951 3.2.7)。 */
static const unsigned char inf_clorder[INF_CLMAX] = {
    16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u
};

/* ── 位读取 ── */

/* 幂等:丢掉当前字节里剩下的零头位。stored 块头之前必须对齐到字节。 */
static void inf_align(sxcl_inflate *inf)
{
    const unsigned drop = inf->bitcnt & 7u;
    inf->bitbuf >>= drop;
    inf->bitcnt -= drop;
}

/* 保证累加器里至少有 n 位(n <= 24)。
 * 返回 0 = 够了;1 = 本段输入用完(位状态未被破坏,可以再 feed)。
 * 注意:成功返回时位可能来自"本段输入",跨段时位留在 bitbuf 里,不会丢。 */
static int inf_fill(sxcl_inflate *inf, unsigned n)
{
    while (inf->bitcnt < n) {
        if (inf->in_pos >= inf->in_len) {
            return INF_NEED;
        }
        inf->bitbuf |= (uint32_t)inf->in[inf->in_pos] << inf->bitcnt;
        ++inf->in_pos;
        inf->bitcnt += 8u;
    }
    return 0;
}

/* 取 n 位(调用前必须 inf_fill 成功,n <= 24)。 */
static uint32_t inf_take(sxcl_inflate *inf, unsigned n)
{
    const uint32_t mask = (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    const uint32_t v = inf->bitbuf & mask;

    inf->bitbuf >>= n;
    inf->bitcnt -= n;
    return v;
}

/* 解符号之前尽量把累加器攒满(24 位),或者一直攒到输入正好用完。
 *
 * 这一步是"分片喂数据不丢字节"的关键:feed 的契约是"整段消费掉",不会把没用上的
 * 尾巴还回去。符号解到一半发现位不够时要回滚位状态,如果这中间曾经从输入里拉过
 * 新字节,回滚就会把那些字节一起丢掉(调用方已经往前走了,不会再喂一遍)。
 * 攒满之后,一次符号解码(<= 15 位 + 附加位)绝不会再去拉新字节,于是
 * "输入没用完"时回滚是纯粹的位回滚;"输入正好用完"时 in_pos 本来就没动过。 */
static void inf_prefill(sxcl_inflate *inf)
{
    (void)inf_fill(inf, 24u);
}

static void inf_bits_save(const sxcl_inflate *inf, inf_bits_state *st)
{
    st->in_pos = inf->in_pos;
    st->bitbuf = inf->bitbuf;
    st->bitcnt = inf->bitcnt;
}

static void inf_bits_restore(sxcl_inflate *inf, const inf_bits_state *st)
{
    inf->in_pos = st->in_pos;
    inf->bitbuf = st->bitbuf;
    inf->bitcnt = st->bitcnt;
}

/* ── Huffman 表 ── */

/* 从码长表建规范 Huffman 表。
 * 返回 0 成功;-1 = 超额(Kraft 和 > 1,不可能由合法压缩数据产生)。 */
static int inf_huff_build(inf_huff *h, const unsigned char *lens, unsigned n)
{
    unsigned offs[INF_MAXBITS + 1];
    unsigned i;
    unsigned len;
    unsigned left;
    unsigned code;

    for (len = 0u; len <= INF_MAXBITS; ++len) {
        h->counts[len] = 0u;
    }
    for (i = 0u; i < n; ++i) {
        if ((unsigned)lens[i] > INF_MAXBITS) {
            return -1;
        }
        h->counts[lens[i]] += 1u;
    }

    /* 超额校验。left > 0(不完整)不报错:单符号距离树、空表都合法,
     * 真解到不存在的码时会由 inf_decode_sym 判损坏。 */
    left = 1u;
    for (len = 1u; len <= INF_MAXBITS; ++len) {
        left <<= 1;
        if (h->counts[len] > left) {
            return -1;
        }
        left -= h->counts[len];
    }

    /* 符号按 (码长, 符号值) 排序:同一码长内符号值升序即是规范序。 */
    offs[1] = 0u;
    for (len = 1u; len < INF_MAXBITS; ++len) {
        offs[len + 1u] = offs[len] + h->counts[len];
    }
    for (i = 0u; i < n; ++i) {
        if (lens[i] != 0u) {
            h->symbols[offs[lens[i]]] = i;
            offs[lens[i]] += 1u;
        }
    }

    code = 0u;
    h->first_index[1] = 0u;
    for (len = 1u; len <= INF_MAXBITS; ++len) {
        h->first_code[len] = code;
        if (len > 1u) {
            h->first_index[len] = h->first_index[len - 1u] + h->counts[len - 1u];
        }
        code = (code + h->counts[len]) << 1;
    }
    return 0;
}

/* 逐位解一个符号。
 * 返回 0 = *sym 有效;1 = 输入不够(位状态已被本函数消费掉一部分,调用方先快照);
 *     -1 = 非法码(该码长下没有这个码,或 15 位都不是合法码)。 */
static int inf_decode_sym(sxcl_inflate *inf, const inf_huff *h, unsigned *sym)
{
    unsigned code = 0u;
    unsigned first = 0u;
    unsigned index = 0u;
    unsigned len;

    for (len = 1u; len <= INF_MAXBITS; ++len) {
        if (inf_fill(inf, 1u) != 0) {
            return INF_NEED;
        }
        code |= inf_take(inf, 1u);
        if (code >= first && (code - first) < h->counts[len]) {
            *sym = h->symbols[index + (code - first)];
            return 0;
        }
        index += h->counts[len];
        first = (first + h->counts[len]) << 1;
        code <<= 1;
    }
    return INF_ERR;
}

/* 建固定的字面量/长度树与距离树(RFC 1951 3.2.6)。 */
static int inf_fixed_tables(sxcl_inflate *inf)
{
    unsigned i;

    for (i = 0u; i < 144u; ++i) {
        inf->lens[i] = 8u;
    }
    for (; i < 256u; ++i) {
        inf->lens[i] = 9u;
    }
    for (; i < 280u; ++i) {
        inf->lens[i] = 7u;
    }
    for (; i < 288u; ++i) {
        inf->lens[i] = 8u;
    }
    if (inf_huff_build(&inf->lit, inf->lens, INF_LITMAX) != 0) {
        return INF_ERR;
    }
    for (i = 0u; i < 32u; ++i) {
        inf->lens[i] = 5u;
    }
    if (inf_huff_build(&inf->dist, inf->lens, INF_DISTMAX) != 0) {
        return INF_ERR;
    }
    return 0;
}

/* ── 输出 ── */

/* 把窗口里还没交付的数据交给 sink。all = 0 只在水位够高时交,all = 1 全交。 */
static int inf_flush(sxcl_inflate *inf, int all)
{
    while (inf->total_out > inf->flushed) {
        const uint64_t pending = inf->total_out - inf->flushed;
        size_t start;
        size_t run;

        if (all == 0 && pending < (uint64_t)INF_FLUSH_AT) {
            break;
        }
        start = (size_t)(inf->flushed & (uint64_t)INF_WMASK);
        run = (size_t)pending;
        if (run > INF_WINDOW - start) {
            run = INF_WINDOW - start;   /* 环形窗口一次只能连续交付到回绕点 */
        }
        if (inf->sink != NULL && inf->sink(inf->ud, inf->win + start, run) != 0) {
            return SXCL_INFLATE_ERR_ABORT;
        }
        inf->flushed += (uint64_t)run;
    }
    return SXCL_INFLATE_OK;
}

/* 水位到阈值就交付一次,保证窗口里未交付的数据永远 < 24 KiB(不会被回绕覆盖)。 */
static int inf_flush_if_needed(sxcl_inflate *inf)
{
    if (inf->total_out - inf->flushed >= (uint64_t)INF_FLUSH_AT) {
        return inf_flush(inf, 0);
    }
    return SXCL_INFLATE_OK;
}

static void inf_put_byte(sxcl_inflate *inf, unsigned char b)
{
    inf->win[inf->win_pos] = b;
    inf->win_pos = (inf->win_pos + 1u) & INF_WMASK;
    inf->total_out += 1u;
}

/* 从输入直接搬 n 字节进窗口(stored 块用,n <= 窗口大小)。 */
static void inf_copy_in(sxcl_inflate *inf, const unsigned char *src, size_t n)
{
    const size_t first = INF_WINDOW - inf->win_pos;
    if (n <= first) {
        memcpy(inf->win + inf->win_pos, src, n);
    } else {
        memcpy(inf->win + inf->win_pos, src, first);
        memcpy(inf->win, src + first, n - first);
    }
    inf->win_pos = (inf->win_pos + n) & INF_WMASK;
    inf->total_out += (uint64_t)n;
}

/* 拷贝一个匹配:从窗口回溯 dist 字节处起复制 len 字节。
 * 逐字节拷贝,dist < len 时后面的字节能读到本次刚写入的内容(重叠匹配)。 */
static void inf_copy_match(sxcl_inflate *inf, unsigned len, unsigned dist)
{
    size_t src = (inf->win_pos - (size_t)dist) & INF_WMASK;
    unsigned i;

    for (i = 0u; i < len; ++i) {
        inf->win[inf->win_pos] = inf->win[src];
        src = (src + 1u) & INF_WMASK;
        inf->win_pos = (inf->win_pos + 1u) & INF_WMASK;
    }
    inf->total_out += (uint64_t)len;
}

/* 块结束:BFINAL 置位则整条流结束,否则回到块头。返回 1 表示整条流结束。 */
static int inf_end_block(sxcl_inflate *inf)
{
    if (inf->bfinal != 0) {
        inf->done = 1;
        inf->state = INF_ST_DONE;
        return 1;
    }
    inf->state = INF_ST_BLOCK;
    return 0;
}

/* ── 动态块:码长表 ── */

/* 读一段码长表(可重入:每读一个单位就更新 idx,输入不够就原样返回)。 */
static int inf_read_lens(sxcl_inflate *inf)
{
    while (inf->idx < inf->total_syms) {
        inf_bits_state save;
        unsigned sym = 0u;
        unsigned rep;
        unsigned extra = 0u;
        unsigned fill_val = 0u;
        int rc;

        inf_prefill(inf);
        inf_bits_save(inf, &save);
        rc = inf_decode_sym(inf, &inf->cl, &sym);
        if (rc == INF_NEED) {
            inf_bits_restore(inf, &save);
            return INF_NEED;
        }
        if (rc < 0) {
            return INF_ERR;
        }

        if (sym < 16u) {
            inf->lens[inf->idx] = (unsigned char)sym;
            inf->idx += 1u;
            continue;
        }

        if (sym == 16u) {
            if (inf->idx == 0u) {
                return INF_ERR;      /* 没有"上一个码长"可重复 */
            }
            rep = 3u;
            extra = 2u;
            fill_val = inf->lens[inf->idx - 1u];   /* 16 = 重复上一个码长 */
        } else if (sym == 17u) {
            rep = 3u;
            extra = 3u;
            fill_val = 0u;
        } else if (sym == 18u) {
            rep = 11u;
            extra = 7u;
            fill_val = 0u;
        } else {
            return INF_ERR;          /* 码长码只到 18 */
        }

        if (inf_fill(inf, extra) != 0) {
            inf_bits_restore(inf, &save);
            return INF_NEED;
        }
        rep += inf_take(inf, extra);
        if (rep > inf->total_syms - inf->idx) {
            return INF_ERR;          /* 重复次数越过码长表末尾 */
        }
        while (rep > 0u) {
            inf->lens[inf->idx] = (unsigned char)fill_val;
            inf->idx += 1u;
            rep -= 1u;
        }
    }
    return 0;
}

/* ── 主状态机 ── */

/* 返回 0 = 整条流解完(inf->done);1 = 需要更多输入;-1 = 损坏;-3 = sink 中止。 */
static int inf_run(sxcl_inflate *inf)
{
    for (;;) {
        if (inf->state == INF_ST_DONE) {
            return 0;
        }

        switch (inf->state) {
        case INF_ST_BLOCK: {
            unsigned btype;
            uint32_t v;

            if (inf_fill(inf, 3u) != 0) {
                return INF_NEED;
            }
            v = inf_take(inf, 3u);
            inf->bfinal = (int)(v & 1u);
            btype = (v >> 1) & 3u;
            if (btype == 0u) {
                inf->state = INF_ST_STORED;
                inf->stored_hdr = 0;
                inf->hdr_n = 0u;
                inf->stored_left = 0u;
            } else if (btype == 1u) {
                if (inf_fixed_tables(inf) != 0) {
                    return INF_ERR;
                }
                inf->state = INF_ST_SYMBOL;
            } else if (btype == 2u) {
                inf->state = INF_ST_DYN_HDR;
            } else {
                return INF_ERR;      /* BTYPE = 11 保留值 */
            }
            break;
        }

        case INF_ST_STORED: {
            if (inf->stored_hdr == 0) {
                inf_align(inf);          /* 幂等:LEN 之前必须字节对齐 */
                while (inf->hdr_n < 4u) {
                    if (inf_fill(inf, 8u) != 0) {
                        return INF_NEED; /* 位还在累加器里,下次接着读 */
                    }
                    inf->hdr[inf->hdr_n] = (unsigned char)inf_take(inf, 8u);
                    inf->hdr_n += 1u;
                }
                inf->stored_len = (unsigned)inf->hdr[0] | ((unsigned)inf->hdr[1] << 8);
                {
                    const unsigned nlen = (unsigned)inf->hdr[2] | ((unsigned)inf->hdr[3] << 8);
                    if (((inf->stored_len ^ 0xFFFFu) & 0xFFFFu) != nlen) {
                        return INF_ERR;  /* LEN 与 NLEN 不互补 */
                    }
                }
                inf->stored_left = inf->stored_len;
                inf->stored_hdr = 1;
                /* 对齐前可能多读了整字节,那属于块数据,先吐出来 */
                while (inf->bitcnt >= 8u && inf->stored_left > 0u) {
                    inf_put_byte(inf, (unsigned char)inf_take(inf, 8u));
                    inf->stored_left -= 1u;
                }
            }

            if (inf->stored_left == 0u) {
                if (inf_end_block(inf) != 0) {
                    return 0;
                }
                break;                   /* 空块 / 块尾:回块头 */
            }

            {
                const size_t avail = inf->in_len - inf->in_pos;
                size_t take = ((size_t)inf->stored_left < avail) ? (size_t)inf->stored_left : avail;
                if (take > (size_t)8192u) {
                    take = (size_t)8192u;   /* 分块搬,保证交付水位不越界 */
                }
                if (take == 0u) {
                    return INF_NEED;
                }
                inf_copy_in(inf, inf->in + inf->in_pos, take);
                inf->in_pos += take;
                inf->stored_left -= (unsigned)take;
            }
            break;
        }

        case INF_ST_DYN_HDR: {
            uint32_t v;
            if (inf_fill(inf, 14u) != 0) {
                return INF_NEED;
            }
            v = inf_take(inf, 14u);
            inf->hlit = 257u + (v & 31u);
            inf->hdist = 1u + ((v >> 5) & 31u);
            if (inf->hlit > 286u || inf->hdist > 30u) {
                return INF_ERR;          /* HLIT/HDIST 越界:287/288 与 31/32 都不合法 */
            }
            /* 只读 hclen 个 3 位码长,剩下 (19 - hclen) 个按协议恒为 0,不进码流。 */
            inf->hclen = 4u + ((v >> 10) & 15u);
            inf->total_syms = inf->hlit + inf->hdist;
            inf->idx = 0u;
            inf->cl_done = 0;
            memset(inf->cl_lens, 0, sizeof inf->cl_lens);
            inf->state = INF_ST_DYN_CL;
            break;
        }

        case INF_ST_DYN_CL: {
            if (inf->cl_done == 0) {
                while (inf->idx < inf->hclen) {
                    if (inf_fill(inf, 3u) != 0) {
                        return INF_NEED;
                    }
                    inf->cl_lens[inf_clorder[inf->idx]] = (unsigned char)inf_take(inf, 3u);
                    inf->idx += 1u;
                }
                if (inf_huff_build(&inf->cl, inf->cl_lens, INF_CLMAX) != 0) {
                    return INF_ERR;
                }
                inf->cl_done = 1;
                inf->idx = 0u;
                break;
            }
            {
                const int rc = inf_read_lens(inf);
                if (rc == INF_NEED) {
                    return INF_NEED;
                }
                if (rc < 0) {
                    return INF_ERR;
                }
            }
            if (inf_huff_build(&inf->lit, inf->lens, inf->hlit) != 0) {
                return INF_ERR;
            }
            if (inf_huff_build(&inf->dist, inf->lens + inf->hlit, inf->hdist) != 0) {
                return INF_ERR;
            }
            inf->state = INF_ST_SYMBOL;
            break;
        }

        case INF_ST_SYMBOL: {
            inf_bits_state save;
            unsigned sym = 0u;
            int rc;

            inf_prefill(inf);
            inf_bits_save(inf, &save);
            rc = inf_decode_sym(inf, &inf->lit, &sym);
            if (rc == INF_NEED) {
                inf_bits_restore(inf, &save);   /* 符号没解完:位状态回滚,下次重来 */
                return INF_NEED;
            }
            if (rc < 0) {
                return INF_ERR;
            }

            if (sym < 256u) {
                inf_put_byte(inf, (unsigned char)sym);
            } else if (sym == 256u) {
                if (inf_end_block(inf) != 0) {
                    return 0;
                }
            } else if (sym <= 285u) {
                inf->len_sym = sym;
                inf->state = INF_ST_LEN_EXTRA;
            } else {
                return INF_ERR;                 /* 286/287 是无定义的长度码 */
            }
            break;
        }

        case INF_ST_LEN_EXTRA: {
            const unsigned idx = inf->len_sym - 257u;
            const unsigned extra = inf_len_extra[idx];

            if (inf_fill(inf, extra) != 0) {
                return INF_NEED;                /* 附加位没动过,直接重试 */
            }
            inf->len = (unsigned)inf_len_base[idx] + inf_take(inf, extra);
            inf->state = INF_ST_DIST_SYM;
            break;
        }

        case INF_ST_DIST_SYM: {
            inf_bits_state save;
            unsigned sym = 0u;
            int rc;

            inf_prefill(inf);
            inf_bits_save(inf, &save);
            rc = inf_decode_sym(inf, &inf->dist, &sym);
            if (rc == INF_NEED) {
                inf_bits_restore(inf, &save);
                return INF_NEED;
            }
            if (rc < 0 || sym > 29u) {
                return INF_ERR;                 /* 空距离树 / 30、31 无定义 */
            }
            inf->len_sym = sym;                 /* 借用同一字段存距离码 */
            inf->state = INF_ST_DIST_EXTRA;
            break;
        }

        case INF_ST_DIST_EXTRA: {
            const unsigned sym = inf->len_sym;
            const unsigned extra = inf_dist_extra[sym];
            unsigned dist;

            if (inf_fill(inf, extra) != 0) {
                return INF_NEED;
            }
            dist = (unsigned)inf_dist_base[sym] + inf_take(inf, extra);
            if ((uint64_t)dist > inf->total_out) {
                return INF_ERR;                 /* 距离超出已输出长度:不许越界回溯 */
            }
            inf_copy_match(inf, inf->len, dist);
            inf->len = 0u;
            inf->state = INF_ST_SYMBOL;
            break;
        }

        default:
            return INF_ERR;
        }

        {
            const int fr = inf_flush_if_needed(inf);
            if (fr < 0) {
                return fr;
            }
        }
    }
}

/* ── 对外接口 ── */

sxcl_inflate *sxcl_inflate_open(void)
{
    sxcl_inflate *inf = (sxcl_inflate *)malloc(sizeof(sxcl_inflate));
    if (inf == NULL) {
        return NULL;
    }
    memset(inf, 0, sizeof *inf);
    inf->state = INF_ST_BLOCK;
    return inf;
}

void sxcl_inflate_close(sxcl_inflate *inf)
{
    free(inf);
}

int sxcl_inflate_feed(sxcl_inflate *inf, const void *in, size_t in_len,
                      int (*sink)(void *ud, const void *data, size_t len), void *ud)
{
    int rc;

    if (inf == NULL) {
        return SXCL_INFLATE_ERR_DATA;
    }
    if (inf->error != 0) {
        return inf->error;                  /* 出错后粘住 */
    }
    if (inf->done != 0) {
        return 1;                           /* 流已经结束,后续数据忽略 */
    }
    if (in == NULL && in_len != 0u) {
        inf->error = SXCL_INFLATE_ERR_DATA;
        return inf->error;
    }

    inf->in = (const unsigned char *)in;
    inf->in_len = in_len;
    inf->in_pos = 0u;
    inf->sink = sink;
    inf->ud = ud;

    rc = inf_run(inf);
    if (rc >= 0) {
        const int fr = inf_flush(inf, 1);    /* 本次解出来的全部交付 */
        if (fr < 0) {
            rc = fr;
        }
    }
    inf->sink = NULL;
    inf->ud = NULL;
    inf->in = NULL;
    inf->in_len = 0u;
    inf->in_pos = 0u;

    if (rc < 0) {
        inf->error = rc;
        return rc;
    }
    return (inf->done != 0) ? 1 : 0;
}

int sxcl_inflate_finish(sxcl_inflate *inf)
{
    if (inf == NULL) {
        return SXCL_INFLATE_ERR_DATA;
    }
    if (inf->error != 0) {
        return inf->error;
    }
    /* feed 每次都把能解的位都解了:这里还没 done 就只能是数据被截断 */
    return (inf->done != 0) ? SXCL_INFLATE_OK : SXCL_INFLATE_ERR_DATA;
}

/* ── 一次性解压 ── */

typedef struct inf_raw_sink {
    unsigned char *out;     /* 允许 NULL(只数长度) */
    size_t cap;
    size_t len;             /* 实际解出来多少字节(= 需要多少字节) */
} inf_raw_sink;

static int inf_raw_write(void *ud, const void *data, size_t len)
{
    inf_raw_sink *s = (inf_raw_sink *)ud;

    if (s->out != NULL && s->len < s->cap) {
        const size_t room = s->cap - s->len;
        const size_t n = (len < room) ? len : room;
        memcpy(s->out + s->len, data, n);
    }
    s->len += len;          /* 缓冲不够也继续数完整长度,交给调用方扩容 */
    return 0;
}

int sxcl_inflate_raw(const void *in, size_t in_len, void *out, size_t out_cap, size_t *out_len)
{
    inf_raw_sink s;
    sxcl_inflate *inf;
    int result;
    int rc;

    if (in == NULL || (out == NULL && out_cap != 0u)) {
        if (out_len != NULL) {
            *out_len = 0u;
        }
        return SXCL_INFLATE_ERR_DATA;
    }

    s.out = (unsigned char *)out;
    s.cap = out_cap;
    s.len = 0u;

    inf = sxcl_inflate_open();
    if (inf == NULL) {
        if (out_len != NULL) {
            *out_len = 0u;
        }
        return SXCL_INFLATE_ERR_DATA;
    }

    rc = sxcl_inflate_feed(inf, in, in_len, inf_raw_write, &s);
    if (rc < 0) {
        result = rc;                                  /* -1 损坏 / -3 中止 */
    } else if (sxcl_inflate_finish(inf) != SXCL_INFLATE_OK) {
        result = SXCL_INFLATE_ERR_DATA;               /* 截断 */
    } else if (s.len > out_cap) {
        result = SXCL_INFLATE_ERR_SPACE;              /* 缓冲不够,长度已写在 out_len */
    } else {
        result = SXCL_INFLATE_OK;
    }

    sxcl_inflate_close(inf);
    if (out_len != NULL) {
        *out_len = s.len;
    }
    return result;
}
