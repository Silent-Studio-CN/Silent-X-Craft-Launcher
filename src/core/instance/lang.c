/* SXCL-C 国际化查表实现 —— 纯 C11、零第三方依赖。
 *
 * 见 include/sxcl/lang.h 的语义约定。这里只写"怎么做到的"和踩过的坑:
 *
 *   1) 解析逐字段对齐 Python:lang.py:137-150 是
 *          stripped = line.strip()
 *          if not stripped or stripped.startswith("#"): continue
 *          if "=" in stripped: key, value = stripped.split("=", 1)
 *                              self._data[key.strip()] = value.strip()
 *      注意是**整行 strip 之后**再按第一个 '=' 切 —— 所以"  key  =  a=b  " 得到
 *      key -> "a=b",而 "\tkey\t= value" 也认。C 版照抄这个顺序,不自作聪明。
 *      重复键:后者覆盖前者(Python 是 dict,位置沿用第一次出现的位置)。
 *      没写进 Python 但明显该有的容错:BOM(Windows 记事本存的 UTF-8)与 CRLF。
 *   2) 内置表先进、磁盘文件后覆盖:所以"用户改一条文案"只需在 .lang 里写那一条,
 *      不用把 76 条全抄一遍。
 *   3) 中文兜底单独一份表:当前语言查不到就去中文表查(同一份句柄,释放时一起放)。
 *   4) 一点不撒谎的取舍:超过 SXCL_LANG_KEY_MAX 的键整条丢弃(不截断成半个键),
 *      超过 SXCL_LANG_TEXT_MAX 的文案截断 —— 这两个上限远大于真实语言包(最长键 26 字节)。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/lang.h"

#include "sxcl/fs.h"
#include "sxcl/paths.h"
#include "sxcl/settings.h"   /* sxcl_settings_default_dir(配置目录) */

#include "../internal/platform_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* _wgetenv + UTF-8 转换 */
#endif

/* 内置默认表(由 Python 版 config/lang/*.lang 生成) */
#include "lang_table.inc"

/* ── 语言清单 ── */

typedef struct sxcl_lang_meta {
    const char *code;   /* 规范代码(小写短横线) */
    const char *name;   /* 显示名(launcher_config.py:55-60) */
    const char *file;   /* 语言文件名 */
    const char *alias;  /* 常见的另一种写法(归一化用) */
} sxcl_lang_meta;

static const sxcl_lang_meta kSupported[] = {
    { "zh-cn", "简体中文", "zh-cn.lang", "zh"    },
    { "en-us", "English", "en-us.lang", "en"    },
};

#define SXCL_LANG_SUPPORTED_COUNT (sizeof(kSupported) / sizeof(kSupported[0]))

size_t sxcl_lang_supported_count(void) { return SXCL_LANG_SUPPORTED_COUNT; }

const char *sxcl_lang_supported_code(size_t index)
{
    return index < SXCL_LANG_SUPPORTED_COUNT ? kSupported[index].code : NULL;
}

const char *sxcl_lang_supported_name(size_t index)
{
    return index < SXCL_LANG_SUPPORTED_COUNT ? kSupported[index].name : NULL;
}

/* ── 小工具 ── */

static char *dup_range(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void set_text(char *err, size_t err_len, const char *text)
{
    if (err && err_len) {
        snprintf(err, err_len, "%s", text ? text : "");
    }
}

static void set_err2(char *err, size_t err_len, const char *fmt, const char *a)
{
    if (err && err_len) {
        snprintf(err, err_len, fmt, a ? a : "");
    }
}

static size_t copy_str(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) {
        return 0;
    }
    const size_t n = src ? strlen(src) : 0;
    const size_t take = n < out_len - 1 ? n : out_len - 1;
    if (take) {
        memcpy(out, src, take);
    }
    out[take] = '\0';
    return take;
}

/* ASCII 小写(语言代码/键都是 ASCII,不影响 UTF-8 文案) */
static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* UTF-8 环境变量(Windows 上中文路径经 _wgetenv -> CP_UTF8,不受代码页影响)。
 * 返回静态缓冲,POSIX 侧直接返回 getenv 的指针(进程生命期内有效)。 */
