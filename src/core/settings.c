/* SXCL-C 设置存储实现 —— 纯 C11、零第三方依赖、四平台可移植。
 *
 * 设计要点(与 include/sxcl/settings.h 的约定一致):
 *   1) 内存模型:一个有序数组 {key,value},键值都是 malloc 出来的 C 串。
 *      查找线性扫描(设置项只有几十条,不值得上哈希表,也就没有哈希表带来的顺序问题)。
 *   2) 读:自己写 fgets 循环(MSVC 没有 getline),逐行剥 CR/LF;注释/空行/坏行跳过不报错。
 *      同名键后出现的覆盖先出现的(和 json.c"重复键以最后一次为准"一致),位置不变。
 *   3) 数字解析自己写,不用 strtod/strtoll:strtod 受 locale 影响(有的区域拿 ',' 当小数点),
 *      strtoll 也没法干净地区分"非法"和"恰好是 0"。小数按"整数尾数 + 十进制指数"一次性
 *      换算,314/100 这种能得到最接近的 double,不会一位一位加出累积误差。
 *   4) 原子写:先写 <path>.tmp,再 sxcl_fs_rename_replace 覆盖;父目录用
 *      sxcl_fs_mkdirs_for_file 兜底;任何一步失败都把 .tmp 删掉,不留半个文件。
 *   5) UTF-8 路径:Windows 上 fopen 按 ANSI 代码页解释路径(中文路径直接打不开),
 *      所以统一走 _wfopen_s,写法与 src/core/json.c 的 open_utf8_path 一致。
 *   6) 无全局可变状态,但**不承诺**并发:同一句柄多线程读写是数据竞争(设置是主线程的)。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/settings.h"

#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* 仅为了 MultiByteToWideChar,把 UTF-8 路径转宽字符 */
#endif

/* ── 数据结构 ── */

typedef struct sxcl_setting_entry {
    char *key;
    char *value;
} sxcl_setting_entry;

struct sxcl_settings {
    sxcl_setting_entry *entries;
    size_t count;
    size_t capacity;
};

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

static char *dup_str(const char *s)
{
    return dup_range(s ? s : "", s ? strlen(s) : 0);
}

