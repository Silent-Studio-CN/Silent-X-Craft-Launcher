/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/hashcache.h"

#include "sxcl/fs.h"
#include "../internal/platform_lock.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* 仅为了用 _wfopen_s 打开 UTF-8 路径 */
#endif

/* ── 可调参数 ── */
#define SXCL_HC_MAX_ENTRIES ((size_t)20000u)   /* 条目数上限,超过先 prune 再清空 */
#define SXCL_HC_MIN_CAP     ((size_t)64u)      /* 初始槽位数(必须是 2 的幂) */
#define SXCL_HC_LOAD_NUM    ((size_t)7u)       /* 装载因子 = 7/10 */
#define SXCL_HC_LOAD_DEN    ((size_t)10u)
#define SXCL_HC_VERSION_LINE "#sxcl-hashcache 1"
#define SXCL_HC_MAX_HEX     ((size_t)128u)     /* 摘要字段最长接受多少字符 */
#define SXCL_HC_NUM_BUF     ((size_t)32u)      /* 格式化 int64 的临时缓冲 */
#define SXCL_HC_MAX_FILE    ((size_t)64u * 1024u * 1024u)  /* 缓存文件读取上限 */

/* ── 数据结构 ── */

typedef struct hc_entry {
    char *path;             /* 文件路径(UTF-8),NULL 表示空槽 */
    char *hex;              /* 摘要的十六进制串 */
    int64_t size;           /* 键里的 size */
    int64_t mtime_ns;       /* 键里的 mtime_ns */
    uint64_t hash;          /* FNV-1a("path|size|mtime_ns"),比对前先比它 */
} hc_entry;

struct sxcl_hash_cache {
    hc_entry *slots;        /* 开放寻址表,cap 为 2 的幂 */
    size_t cap;             /* 槽位数(0 表示还没建表) */
    size_t count;           /* 有效条目数 */
    char *path;             /* 缓存文件路径,NULL/空 = 纯内存缓存 */
    int dirty;              /* 有未落盘的改动 */
    sxcl_lock_t lock;       /* 一把锁保护上面所有字段 */
};

/* ── 小工具 ── */

static char *hc_strdup(const char *s)
{
    const size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static uint64_t hc_fnv1a(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;              /* FNV-1a 64 位质数 */
    }
    return h;
}

/* key = "path|size|mtime_ns" 的 FNV-1a(与 Python 版键语义一致,只是不存整串)。 */
static uint64_t hc_key_hash(const char *path, int64_t size, int64_t mtime_ns)
{
    char num[SXCL_HC_NUM_BUF];
    int n = 0;
    uint64_t h = 14695981039346656037ULL;   /* FNV-1a 64 位偏移基 */
    h = hc_fnv1a(h, path, strlen(path));
    h = hc_fnv1a(h, "|", 1u);
    n = snprintf(num, sizeof num, "%" PRId64, size);
    if (n > 0) {
        h = hc_fnv1a(h, num, (size_t)n);
    }
    h = hc_fnv1a(h, "|", 1u);
    n = snprintf(num, sizeof num, "%" PRId64, mtime_ns);
    if (n > 0) {
        h = hc_fnv1a(h, num, (size_t)n);
    }
    return h;
}

static void hc_entry_dispose(hc_entry *e)
{
    free(e->path);
    free(e->hex);
    e->path = NULL;
    e->hex = NULL;
    e->size = 0;
    e->mtime_ns = 0;
    e->hash = 0;
}

/* 线性探测:命中返回该槽下标,未命中返回第一个空槽下标。cap 必须非 0。 */
static size_t hc_probe(const hc_entry *slots, size_t cap, const char *path, int64_t size,
                       int64_t mtime_ns, uint64_t hash)
{
    const size_t mask = cap - 1u;
    size_t i = (size_t)(hash & (uint64_t)mask);
    while (slots[i].path != NULL) {
        if (slots[i].hash == hash && slots[i].size == size && slots[i].mtime_ns == mtime_ns &&
            strcmp(slots[i].path, path) == 0) {
            return i;
        }
        i = (i + 1u) & mask;
    }
    return i;
}

/* ── 表操作(调用方必须持锁;除 open 期间的载入外) ── */