static const char *env_utf8(const char *name)
{
    if (!name || !*name) {
        return NULL;
    }
#if defined(_WIN32)
    static char buf[SXCL_LANG_PATH_MAX * 2];
    wchar_t name_w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, name_w, 64) <= 0) {
        return NULL;
    }
    const wchar_t *value = _wgetenv(name_w);
    if (!value || !*value) {
        return NULL;
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, value, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (need <= 0) {
        return NULL;
    }
    return buf;
#else
    const char *value = getenv(name);
    return (value && *value) ? value : NULL;
#endif
}

/* ── 表结构 ── */

typedef struct sxcl_lang_entry {
    char *key;
    char *text;
} sxcl_lang_entry;

typedef struct sxcl_lang_table {
    sxcl_lang_entry *items;
    size_t count;
    size_t capacity;
} sxcl_lang_table;

struct sxcl_lang {
    char code[SXCL_LANG_CODE_MAX];
    sxcl_lang_table primary;   /* 当前语言 */
    sxcl_lang_table backstop;  /* 简体中文兜底(当前语言就是中文时与 primary 共用) */
    int shared_backstop;       /* 1 = backstop 与 primary 是同一份(不重复释放) */
};

static size_t table_find(const sxcl_lang_table *t, const char *key)
{
    for (size_t i = 0; i < t->count; ++i) {
        if (strcmp(t->items[i].key, key) == 0) {
            return i;
        }
    }
    return t->count;
}

/* 落一条键值:已在表里就只换文案(位置不变,与 Python dict 的语义一致),否则追加。 */
static int table_put(sxcl_lang_table *t, const char *key, size_t klen, const char *text, size_t tlen)
{
    char kbuf[SXCL_LANG_KEY_MAX];
    const size_t take = klen < sizeof(kbuf) - 1 ? klen : sizeof(kbuf) - 1;
    memcpy(kbuf, key, take);
    kbuf[take] = '\0';

    char *v = dup_range(text, tlen);
    if (!v) {
        return -1;
    }
    const size_t found = table_find(t, kbuf);
    if (found < t->count) {
        free(t->items[found].text);
        t->items[found].text = v;
        return 0;
    }
    char *k = dup_range(key, klen);
    if (!k) {
        free(v);
        return -1;
    }
    if (t->count == t->capacity) {
        const size_t next = t->capacity ? t->capacity * 2 : 32;
        sxcl_lang_entry *grown = (sxcl_lang_entry *)realloc(t->items, next * sizeof(*grown));
        if (!grown) {
            free(k);
            free(v);
            return -1;
        }
        t->items = grown;
        t->capacity = next;
    }
    t->items[t->count].key = k;
    t->items[t->count].text = v;
    ++t->count;
    return 0;
}

static void table_free(sxcl_lang_table *t)
{
    for (size_t i = 0; i < t->count; ++i) {
        free(t->items[i].key);
        free(t->items[i].text);
    }
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->capacity = 0;
}

