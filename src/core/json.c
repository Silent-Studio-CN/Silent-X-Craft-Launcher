/* SXCL-C JSON 解析器实现 —— 零第三方依赖、纯 C11、四平台可移植。
 *
 * 设计要点(与 include/sxcl/json.h 的约定一致):
 *   1) 一次性解析成 DOM。节点、成员表、字符串全部放在"文档私有 arena"里,
 *      按 64 KiB 起步、最大 8 MiB 的块 bump 分配 —— 每个节点不再单独 malloc,
 *      整份文档 sxcl_json_free 一次释放(块链表头插,释放时遍历)。
 *   2) 字符串在 arena 内就地解码并 NUL 结尾:转义 \" \\ \/ \b \f \n \r \t \uXXXX
 *      全部解码;代理对 \uD83D\uDE00 合成码点后按 UTF-8 四字节写出;非 UTF-8 字节
 *      原样透传(不重编码,中文因此"原样透传")。非法转义一律报错,绝不猜。
 *      解码只会变短,所以先按原始长度预留、解码完再把没用到的尾部还给 arena。
 *   3) 数字按 JSON 严格文法扫描(-?int frac? exp?),拒绝前导零 / .5 / 1. / +1;
 *      用整数尾数 + 十进制指数自己换算成 double,不依赖 strtod(不受 locale 影响),
 *      超长数字只保留前 18 位有效数字,不会溢出 UB;上溢得 ±inf,下溢得 0。
 *   4) 严格模式:拒绝尾随逗号、单引号字符串、注释、NaN/Infinity、裸控制字符、
 *      根值之后的多余内容;对象重复键以最后一次出现的值为准(查找反向扫描 +
 *      小对象原地去重)。
 *   5) 错误信息带 1 基行号/列号(列按 UTF-8 码点数,中文不会一列顶三列),
 *      例如:第 12 行第 34 列: 期望 ',' 或 '}'
 *   6) 嵌套深度上限 64 层,防恶意元数据爆栈;解析器状态全部在栈上的 json_parser,
 *      没有任何全局可变状态,可重入、线程安全(每个文档互相独立)。
 */
#include "sxcl/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* 仅为了用 _wfopen 打开 UTF-8 路径(与 fs.h 的约定一致) */
#endif

/* ── 可调参数 ── */
#define SXCL_JSON_MAX_DEPTH 64                        /* 最大嵌套层数 */
#define SXCL_JSON_BLOCK_MIN (64u * 1024u)             /* arena 首块 */
#define SXCL_JSON_BLOCK_MAX (8u * 1024u * 1024u)      /* arena 块上限 */
#define SXCL_JSON_ARENA_ALIGN 8u                      /* 8 字节对齐(double/指针都够) */
#define SXCL_JSON_DEDUP_MAX 512u                      /* 只对这么多成员的对象的重复键做原地去重 */

/* ── 数据结构 ── */

typedef struct sxcl_json_member {
    const char *key;                /* arena 内,NUL 结尾,转义已解码 */
    size_t key_len;                 /* 解码后长度(键里可能有 \u0000) */
    const sxcl_json_value *value;
} sxcl_json_member;

struct sxcl_json_value {
    sxcl_json_type type;
    union {
        int boolean;
        double number;
        struct {
            const char *ptr;
            size_t len;
        } string;
        struct {
            const sxcl_json_value **items;
            size_t count;
        } array;
        struct {
            const sxcl_json_member *members;
            size_t count;
        } object;
    } u;
};

/* arena 块:整块 malloc,块内 bump 分配,链表串起来统一释放。 */
typedef struct sxcl_json_block {
    struct sxcl_json_block *next;
    size_t cap;
    size_t used;
    unsigned char data[1];
} sxcl_json_block;

struct sxcl_json {
    sxcl_json_block *blocks;        /* 全部块(头插) */
    sxcl_json_block *cur;           /* 当前 bump 块 */
    size_t next_cap;                /* 下一个新块的容量(逐块翻倍,封顶) */
    const sxcl_json_value *root;
};

/* ── arena ── */