/* 键名允许 [A-Za-z0-9._-];另外放行 UTF-8 高位字节,让中文实例名也能用。 */
static int key_char_ok(unsigned char c)
{
    if ((c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
        (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
        (c >= (unsigned char)'0' && c <= (unsigned char)'9')) {
        return 1;
    }
    if (c == (unsigned char)'.' || c == (unsigned char)'_' || c == (unsigned char)'-') {
        return 1;
    }
    if (c > 0x7F) {
        return 1;
    }
    return 0;
}

static int key_range_ok(const char *key, size_t n)
{
    if (!key || n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!key_char_ok((unsigned char)key[i])) {
            return 0;
        }
    }
    return 1;
}

static int key_ok(const char *key)
{
    return key ? key_range_ok(key, strlen(key)) : 0;
}

/* 返回键所在下标;没找到返回 count(即"末尾",可安全地与 count 比较)。 */
static size_t find_index(const sxcl_settings *s, const char *key, size_t klen)
{
    for (size_t i = 0; i < s->count; ++i) {
        if (strlen(s->entries[i].key) == klen && memcmp(s->entries[i].key, key, klen) == 0) {
            return i;
        }
    }
    return s->count;
}

static int push(sxcl_settings *s, char *key, char *value)
{
    if (s->count == s->capacity) {
        const size_t next = s->capacity ? s->capacity * 2 : 16;
        sxcl_setting_entry *grown = (sxcl_setting_entry *)realloc(s->entries, next * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        s->entries = grown;
        s->capacity = next;
    }
    s->entries[s->count].key = key;
    s->entries[s->count].value = value;
    ++s->count;
    return 0;
}

/* ── 文件 IO(UTF-8 路径) ── */

#if defined(_WIN32)
static wchar_t *utf8_to_wide(const char *utf8)
{
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, need) != need) {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

static FILE *open_utf8(const char *path, int for_write)
{
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) {
        return NULL;
    }
    FILE *f = NULL;
    /* _wfopen 会触发 C4996,/WX 下必须用 _s 版 */
    if (_wfopen_s(&f, wide, for_write ? L"wb" : L"rb") != 0) {
        f = NULL;
    }
    free(wide);
    return f;
#else
    return fopen(path, for_write ? "wb" : "rb");
#endif
}

/* 可移植读行(MSVC 没有 getline):返回读到的字节数(不含结尾 NUL),EOF 且无数据返回 -1。 */
static long read_line(FILE *fh, char **buf, size_t *cap)
{
    if (!*buf || *cap == 0) {
        *cap = 256;
        *buf = (char *)malloc(*cap);
        if (!*buf) {
            return -1;
        }
    }
    size_t n = 0;
    for (;;) {
        if (!fgets(*buf + n, (int)(*cap - n), fh)) {
            return n > 0 ? (long)n : -1;
        }
        n += strlen(*buf + n);
        if (n > 0 && (*buf)[n - 1] == '\n') {
            return (long)n;
        }
        if (n + 1 < *cap) {
            return (long)n; /* 文件末尾没有换行 */
        }
        const size_t ncap = *cap * 2;
        char *grown = (char *)realloc(*buf, ncap);
        if (!grown) {
            return -1;
        }
        *buf = grown;
        *cap = ncap;
    }
}

/* ── 公开 API ── */

sxcl_settings *sxcl_settings_open(const char *path)
{
    if (!path) {
        return NULL;
    }
    sxcl_settings *s = (sxcl_settings *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    FILE *fh = open_utf8(path, 0);
    if (!fh) {
        return s; /* 不存在 / 打不开 = 全新,用默认值 */
    }
    char *line = NULL;
    size_t cap = 0;
    long n = 0;
    int first = 1;
    while ((n = read_line(fh, &line, &cap)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0'; /* CRLF / LF 都剥掉,值里不会残留 CR */
        }
        const char *p = line;
        if (first) {
            first = 0;
            if ((unsigned char)p[0] == 0xEFu && (unsigned char)p[1] == 0xBBu && (unsigned char)p[2] == 0xBFu) {
                p += 3; /* 有 BOM 就当没看见 */
            }
        }
        const char *probe = p;
        while (*probe == ' ' || *probe == '\t') {
            ++probe;
        }
        if (*probe == '#' || *probe == '\0') {
            continue; /* 注释行 / 空行 */
        }
        const char *eq = strchr(p, '=');
        if (!eq) {
            continue; /* 坏行:没有 '=' */
        }
        const size_t klen = (size_t)(eq - p);
        if (!key_range_ok(p, klen)) {
            continue; /* 坏行:空键名或键名非法 */
        }
        const size_t idx = find_index(s, p, klen);
        if (idx < s->count) {
            /* 同名键:后出现的值覆盖先出现的,位置不变 */
            char *v = dup_str(eq + 1);
            if (v) {
                free(s->entries[idx].value);
                s->entries[idx].value = v;
            }
            continue;
        }
        char *k = dup_range(p, klen);
        char *v = dup_str(eq + 1);
        if (!k || !v || push(s, k, v) != 0) {
            free(k);
            free(v);
        }
    }
    free(line);
    fclose(fh);
    return s;
}

int sxcl_settings_save(sxcl_settings *settings, const char *path)
{
    if (!settings || !path) {
        return -1;
    }
    const size_t len = strlen(path);
    char *tmp = (char *)malloc(len + 5);
    if (!tmp) {
        return -1;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, ".tmp", 5);

    int rc = -1;
    if (sxcl_fs_mkdirs_for_file(path) == 0) {
        FILE *fh = open_utf8(tmp, 1);
        if (fh) {
            rc = 0;
            for (size_t i = 0; i < settings->count; ++i) {
                const sxcl_setting_entry *e = &settings->entries[i];
                if (fputs(e->key, fh) < 0 || fputc('=', fh) == EOF ||
                    fputs(e->value, fh) < 0 || fputc('\n', fh) == EOF) {
                    rc = -1;
                    break;
                }
            }
            if (fclose(fh) != 0) {
                rc = -1;
            }
            if (rc == 0 && sxcl_fs_rename_replace(tmp, path) != 0) {
                rc = -1;
            }
        }
    }
    if (rc != 0) {
        sxcl_fs_remove(tmp); /* 失败了不留 .tmp 垃圾 */
    }
    free(tmp);
    return rc;
}

void sxcl_settings_free(sxcl_settings *settings)
{
    if (!settings) {
        return;
    }
    for (size_t i = 0; i < settings->count; ++i) {
        free(settings->entries[i].key);
        free(settings->entries[i].value);
    }
    free(settings->entries);
    free(settings);
}

const char *sxcl_settings_get(sxcl_settings *settings, const char *key, const char *def)
{
    if (!settings || !key || !*key) {
        return def;
    }
    const size_t idx = find_index(settings, key, strlen(key));
    return idx < settings->count ? settings->entries[idx].value : def;
}

int sxcl_settings_set(sxcl_settings *settings, const char *key, const char *value)
{
    if (!settings || !key_ok(key)) {
        return -1;
    }
    if (!value) {
        value = "";
    }
    const size_t idx = find_index(settings, key, strlen(key));
    if (idx < settings->count) {
        char *v = dup_str(value); /* 先分配再释放:内存不足时旧值还在 */
        if (!v) {
            return -1;
        }
        free(settings->entries[idx].value);
        settings->entries[idx].value = v;
        return 0;
    }
    char *k = dup_str(key);
    char *v = dup_str(value);
    if (!k || !v || push(settings, k, v) != 0) {
        free(k);
        free(v);
        return -1;
    }
    return 0;
}

int sxcl_settings_remove(sxcl_settings *settings, const char *key)
{
    if (!settings || !key) {
        return -1;
    }
    const size_t idx = find_index(settings, key, strlen(key));
    if (idx >= settings->count) {
        return 0; /* 不存在不算错 */
    }
    free(settings->entries[idx].key);
    free(settings->entries[idx].value);
    for (size_t i = idx + 1; i < settings->count; ++i) {
        settings->entries[i - 1] = settings->entries[i];
    }
    --settings->count;
    return 0;
}

/* ── 值解析(不依赖 locale) ── */

static void skip_space(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r' || **p == '\v' || **p == '\f') {
        ++(*p);
    }
}

static int parse_int64(const char *s, int64_t *out)
{
    if (!s || !out) {
        return -1;
    }
    const char *p = s;
    skip_space(&p);
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    uint64_t acc = 0;
    int overflow = 0;
    const uint64_t limit = neg ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    while (*p >= '0' && *p <= '9') {
        const uint64_t d = (uint64_t)(*p - '0');
        if (acc > (limit - d) / 10u) {
            overflow = 1;
        } else {
            acc = acc * 10u + d;
        }
        ++p;
    }
    skip_space(&p);
    if (*p != '\0' || overflow) {
        return -1;
    }
    if (neg) {
        *out = (acc == (uint64_t)INT64_MAX + 1u) ? INT64_MIN : -(int64_t)acc;
    } else {
        *out = (int64_t)acc;
    }
    return 0;
}

static double apply_exp10(double v, int e)
{
    if (v == 0.0) {
        return 0.0;
    }
    if (e > 400) {
        e = 400; /* 再大也是 ±inf,不用真乘几百次 */
    } else if (e < -400) {
        e = -400; /* 再小也是 0 */
    }
    while (e > 0) {
        v *= 10.0;
        --e;
    }
    while (e < 0) {
        v /= 10.0;
        ++e;
    }
    return v;
}

/* 文法: [+-]? digits [ . digits ] [ (e|E) [+-]? digits ],前后允许空白,其余一律非法。 */
static int parse_double(const char *s, double *out)
{
    if (!s || !out) {
        return -1;
    }
    const char *p = s;
    skip_space(&p);
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        ++p;
    }
    uint64_t mant = 0;
    int digits = 0;
    int exp10 = 0;
    int any = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        any = 1;
        if (digits < 19) {
            mant = mant * 10u + (uint64_t)(*p - '0');
            ++digits;
        } else {
            ++exp10; /* 尾数装不下的整数位:改成指数 +1 */
        }
    }
    if (*p == '.') {
        ++p;
        for (; *p >= '0' && *p <= '9'; ++p) {
            any = 1;
            if (digits < 19) {
                mant = mant * 10u + (uint64_t)(*p - '0');
                ++digits;
                --exp10;
            }
            /* 精度已经用满:多出来的小数位直接丢(不影响最接近的 double) */
        }
    }
    if (!any) {
        return -1;
    }
    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        ++p;
        int eneg = 0;
        if (*p == '+' || *p == '-') {
            eneg = (*p == '-');
            ++p;
        }
        if (*p >= '0' && *p <= '9') {
            int e = 0;
            while (*p >= '0' && *p <= '9') {
                if (e < 1000000) {
                    e = e * 10 + (*p - '0');
                }
                ++p;
            }
            exp10 += eneg ? -e : e;
        } else {
            p = save; /* "1e" 这种:回退,交给后面的尾巴检查判非法 */
        }
    }
    skip_space(&p);
    if (*p != '\0') {
        return -1;
    }
    const double v = apply_exp10((double)mant, exp10);
    *out = neg ? -v : v;
    return 0;
}