/* 内置表 -> 内存表。 */
static int table_load_builtin(sxcl_lang_table *t, const sxcl_lang_kv *kv, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (table_put(t, kv[i].key, strlen(kv[i].key), kv[i].text, strlen(kv[i].text)) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ── 语言代码归一化 ── */

static const sxcl_lang_meta *find_meta(const char *code)
{
    if (!code || !*code) {
        return NULL;
    }
    char norm[SXCL_LANG_CODE_MAX];
    size_t n = 0;
    for (const char *p = code; *p && n < sizeof(norm) - 1; ++p) {
        norm[n++] = (*p == '_') ? '-' : lower_ascii(*p);
    }
    norm[n] = '\0';
    for (size_t i = 0; i < SXCL_LANG_SUPPORTED_COUNT; ++i) {
        if (strcmp(norm, kSupported[i].code) == 0) {
            return &kSupported[i];
        }
    }
    /* 再按"主语言"认一次:zh-Hans-CN / en-GB 这类也归到我们支持的两种 */
    for (size_t i = 0; i < SXCL_LANG_SUPPORTED_COUNT; ++i) {
        const size_t alen = strlen(kSupported[i].alias);
        if (strncmp(norm, kSupported[i].alias, alen) == 0 && (norm[alen] == '\0' || norm[alen] == '-')) {
            return &kSupported[i];
        }
    }
    return NULL;
}

int sxcl_lang_normalize_code(const char *code, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return SXCL_LANG_ERR_ARG;
    }
    const sxcl_lang_meta *meta = find_meta(code);
    /* 认不出就是中文 —— 与 lang.py:164-165 的 \`lang_code = "zh-cn"\` 一致 */
    const char *canonical = meta ? meta->code : kSupported[0].code;
    const size_t n = strlen(canonical);
    if (n + 1 > out_len) {
        copy_str(out, out_len, "");
        return SXCL_LANG_ERR_SPACE;
    }
    memcpy(out, canonical, n + 1);
    return SXCL_LANG_OK;
}

int sxcl_lang_is_supported(const char *code)
{
    return find_meta(code) != NULL ? 1 : 0;
}

/* ── 解析(与 Python lang.py:137-150 逐字段一致) ── */

/* 解析进**指定**的表:public 的 load_text 写当前语言表,open 里加载中文兜底时写兜底表。
 * (踩过的坑:一开始 open 里两份磁盘文件都走 load_text,于是中文兜底把英文主表覆盖成中文了。) */
static int load_text_into(sxcl_lang_table *table, const char *text, size_t len, char *err,
                          size_t err_len)
{
    if (!table || !text) {
        set_text(err, err_len, "参数不合法");
        return SXCL_LANG_ERR_ARG;
    }
    size_t loaded = 0;
    size_t pos = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu) {
        pos = 3; /* BOM:Python 的 read_text(encoding="utf-8") 会自动吃掉,这里补上 */
    }
    while (pos <= len) {
        /* 取一行(不含换行符) */
        size_t end = pos;
        while (end < len && text[end] != '\n' && text[end] != '\r') {
            ++end;
        }
        const char *line = text + pos;
        size_t line_len = end - pos;
        pos = end;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) {
            ++pos; /* CRLF / LF / CR 都当行尾 */
        }

        /* strip(整行) */
        while (line_len > 0 && (line[0] == ' ' || line[0] == '\t')) {
            ++line;
            --line_len;
        }
        while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t')) {
            --line_len;
        }
        if (line_len == 0 || line[0] == '#') {
            continue;
        }
        /* 第一个 '=' 切分 */
        size_t eq = 0;
        while (eq < line_len && line[eq] != '=') {
            ++eq;
        }
        if (eq >= line_len) {
            continue;
        }
        size_t klen = eq;
        size_t vstart = eq + 1;
        /* 键/值各自 strip */
        while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) {
            --klen;
        }
        while (vstart < line_len && (line[vstart] == ' ' || line[vstart] == '\t')) {
            ++vstart;
        }
        size_t vlen = line_len - vstart;
        while (vlen > 0 && (line[vlen + vstart - 1] == ' ' || line[vlen + vstart - 1] == '\t')) {
            --vlen;
        }
        if (klen == 0) {
            continue; /* "= value" 这种没有键的行:Python 会存成空键,这里丢弃 */
        }
        if (klen >= SXCL_LANG_KEY_MAX) {
            continue; /* 键太长:整条丢,不截断成半个键 */
        }
        if (vlen > SXCL_LANG_TEXT_MAX - 1) {
            vlen = SXCL_LANG_TEXT_MAX - 1; /* 文案超长:截断(真实语言包最长 100 出头字节) */
        }
        if (table_put(table, line, klen, line + vstart, vlen) != 0) {
            set_text(err, err_len, "内存不足");
            return SXCL_LANG_ERR_NOMEM;
        }
        ++loaded;
        if (pos >= len) {
            break;
        }
    }
    return (int)loaded;
}