static sxcl_json_block *block_new(size_t cap)
{
    sxcl_json_block *b = (sxcl_json_block *)malloc(sizeof(sxcl_json_block) + cap);
    if (!b) {
        return NULL;
    }
    b->next = NULL;
    b->cap = cap;
    b->used = 0;
    return b;
}

/* 分配 8 字节对齐的一块内存;失败返回 NULL。 */
static void *arena_alloc(sxcl_json *doc, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    size = (size + (SXCL_JSON_ARENA_ALIGN - 1u)) & ~(size_t)(SXCL_JSON_ARENA_ALIGN - 1u);

    sxcl_json_block *b = doc->cur;
    if (!b || b->cap - b->used < size) {
        size_t cap = doc->next_cap;
        if (cap < size) {
            cap = size;             /* 单个大对象(如巨大数组)独占一块 */
        }
        if (doc->next_cap < SXCL_JSON_BLOCK_MAX) {
            doc->next_cap = (doc->next_cap > SXCL_JSON_BLOCK_MAX / 2u) ? SXCL_JSON_BLOCK_MAX
                                                                      : doc->next_cap * 2u;
        }
        b = block_new(cap);
        if (!b) {
            return NULL;
        }
        b->next = doc->blocks;
        doc->blocks = b;
        doc->cur = b;
    }
    void *p = b->data + b->used;
    b->used += size;
    return p;
}

/* ── 解析器状态(全部在栈上,无全局可变状态) ── */

typedef struct json_parser {
    const char *begin;              /* 输入起点,算行列用 */
    const char *cur;
    const char *end;                /* begin + len(输入不要求 NUL 结尾) */
    sxcl_json *doc;
    char *err;
    size_t err_len;
    int depth;                      /* 当前嵌套层数 */
    int has_err;                    /* 只保留第一个错误 */
} json_parser;

static void err_write(char *err, size_t err_len, const char *fmt, ...)
{
    if (!err || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* 记录第一个错误:把 at 处的字节偏移换算成 1 基行号/列号(列按 UTF-8 码点数)。 */
static void parse_error(json_parser *ps, const char *at, const char *fmt, ...)
{
    if (ps->has_err) {
        return;
    }
    ps->has_err = 1;
    if (!ps->err || ps->err_len == 0) {
        return;
    }
    size_t line = 1;
    size_t col = 1;
    const char *stop = (at < ps->end) ? at : ps->end;
    for (const char *p = ps->begin; p < stop; ++p) {
        if (*p == '\n') {
            ++line;
            col = 1;
        } else if (((unsigned char)*p & 0xC0u) != 0x80u) {  /* 跳过 UTF-8 续字节 */
            ++col;
        }
    }
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(ps->err, ps->err_len, "第 %zu 行第 %zu 列: %s", line, col, msg);
}

static void parse_oom(json_parser *ps)
{
    parse_error(ps, ps->cur, "内存不足(arena 分配失败)");
}

static void skip_ws(json_parser *ps)
{
    while (ps->cur < ps->end) {
        const char c = *ps->cur;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++ps->cur;
        } else {
            break;
        }
    }
}

static int char_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 读 4 位十六进制(调用方保证 p..p+4 在字符串内)。 */
static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        const int h = hex_value(p[i]);
        if (h < 0) {
            return -1;
        }
        v = (v << 4) | (unsigned)h;
    }
    *out = v;
    return 0;
}