int64_t sxcl_settings_get_int(sxcl_settings *settings, const char *key, int64_t def)
{
    int64_t v = 0;
    if (parse_int64(sxcl_settings_get(settings, key, NULL), &v) != 0) {
        return def;
    }
    return v;
}

double sxcl_settings_get_double(sxcl_settings *settings, const char *key, double def)
{
    double v = 0.0;
    if (parse_double(sxcl_settings_get(settings, key, NULL), &v) != 0) {
        return def;
    }
    return v;
}

static int value_is(const char *v, const char *lit)
{
    const char *b = v;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') {
        ++b;
    }
    const char *e = b + strlen(b);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        --e;
    }
    const size_t n = (size_t)(e - b);
    if (n != strlen(lit)) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        char c = b[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != lit[i]) {
            return 0;
        }
    }
    return 1;
}

int sxcl_settings_get_bool(sxcl_settings *settings, const char *key, int def)
{
    const char *v = sxcl_settings_get(settings, key, NULL);
    if (!v) {
        return def;
    }
    if (value_is(v, "1") || value_is(v, "true") || value_is(v, "yes")) {
        return 1;
    }
    if (value_is(v, "0") || value_is(v, "false") || value_is(v, "no")) {
        return 0;
    }
    return def;
}

/* ── 带默认值的便捷读取 ── */