int sxcl_lang_load_text(sxcl_lang *lang, const char *text, size_t len, char *err, size_t err_len)
{
    if (!lang) {
        set_text(err, err_len, "参数不合法");
        return SXCL_LANG_ERR_ARG;
    }
    return load_text_into(&lang->primary, text, len, err, err_len);
}

/* 从文件读进指定表。 */
static int load_file_into(sxcl_lang_table *table, const char *path, char *err, size_t err_len)
{
    if (!table || !path || !*path) {
        set_text(err, err_len, "参数不合法");
        return SXCL_LANG_ERR_ARG;
    }
    FILE *fh = sxcl_fs_fopen(path, "rb");
    if (!fh) {
        set_err2(err, err_len, "语言文件打不开: %s", path);
        return SXCL_LANG_ERR_IO;
    }
    size_t cap = 8192;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        fclose(fh);
        set_text(err, err_len, "内存不足");
        return SXCL_LANG_ERR_NOMEM;
    }
    for (;;) {
        if (len == cap) {
            const size_t next = cap * 2;
            char *grown = (char *)realloc(buf, next);
            if (!grown) {
                free(buf);
                fclose(fh);
                set_text(err, err_len, "内存不足");
                return SXCL_LANG_ERR_NOMEM;
            }
            buf = grown;
            cap = next;
        }
        const size_t got = fread(buf + len, 1, cap - len, fh);
        len += got;
        if (got == 0) {
            break;
        }
    }
    fclose(fh);
    const int rc = load_text_into(table, buf, len, err, err_len);
    free(buf);
    return rc;
}

/* 从文件读进当前语言表(public;给"热更新语言包"用)。 */
int sxcl_lang_load_file(sxcl_lang *lang, const char *path, char *err, size_t err_len)
{
    if (!lang) {
        set_text(err, err_len, "参数不合法");
        return SXCL_LANG_ERR_ARG;
    }
    return load_file_into(&lang->primary, path, err, err_len);
}

/* ── 打开 / 释放 ── */

size_t sxcl_lang_default_dirs(char (*out)[SXCL_LANG_PATH_MAX], size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    size_t n = 0;
    const char *explicit_dir = env_utf8("SXCL_LANG_DIR");
    if (explicit_dir && n < cap) {
        copy_str(out[n], SXCL_LANG_PATH_MAX, explicit_dir);
        ++n;
    }
    char config[SXCL_LANG_PATH_MAX];
    char err[64];
    if (n < cap && sxcl_settings_default_dir(config, sizeof(config), err, sizeof(err)) == SXCL_SETTINGS_OK) {
        snprintf(out[n], SXCL_LANG_PATH_MAX, "%s/lang", config);
        ++n;
    }
    char program[SXCL_PATHS_PATH_MAX];
    if (n < cap && sxcl_paths_program_dir(program, sizeof(program), err, sizeof(err)) == SXCL_PATHS_OK) {
        snprintf(out[n], SXCL_LANG_PATH_MAX, "%s/lang", program);
        ++n;
    }
    return n;
}

/* 在搜索目录里找 <file>;找到写进 out 返回 1,没有返回 0。 */
static int find_lang_file(const char *dir, const char *file, char *out, size_t out_len)
{
    if (dir && *dir) {
        snprintf(out, out_len, "%s/%s", dir, file);
        /* Windows 上 "/" 也认(_wfopen 吃正斜杠),不用换分隔符 */
        return sxcl_fs_exists(out) ? 1 : 0;
    }
    char dirs[4][SXCL_LANG_PATH_MAX];
    const size_t n = sxcl_lang_default_dirs(dirs, 4);
    for (size_t i = 0; i < n; ++i) {
        snprintf(out, out_len, "%s/%s", dirs[i], file);
        if (sxcl_fs_exists(out)) {
            return 1;
        }
    }
    return 0;
}