/* 码点 -> UTF-8(1..4 字节),返回写入字节数。 */
static size_t utf8_encode(unsigned cp, char *dst)
{
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ── 建节点 ── */

static const sxcl_json_value *make_scalar(json_parser *ps, sxcl_json_type type)
{
    sxcl_json_value *v = (sxcl_json_value *)arena_alloc(ps->doc, sizeof *v);
    if (!v) {
        parse_oom(ps);
        return NULL;
    }
    v->type = type;
    v->u.number = 0.0;
    v->u.boolean = 0;
    return v;
}

static const sxcl_json_value *make_bool(json_parser *ps, int value)
{
    const sxcl_json_value *v = make_scalar(ps, SXCL_JSON_BOOL);
    if (v) {
        ((sxcl_json_value *)v)->u.boolean = value ? 1 : 0;
    }
    return v;
}

static const sxcl_json_value *make_number(json_parser *ps, double value)
{
    const sxcl_json_value *v = make_scalar(ps, SXCL_JSON_NUMBER);
    if (v) {
        ((sxcl_json_value *)v)->u.number = value;
    }
    return v;
}

static const sxcl_json_value *make_string(json_parser *ps, const char *ptr, size_t len)
{
    const sxcl_json_value *v = make_scalar(ps, SXCL_JSON_STRING);
    if (v) {
        sxcl_json_value *m = (sxcl_json_value *)v;
        m->u.string.ptr = ptr;
        m->u.string.len = len;
    }
    return v;
}

/* ── 字符串 ── */

/* 解析一个 JSON 字符串(ps->cur 必须指向开引号);成功时 *out_ptr 指向 arena 内
 * 已解码、已 NUL 结尾的内容,*out_len 是解码后字节数。 */
static int parse_string_ref(json_parser *ps, const char **out_ptr, size_t *out_len)
{
    const char *start = ps->cur + 1;
    const char *q = start;
    while (q < ps->end) {
        const unsigned char c = (unsigned char)*q;
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            if (ps->end - q < 2) {
                q = ps->end;
                break;
            }
            q += 2;                 /* 跳过被转义的字符,转义合法性在解码阶段校验 */
            continue;
        }
        if (c < 0x20u) {
            parse_error(ps, q, "字符串里出现未转义的裸控制字符(0x%02X)", (unsigned)c);
            return -1;
        }
        ++q;
    }
    if (q >= ps->end) {
        parse_error(ps, ps->end, "字符串未闭合:缺少结尾的双引号");
        return -1;
    }

    const size_t raw = (size_t)(q - start);
    char *buf = (char *)arena_alloc(ps->doc, raw + 1u);
    if (!buf) {
        parse_oom(ps);
        return -1;
    }
    sxcl_json_block *blk = ps->doc->cur;      /* 本串所在的块(解码期间不会再分配) */
    const size_t buf_off = (size_t)((unsigned char *)buf - blk->data);

    size_t n = 0;
    const char *r = start;
    while (r < q) {
        const char c = *r;
        if (c != '\\') {
            buf[n++] = c;
            ++r;
            continue;
        }
        if (q - r < 2) {
            parse_error(ps, r, "字符串以孤立的反斜杠结尾");
            return -1;
        }
        const char e = r[1];
        switch (e) {
        case '"':
            buf[n++] = '"';
            r += 2;
            break;
        case '\\':
            buf[n++] = '\\';
            r += 2;
            break;
        case '/':
            buf[n++] = '/';
            r += 2;
            break;
        case 'b':
            buf[n++] = '\b';
            r += 2;
            break;
        case 'f':
            buf[n++] = '\f';
            r += 2;
            break;
        case 'n':
            buf[n++] = '\n';
            r += 2;
            break;
        case 'r':
            buf[n++] = '\r';
            r += 2;
            break;
        case 't':
            buf[n++] = '\t';
            r += 2;
            break;
        case 'u': {
            unsigned cp = 0;
            if (q - r < 6 || hex4(r + 2, &cp) != 0) {
                parse_error(ps, r, "\\u 转义需要 4 位十六进制数字");
                return -1;
            }
            r += 6;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {          /* 高位代理:必须跟低位代理 */
                unsigned lo = 0;
                if (q - r < 6 || r[0] != '\\' || r[1] != 'u' || hex4(r + 2, &lo) != 0 ||
                    lo < 0xDC00u || lo > 0xDFFFu) {
                    parse_error(ps, r, "高位代理项 \\u%04X 后面缺少合法的低位代理项", cp);
                    return -1;
                }
                r += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {   /* 孤立低位代理 */
                parse_error(ps, r - 6, "孤立的低位代理项 \\u%04X", cp);
                return -1;
            }
            n += utf8_encode(cp, buf + n);
            break;
        }
        default:
            parse_error(ps, r, "非法的转义序列 '\\%c'", (e >= 0x20 && e < 0x7F) ? e : '?');
            return -1;
        }
    }
    buf[n] = '\0';
    /* 转义只会让内容变短,把没用到的尾部还给 arena(字符串密集的元数据省内存)。 */
    blk->used = buf_off + n + 1u;

    ps->cur = q + 1;              /* 跳过结尾引号 */
    *out_ptr = buf;
    *out_len = n;
    return 0;
}

/* ── 数字 ── */

/* 10 的幂表:0..22 次方都能被 double 精确表示,分块乘除避免 libm/locale 依赖。 */
static const double k_pow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static double scale_pow10(double v, int exp10)
{
    if (v == 0.0) {
        return v;                 /* 0 * 10^n 恒为 0,不用循环 */
    }
    if (exp10 > 1100) {
        exp10 = 1100;             /* 再大也是 ±inf,别做无谓的循环 */
    } else if (exp10 < -1100) {
        exp10 = -1100;
    }
    while (exp10 > 0) {
        const int step = (exp10 > 22) ? 22 : exp10;
        v *= k_pow10[step];
        exp10 -= step;
    }
    while (exp10 < 0) {
        const int step = (exp10 < -22) ? 22 : -exp10;
        v /= k_pow10[step];
        exp10 += step;
    }
    return v;
}

/* 严格 JSON 数字文法:-? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)? */
static int parse_number(json_parser *ps, double *out)
{
    const char *p = ps->cur;
    const char *end = ps->end;
    int negative = 0;
    if (*p == '-') {
        negative = 1;
        ++p;
    }
    if (p >= end || !char_is_digit(*p)) {
        parse_error(ps, p, "数字格式错误:缺少整数部分");
        return -1;
    }
    unsigned long long mant = 0;   /* 前 18 位有效数字 */
    int sig = 0;                   /* 已收进 mant 的有效位数 */
    int exp10 = 0;                 /* 十进制指数 */

    if (*p == '0') {
        ++p;
        if (p < end && char_is_digit(*p)) {
            parse_error(ps, p, "数字格式错误:不允许前导零");
            return -1;
        }
    } else {
        while (p < end && char_is_digit(*p)) {
            if (sig < 18) {
                mant = mant * 10u + (unsigned long long)(*p - '0');
                ++sig;
            } else {
                ++exp10;           /* 超出 18 位的整数位只抬指数 */
            }
            ++p;
        }
    }
    if (p < end && *p == '.') {
        ++p;
        if (p >= end || !char_is_digit(*p)) {
            parse_error(ps, p, "数字格式错误:小数点后缺少数字");
            return -1;
        }
        while (p < end && char_is_digit(*p)) {
            if (sig < 18) {
                mant = mant * 10u + (unsigned long long)(*p - '0');
                ++sig;
            }
            --exp10;               /* 每一位小数都降一次指数(即使没进 mant) */
            ++p;
        }
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        int esign = 0;
        if (p < end && (*p == '+' || *p == '-')) {
            esign = (*p == '-') ? 1 : 0;
            ++p;
        }
        if (p >= end || !char_is_digit(*p)) {
            parse_error(ps, p, "数字格式错误:指数部分缺少数字");
            return -1;
        }
        long e = 0;
        while (p < end && char_is_digit(*p)) {
            if (e < 1000000L) {    /* 夹住,防止 long 溢出 */
                e = e * 10L + (long)(*p - '0');
            }
            ++p;
        }
        exp10 += (int)(esign ? -e : e);
    }
    ps->cur = p;

    double v = scale_pow10((double)mant, exp10);
    if (negative) {
        v = -v;
    }
    *out = v;
    return 0;
}

/* ── 临时动态数组(只在解析单个容器期间存在,提交后立刻 free) ── */

typedef struct json_tmpvec {
    void *data;
    size_t count;
    size_t cap;
    size_t elem;
} json_tmpvec;

static int tmpvec_push(json_tmpvec *v, const void *elem)
{
    if (v->count == v->cap) {
        const size_t ncap = v->cap ? v->cap * 2u : 16u;
        void *nd = realloc(v->data, ncap * v->elem);
        if (!nd) {
            return -1;
        }
        v->data = nd;
        v->cap = ncap;
    }
    memcpy((unsigned char *)v->data + v->count * v->elem, elem, v->elem);
    ++v->count;
    return 0;
}

static int tmpvec_commit(json_tmpvec *v, sxcl_json *doc, void **out)
{
    if (v->count == 0) {
        *out = NULL;
    } else {
        void *dst = arena_alloc(doc, v->count * v->elem);
        if (!dst) {
            free(v->data);
            v->data = NULL;
            return -1;
        }
        memcpy(dst, v->data, v->count * v->elem);
        *out = dst;
    }
    free(v->data);
    v->data = NULL;
    return 0;
}

/* 重复键:保留最后一次出现的值(小对象原地去重;大对象靠 sxcl_json_get 反向扫描兜底)。 */
static void dedup_members(sxcl_json_member *members, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (members[j].key_len == members[i].key_len &&
                memcmp(members[j].key, members[i].key, members[j].key_len) == 0) {
                members[j].value = members[i].value;
                break;
            }
        }
    }
}

