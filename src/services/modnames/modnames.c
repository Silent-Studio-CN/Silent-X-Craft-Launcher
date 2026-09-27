/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组中文名表（声明与口径见 sxcl/modnames.h；数据来源与许可见 docs/29）。
//
// 只做纯逻辑：解析 / 建哈希 / 查表 / 摘要校验 / 原子替换 / 挑"缓存还是随包"。
// 联网那一跳走核心库现成的 sxcl_http_get_text_ex，transports 由调用方给
// （界面走它自己的工作线程 / 命令行给 Qt 后端），所以假 transport 就能把整条更新路径单测完。

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* getenv/fopen/… 在 MSVC 下默认被标记弃用(C4996) */
#endif

#include "sxcl/modnames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/http.h"

/* ── 索引种类（哈希槽里存的就是它 + 池内偏移） ── */
#define IDX_SLUG    0u
#define IDX_CF_ID   1u
#define IDX_CF_SLUG 2u
#define IDX_MODID   3u
#define IDX_NAME_EN 4u
#define IDX_NAME_ZH 5u
#define IDX_KINDS   6u

/* 表里的列（默认顺序；表头可以用 "# columns" 行改写顺序，认不出的列名**忽略**不报错，
 * 这样云端将来加列不会把老客户端弄崩）。 */
enum {
    COL_SLUG = 0,
    COL_CF_ID,
    COL_CF_SLUG,
    COL_MODID,
    COL_NAME_ZH,
    COL_NAME_EN,
    COL_SOURCE,
    COL_MAX
};

#define MAX_FIELDS 24                          /* 一行的字段上限（多出来的忽略，不报错） */
#define MAX_TABLE_BYTES (64u * 1024u * 1024u)  /* 单份表最大读这么大（坏文件不许把内存吃光） */
#define KEY_NORM_MAX 512                       /* 规范化后的键上限 */
#define CLOUD_MAX_BYTES (32u * 1024u * 1024u)  /* 云端整表下载上限（随包表约 2 MB） */
#define PATH_MAX_BYTES 1024

static const char *const kDefaultColumns[COL_MAX] = {
    "slug", "cf_id", "cf_slug", "modid", "name_zh", "name_en", "source"
};

/* ── 小工具 ── */