sxcl_lang *sxcl_lang_open(const char *code, const char *dir, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    const sxcl_lang_meta *meta = find_meta(code);
    if (!meta) {
        meta = &kSupported[0]; /* 认不出就中文(不报错,与 Python 一致) */
    }
    sxcl_lang *lang = (sxcl_lang *)calloc(1, sizeof(*lang));
    if (!lang) {
        set_text(err, err_len, "内存不足");
        return NULL;
    }
    copy_str(lang->code, sizeof(lang->code), meta->code);

    /* 1) 内置默认(离线也有完整两份) */
    const sxcl_lang_kv *primary_kv = (strcmp(meta->code, "en-us") == 0) ? kLangTableEnUs : kLangTableZhCn;
    const size_t primary_n = (strcmp(meta->code, "en-us") == 0)
                                 ? sizeof(kLangTableEnUs) / sizeof(kLangTableEnUs[0])
                                 : sizeof(kLangTableZhCn) / sizeof(kLangTableZhCn[0]);
    if (table_load_builtin(&lang->primary, primary_kv, primary_n) != 0) {
        sxcl_lang_free(lang);
        set_text(err, err_len, "内存不足");
        return NULL;
    }
    if (strcmp(meta->code, "zh-cn") == 0) {
        lang->shared_backstop = 1; /* 当前就是中文:兜底就是它自己 */
    } else if (table_load_builtin(&lang->backstop, kLangTableZhCn,
                                  sizeof(kLangTableZhCn) / sizeof(kLangTableZhCn[0])) != 0) {
        sxcl_lang_free(lang);
        set_text(err, err_len, "内存不足");
        return NULL;
    }

    /* 2) 磁盘上的 .lang 逐键覆盖(找不到不算错) */
    char path[SXCL_LANG_PATH_MAX * 2];
    int loaded = 0;
    if (find_lang_file(dir, meta->file, path, sizeof(path))) {
        const int got = load_file_into(&lang->primary, path, err, err_len);
        if (got >= 0) {
            loaded += got;
        }
    }
    if (!lang->shared_backstop && find_lang_file(dir, "zh-cn.lang", path, sizeof(path))) {
        /* 中文兜底也要吃磁盘版本(写进 backstop,不能碰 primary):
         * 英文包翻译不全时,兜底的那条也该是最新的中文 */
        const int got = load_file_into(&lang->backstop, path, err, err_len);
        if (got >= 0) {
            loaded += got;
        }
    }
    if (loaded == 0 && err && err_len && err[0] == '\0') {
        snprintf(err, err_len, "没找到 %s 的磁盘语言包,使用内置默认文案(共 %d 条)",
                 meta->code, (int)primary_n);
    }
    return lang;
}

void sxcl_lang_free(sxcl_lang *lang)
{
    if (!lang) {
        return;
    }
    table_free(&lang->primary);
    if (!lang->shared_backstop) {
        table_free(&lang->backstop);
    }
    free(lang);
}

const char *sxcl_lang_code(const sxcl_lang *lang)
{
    return lang ? lang->code : kSupported[0].code;
}

/* ── 查表 ── */

static const char *table_get(const sxcl_lang_table *t, const char *key)
{
    if (!t || !key || !*key) {
        return NULL;
    }
    const size_t idx = table_find(t, key);
    return idx < t->count ? t->items[idx].text : NULL;
}

const char *sxcl_lang_get(const sxcl_lang *lang, const char *key, const char *def)
{
    if (!lang || !key || !*key) {
        return def;
    }
    const char *text = table_get(&lang->primary, key);
    if (text) {
        return text;
    }
    /* 找不到的键回落到中文 */
    text = table_get(&lang->backstop, key);
    return text ? text : def;
}

int sxcl_lang_has(const sxcl_lang *lang, const char *key)
{
    if (!lang || !key || !*key) {
        return 0;
    }
    return (table_get(&lang->primary, key) || table_get(&lang->backstop, key)) ? 1 : 0;
}