const char *sxcl_settings_download_rate_text(sxcl_settings *settings)
{
    return sxcl_settings_get(settings, "download.rate", "0");
}

int64_t sxcl_settings_download_workers(sxcl_settings *settings)
{
    return sxcl_settings_get_int(settings, "download.workers", 0);
}

int64_t sxcl_settings_download_max_conn(sxcl_settings *settings)
{
    return sxcl_settings_get_int(settings, "download.max_conn", 1);
}

const char *sxcl_settings_download_cache_dir(sxcl_settings *settings)
{
    return sxcl_settings_get(settings, "download.cache_dir", "");
}

const char *sxcl_settings_game_default_dir(sxcl_settings *settings)
{
    return sxcl_settings_get(settings, "game.default_dir", "");
}

const char *sxcl_settings_ui_theme(sxcl_settings *settings)
{
    return sxcl_settings_get(settings, "ui.theme", "auto");
}

const char *sxcl_settings_ui_language(sxcl_settings *settings)
{
    return sxcl_settings_get(settings, "ui.language", "zh-CN");
}

/* ── 跨平台设置位置(见 settings.h 的语义) ── */

/* UTF-8 环境变量:Windows 走 _wgetenv + CP_UTF8(中文用户名/路径不被代码页毁掉);
 * POSIX 直接 getenv。返回的指针:Windows 侧是静态缓冲(下一次调用会被覆盖),
 * 所以调用方要**立刻**用掉或拷走 —— 本文件内部都只在本函数附近用。 */