/* ── 值与容器 ── */

static const sxcl_json_value *parse_value(json_parser *ps);
static int make_array_commit(json_parser *ps, json_tmpvec *items, const sxcl_json_value **out);
static int make_object_commit(json_parser *ps, json_tmpvec *members, const sxcl_json_value **out);

static int parse_array(json_parser *ps, const sxcl_json_value **out)
{
    if (ps->depth >= SXCL_JSON_MAX_DEPTH) {
        parse_error(ps, ps->cur, "嵌套层数超过上限 %d 层", SXCL_JSON_MAX_DEPTH);
        return -1;
    }
    ++ps->depth;
    ++ps->cur;                                            /* '[' */

    json_tmpvec items = {NULL, 0, 0, sizeof(const sxcl_json_value *)};
    skip_ws(ps);
    if (ps->cur < ps->end && *ps->cur == ']') {
        ++ps->cur;
        --ps->depth;
        return make_array_commit(ps, &items, out);
    }
    for (;;) {
        const sxcl_json_value *v = parse_value(ps);
        if (!v) {
            free(items.data);
            return -1;
        }
        if (tmpvec_push(&items, &v) != 0) {
            parse_oom(ps);
            free(items.data);
            return -1;
        }
        skip_ws(ps);
        if (ps->cur >= ps->end) {
            parse_error(ps, ps->end, "数组未闭合:期望 ',' 或 ']'");
            free(items.data);
            return -1;
        }
        if (*ps->cur == ',') {
            ++ps->cur;
            skip_ws(ps);
            if (ps->cur < ps->end && *ps->cur == ']') {
                parse_error(ps, ps->cur, "数组里出现尾随逗号");
                free(items.data);
                return -1;
            }
            continue;
        }
        if (*ps->cur == ']') {
            ++ps->cur;
            break;
        }
        parse_error(ps, ps->cur, "期望 ',' 或 ']',却遇到 '%c'", *ps->cur);
        free(items.data);
        return -1;
    }
    --ps->depth;
    return make_array_commit(ps, &items, out);
}