/* 保证还能放下 want 条;装不下就翻倍重建。返回 0 成功,-1 表示内存不足。 */
static int hc_table_reserve(sxcl_hash_cache *c, size_t want)
{
    if (c->cap != 0u && want * SXCL_HC_LOAD_DEN <= c->cap * SXCL_HC_LOAD_NUM) {
        return 0;
    }
    size_t ncap = (c->cap != 0u) ? c->cap : SXCL_HC_MIN_CAP;
    while (want * SXCL_HC_LOAD_DEN > ncap * SXCL_HC_LOAD_NUM) {
        if (ncap > (SIZE_MAX / 2u)) {
            return -1;
        }
        ncap *= 2u;
    }
    if (ncap == c->cap) {
        return 0;
    }
    hc_entry *ns = (hc_entry *)calloc(ncap, sizeof *ns);
    if (!ns) {
        return -1;
    }
    for (size_t i = 0; i < c->cap; ++i) {
        const hc_entry *e = &c->slots[i];
        if (e->path == NULL) {
            continue;
        }
        const size_t at = hc_probe(ns, ncap, e->path, e->size, e->mtime_ns, e->hash);
        ns[at] = *e;                        /* 指针搬过去,不做拷贝 */
    }
    free(c->slots);
    c->slots = ns;
    c->cap = ncap;
    return 0;
}

/* 插入或覆盖一条。成功返回 0,-1 表示内存不足(此时表内容不变)。 */
static int hc_table_put(sxcl_hash_cache *c, const char *path, int64_t size, int64_t mtime_ns,
                        const char *hex)
{
    if (hc_table_reserve(c, c->count + 1u) != 0) {
        return -1;
    }
    const uint64_t hash = hc_key_hash(path, size, mtime_ns);
    const size_t idx = hc_probe(c->slots, c->cap, path, size, mtime_ns, hash);
    if (c->slots[idx].path != NULL) {        /* 同键:只换摘要 */
        char *nh = hc_strdup(hex);
        if (!nh) {
            return -1;
        }
        free(c->slots[idx].hex);
        c->slots[idx].hex = nh;
        c->dirty = 1;
        return 0;
    }
    char *np = hc_strdup(path);
    char *nh = hc_strdup(hex);
    if (!np || !nh) {
        free(np);
        free(nh);
        return -1;
    }
    c->slots[idx].path = np;
    c->slots[idx].hex = nh;
    c->slots[idx].size = size;
    c->slots[idx].mtime_ns = mtime_ns;
    c->slots[idx].hash = hash;
    c->count += 1u;
    c->dirty = 1;
    return 0;
}

/* 删掉 idx 槽:后移填补(搬过来的条目在新位置依然能被探测到),不留墓碑。 */
static void hc_table_erase(sxcl_hash_cache *c, size_t idx)
{
    const size_t mask = c->cap - 1u;
    hc_entry dead = c->slots[idx];           /* 按值留一份,循环里不再碰这个槽的数据 */
    size_t hole = idx;
    size_t scan = idx;
    for (size_t step = 1u; step <= mask; ++step) {
        scan = (scan + 1u) & mask;
        if (c->slots[scan].path == NULL) {
            break;                           /* 探测链断了,后面不用看 */
        }
        const size_t home = (size_t)(c->slots[scan].hash & (uint64_t)mask);
        /* home 落在环上 (hole, scan] 之内 => 搬到 hole 会让它找不回来,跳过 */
        const int blocked = (hole < scan) ? (home > hole && home <= scan)
                                          : (home > hole || home <= scan);
        if (blocked) {
            continue;
        }
        c->slots[hole] = c->slots[scan];
        hole = scan;
    }
    /* hole 处的数据已经搬到前面(或本来就空),只清指针;真正要释放的是 dead。 */
    c->slots[hole].path = NULL;
    c->slots[hole].hex = NULL;
    c->slots[hole].size = 0;
    c->slots[hole].mtime_ns = 0;
    c->slots[hole].hash = 0;
    hc_entry_dispose(&dead);
    c->count -= 1u;
    c->dirty = 1;
}

/* 整体清空(保留已分配的槽位,省得下次再重建)。 */
static void hc_table_clear(sxcl_hash_cache *c)
{
    for (size_t i = 0; i < c->cap; ++i) {
        if (c->slots[i].path != NULL) {
            hc_entry_dispose(&c->slots[i]);
        }
    }
    c->count = 0u;
    c->dirty = 1;
}