static const char *env_utf8(const char *name)
{
    if (!name || !*name) {
        return NULL;
    }
#if defined(_WIN32)
    static char buf[1024];
    wchar_t name_w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, name_w, 64) <= 0) {
        return NULL;
    }
    const wchar_t *value = _wgetenv(name_w);
    if (!value || !*value) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, buf, (int)sizeof(buf), NULL, NULL) <= 0) {
        return NULL;
    }
    return buf;
#else
    const char *value = getenv(name);
    return (value && *value) ? value : NULL;
#endif
}

#if defined(_WIN32)
#  define SXCL_SETTINGS_SEP "\\"
#else
#  define SXCL_SETTINGS_SEP "/"
#endif

/* dir + 分隔符 + leaf;装不下返回 -1。dir 末尾已有分隔符就不重复加。 */
static int settings_join(char *out, size_t out_len, const char *dir, const char *leaf)
{
    if (!out || !dir || !leaf) {
        return -1;
    }
    const size_t dlen = strlen(dir);
    const size_t llen = strlen(leaf);
    const int need_sep = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) ? 0 : 1;
    if (dlen + (size_t)need_sep + llen + 1 > out_len) {
        return -1;
    }
    memcpy(out, dir, dlen);
    size_t n = dlen;
    if (need_sep) {
        memcpy(out + n, SXCL_SETTINGS_SEP, strlen(SXCL_SETTINGS_SEP));
        n += strlen(SXCL_SETTINGS_SEP);
    }
    memcpy(out + n, leaf, llen + 1);
    return 0;
}

static int settings_arg_err(char *err, size_t err_len, const char *text)
{
    if (err && err_len) {
        snprintf(err, err_len, "%s", text);
    }
    return SXCL_SETTINGS_ERR_ARG;
}

int sxcl_settings_default_dir(char *out, size_t out_len, char *err, size_t err_len)
{
    if (!out || out_len == 0) {
        return settings_arg_err(err, err_len, "参数不合法");
    }
    out[0] = '\0';
    /* 显式覆盖:便携版/测试/多配置并存都用它 */
    const char *override_dir = env_utf8("SXCL_CONFIG_DIR");
    if (override_dir && *override_dir) {
        if (strlen(override_dir) + 1 > out_len) {
            return settings_arg_err(err, err_len, "SXCL_CONFIG_DIR 太长");
        }
        memcpy(out, override_dir, strlen(override_dir) + 1);
        return SXCL_SETTINGS_OK;
    }
#if defined(_WIN32)
    {
        const char *appdata = env_utf8("APPDATA");
        if (appdata && *appdata) {
            /* env_utf8 在 Windows 上返回静态缓冲,先拷进局部再用(settings_join 不会再取环境变量,
             * 但拷一份能免掉"将来有人在这里插一次 env 调用"的隐患) */
            char base[768];
            snprintf(base, sizeof(base), "%s", appdata);
            if (settings_join(out, out_len, base, "SilentXCraftLauncher") == 0) {
                return SXCL_SETTINGS_OK;
            }
            return settings_arg_err(err, err_len, "配置目录路径太长");
        }
        const char *home = env_utf8("USERPROFILE");
        if (home && *home) {
            char base[768];
            snprintf(base, sizeof(base), "%s\\AppData\\Roaming", home);
            if (settings_join(out, out_len, base, "SilentXCraftLauncher") == 0) {
                return SXCL_SETTINGS_OK;
            }
            return settings_arg_err(err, err_len, "配置目录路径太长");
        }
        return settings_arg_err(err, err_len, "拿不到 APPDATA/USERPROFILE,拼不出配置目录");
    }
#elif defined(__ANDROID__)
    {
        /* Android:HOME 是 "/",~/.config 写不进去。应用私有 files 目录由打包层通过
         * SXCL_ANDROID_FILES 传进来(Activity 的 getFilesDir()),与 paths.c 同口径。 */
        const char *files = env_utf8("SXCL_ANDROID_FILES");
        if (files && *files) {
            char base[768];
            snprintf(base, sizeof(base), "%s", files);
            if (settings_join(out, out_len, base, "SilentXCraftLauncher") == 0) {
                return SXCL_SETTINGS_OK;
            }
            return settings_arg_err(err, err_len, "配置目录路径太长");
        }
        return settings_arg_err(err, err_len,
                                "Android 上没拿到 SXCL_ANDROID_FILES(打包层入口应设为应用 files 目录)");
    }
#elif defined(__APPLE__)
    {
        const char *home = env_utf8("HOME");
        if (home && *home) {
            char base[768];
            snprintf(base, sizeof(base), "%s/Library/Application Support", home);
            if (settings_join(out, out_len, base, "SilentXCraftLauncher") == 0) {
                return SXCL_SETTINGS_OK;
            }
            return settings_arg_err(err, err_len, "配置目录路径太长");
        }
        return settings_arg_err(err, err_len, "拿不到 HOME,拼不出配置目录");
    }
#else
    {
        /* 与 Python platform.py:156-157 一致:XDG 优先,目录名小写短横线形式 */
        const char *xdg = env_utf8("XDG_CONFIG_HOME");
        if (xdg && *xdg) {
            char base[768];
            snprintf(base, sizeof(base), "%s", xdg);
            if (settings_join(out, out_len, base, "silentxcraftlauncher") == 0) {
                return SXCL_SETTINGS_OK;
            }
            return settings_arg_err(err, err_len, "配置目录路径太长");
        }
        const char *home = env_utf8("HOME");
        if (home && *home) {
            char base[768];
            snprintf(base, sizeof(base), "%s/.config", home);
            if (settings_join(out, out_len, base, "silentxcraftlauncher") == 0) {
                return SXCL_SETTINGS_OK;
            }
            return settings_arg_err(err, err_len, "配置目录路径太长");
        }
        return settings_arg_err(err, err_len, "拿不到 HOME/XDG_CONFIG_HOME,拼不出配置目录");
    }
#endif
}