static int make_array_commit(json_parser *ps, json_tmpvec *items, const sxcl_json_value **out)
{
    const sxcl_json_value **slots = NULL;
    if (tmpvec_commit(items, ps->doc, (void **)&slots) != 0) {
        parse_oom(ps);
        return -1;
    }
    sxcl_json_value *node = (sxcl_json_value *)arena_alloc(ps->doc, sizeof *node);
    if (!node) {
        parse_oom(ps);
        return -1;
    }
    node->type = SXCL_JSON_ARRAY;
    node->u.array.items = slots;
    node->u.array.count = items->count;
    *out = node;
    return 0;
}

static int make_object_commit(json_parser *ps, json_tmpvec *members, const sxcl_json_value **out)
{
    if (members->count > 0 && members->count <= SXCL_JSON_DEDUP_MAX) {
        dedup_members((sxcl_json_member *)members->data, members->count);
    }
    sxcl_json_member *slots = NULL;
    if (tmpvec_commit(members, ps->doc, (void **)&slots) != 0) {
        parse_oom(ps);
        return -1;
    }
    sxcl_json_value *node = (sxcl_json_value *)arena_alloc(ps->doc, sizeof *node);
    if (!node) {
        parse_oom(ps);
        return -1;
    }
    node->type = SXCL_JSON_OBJECT;
    node->u.object.members = slots;
    node->u.object.count = members->count;
    *out = node;
    return 0;
}