/* 删掉"文件已不在"的条目,返回删除条数。调用方持锁。 */
static size_t hc_prune_locked(sxcl_hash_cache *c)
{
    size_t removed = 0;
    size_t i = 0;
    while (i < c->cap) {
        if (c->slots[i].path == NULL) {
            ++i;
            continue;
        }
        if (sxcl_fs_exists(c->slots[i].path)) {
            ++i;
            continue;
        }
        hc_table_erase(c, i);
        ++removed;
        /* 不前进:i 处现在是后移过来的条目或空槽,得重新判一次 */
    }
    return removed;
}

/* ── 文件读写(UTF-8 路径) ── */

static FILE *hc_fopen_utf8(const char *path, const char *mode)
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
    wchar_t wmode[8];
    FILE *f = NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, need) == need) {
        const size_t n = strlen(mode);
        size_t i = 0;
        for (; i < n && i + 1u < (sizeof wmode / sizeof wmode[0]); ++i) {
            wmode[i] = (wchar_t)(unsigned char)mode[i];
        }
        wmode[i] = L'\0';
        if (_wfopen_s(&f, wide, wmode) != 0) {   /* _wfopen 会触发 C4996,/WX 下必须用 _s 版 */
            f = NULL;
        }
    }
    free(wide);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int hc_hex_ok(const char *s, size_t len)
{
    if (len == 0u || len > SXCL_HC_MAX_HEX) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        const char ch = s[i];
        const int digit = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                          (ch >= 'A' && ch <= 'F');
        if (!digit) {
            return 0;
        }
    }
    return 1;
}

/* 严格十进制 int64:整个字段必须合法(不允许空格、正号、前导零以外的花活)。 */
static int hc_parse_i64(const char *s, size_t len, int64_t *out)
{
    if (len == 0u || len > 20u) {
        return 0;
    }
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1u;
        if (len == 1u) {
            return 0;
        }
    }
    uint64_t v = 0;
    for (; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        const uint64_t d = (uint64_t)(unsigned)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
    }
    if (neg) {
        const uint64_t lim = (uint64_t)INT64_MAX + 1u;
        if (v > lim) {
            return 0;
        }
        *out = (v == lim) ? INT64_MIN : -(int64_t)v;
    } else {
        if (v > (uint64_t)INT64_MAX) {
            return 0;
        }
        *out = (int64_t)v;
    }
    return 1;
}

/* 解析一行 "hex<TAB>size<TAB>mtime_ns<TAB>path"(line 可写,len 是不含换行的长度;
 * 成功返回 1,并把 hex/path 就地 NUL 截断后指出来)。 */
static int hc_parse_line(char *line, size_t len, const char **hex, int64_t *size,
                         int64_t *mtime_ns, const char **path)
{
    size_t t1 = (size_t)-1;
    size_t t2 = (size_t)-1;
    size_t t3 = (size_t)-1;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != '\t') {
            continue;
        }
        if (t1 == (size_t)-1) {
            t1 = i;
        } else if (t2 == (size_t)-1) {
            t2 = i;
        } else {
            t3 = i;
            break;
        }
    }
    if (t3 == (size_t)-1) {                  /* 少字段 */
        return 0;
    }
    if (!hc_hex_ok(line, t1)) {
        return 0;
    }
    if (!hc_parse_i64(line + t1 + 1u, t2 - t1 - 1u, size)) {
        return 0;
    }
    if (!hc_parse_i64(line + t2 + 1u, t3 - t2 - 1u, mtime_ns)) {
        return 0;
    }
    if (len - t3 - 1u == 0u) {               /* 空路径 */
        return 0;
    }
    line[t1] = '\0';                         /* hex 在这里结束 */
    line[len] = '\0';                        /* 原来放换行的位置,path 在这里结束 */
    *hex = line;
    *path = line + t3 + 1u;
    return 1;
}