int sxcl_settings_default_path(char *out, size_t out_len, char *err, size_t err_len)
{
    if (!out || out_len == 0) {
        return settings_arg_err(err, err_len, "参数不合法");
    }
    out[0] = '\0';
    char dir[768];
    const int rc = sxcl_settings_default_dir(dir, sizeof(dir), err, err_len);
    if (rc != SXCL_SETTINGS_OK) {
        return rc;
    }
    if (settings_join(out, out_len, dir, "settings.conf") != 0) {
        return settings_arg_err(err, err_len, "设置文件路径太长");
    }
    return SXCL_SETTINGS_OK;
}

/* ── 启动期解析(环境变量优先,见 settings.h) ── */

static int text_is_one_of(const char *value, const char *const *items, size_t count)
{
    if (!value) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (value_is(value, items[i])) {
            return 1;
        }
    }
    return 0;
}

const char *sxcl_settings_resolved_theme(sxcl_settings *settings)
{
    static const char *const kThemes[] = { "auto", "light", "dark" };
    const char *env = env_utf8("SXCL_UI_THEME");
    if (env && text_is_one_of(env, kThemes, 3)) {
        return env;
    }
    const char *stored = sxcl_settings_ui_theme(settings);
    if (stored && text_is_one_of(stored, kThemes, 3)) {
        return stored;
    }
    return SXCL_SETTINGS_DEFAULT_THEME;
}

/* #rrggbb(大小写不敏感)。不合法的值一律回默认,免得把"随手写错的颜色"套到界面上。 */
static int accent_looks_valid(const char *value)
{
    if (!value || value[0] != '#' || strlen(value) != 7) {
        return 0;
    }
    for (int i = 1; i < 7; ++i) {
        const char c = value[i];
        const int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return 0;
        }
    }
    return 1;
}

const char *sxcl_settings_resolved_accent(sxcl_settings *settings)
{
    const char *env = env_utf8("SXCL_UI_ACCENT");
    if (accent_looks_valid(env)) {
        return env;
    }
    const char *stored = sxcl_settings_get(settings, "ui.accent", NULL);
    if (accent_looks_valid(stored)) {
        return stored;
    }
    return SXCL_SETTINGS_DEFAULT_ACCENT;
}