static int parse_object(json_parser *ps, const sxcl_json_value **out)
{
    if (ps->depth >= SXCL_JSON_MAX_DEPTH) {
        parse_error(ps, ps->cur, "嵌套层数超过上限 %d 层", SXCL_JSON_MAX_DEPTH);
        return -1;
    }
    ++ps->depth;
    ++ps->cur;                                            /* '{' */

    json_tmpvec members = {NULL, 0, 0, sizeof(sxcl_json_member)};
    skip_ws(ps);
    if (ps->cur < ps->end && *ps->cur == '}') {
        ++ps->cur;
        --ps->depth;
        return make_object_commit(ps, &members, out);
    }
    for (;;) {
        skip_ws(ps);
        if (ps->cur >= ps->end) {
            parse_error(ps, ps->end, "对象未闭合:期望键字符串");
            free(members.data);
            return -1;
        }
        if (*ps->cur != '"') {
            parse_error(ps, ps->cur, "期望对象的键(双引号字符串),却遇到 '%c'", *ps->cur);
            free(members.data);
            return -1;
        }
        const char *key = NULL;
        size_t key_len = 0;
        if (parse_string_ref(ps, &key, &key_len) != 0) {
            free(members.data);
            return -1;
        }
        skip_ws(ps);
        if (ps->cur >= ps->end || *ps->cur != ':') {
            parse_error(ps, ps->cur, "期望键和值之间的 ':'");
            free(members.data);
            return -1;
        }
        ++ps->cur;
        const sxcl_json_value *value = parse_value(ps);
        if (!value) {
            free(members.data);
            return -1;
        }
        sxcl_json_member m;
        m.key = key;
        m.key_len = key_len;
        m.value = value;
        if (tmpvec_push(&members, &m) != 0) {
            parse_oom(ps);
            free(members.data);
            return -1;
        }
        skip_ws(ps);
        if (ps->cur >= ps->end) {
            parse_error(ps, ps->end, "对象未闭合:期望 ',' 或 '}'");
            free(members.data);
            return -1;
        }
        if (*ps->cur == ',') {
            ++ps->cur;
            skip_ws(ps);
            if (ps->cur < ps->end && *ps->cur == '}') {
                parse_error(ps, ps->cur, "对象里出现尾随逗号");
                free(members.data);
                return -1;
            }
            continue;
        }
        if (*ps->cur == '}') {
            ++ps->cur;
            break;
        }
        parse_error(ps, ps->cur, "期望 ',' 或 '}',却遇到 '%c'", *ps->cur);
        free(members.data);
        return -1;
    }
    --ps->depth;
    return make_object_commit(ps, &members, out);
}

static int match_literal(json_parser *ps, const char *literal)
{
    const size_t n = strlen(literal);
    if ((size_t)(ps->end - ps->cur) < n || memcmp(ps->cur, literal, n) != 0) {
        parse_error(ps, ps->cur, "非法字面量:期望 %s", literal);
        return -1;
    }
    ps->cur += n;
    return 0;
}