int sxcl_lang_format(const sxcl_lang *lang, const char *key, const char *def,
                     const char *const *names, const char *const *values, size_t count,
                     char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return SXCL_LANG_ERR_ARG;
    }
    out[0] = '\0';
    const char *text = sxcl_lang_get(lang, key, def);
    if (!text) {
        return SXCL_LANG_OK; /* 没有这条键也没有 def:留空,不当错误(与 get 的语义一致) */
    }
    size_t n = 0;
    int truncated = 0;
    for (const char *p = text; *p;) {
        if (*p != '{') {
            if (n + 1 < out_len) {
                out[n++] = *p;
            } else {
                truncated = 1;
            }
            ++p;
            continue;
        }
        const char *close = strchr(p + 1, '}');
        if (!close) {
            if (n + 1 < out_len) {
                out[n++] = *p;
            } else {
                truncated = 1;
            }
            ++p;
            continue;
        }
        const size_t name_len = (size_t)(close - (p + 1));
        const char *replacement = NULL;
        for (size_t i = 0; i < count && names && values; ++i) {
            if (names[i] && strlen(names[i]) == name_len &&
                strncmp(names[i], p + 1, name_len) == 0) {
                replacement = values[i] ? values[i] : "";
                break;
            }
        }
        if (!replacement) {
            /* 未知占位符原样写出 */
            for (const char *q = p; q <= close; ++q) {
                if (n + 1 < out_len) {
                    out[n++] = *q;
                } else {
                    truncated = 1;
                }
            }
        } else {
            for (const char *q = replacement; *q; ++q) {
                if (n + 1 < out_len) {
                    out[n++] = *q;
                } else {
                    truncated = 1;
                }
            }
        }
        p = close + 1;
    }
    out[n] = '\0';
    return truncated ? SXCL_LANG_ERR_SPACE : SXCL_LANG_OK;
}

/* ── 遍历 ── */

size_t sxcl_lang_count(const sxcl_lang *lang) { return lang ? lang->primary.count : 0; }

const char *sxcl_lang_key_at(const sxcl_lang *lang, size_t index)
{
    return (lang && index < lang->primary.count) ? lang->primary.items[index].key : NULL;
}

const char *sxcl_lang_text_at(const sxcl_lang *lang, size_t index)
{
    return (lang && index < lang->primary.count) ? lang->primary.items[index].text : NULL;
}

/* ── 进程级默认表 ── */

static sxcl_lang *g_default_lang = NULL;
static sxcl_lock_t g_default_lock;
static int g_default_lock_ready = 0;

static void ensure_default_lock(void)
{
    if (!g_default_lock_ready) {
        sxcl_lock_init(&g_default_lock);
        g_default_lock_ready = 1;
    }
}

int sxcl_lang_set_default(const char *code, const char *dir, char *err, size_t err_len)
{
    char norm[SXCL_LANG_CODE_MAX];
    if (sxcl_lang_normalize_code(code, norm, sizeof(norm)) != SXCL_LANG_OK) {
        set_err2(err, err_len, "语言代码太长: %s", code);
        return SXCL_LANG_ERR_SPACE;
    }
    /* 先建新的,成功了再换 —— 失败时旧的默认表保持不动 */
    sxcl_lang *fresh = sxcl_lang_open(norm, dir, err, err_len);
    if (!fresh) {
        return SXCL_LANG_ERR_NOMEM;
    }
    ensure_default_lock();
    sxcl_lock_acquire(&g_default_lock);
    sxcl_lang *old = g_default_lang;
    g_default_lang = fresh;
    sxcl_lock_release(&g_default_lock);
    sxcl_lang_free(old);
    return SXCL_LANG_OK;
}

const sxcl_lang *sxcl_lang_default(void) { return g_default_lang; }

const char *sxcl_lang_tr(const char *key, const char *def)
{
    const sxcl_lang *lang = sxcl_lang_default();
    return lang ? sxcl_lang_get(lang, key, def) : def;
}

void sxcl_lang_shutdown(void)
{
    if (g_default_lock_ready) {
        sxcl_lock_acquire(&g_default_lock);
    }
    sxcl_lang *old = g_default_lang;
    g_default_lang = NULL;
    if (g_default_lock_ready) {
        sxcl_lock_release(&g_default_lock);
    }
    sxcl_lang_free(old);
}