/* 读盘。任何异常都只是"少读到几条",不会失败、不会崩。 */
static void hc_load(sxcl_hash_cache *c, const char *path)
{
    FILE *f = hc_fopen_utf8(path, "rb");
    if (!f) {
        return;                              /* 不存在/打不开 = 空缓存 */
    }
    size_t cap = 64u * 1024u;
    size_t len = 0;
    char *buf = (char *)malloc(cap + 1u);
    if (!buf) {
        fclose(f);
        return;
    }
    for (;;) {
        if (len >= SXCL_HC_MAX_FILE) {
            break;                           /* 异常大的缓存文件别整个吃进内存 */
        }
        if (len == cap) {
            const size_t ncap = cap * 2u;
            char *nb = (char *)realloc(buf, ncap + 1u);
            if (!nb) {
                break;                       /* 内存不足:用已经读到的部分 */
            }
            buf = nb;
            cap = ncap;
        }
        const size_t got = fread(buf + len, 1u, cap - len, f);
        if (got == 0u) {
            break;                           /* EOF 或读错误:按读到的算 */
        }
        len += got;
    }
    fclose(f);
    buf[len] = '\0';

    /* 第一行:版本标记。不认识的版本(含未来版本)= 整份当空缓存。 */
    const char *nl = (const char *)memchr(buf, '\n', len);
    if (!nl) {
        free(buf);
        return;                              /* 连第一行都不完整 */
    }
    size_t first = (size_t)(nl - buf);
    if (first > 0u && buf[first - 1u] == '\r') {
        --first;
    }
    const size_t vlen = strlen(SXCL_HC_VERSION_LINE);
    if (first != vlen || memcmp(buf, SXCL_HC_VERSION_LINE, vlen) != 0) {
        free(buf);
        return;
    }

    const int was_dirty = c->dirty;
    size_t pos = (size_t)(nl - buf) + 1u;
    while (pos < len && c->count < SXCL_HC_MAX_ENTRIES) {
        const char *end = (const char *)memchr(buf + pos, '\n', len - pos);
        if (!end) {
            break;                           /* 末行没有换行 = 半截内容,丢掉 */
        }
        char *line = buf + pos;
        size_t llen = (size_t)(end - buf) - pos;
        pos = (size_t)(end - buf) + 1u;
        if (llen > 0u && line[llen - 1u] == '\r') {
            line[--llen] = '\0';
        }
        if (llen == 0u) {
            continue;
        }
        const char *hex = NULL;
        const char *p = NULL;
        int64_t size = 0;
        int64_t mtime_ns = 0;
        if (!hc_parse_line(line, llen, &hex, &size, &mtime_ns, &p)) {
            continue;                        /* 坏行忽略,不影响其它行 */
        }
        (void)hc_table_put(c, p, size, mtime_ns, hex);
    }
    c->dirty = was_dirty;                    /* 只是读盘,不算改动 */
    free(buf);
}