static const sxcl_json_value *parse_value(json_parser *ps)
{
    skip_ws(ps);
    if (ps->cur >= ps->end) {
        parse_error(ps, ps->end, "期望一个值,却已到输入末尾");
        return NULL;
    }
    switch (*ps->cur) {
    case '{': {
        const sxcl_json_value *v = NULL;
        return (parse_object(ps, &v) == 0) ? v : NULL;
    }
    case '[': {
        const sxcl_json_value *v = NULL;
        return (parse_array(ps, &v) == 0) ? v : NULL;
    }
    case '"': {
        const char *s = NULL;
        size_t n = 0;
        if (parse_string_ref(ps, &s, &n) != 0) {
            return NULL;
        }
        return make_string(ps, s, n);
    }
    case 't':
        return (match_literal(ps, "true") == 0) ? make_bool(ps, 1) : NULL;
    case 'f':
        return (match_literal(ps, "false") == 0) ? make_bool(ps, 0) : NULL;
    case 'n':
        return (match_literal(ps, "null") == 0) ? make_scalar(ps, SXCL_JSON_NULL) : NULL;
    default:
        break;
    }
    if (*ps->cur == '-' || char_is_digit(*ps->cur)) {
        double d = 0.0;
        if (parse_number(ps, &d) != 0) {
            return NULL;
        }
        return make_number(ps, d);
    }
    parse_error(ps, ps->cur, "意外的字符 '%c'(这里应该是一个 JSON 值)", *ps->cur);
    return NULL;
}

/* ── 文件读取 ── */

static FILE *open_utf8_path(const char *path)
{
#if defined(_WIN32)
    const int need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    FILE *f = NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, need) == need) {
        if (_wfopen_s(&f, wide, L"rb") != 0) {  /* _wfopen 会触发 C4996,/WX 下必须用 _s 版 */
            f = NULL;
        }
    }
    free(wide);
    return f;
#else
    return fopen(path, "rb");
#endif
}

/* ── 公开 API ── */

sxcl_json *sxcl_json_parse(const char *text, size_t len, char *err, size_t err_len)
{
    if (err && err_len > 0) {
        err[0] = '\0';
    }
    if (!text) {
        err_write(err, err_len, "输入为空指针");
        return NULL;
    }

    sxcl_json *doc = (sxcl_json *)calloc(1, sizeof *doc);
    if (!doc) {
        err_write(err, err_len, "内存不足(文档句柄分配失败)");
        return NULL;
    }
    doc->next_cap = SXCL_JSON_BLOCK_MIN;

    json_parser ps;
    ps.begin = text;
    ps.cur = text;
    ps.end = text + len;
    ps.doc = doc;
    ps.err = err;
    ps.err_len = err_len;
    ps.depth = 0;
    ps.has_err = 0;

    /* 容忍 UTF-8 BOM(真实世界的元数据文件常带)。 */
    if (len >= 3 && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu) {
        ps.cur += 3;
    }

    const sxcl_json_value *root = parse_value(&ps);
    if (!root) {
        sxcl_json_free(doc);
        return NULL;
    }
    skip_ws(&ps);
    if (ps.cur != ps.end) {
        parse_error(&ps, ps.cur, "根值之后还有多余内容");
        sxcl_json_free(doc);
        return NULL;
    }
    doc->root = root;
    return doc;
}

sxcl_json *sxcl_json_parse_file(const char *path, char *err, size_t err_len)
{
    if (err && err_len > 0) {
        err[0] = '\0';
    }
    if (!path || path[0] == '\0') {
        err_write(err, err_len, "文件路径为空");
        return NULL;
    }
    FILE *f = open_utf8_path(path);
    if (!f) {
        err_write(err, err_len, "打不开文件: %s", path);
        return NULL;
    }

    /* 边读边扩容(不依赖 fseek/ftell,大文件也就多几次 realloc)。 */
    size_t cap = 256u * 1024u;
    size_t len = 0;
    char *buf = (char *)malloc(cap + 1u);
    if (!buf) {
        fclose(f);
        err_write(err, err_len, "内存不足(读文件缓冲分配失败)");
        return NULL;
    }
    for (;;) {
        if (len == cap) {
            const size_t ncap = cap * 2u;
            char *nb = (char *)realloc(buf, ncap + 1u);
            if (!nb) {
                free(buf);
                fclose(f);
                err_write(err, err_len, "内存不足(读文件缓冲扩容失败)");
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        const size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                err_write(err, err_len, "读取文件失败: %s", path);
                return NULL;
            }
            break;                /* EOF */
        }
    }
    fclose(f);
    buf[len] = '\0';

    sxcl_json *doc = sxcl_json_parse(buf, len, err, err_len);
    free(buf);                    /* 文档已经把需要的内容拷进 arena,原文不留 */
    return doc;
}