static void copy_cap(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src != NULL) {
        for (; src[i] != '\0' && i + 1u < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/** 截断安全地写一段"切片"文本（不是 NUL 结尾的）。 */
static void copy_slice(char *dst, size_t cap, const char *src, size_t len)
{
    size_t i = 0;
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src != NULL) {
        for (; i < len && i + 1u < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void set_err(char *err, size_t err_len, const char *text)
{
    if (err != NULL && err_len > 0u) {
        copy_cap(err, err_len, text);
    }
}

static void set_note(char *note, size_t note_len, const char *text)
{
    if (note != NULL && note_len > 0u) {
        copy_cap(note, note_len, text);
    }
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** 规范化一个查询键：去掉空白与常见标点、ASCII 转小写。
 *  **只做这一种变换** —— 不做拼音、不做模糊、不做编辑距离；中文按 UTF-8 原样保留。
 *  返回长度；0 = 这个键"什么都不是"（全被去掉了），不建索引也不查。 */
static size_t normalize_key(const char *src, char *dst, size_t cap)
{
    size_t n = 0;
    if (src == NULL || dst == NULL || cap == 0u) {
        return 0u;
    }
    for (; *src != '\0'; ++src) {
        const unsigned char c = (unsigned char)*src;
        if (is_space((char)c)) {
            continue;
        }
        switch (c) {
        case '-': case '_': case '.': case '\'': case '"': case ':': case ';':
        case '!': case '?': case ',': case '&': case '(': case ')': case '[':
        case ']': case '{': case '}': case '/': case '\\': case '+': case '|':
        case '~': case '*': case '#': case '@': case '%': case '^': case '=':
        case '<': case '>':
            continue;
        default:
            break;
        }
        /* 全角空格 U+3000（E3 80 80）与全角顿号 U+3001 一带不参与比较：按字节跳过。 */
        if (c == 0xE3u && (unsigned char)src[1] == 0x80u && (unsigned char)src[2] == 0x80u) {
            src += 2;
            continue;
        }
        if (n + 1u >= cap) {
            break;
        }
        dst[n++] = lower_ascii((char)c);
    }
    dst[n] = '\0';
    return n;
}

/** FNV-1a 64：先吃一个种类字节，再吃规范化后的键。 */
static uint64_t hash_key(unsigned kind, const char *key, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    h ^= (uint64_t)(kind & 0xFFu);
    h *= 1099511628211ull;
    for (i = 0; i < len; ++i) {
        h ^= (unsigned char)key[i];
        h *= 1099511628211ull;
    }
    return (h == 0u) ? 1u : h; /* 0 留给"空槽" */
}

/* ── 表内部 ── */

typedef struct modname_slot {
    uint64_t hash;    /**< 0 = 空槽 */
    uint32_t kind;    /**< IDX_* */
    uint32_t entry;   /**< entries[] 下标 */
    uint32_t key_off; /**< 池内偏移（NUL 结尾的规范化键） */
} modname_slot;

struct sxcl_modnames {
    char *pool;                     /* 字符串池：字段原文 + 规范化键，全部 NUL 结尾 */
    size_t pool_len, pool_cap;
    sxcl_modname_entry *entries;
    size_t count, cap;
    /* 每一行的字段**偏移**（COL_MAX 个一行）。解析期间只有偏移是稳的 —— 池会 realloc。
     * 整张表读完之后才把偏移统一换成 entries[] 里的指针，换完这份就 free。 */
    uint32_t *cells;
    size_t cells_cap;
    modname_slot *slots;
    size_t slot_cap, slot_used;
    sxcl_modname_meta meta;
    size_t bad_lines;
    unsigned last_probes;
    int count_mismatch;
    int sha_mismatch;
    char declared_sha[SXCL_MODNAMES_SHA256_MAX];
};

/** 往池里放一段切片，返回偏移；失败返回 (size_t)-1。 */
static size_t pool_intern(sxcl_modnames *t, const char *text, size_t len)
{
    size_t off;
    if (t->pool_len + len + 1u > t->pool_cap) {
        size_t want = (t->pool_cap == 0u) ? 8192u : t->pool_cap;
        char *np;
        while (want < t->pool_len + len + 1u) {
            want *= 2u;
        }
        np = (char *)realloc(t->pool, want);
        if (np == NULL) {
            return (size_t)-1;
        }
        t->pool = np;
        t->pool_cap = want;
    }
    off = t->pool_len;
    if (len > 0u && text != NULL) {
        (void)memcpy(t->pool + off, text, len);
    }
    t->pool[off + len] = '\0';
    t->pool_len = off + len + 1u;
    return off;
}

static int hash_grow(sxcl_modnames *t)
{
    const size_t want = (t->slot_cap == 0u) ? 1024u : t->slot_cap * 2u;
    modname_slot *ns = (modname_slot *)calloc(want, sizeof(modname_slot));
    size_t i;
    if (ns == NULL) {
        return -1;
    }
    for (i = 0; i < t->slot_cap; ++i) {
        if (t->slots[i].hash != 0u) {
            const uint64_t h = t->slots[i].hash;
            size_t at = (size_t)(h & (uint64_t)(want - 1u));
            while (ns[at].hash != 0u) {
                at = (at + 1u) & (want - 1u);
            }
            ns[at] = t->slots[i];
        }
    }
    free(t->slots);
    t->slots = ns;
    t->slot_cap = want;
    return 0;
}

/** 往槽里插一个**已经进过池**的键。
 *
 *  为什么不在这里 pool_intern：池在扩容时会 realloc，**表里已经存好的指针会全体作废**。
 *  所以解析时先把键/值都 intern 完、只留下偏移，指针等到整张表读完（池不再动）才统一算出来
 *  （见 sxcl_modnames_parse 末尾那段）。这个函数只动槽数组，绝不动池。 */
static int hash_insert_prepared(sxcl_modnames *t, unsigned kind, size_t key_off, uint64_t h, size_t entry)
{
    size_t at;
    if (t->slot_cap == 0u || (t->slot_used + 1u) * 2u >= t->slot_cap) {
        if (hash_grow(t) != 0) {
            return -1;
        }
    }
    at = (size_t)(h & (uint64_t)(t->slot_cap - 1u));
    while (t->slots[at].hash != 0u) {
        /* 同一个键先到先得：表是生成器排好序的，重复只可能是数据脏，
         * "先出现的那条为准"比"看谁的字符串大"更可预测。 */
        if (t->slots[at].hash == h && t->slots[at].kind == kind &&
            strcmp(t->pool + t->slots[at].key_off, t->pool + key_off) == 0) {
            return 0;
        }
        at = (at + 1u) & (t->slot_cap - 1u);
    }
    t->slots[at].hash = h;
    t->slots[at].kind = kind;
    t->slots[at].entry = (uint32_t)entry;
    t->slots[at].key_off = (uint32_t)key_off;
    t->slot_used++;
    return 0;
}

static int entry_push(sxcl_modnames *t, const sxcl_modname_entry *e)
{
    if (t->count == t->cap) {
        size_t want = (t->cap == 0u) ? 256u : t->cap * 2u;
        sxcl_modname_entry *ne;
        if (want > SXCL_MODNAMES_MAX_ENTRIES) {
            want = SXCL_MODNAMES_MAX_ENTRIES;
        }
        if (t->count >= want) {
            return -1; /* 到上限：后面的行按坏行计数，不静默丢 */
        }
        ne = (sxcl_modname_entry *)realloc(t->entries, want * sizeof(sxcl_modname_entry));
        if (ne == NULL) {
            return -1;
        }
        t->entries = ne;
        t->cap = want;
    }
    t->entries[t->count] = *e;
    t->count++;
    return 0;
}

/** 保证 cells[] 装得下 rows 行（放不下就翻倍）。**必须在 entry_push 之前调**：
 *  只有它可能失败，先过了它，entry_push 成功之后再写 cells 就不会出现"条目与偏移错行"。 */
static int cells_reserve(sxcl_modnames *t, size_t rows)
{
    if (rows <= t->cells_cap) {
        return 0;
    }
    {
        size_t want = (t->cells_cap == 0u) ? 256u : t->cells_cap;
        uint32_t *nc;
        while (want < rows) {
            want *= 2u;
        }
        nc = (uint32_t *)realloc(t->cells, want * COL_MAX * sizeof(uint32_t));
        if (nc == NULL) {
            return -1;
        }
        t->cells = nc;
        t->cells_cap = want;
    }
    return 0;
}

typedef struct line_slice {
    const char *p;
    size_t len;
} line_slice;

static size_t split_tabs(const char *line, size_t len, line_slice *out, size_t cap)
{
    size_t n = 0;
    size_t start = 0;
    size_t i;
    for (i = 0; i <= len; ++i) {
        if (i == len || line[i] == '\t') {
            if (n < cap) {
                out[n].p = line + start;
                out[n].len = i - start;
                n++;
            }
            start = i + 1u;
        }
    }
    return n;
}

static line_slice trim_slice(line_slice s)
{
    while (s.len > 0u && is_space(s.p[0])) {
        s.p++;
        s.len--;
    }
    while (s.len > 0u && is_space(s.p[s.len - 1u])) {
        s.len--;
    }
    return s;
}

/** 表头行：去掉开头的 '#'，再按第一个空白/TAB 拆成 key 与 value。 */
static void parse_header_kv_local(line_slice s, line_slice *key, line_slice *val)
{
    size_t i;
    if (s.len > 0u && s.p[0] == '#') {
        s.p++;
        s.len--;
    }
    while (s.len > 0u && is_space(s.p[0])) {
        s.p++;
        s.len--;
    }
    for (i = 0; i < s.len; ++i) {
        if (s.p[i] == '\t' || s.p[i] == ' ') {
            break;
        }
    }
    key->p = s.p;
    key->len = i;
    while (i < s.len && is_space(s.p[i])) {
        i++;
    }
    val->p = s.p + i;
    val->len = s.len - i;
    *val = trim_slice(*val);
}

static int slice_is(const line_slice *s, const char *lit)
{
    const size_t n = strlen(lit);
    return s->len == n && s->p != NULL && strncmp(s->p, lit, n) == 0;
}

/** 解析一个十进制数（切片）。成功返回 0 并写出 *out；不是纯十进制返回 -1。 */
static int slice_to_long(const line_slice *s, long *out)
{
    long v = 0;
    size_t i;
    if (s->len == 0u || s->len > 18u) {
        return -1;
    }
    for (i = 0; i < s->len; ++i) {
        if (s->p[i] < '0' || s->p[i] > '9') {
            return -1;
        }
        v = v * 10 + (long)(s->p[i] - '0');
    }
    *out = v;
    return 0;
}

/* ── 解析 ── */

typedef struct parse_ctx {
    int columns[COL_MAX]; /* 第 i 个字段落到哪一列（-1 = 认不出的列名，忽略） */
    int col_count;
} parse_ctx;

/** 表头 "columns" 行：按列名钉住字段顺序。认不出的列名映射成 -1（忽略该字段），不报错。 */
static void apply_columns(parse_ctx *ctx, const char *text, size_t len, size_t max_fields)
{
    size_t pos = 0;
    size_t n = 0;
    int k;
    ctx->col_count = 0;
    for (k = 0; k < COL_MAX; ++k) {
        ctx->columns[k] = -1;
    }
    while (pos <= len && n < (size_t)COL_MAX && n < max_fields) {
        size_t end = pos;
        line_slice name;
        int found = -1;
        while (end < len && text[end] != '\t') {
            end++;
        }
        name.p = text + pos;
        name.len = end - pos;
        name = trim_slice(name);
        for (k = 0; k < COL_MAX; ++k) {
            if (slice_is(&name, kDefaultColumns[k])) {
                found = k;
                break;
            }
        }
        ctx->columns[ctx->col_count++] = found;
        n++;
        if (end >= len) {
            break;
        }
        pos = end + 1u;
    }
}

static void apply_meta_kv(sxcl_modnames *t, const line_slice *key, const line_slice *val, parse_ctx *ctx)
{
    if (slice_is(key, "version")) {
        long v = 0;
        if (slice_to_long(val, &v) == 0) {
            t->meta.version = v;
        }
    } else if (slice_is(key, "count")) {
        long v = 0;
        if (slice_to_long(val, &v) == 0 && v >= 0) {
            t->meta.count = (size_t)v;
        }
    } else if (slice_is(key, "sha256")) {
        copy_slice(t->declared_sha, sizeof(t->declared_sha), val->p, val->len);
        copy_cap(t->meta.sha256, sizeof(t->meta.sha256), t->declared_sha);
    } else if (slice_is(key, "source")) {
        copy_slice(t->meta.source, sizeof(t->meta.source), val->p, val->len);
    } else if (slice_is(key, "license")) {
        copy_slice(t->meta.license, sizeof(t->meta.license), val->p, val->len);
    } else if (slice_is(key, "generated")) {
        copy_slice(t->meta.generated, sizeof(t->meta.generated), val->p, val->len);
    } else if (slice_is(key, "columns")) {
        apply_columns(ctx, val->p, val->len, MAX_FIELDS);
    } else {
        /* 认不出的表头键：**忽略**（云端将来加键不许把老客户端弄崩）。 */
    }
}

/** 正文起点：跳过 BOM、空行与 '#' 开头的行，第一个数据行的起点就是它。
 *  写表（生成器）与校验（这里）用**同一套**规则，"表头里的 sha256" 才不会自指。 */
static size_t body_offset(const char *text, size_t len)
{
    size_t pos = 0;
    if (text == NULL) {
        return 0u;
    }
    if (len >= 3u && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu) {
        pos = 3u;
    }
    while (pos < len) {
        size_t end = pos;
        size_t l;
        while (end < len && text[end] != '\n') {
            end++;
        }
        l = end - pos;
        if (l > 0u && text[pos + l - 1u] == '\r') {
            l--;
        }
        if (l > 0u && text[pos] != '#') {
            return pos; /* 第一个数据行 */
        }
        pos = (end < len) ? end + 1u : len;
    }
    return len;
}

int sxcl_modnames_body_sha256(const char *text, size_t len, char *out, size_t out_len)
{
    size_t off;
    if (out == NULL || out_len < SXCL_MODNAMES_SHA256_MAX) {
        return SXCL_MODNAMES_ERR_ARG;
    }
    out[0] = '\0';
    if (text == NULL) {
        len = 0u;
    }
    off = body_offset(text, len);
    if (sxcl_hash_digest(SXCL_HASH_SHA256, text + off, len - off, out, out_len) != 0) {
        return SXCL_MODNAMES_ERR_ARG;
    }
    return SXCL_MODNAMES_OK;
}

/** 一个待插槽的键（规范化后已进池，偏移 + 哈希先记着，等表读完再插）。 */
typedef struct pending_key {
    unsigned kind;
    size_t off;
    uint64_t hash;
} pending_key;

/** 把一个"可查键"规范化、进池、算哈希。没有可查内容返回 0（不算错），内存不够返回 -1。 */
static int prepare_key(sxcl_modnames *t, unsigned kind, const char *raw, size_t raw_len,
                       pending_key *out)
{
    char buf[KEY_NORM_MAX];
    char norm[KEY_NORM_MAX];
    size_t n;
    size_t off;
    if (raw == NULL || raw_len == 0u) {
        return 0;
    }
    copy_slice(buf, sizeof(buf), raw, raw_len);
    n = normalize_key(buf, norm, sizeof(norm));
    if (n == 0u) {
        return 0; /* 全是标点/空白：这个键没有意义，不建索引也不算错 */
    }
    off = pool_intern(t, norm, n);
    if (off == (size_t)-1) {
        return -1;
    }
    out->kind = kind;
    out->off = off;
    out->hash = hash_key(kind, norm, n);
    return 1;
}

int sxcl_modnames_parse(const char *text, size_t len, sxcl_modnames **out, char *err, size_t err_len)
{
    sxcl_modnames *t;
    parse_ctx ctx;
    const char *orig = text;
    size_t orig_len = len;
    size_t pos = 0;
    size_t ei;
    int first_content = 1;
    int have_magic = 0;
    int i;

    if (out == NULL) {
        set_err(err, err_len, "参数不合法（out 为空）");
        return SXCL_MODNAMES_ERR_ARG;
    }
    *out = NULL;
    if (text == NULL) {
        len = 0u;
    }
    if (len >= 3u && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu) {
        text += 3;
        len -= 3u;
    }
    t = (sxcl_modnames *)calloc(1u, sizeof(*t));
    if (t == NULL) {
        set_err(err, err_len, "内存不足");
        return SXCL_MODNAMES_ERR_NOMEM;
    }
    t->meta.format = SXCL_MODNAMES_FORMAT;
    /* 池的第 0 个字节是那个共享的空串：空字段全指它，不重复拷一份空串。 */
    if (pool_intern(t, "", 0u) == (size_t)-1) {
        set_err(err, err_len, "内存不足");
        sxcl_modnames_free(t);
        return SXCL_MODNAMES_ERR_NOMEM;
    }
    for (i = 0; i < COL_MAX; ++i) {
        ctx.columns[i] = i;
    }
    ctx.col_count = COL_MAX;

    while (pos < len) {
        size_t end = pos;
        line_slice line;
        while (end < len && text[end] != '\n') {
            end++;
        }
        line.p = text + pos;
        line.len = end - pos;
        if (line.len > 0u && line.p[line.len - 1u] == '\r') {
            line.len--;
        }
        pos = (end < len) ? end + 1u : len;

        if (line.len == 0u) {
            continue; /* 空行随便来 */
        }
        if (line.len > SXCL_MODNAMES_LINE_MAX) {
            t->bad_lines++;
            continue;
        }
        if (line.p[0] == '#') {
            line_slice key, val;
            parse_header_kv_local(line, &key, &val);
            if (first_content) {
                if (!slice_is(&key, SXCL_MODNAMES_MAGIC)) {
                    set_err(err, err_len, "这不是模组中文名表（缺表头 # sxcl-modnames）");
                    sxcl_modnames_free(t);
                    return SXCL_MODNAMES_ERR_FORMAT;
                }
                {
                    long fmt = SXCL_MODNAMES_FORMAT;
                    if (slice_to_long(&val, &fmt) == 0 && fmt > 0) {
                        t->meta.format = (int)fmt;
                    }
                }
                have_magic = 1;
                first_content = 0;
                continue;
            }
            apply_meta_kv(t, &key, &val, &ctx);
            continue;
        }
        if (!have_magic) {
            set_err(err, err_len, "这不是模组中文名表（第一行不是表头）");
            sxcl_modnames_free(t);
            return SXCL_MODNAMES_ERR_FORMAT;
        }
        first_content = 0;
        {
            line_slice f[MAX_FIELDS];
            line_slice v[COL_MAX];
            int have[COL_MAX];
            size_t nf = split_tabs(line.p, line.len, f, MAX_FIELDS);
            size_t off[COL_MAX];
            pending_key keys[IDX_KINDS];
            int key_count = 0;
            sxcl_modname_entry e;
            size_t idx;
            int c;
            int k;

            for (c = 0; c < COL_MAX; ++c) {
                have[c] = 0;
                v[c].p = NULL;
                v[c].len = 0u;
                off[c] = 0u;
            }
            for (i = 0; i < ctx.col_count && i < (int)nf; ++i) {
                const int col = ctx.columns[i];
                if (col >= 0 && col < COL_MAX) {
                    v[col] = trim_slice(f[i]);
                    have[col] = 1;
                }
            }
            t->meta.entries_lines++;
            if (!have[COL_NAME_ZH] || v[COL_NAME_ZH].len == 0u) {
                t->bad_lines++; /* 没有中文名 = 这一行没有意义 */
                continue;
            }
            /* 1) 字段原样进池。只留**偏移** —— 池一扩容就 realloc，指针现在算出来也是废的。 */
            for (c = 0; c < COL_MAX; ++c) {
                size_t o = 0u;
                if (have[c] && v[c].len > 0u) {
                    o = pool_intern(t, v[c].p, v[c].len);
                    if (o == (size_t)-1) {
                        set_err(err, err_len, "内存不足");
                        sxcl_modnames_free(t);
                        return SXCL_MODNAMES_ERR_NOMEM;
                    }
                }
                off[c] = o; /* 0 是池里那个共享的空串 */
            }
            /* 2) 索引键：规范化后也进池，同样只留偏移 + 哈希 */
            {
                static const unsigned kKinds[IDX_KINDS] = {IDX_SLUG, IDX_CF_ID, IDX_CF_SLUG,
                                                           IDX_MODID, IDX_NAME_EN, IDX_NAME_ZH};
                static const int kCols[IDX_KINDS] = {COL_SLUG, COL_CF_ID, COL_CF_SLUG,
                                                     COL_MODID, COL_NAME_EN, COL_NAME_ZH};
                for (k = 0; k < (int)IDX_KINDS; ++k) {
                    const int rc = prepare_key(t, kKinds[k], v[kCols[k]].p,
                                               have[kCols[k]] ? v[kCols[k]].len : 0u,
                                               &keys[key_count]);
                    if (rc < 0) {
                        set_err(err, err_len, "内存不足");
                        sxcl_modnames_free(t);
                        return SXCL_MODNAMES_ERR_NOMEM;
                    }
                    if (rc > 0) {
                        key_count++;
                    }
                }
            }
            /* 3) 占位一条（指针等整张表读完再统一算；cells 先留好容量，失败就不会错行） */
            if (cells_reserve(t, t->count + 1u) != 0) {
                t->bad_lines++;
                continue;
            }
            (void)memset(&e, 0, sizeof(e));
            idx = t->count;
            if (entry_push(t, &e) != 0) {
                t->bad_lines++;
                continue;
            }
            {
                /* 注意：off[] 是 size_t（8 字节），cells 一行是 COL_MAX 个 uint32_t（4 字节）——
                 * 直接 memcpy(sizeof(off)) 会把后面几行的格子踩烂（踩过：查出来的名字串到隔壁行）。 */
                uint32_t *row = t->cells + idx * (size_t)COL_MAX;
                for (c = 0; c < COL_MAX; ++c) {
                    row[c] = (uint32_t)off[c];
                }
            }
            /* 4) 插槽：只碰槽数组，不碰池 —— 偏移在这之后一直有效 */
            for (k = 0; k < key_count; ++k) {
                if (hash_insert_prepared(t, keys[k].kind, keys[k].off, keys[k].hash, idx) != 0) {
                    set_err(err, err_len, "内存不足");
                    sxcl_modnames_free(t);
                    return SXCL_MODNAMES_ERR_NOMEM;
                }
            }
        }
    }

    /* 池从这一刻起不会再动：把每一行的字段偏移统一换成指针（换完 cells 就可以扔了）。 */
    for (ei = 0; ei < t->count; ++ei) {
        const uint32_t *o = t->cells + ei * (size_t)COL_MAX;
        t->entries[ei].slug = t->pool + o[COL_SLUG];
        t->entries[ei].cf_id = t->pool + o[COL_CF_ID];
        t->entries[ei].cf_slug = t->pool + o[COL_CF_SLUG];
        t->entries[ei].modid = t->pool + o[COL_MODID];
        t->entries[ei].name_zh = t->pool + o[COL_NAME_ZH];
        t->entries[ei].name_en = t->pool + o[COL_NAME_EN];
        t->entries[ei].source = t->pool + o[COL_SOURCE];
    }
    free(t->cells);
    t->cells = NULL;
    t->cells_cap = 0u;

    if (t->meta.count != 0u && t->meta.count != t->count) {
        t->count_mismatch = 1;
    }
    if (t->declared_sha[0] != '\0') {
        char got[SXCL_MODNAMES_SHA256_MAX];
        if (sxcl_modnames_body_sha256(orig, orig_len, got, sizeof(got)) != SXCL_MODNAMES_OK ||
            !sxcl_hash_hex_equal(got, t->declared_sha)) {
            t->sha_mismatch = 1;
        }
    }
    *out = t;
    return SXCL_MODNAMES_OK;
}

int sxcl_modnames_load_file(const char *path, sxcl_modnames **out, char *err, size_t err_len)
{
    FILE *fh;
    long size;
    char *text;
    int rc;
    if (path == NULL || *path == '\0' || out == NULL) {
        set_err(err, err_len, "参数不合法（路径为空）");
        return SXCL_MODNAMES_ERR_ARG;
    }
    *out = NULL;
    fh = sxcl_fs_fopen(path, "rb");
    if (fh == NULL) {
        set_err(err, err_len, "读不到这份表（文件不存在或打不开）");
        return SXCL_MODNAMES_ERR_IO;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        set_err(err, err_len, "读不到这份表（长度取不到）");
        return SXCL_MODNAMES_ERR_IO;
    }
    size = ftell(fh);
    if (size < 0 || (unsigned long)size > MAX_TABLE_BYTES) {
        fclose(fh);
        set_err(err, err_len, "这份表太大或长度不可信，已放弃");
        return SXCL_MODNAMES_ERR_IO;
    }
    rewind(fh);
    text = (char *)malloc((size_t)size + 1u);
    if (text == NULL) {
        fclose(fh);
        set_err(err, err_len, "内存不足");
        return SXCL_MODNAMES_ERR_NOMEM;
    }
    if (size > 0 && fread(text, 1u, (size_t)size, fh) != (size_t)size) {
        free(text);
        fclose(fh);
        set_err(err, err_len, "这份表读了一半（截断）");
        return SXCL_MODNAMES_ERR_IO;
    }
    fclose(fh);
    text[size] = '\0';
    rc = sxcl_modnames_parse(text, (size_t)size, out, err, err_len);
    free(text);
    return rc;
}

void sxcl_modnames_free(sxcl_modnames *table)
{
    if (table == NULL) {
        return;
    }
    free(table->pool);
    free(table->entries);
    free(table->cells);
    free(table->slots);
    free(table);
}

const sxcl_modname_meta *sxcl_modnames_meta(const sxcl_modnames *table)
{
    static const sxcl_modname_meta kEmpty = {0, 0, 0u, {0}, {0}, {0}, {0}, 0u};
    if (table == NULL) {
        return &kEmpty;
    }
    return &table->meta;
}

size_t sxcl_modnames_count(const sxcl_modnames *table)
{
    return (table == NULL) ? 0u : table->count;
}

size_t sxcl_modnames_bad_lines(const sxcl_modnames *table)
{
    return (table == NULL) ? 0u : table->bad_lines;
}

const sxcl_modname_entry *sxcl_modnames_entry(const sxcl_modnames *table, size_t index)
{
    if (table == NULL || index >= table->count) {
        return NULL;
    }
    return &table->entries[index];
}

int sxcl_modnames_self_consistent(const sxcl_modnames *table)
{
    if (table == NULL) {
        return 0;
    }
    if (table->count_mismatch || table->sha_mismatch) {
        return 0;
    }
    return 1;
}

/* ── 查表 ── */

/** 索引查询：规范化 -> 哈希 -> 线性探测。**只走 O(1) 条槽位**（这就是"不许退化成线性扫"的落地）。
 *  last_probes 只是为了能在单测里把"走了几个槽"断言出来（诊断字段，改了不影响语义）。 */
static const char *lookup_kind(const sxcl_modnames *table, unsigned kind, const char *value)
{
    char norm[KEY_NORM_MAX];
    size_t n;
    uint64_t h;
    size_t at, mask;
    unsigned probes = 0;

    if (table == NULL || value == NULL || table->slot_cap == 0u) {
        if (table != NULL) {
            ((sxcl_modnames *)table)->last_probes = 0u;
        }
        return NULL;
    }
    n = normalize_key(value, norm, sizeof(norm));
    if (n == 0u) {
        ((sxcl_modnames *)table)->last_probes = 0u;
        return NULL;
    }
    h = hash_key(kind, norm, n);
    mask = table->slot_cap - 1u;
    at = (size_t)(h & (uint64_t)mask);
    for (;;) {
        const modname_slot *s = &table->slots[at];
        probes++;
        if (s->hash == 0u) {
            break;
        }
        if (s->hash == h && s->kind == kind && strcmp(table->pool + s->key_off, norm) == 0) {
            const char *zh = table->entries[s->entry].name_zh;
            ((sxcl_modnames *)table)->last_probes = probes;
            return (zh != NULL && zh[0] != '\0') ? zh : NULL;
        }
        at = (at + 1u) & mask;
        if ((size_t)probes > table->slot_cap) {
            break; /* 理论上到不了：满表也留了空槽。防死循环。 */
        }
    }
    ((sxcl_modnames *)table)->last_probes = probes;
    return NULL;
}

const char *sxcl_modnames_lookup_slug(const sxcl_modnames *table, const char *slug)
{
    return lookup_kind(table, IDX_SLUG, slug);
}

const char *sxcl_modnames_lookup_cf_id(const sxcl_modnames *table, const char *cf_id)
{
    return lookup_kind(table, IDX_CF_ID, cf_id);
}

const char *sxcl_modnames_lookup_cf_slug(const sxcl_modnames *table, const char *cf_slug)
{
    return lookup_kind(table, IDX_CF_SLUG, cf_slug);
}

const char *sxcl_modnames_lookup_modid(const sxcl_modnames *table, const char *modid)
{
    return lookup_kind(table, IDX_MODID, modid);
}

const char *sxcl_modnames_lookup_name(const sxcl_modnames *table, const char *name)
{
    const char *hit = lookup_kind(table, IDX_NAME_EN, name);
    if (hit == NULL) {
        hit = lookup_kind(table, IDX_NAME_ZH, name);
    }
    return hit;
}

const char *sxcl_modnames_lookup(const sxcl_modnames *table, const char *slug, const char *cf_id,
                                 const char *name)
{
    const char *hit;
    if (table == NULL) {
        return NULL;
    }
    hit = sxcl_modnames_lookup_slug(table, slug); /* Modrinth 那一列的 slug 优先 */
    if (hit != NULL) {
        return hit;
    }
    hit = sxcl_modnames_lookup_cf_slug(table, slug); /* 调用方给的是 CF 的 slug 时也认 */
    if (hit != NULL) {
        return hit;
    }
    hit = sxcl_modnames_lookup_cf_id(table, cf_id);
    if (hit != NULL) {
        return hit;
    }
    return sxcl_modnames_lookup_name(table, name);
}

int sxcl_modnames_apply(const sxcl_modnames *table, const char *slug, const char *cf_id,
                        const char *name, char *out, size_t out_len)
{
    const char *hit;
    if (out == NULL || out_len == 0u) {
        return -1;
    }
    hit = sxcl_modnames_lookup(table, slug, cf_id, name);
    if (hit == NULL || hit[0] == '\0') {
        return 0; /* **一个字都不写**：调用方传进来的原名原样留着 */
    }
    copy_cap(out, out_len, hit);
    return 1;
}

unsigned sxcl_modnames_last_probes(const sxcl_modnames *table)
{
    return (table == NULL) ? 0u : table->last_probes;
}

size_t sxcl_modnames_bucket_count(const sxcl_modnames *table)
{
    return (table == NULL) ? 0u : table->slot_cap;
}

/* ── 路径 ── */

static int join_two(const char *root, const char *suffix, char *out, size_t out_len)
{
    size_t n;
    if (root == NULL || *root == '\0' || suffix == NULL || out == NULL || out_len == 0u) {
        return SXCL_MODNAMES_ERR_ARG;
    }
    n = strlen(root);
    while (n > 0u && (root[n - 1u] == '/' || root[n - 1u] == '\\')) {
        n--;
    }
    if (n == 0u) {
        return SXCL_MODNAMES_ERR_ARG;
    }
    {
        const size_t slen = strlen(suffix);
        if (n + slen + 1u > out_len) {
            return SXCL_MODNAMES_ERR_ARG;
        }
        (void)memcpy(out, root, n);
        (void)memcpy(out + n, suffix, slen + 1u); /* 含 NUL；上面已经算过长度，不会溢出 */
    }
    return SXCL_MODNAMES_OK;
}

int sxcl_modnames_bundled_path(const char *asset_root, char *out, size_t out_len)
{
    const char *env;
    if (out == NULL || out_len == 0u) {
        return SXCL_MODNAMES_ERR_ARG;
    }
    out[0] = '\0';
    env = getenv("SXCL_MODNAMES_FILE");
    if (env != NULL && *env != '\0') {
        copy_cap(out, out_len, env);
        return SXCL_MODNAMES_OK;
    }
    return join_two(asset_root, "/data/modnames.tsv", out, out_len);
}

int sxcl_modnames_cache_path(const char *cache_dir, char *out, size_t out_len)
{
    return join_two(cache_dir, "/meta/modnames.tsv", out, out_len);
}

int sxcl_modnames_remote_url(const char *base, char *out, size_t out_len)
{
    return join_two(base, "/meta/modnames.tsv", out, out_len);
}

int sxcl_modnames_cache_write(const char *cache_dir, const char *text, size_t len, char *out_path,
                              size_t out_path_len, char *err, size_t err_len)
{
    char final_path[PATH_MAX_BYTES];
    char part_path[PATH_MAX_BYTES + 8];
    FILE *fh;
    if (text == NULL || sxcl_modnames_cache_path(cache_dir, final_path, sizeof(final_path)) != 0) {
        set_err(err, err_len, "缓存目录不合法");
        return SXCL_MODNAMES_ERR_ARG;
    }
    if (strlen(final_path) + 6u > sizeof(part_path)) {
        set_err(err, err_len, "缓存目录太长");
        return SXCL_MODNAMES_ERR_ARG;
    }
    (void)snprintf(part_path, sizeof(part_path), "%s.part", final_path);
    if (sxcl_fs_mkdirs_for_file(final_path) != 0) {
        set_err(err, err_len, "缓存目录建不出来");
        return SXCL_MODNAMES_ERR_IO;
    }
    fh = sxcl_fs_fopen(part_path, "wb");
    if (fh == NULL) {
        set_err(err, err_len, "缓存写不进去（打不开临时文件）");
        return SXCL_MODNAMES_ERR_IO;
    }
    if (len > 0u && fwrite(text, 1u, len, fh) != len) {
        fclose(fh);
        (void)sxcl_fs_remove(part_path);
        set_err(err, err_len, "缓存写了一半（磁盘满？）");
        return SXCL_MODNAMES_ERR_IO;
    }
    if (fclose(fh) != 0) {
        (void)sxcl_fs_remove(part_path);
        set_err(err, err_len, "缓存收尾失败");
        return SXCL_MODNAMES_ERR_IO;
    }
    if (sxcl_fs_rename_replace(part_path, final_path) != 0) {
        (void)sxcl_fs_remove(part_path);
        set_err(err, err_len, "缓存替换失败（旧的那份没动）");
        return SXCL_MODNAMES_ERR_IO;
    }
    if (out_path != NULL && out_path_len > 0u) {
        copy_cap(out_path, out_path_len, final_path);
    }
    return SXCL_MODNAMES_OK;
}

/* ── 组装：先缓存（校验过的新版）再随包 ── */

static sxcl_modnames *empty_table(void)
{
    return (sxcl_modnames *)calloc(1u, sizeof(sxcl_modnames));
}

int sxcl_modnames_load_cached(const char *bundled_path, const char *cache_dir, sxcl_modnames **out,
                              sxcl_modnames_source *source, char *err, size_t err_len)
{
    sxcl_modnames *bundled = NULL;
    sxcl_modnames *cached = NULL;
    char cache_path[PATH_MAX_BYTES];
    char local_err[SXCL_MODNAMES_ERR_MAX];
    int bundled_bad = 0;

    if (out == NULL) {
        set_err(err, err_len, "参数不合法（out 为空）");
        return SXCL_MODNAMES_ERR_ARG;
    }
    *out = NULL;
    if (source != NULL) {
        (void)memset(source, 0, sizeof(*source));
    }
    local_err[0] = '\0';

    if (bundled_path != NULL && *bundled_path != '\0' && sxcl_fs_exists(bundled_path) == 1) {
        if (sxcl_modnames_load_file(bundled_path, &bundled, local_err, sizeof(local_err)) !=
            SXCL_MODNAMES_OK) {
            bundled = NULL;
            bundled_bad = 1;
        }
    }

    if (cache_dir != NULL && *cache_dir != '\0' &&
        sxcl_modnames_cache_path(cache_dir, cache_path, sizeof(cache_path)) == SXCL_MODNAMES_OK &&
        sxcl_fs_exists(cache_path) == 1) {
        if (sxcl_modnames_load_file(cache_path, &cached, local_err, sizeof(local_err)) !=
            SXCL_MODNAMES_OK) {
            cached = NULL;
            if (source != NULL) {
                source->cache_rejected = 1;
            }
        } else if (!sxcl_modnames_self_consistent(cached)) {
            sxcl_modnames_free(cached);
            cached = NULL;
            if (source != NULL) {
                source->cache_rejected = 1;
            }
        } else if (cached->meta.version < 1) {
            /* 没有版本号的缓存：无法判断新旧，**不用**（宁可回随包那份）。 */
            sxcl_modnames_free(cached);
            cached = NULL;
            if (source != NULL) {
                source->cache_rejected = 1;
            }
        } else if (bundled != NULL && bundled->meta.version >= cached->meta.version) {
            sxcl_modnames_free(cached);
            cached = NULL;
            if (source != NULL) {
                source->cache_rejected = 1;
            }
        }
    }

    if (cached != NULL) {
        sxcl_modnames_free(bundled);
        *out = cached;
        if (source != NULL) {
            source->from_cache = 1;
            (void)snprintf(source->note, sizeof(source->note),
                           "用的是云端更新的那份（version %ld，%lu 条）", cached->meta.version,
                           (unsigned long)cached->count);
        }
        return SXCL_MODNAMES_OK;
    }

    if (bundled != NULL) {
        *out = bundled;
        if (source != NULL) {
            source->from_bundled = 1;
            (void)snprintf(source->note, sizeof(source->note),
                           "用的是随包那份（version %ld，%lu 条%s）", bundled->meta.version,
                           (unsigned long)bundled->count,
                           (source->cache_rejected != 0) ? "；缓存那份被否掉了" : "");
        }
        return SXCL_MODNAMES_OK;
    }

    *out = empty_table();
    if (*out == NULL) {
        set_err(err, err_len, "内存不足");
        return SXCL_MODNAMES_ERR_NOMEM;
    }
    if (source != NULL) {
        source->empty = 1;
        (void)snprintf(source->note, sizeof(source->note), "%s",
                       bundled_bad != 0
                           ? "随包那份读不出来，也没有可用的缓存 —— 这次没有中文名（模组名照旧）"
                           : "没有中文名表 —— 这次没有中文名（模组名照旧）");
    }
    return SXCL_MODNAMES_OK;
}

/* ── 云端更新 ── */

/** 从一段（可能只有前几 KB 的）表头里把 version 读出来。读不到返回 -1。 */
static long meta_version_of_head(const char *text, size_t len)
{
    size_t pos = 0;
    parse_ctx ctx;
    long version = -1L;
    int first = 1;
    int i;

    if (text == NULL) {
        return -1L;
    }
    if (len >= 3u && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu) {
        pos = 3u;
    }
    for (i = 0; i < COL_MAX; ++i) {
        ctx.columns[i] = i;
    }
    ctx.col_count = COL_MAX;

    while (pos < len) {
        size_t end = pos;
        line_slice line, key, val;
        while (end < len && text[end] != '\n') {
            end++;
        }
        line.p = text + pos;
        line.len = end - pos;
        if (line.len > 0u && line.p[line.len - 1u] == '\r') {
            line.len--;
        }
        pos = (end < len) ? end + 1u : len;
        if (line.len == 0u) {
            continue;
        }
        if (line.p[0] != '#') {
            break; /* 到数据区了：表头结束 */
        }
        parse_header_kv_local(line, &key, &val);
        if (first) {
            if (!slice_is(&key, SXCL_MODNAMES_MAGIC)) {
                return -1L;
            }
            first = 0;
            continue;
        }
        if (slice_is(&key, "version")) {
            long v = 0;
            if (slice_to_long(&val, &v) == 0) {
                version = v;
            }
        }
    }
    return version;
}

int sxcl_modnames_update_from_cloud(sxcl_transport *tr, const char *base, const char *cache_dir,
                                    const sxcl_modnames *current, sxcl_modnames_update *result,
                                    char *err, size_t err_len)
{
    char url[PATH_MAX_BYTES];
    char *body = NULL;
    size_t body_len = 0u;
    sxcl_http_opts opts;
    const sxcl_modname_meta *m;
    sxcl_modnames *fresh = NULL;
    int status = 0;
    int rc;

    if (result != NULL) {
        (void)memset(result, 0, sizeof(*result));
    }
    if (base == NULL || *base == '\0' || cache_dir == NULL || *cache_dir == '\0') {
        set_err(err, err_len, "云端基址或缓存目录没给");
        return SXCL_MODNAMES_ERR_ARG;
    }
    if (sxcl_modnames_remote_url(base, url, sizeof(url)) != SXCL_MODNAMES_OK) {
        set_err(err, err_len, "云端基址不合法（太长或为空）");
        return SXCL_MODNAMES_ERR_ARG;
    }
    if (tr == NULL) {
        if (result != NULL) {
            set_note(result->note, sizeof(result->note), "没有传输后端：这次不联网，继续用现有那份");
        }
        set_err(err, err_len, "没有传输后端");
        return SXCL_MODNAMES_OK;
    }

    (void)memset(&opts, 0, sizeof(opts));
    opts.max_bytes = CLOUD_MAX_BYTES;
    opts.status_out = &status;
    opts.timeout_ms = 10000;

    rc = sxcl_http_get_text_ex(tr, url, NULL, &opts, &body, &body_len, err, err_len);
    if (result != NULL) {
        result->status = status;
    }
    if (rc != SXCL_HTTP_OK) {
        free(body);
        if (result != NULL) {
            set_note(result->note, sizeof(result->note), "取云端那张表失败，继续用现有那份");
        }
        return SXCL_MODNAMES_OK; /* 拿不到不是错误：docs/28 的口径 */
    }
    if (sxcl_modnames_parse(body, body_len, &fresh, err, err_len) != SXCL_MODNAMES_OK) {
        free(body);
        if (result != NULL) {
            set_note(result->note, sizeof(result->note), "云端那张表不是我们的表，继续用现有那份");
        }
        return SXCL_MODNAMES_OK;
    }
    m = sxcl_modnames_meta(fresh);
    if (m->sha256[0] == '\0' || m->count == 0u) {
        if (result != NULL) {
            set_note(result->note, sizeof(result->note),
                     "云端那张表没带 sha256/条数：拒（没法确认完整性）");
        }
        sxcl_modnames_free(fresh);
        free(body);
        return SXCL_MODNAMES_OK;
    }
    if (!sxcl_modnames_self_consistent(fresh)) {
        if (result != NULL) {
            set_note(result->note, sizeof(result->note),
                     "云端那张表的 sha256/条数与正文对不上：拒（缓存原样不动）");
        }
        sxcl_modnames_free(fresh);
        free(body);
        return SXCL_MODNAMES_OK;
    }
    if (m->version < 1L) {
        if (result != NULL) {
            set_note(result->note, sizeof(result->note), "云端那张表没带 version：拒");
        }
        sxcl_modnames_free(fresh);
        free(body);
        return SXCL_MODNAMES_OK;
    }
    if (current != NULL && sxcl_modnames_meta(current)->version >= m->version) {
        if (result != NULL) {
            (void)snprintf(result->note, sizeof(result->note),
                           "云端 version=%ld 没有比手上的 %ld 新，不换", m->version,
                           sxcl_modnames_meta(current)->version);
        }
        sxcl_modnames_free(fresh);
        free(body);
        return SXCL_MODNAMES_OK;
    }
    if (sxcl_modnames_cache_write(cache_dir, body, body_len, NULL, 0u, err, err_len) !=
        SXCL_MODNAMES_OK) {
        sxcl_modnames_free(fresh);
        free(body);
        return SXCL_MODNAMES_ERR_IO;
    }
    if (result != NULL) {
        result->updated = 1;
        result->version = m->version;
        result->count = fresh->count;
        copy_cap(result->sha256, sizeof(result->sha256), m->sha256);
        (void)snprintf(result->note, sizeof(result->note),
                       "已换成云端那份（version %ld，%lu 条）", m->version,
                       (unsigned long)fresh->count);
    }
    sxcl_modnames_free(fresh);
    free(body);
    return SXCL_MODNAMES_OK;
}

int sxcl_modnames_probe_remote(sxcl_transport *tr, const char *base, const sxcl_modnames *current,
                               long *out_version, char *err, size_t err_len)
{
    char url[PATH_MAX_BYTES];
    const char *headers[3];
    sxcl_http_opts opts;
    char *body = NULL;
    size_t body_len = 0u;
    long version;
    int status = 0;
    int rc;

    if (out_version != NULL) {
        *out_version = 0L;
    }
    if (tr == NULL || base == NULL || *base == '\0' || out_version == NULL) {
        set_err(err, err_len, "参数不合法（没有传输后端或没给基址）");
        return SXCL_MODNAMES_ERR_ARG;
    }
    if (sxcl_modnames_remote_url(base, url, sizeof(url)) != SXCL_MODNAMES_OK) {
        set_err(err, err_len, "云端基址不合法");
        return SXCL_MODNAMES_ERR_ARG;
    }
    headers[0] = "Range: bytes=0-4095";
    headers[1] = "Accept-Encoding: identity";
    headers[2] = NULL;
    (void)memset(&opts, 0, sizeof(opts));
    opts.max_bytes = 8192u;
    opts.status_out = &status;
    opts.timeout_ms = 8000;
    rc = sxcl_http_get_text_ex(tr, url, headers, &opts, &body, &body_len, err, err_len);
    if (rc != SXCL_HTTP_OK) {
        free(body);
        /* 云端不兑现 Range（整份回来超了上限）或网络不通：都只是"不知道"，调用方走整表那条路。 */
        return -1;
    }
    version = meta_version_of_head(body, body_len);
    free(body);
    if (version < 0L) {
        set_err(err, err_len, "云端返回的不是我们的表（或这段里没有表头）");
        return -1;
    }
    *out_version = version;
    if (current != NULL && sxcl_modnames_meta(current)->version >= version) {
        return 1; /* 不用更新 */
    }
    return 0; /* 值得走一次整表更新 */
}