/* 把整张表写成文本(不负责改名)。返回 0 成功。 */
static int hc_write_all(const sxcl_hash_cache *c, const char *tmp)
{
    FILE *f = hc_fopen_utf8(tmp, "wb");
    if (!f) {
        return -1;
    }
    int ok = (fputs(SXCL_HC_VERSION_LINE "\n", f) >= 0) ? 1 : 0;
    for (size_t i = 0; ok && i < c->cap; ++i) {
        const hc_entry *e = &c->slots[i];
        if (e->path == NULL) {
            continue;
        }
        if (strpbrk(e->path, "\t\n") != NULL) {
            continue;                        /* 路径里有 TAB/换行:写了会毁掉整份文件 */
        }
        if (fprintf(f, "%s\t%" PRId64 "\t%" PRId64 "\t%s\n", e->hex, e->size, e->mtime_ns,
                    e->path) < 0) {
            ok = 0;
        }
    }
    if (ok && fflush(f) != 0) {
        ok = 0;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* ── 公开 API ── */

sxcl_hash_cache *sxcl_hash_cache_open(const char *path)
{
    sxcl_hash_cache *c = (sxcl_hash_cache *)calloc(1, sizeof *c);
    if (!c) {
        return NULL;
    }
    sxcl_lock_init(&c->lock);
    if (path && path[0] != '\0') {
        c->path = hc_strdup(path);
        if (c->path) {
            hc_load(c, c->path);             /* 读失败 = 空缓存,照样返回可用句柄 */
        }
    }
    return c;
}

int sxcl_hash_cache_save(sxcl_hash_cache *cache)
{
    if (!cache || !cache->path || cache->path[0] == '\0') {
        return 0;
    }
    sxcl_lock_acquire(&cache->lock);
    int rc = 0;
    if (cache->dirty) {
        rc = -1;
        const size_t len = strlen(cache->path);
        char *tmp = (char *)malloc(len + 5u);
        if (tmp) {
            memcpy(tmp, cache->path, len);
            memcpy(tmp + len, ".tmp", 5u);
            if (sxcl_fs_mkdirs_for_file(cache->path) == 0 && hc_write_all(cache, tmp) == 0 &&
                sxcl_fs_rename_replace(tmp, cache->path) == 0) {
                cache->dirty = 0;
                rc = 0;
            } else {
                sxcl_fs_remove(tmp);         /* 失败不留半截 .tmp */
            }
            free(tmp);
        }
    }
    sxcl_lock_release(&cache->lock);
    return rc;
}

void sxcl_hash_cache_close(sxcl_hash_cache *cache)
{
    if (!cache) {
        return;
    }
    (void)sxcl_hash_cache_save(cache);       /* 落盘失败也无处可报,照样释放 */
    for (size_t i = 0; i < cache->cap; ++i) {
        if (cache->slots[i].path != NULL) {
            hc_entry_dispose(&cache->slots[i]);
        }
    }
    free(cache->slots);
    free(cache->path);
    sxcl_lock_destroy(&cache->lock);
    free(cache);
}

const char *sxcl_hash_cache_get(sxcl_hash_cache *cache, const char *path, int64_t size,
                                int64_t mtime_ns)
{
    if (!cache || !path || cache->cap == 0u) {
        return NULL;
    }
    sxcl_lock_acquire(&cache->lock);
    const char *hex = NULL;
    const uint64_t hash = hc_key_hash(path, size, mtime_ns);
    const size_t idx = hc_probe(cache->slots, cache->cap, path, size, mtime_ns, hash);
    if (cache->slots[idx].path != NULL) {
        hex = cache->slots[idx].hex;
    }
    sxcl_lock_release(&cache->lock);
    return hex;
}

void sxcl_hash_cache_put(sxcl_hash_cache *cache, const char *path, int64_t size, int64_t mtime_ns,
                         const char *hex)
{
    if (!cache || !path || path[0] == '\0' || !hex) {
        return;
    }
    sxcl_lock_acquire(&cache->lock);
    if (cache->count >= SXCL_HC_MAX_ENTRIES) {
        /* 上限保护:先清"文件已不在"的,还是满就整体清空(本次写入随后照常落进去) */
        (void)hc_prune_locked(cache);
        if (cache->count >= SXCL_HC_MAX_ENTRIES) {
            hc_table_clear(cache);
        }
    }
    (void)hc_table_put(cache, path, size, mtime_ns, hex);
    sxcl_lock_release(&cache->lock);
}

int sxcl_hash_cache_remove(sxcl_hash_cache *cache, const char *path, int64_t size, int64_t mtime_ns)
{
    if (!cache || !path) {
        return -1;
    }
    sxcl_lock_acquire(&cache->lock);
    if (cache->cap != 0u) {
        const uint64_t hash = hc_key_hash(path, size, mtime_ns);
        const size_t idx = hc_probe(cache->slots, cache->cap, path, size, mtime_ns, hash);
        if (cache->slots[idx].path != NULL) {
            hc_table_erase(cache, idx);      /* 不存在的键 = 什么都不做,不算错误 */
        }
    }
    sxcl_lock_release(&cache->lock);
    return 0;
}

size_t sxcl_hash_cache_count(const sxcl_hash_cache *cache)
{
    if (!cache) {
        return 0u;
    }
    sxcl_lock_acquire((sxcl_lock_t *)&cache->lock);
    const size_t n = cache->count;
    sxcl_lock_release((sxcl_lock_t *)&cache->lock);
    return n;
}

size_t sxcl_hash_cache_prune_missing(sxcl_hash_cache *cache)
{
    if (!cache || cache->cap == 0u) {
        return 0u;
    }
    sxcl_lock_acquire(&cache->lock);
    const size_t removed = hc_prune_locked(cache);
    sxcl_lock_release(&cache->lock);
    return removed;
}