void sxcl_json_free(sxcl_json *doc)
{
    if (!doc) {
        return;
    }
    sxcl_json_block *b = doc->blocks;
    while (b) {
        sxcl_json_block *next = b->next;
        free(b);
        b = next;
    }
    free(doc);
}

const sxcl_json_value *sxcl_json_root(const sxcl_json *doc)
{
    return doc ? doc->root : NULL;
}

sxcl_json_type sxcl_json_type_of(const sxcl_json_value *value)
{
    return value ? value->type : SXCL_JSON_NULL;
}

const sxcl_json_value *sxcl_json_get(const sxcl_json_value *object, const char *key)
{
    if (!object || !key || object->type != SXCL_JSON_OBJECT) {
        return NULL;
    }
    /* 反向扫描:重复键以最后一次出现的为准(小对象在提交时已原地去重)。 */
    const size_t key_len = strlen(key);
    for (size_t i = object->u.object.count; i > 0; --i) {
        const sxcl_json_member *m = &object->u.object.members[i - 1];
        if (m->key_len == key_len && memcmp(m->key, key, key_len) == 0) {
            return m->value;
        }
    }
    return NULL;
}

size_t sxcl_json_member_count(const sxcl_json_value *object)
{
    if (!object || object->type != SXCL_JSON_OBJECT) {
        return 0;
    }
    return object->u.object.count;
}

const char *sxcl_json_member_key(const sxcl_json_value *object, size_t index)
{
    if (!object || object->type != SXCL_JSON_OBJECT || index >= object->u.object.count) {
        return NULL;
    }
    return object->u.object.members[index].key;
}

const sxcl_json_value *sxcl_json_member_value(const sxcl_json_value *object, size_t index)
{
    if (!object || object->type != SXCL_JSON_OBJECT || index >= object->u.object.count) {
        return NULL;
    }
    return object->u.object.members[index].value;
}

size_t sxcl_json_size(const sxcl_json_value *array)
{
    return (array && array->type == SXCL_JSON_ARRAY) ? array->u.array.count : 0;
}

const sxcl_json_value *sxcl_json_at(const sxcl_json_value *array, size_t index)
{
    if (!array || array->type != SXCL_JSON_ARRAY || index >= array->u.array.count) {
        return NULL;
    }
    return array->u.array.items[index];
}

const char *sxcl_json_string(const sxcl_json_value *value)
{
    return (value && value->type == SXCL_JSON_STRING) ? value->u.string.ptr : NULL;
}

int sxcl_json_bool(const sxcl_json_value *value)
{
    if (!value || value->type != SXCL_JSON_BOOL) {
        return 0;
    }
    return value->u.boolean;
}

double sxcl_json_number(const sxcl_json_value *value)
{
    return (value && value->type == SXCL_JSON_NUMBER) ? value->u.number : 0.0;
}

const char *sxcl_json_get_string(const sxcl_json_value *object, const char *key, const char *def)
{
    const sxcl_json_value *v = sxcl_json_get(object, key);
    if (!v || v->type != SXCL_JSON_STRING) {
        return def;
    }
    return v->u.string.ptr;
}

int64_t sxcl_json_get_int64(const sxcl_json_value *object, const char *key, int64_t def)
{
    const sxcl_json_value *v = sxcl_json_get(object, key);
    if (!v || v->type != SXCL_JSON_NUMBER) {
        return def;
    }
    const double d = v->u.number;
    if (d != d) {                 /* NaN:解析器不会产生,防御性处理 */
        return def;
    }
    if (d >= 9223372036854775807.0) {
        return INT64_MAX;
    }
    if (d <= -9223372036854775808.0) {
        return INT64_MIN;
    }
    return (int64_t)d;            /* 截断取整(与 C 的 double -> 整数转换一致) */
}

int sxcl_json_get_bool(const sxcl_json_value *object, const char *key, int def)
{
    const sxcl_json_value *v = sxcl_json_get(object, key);
    if (!v || v->type != SXCL_JSON_BOOL) {
        return def;
    }
    return v->u.boolean ? 1 : 0;
}