const char *sxcl_settings_resolved_language(sxcl_settings *settings)
{
    /* 不在这里统一大小写:设置文件里一直是 "zh-CN"/"en-US"(与 launcher_config.py 的
     * 枚举值一致),归一化交给 sxcl_lang_open/sxcl_lang_normalize_code。 */
    const char *env = env_utf8("SXCL_UI_LANG");
    if (env && *env) {
        return env;
    }
    return sxcl_settings_ui_language(settings);
}

const char *sxcl_settings_resolved_game_dir(sxcl_settings *settings)
{
    const char *env = env_utf8("SXCL_GAME_DIR");
    if (env && *env) {
        return env;
    }
    return sxcl_settings_game_default_dir(settings);
}

void sxcl_settings_resolve_download(sxcl_settings *settings, sxcl_settings_download *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    const char *env_workers = env_utf8("SXCL_DL_WORKERS");
    int64_t workers = 0;
    if (env_workers && parse_int64(env_workers, &workers) == 0) {
        out->workers = (int)workers;
    } else {
        out->workers = (int)sxcl_settings_download_workers(settings);
    }

    const char *env_rate = env_utf8("SXCL_DL_RATE");
    double rate = 0.0;
    if (env_rate && parse_double(env_rate, &rate) == 0) {
        out->rate_bps = rate;
    } else {
        out->rate_bps = sxcl_settings_get_double(settings, "download.rate", 0.0);
    }

    const char *env_conn = env_utf8("SXCL_DL_MAX_CONN");
    int64_t conn = 0;
    if (env_conn && parse_int64(env_conn, &conn) == 0) {
        out->max_conn_per_file = (int)conn;
    } else {
        out->max_conn_per_file = (int)sxcl_settings_download_max_conn(settings);
    }

    const char *env_cache = env_utf8("SXCL_DL_CACHE_DIR");
    const char *cache = (env_cache && *env_cache) ? env_cache : sxcl_settings_download_cache_dir(settings);
    if (cache && *cache) {
        snprintf(out->cache_dir, sizeof(out->cache_dir), "%s", cache);
    }
}

/* ── 每实例设置("instance.<实例名>.<键>") ── */

static char *make_instance_key(const char *instance, const char *key)
{
    if (!instance || !*instance || !key || !*key) {
        return NULL;
    }
    static const char prefix[] = "instance.";
    const size_t ilen = strlen(instance);
    const size_t klen = strlen(key);
    char *out = (char *)malloc(sizeof(prefix) + ilen + klen + 1); /* prefix 自带 NUL,再 +1 给 '.' */
    if (!out) {
        return NULL;
    }
    memcpy(out, prefix, sizeof(prefix) - 1);
    memcpy(out + sizeof(prefix) - 1, instance, ilen);
    out[sizeof(prefix) - 1 + ilen] = '.';
    memcpy(out + sizeof(prefix) + ilen, key, klen + 1);
    if (!key_ok(out)) {
        free(out);
        return NULL;
    }
    return out;
}

const char *sxcl_settings_instance_get(sxcl_settings *settings, const char *instance, const char *key, const char *def)
{
    char *full = make_instance_key(instance, key);
    if (!full) {
        return def;
    }
    const char *v = sxcl_settings_get(settings, full, def);
    free(full); /* 返回的是表里的值,不是 full,可以安全释放 */
    return v;
}

int sxcl_settings_instance_set(sxcl_settings *settings, const char *instance, const char *key, const char *value)
{
    char *full = make_instance_key(instance, key);
    if (!full) {
        return -1;
    }
    const int rc = sxcl_settings_set(settings, full, value);
    free(full);
    return rc;
}

const char *sxcl_settings_instance_graphics_api(sxcl_settings *settings, const char *instance)
{
    return sxcl_settings_instance_get(settings, instance, "graphicsApi", "default");
}

/* ── 遍历 ── */

size_t sxcl_settings_count(const sxcl_settings *settings)
{
    return settings ? settings->count : 0;
}

const char *sxcl_settings_key_at(const sxcl_settings *settings, size_t index)
{
    return (settings && index < settings->count) ? settings->entries[index].key : NULL;
}

const char *sxcl_settings_value_at(const sxcl_settings *settings, size_t index)
{
    return (settings && index < settings->count) ? settings->entries[index].value : NULL;
}
