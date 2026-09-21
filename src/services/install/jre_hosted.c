/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* jre_hosted.c - 自托管 JRE 的下载/校验/解包/落标记(见 include/sxcl/jre_hosted.h)。
 *
 * 链路:index.json -> 挑组件 -> 引擎下载 .tar.xz(SHA-256 强校验)-> sxcl_tar_extract_file
 *       解到 <目标>/ -> 核对 bin/java -> 写 jre.json。
 *
 * 三道闸,顺序固定(越早点拦住越好):
 *   1) **version 判据**:<目标>/jre.json 里的 version 与 index 一致 -> 整个跳过(不联网);
 *   2) **磁盘**:下第一个字节之前先算"还差多少字节",可用空间不够直接 ERR_DISK;
 *   3) **逐文件 sha256**:<目标>/.archives/<名字> 已在盘上且 sha256 相符 -> 跳过下载;
 *      引擎那边对每个任务仍然按 sha256 强校验(校验不过不许落盘)。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/jre_hosted.h"

#include <ctype.h>   /* isxdigit:解析 .sha256 侧车文件 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* GetTickCount64:下载速度采样 */
#else
#  include <sys/time.h>  /* gettimeofday:下载速度采样(不引 clock_gettime,理由同 java_runtime.c) */
#endif

#include "sxcl/android.h"
#include "sxcl/fs.h"
#include "sxcl/http.h"
#include "sxcl/java_runtime.h"   /* sxcl_java_runtime_default_root / _java_path / _is_installed */
#include "sxcl/json.h"
#include "sxcl/lang.h"
#include "sxcl/tar.h"
#include "sxcl/verify.h"

#define JRE_ARCHIVE_DIR ".archives"
#define JRE_MAX_COMPONENTS 32
#define JRE_URL_CANDIDATES 4

/* ── 小工具 ── */

static void set_text(char *err, size_t err_len, const char *text)
{
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", text != NULL ? text : "");
        err[err_len - 1] = '\0';
    }
}

static void set_textf(char *err, size_t err_len, const char *fmt, ...)
{
    char buf[SXCL_JRE_ERROR_MAX];
    va_list ap;
    if (err == NULL || err_len == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    set_text(err, err_len, buf);
}

static void copy_str(char *out, size_t out_len, const char *src)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (src == NULL) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%s", src);
}

static int join_path(char *out, size_t out_len, const char *dir, const char *leaf)
{
    const size_t dl = strlen(dir);
    const size_t ll = strlen(leaf);
    const int need_sep = (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\') ? 1 : 0;
    if (dl + (size_t)need_sep + ll + 1 > out_len) {
        return -1;
    }
    memcpy(out, dir, dl);
    if (need_sep) {
        out[dl] = '/';
    }
    memcpy(out + dl + (size_t)need_sep, leaf, ll + 1);
    return 0;
}

/** 取 basename("jre17/universal.tar.xz" -> "universal.tar.xz")。 */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    const char *best = path;
    if (slash != NULL) {
        best = slash + 1;
    }
    if (back != NULL && back + 1 > best) {
        best = back + 1;
    }
    return best;
}

static int is_hex(const char *s, size_t want)
{
    size_t i;
    if (s == NULL || strlen(s) != want) {
        return 0;
    }
    for (i = 0; i < want; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

/** ISO8601(UTC,秒精度)。写进 index 的 generatedAt 与 jre.json 的 installedAt。
 *  自己把"从 1970 起的秒数"折成年月日,而不是用 gmtime/gmtime_r —— 后者要么不是线程安全
 *  (gmtime 的静态缓冲),要么要吃 _POSIX_C_SOURCE(本文件不定义,GCC 下会变成隐式声明)。 */
static void now_iso8601(char *out, size_t out_len)
{
    const time_t now = time(NULL);
    long long secs = (long long)now;
    long long days;
    int rem;
    int year = 1970;
    int month = 1;
    int day;
    static const int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int hour, minute, second;
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (secs < 0) {
        snprintf(out, out_len, "1970-01-01T00:00:00Z");
        return;
    }
    days = secs / 86400;
    rem = (int)(secs % 86400);
    hour = rem / 3600;
    minute = (rem % 3600) / 60;
    second = rem % 60;
    for (;;) {
        const int leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 1 : 0;
        const long long ydays = 365 + leap;
        if (days < ydays) {
            break;
        }
        days -= ydays;
        ++year;
    }
    day = (int)days + 1;
    for (;;) {
        int mdays = kMonthDays[month - 1];
        if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
            mdays = 29;
        }
        if (day <= mdays) {
            break;
        }
        day -= mdays;
        ++month;
    }
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hour, minute,
             second);
}

/** 墙钟秒(只用于速度采样)。Windows 用 GetTickCount64(不受系统时间调整影响);
 *  POSIX 用 gettimeofday(到处都有,且不需要 _POSIX_C_SOURCE)。 */
static double now_seconds(void)
{
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
    }
    return (double)time(NULL);
#endif
}

/* ── 返回码/阶段名 ── */

const char *sxcl_jre_code_name(int code)
{
    switch (code) {
    case SXCL_JRE_OK:
        return "ok";
    case SXCL_JRE_ERR_ARG:
        return "arg";
    case SXCL_JRE_ERR_INDEX:
        return "index";
    case SXCL_JRE_ERR_COMPONENT:
        return "component";
    case SXCL_JRE_ERR_NET:
        return "net";
    case SXCL_JRE_ERR_DOWNLOAD:
        return "download";
    case SXCL_JRE_ERR_EXTRACT:
        return "extract";
    case SXCL_JRE_ERR_FINISH:
        return "finish";
    case SXCL_JRE_ERR_CANCELLED:
        return "cancelled";
    case SXCL_JRE_ERR_NOMEM:
        return "nomem";
    case SXCL_JRE_ERR_IO:
        return "io";
    case SXCL_JRE_ERR_DISK:
        return "disk";
    case SXCL_JRE_ERR_NO_ABI:
        return "abi";
    default:
        return "?";
    }
}

const char *sxcl_jre_stage_id(sxcl_jre_stage stage)
{
    switch (stage) {
    case SXCL_JRE_STAGE_INDEX:
        return "index";
    case SXCL_JRE_STAGE_DOWNLOAD:
        return "download";
    case SXCL_JRE_STAGE_EXTRACT:
        return "extract";
    case SXCL_JRE_STAGE_FINISH:
        return "finish";
    default:
        return "none";
    }
}

const char *sxcl_jre_stage_name(sxcl_jre_stage stage)
{
    switch (stage) {
    case SXCL_JRE_STAGE_INDEX:
        return "获取组件清单";
    case SXCL_JRE_STAGE_DOWNLOAD:
        return "下载并校验安装包";
    case SXCL_JRE_STAGE_EXTRACT:
        return "解包安装包";
    case SXCL_JRE_STAGE_FINISH:
        return "收尾";
    default:
        return "未失败";
    }
}

/* ── index.json 解析 ── */

/* ── 一条 file 条目的公共校验 ──
 * 硬规矩(引擎本来就按它们强校验,这里在解析阶段就先拦一道):
 *   size 必须是正数;sha256 要么是 64 位十六进制,要么是一个 .sha256 文件的 URL。 */
static int jre_check_file(const char *what, const char *rel, int64_t size, const char *sha256,
                          const char *sha1, sxcl_jre_file *dst, char *err, size_t err_len)
{
    if (rel == NULL || rel[0] == '\0') {
        set_textf(err, err_len, "%s 缺 file/url 字段", what);
        return SXCL_JRE_ERR_INDEX;
    }
    if (strlen(rel) >= sizeof(dst->file)) {
        set_textf(err, err_len, "%s 的路径/URL 太长(上限 %u 字节)", what,
                  (unsigned)(sizeof(dst->file) - 1));
        return SXCL_JRE_ERR_INDEX;
    }
    /* rel 允许两种:相对路径(相对清单)或**绝对 URL**。相对路径不许 ..、不许以 '/' 开头。 */
    if (strncmp(rel, "http://", 7) != 0 && strncmp(rel, "https://", 8) != 0 &&
        (strstr(rel, "://") != NULL || strstr(rel, "..") != NULL || rel[0] == '/')) {
        set_textf(err, err_len, "%s 的路径不合法(相对路径不许绝对/不许 ..): %s", what, rel);
        return SXCL_JRE_ERR_INDEX;
    }
    if (size <= 0) {
        set_textf(err, err_len,
                  "%s 缺 size(字节数,必须是正数;size_mb 只是给人看的,不能拿来当 size)",
                  what);
        return SXCL_JRE_ERR_INDEX;
    }
    /* sha256 三种形态(见头文件与 docs/19) */
    if (sha256 == NULL || sha256[0] == '\0') {
        dst->no_hash = 1; /* 形态 3:没有校验信息 -> 只校验 size,并如实报出来 */
    } else if (is_hex(sha256, 64)) {
        copy_str(dst->sha256, sizeof(dst->sha256), sha256);
    } else if (strncmp(sha256, "http://", 7) == 0 || strncmp(sha256, "https://", 8) == 0) {
        if (strlen(sha256) >= sizeof(dst->sha256_url)) {
            set_textf(err, err_len, "%s 的 sha256 侧车 URL 太长", what);
            return SXCL_JRE_ERR_INDEX;
        }
        copy_str(dst->sha256_url, sizeof(dst->sha256_url), sha256);
    } else {
        set_textf(err, err_len,
                  "%s 的 sha256 既不是 64 位十六进制,也不是一个 http(s) 的 .sha256 URL", what);
        return SXCL_JRE_ERR_INDEX;
    }
    /* sha1 同样三种形态(可以整个不给;给了就要说得清) */
    if (sha1 != NULL && sha1[0] != '\0') {
        if (is_hex(sha1, 40)) {
            copy_str(dst->sha1, sizeof(dst->sha1), sha1);
        } else if (strncmp(sha1, "http://", 7) == 0 || strncmp(sha1, "https://", 8) == 0) {
            if (strlen(sha1) >= sizeof(dst->sha1_url)) {
                set_textf(err, err_len, "%s 的 sha1 侧车 URL 太长", what);
                return SXCL_JRE_ERR_INDEX;
            }
            copy_str(dst->sha1_url, sizeof(dst->sha1_url), sha1);
        } else {
            set_textf(err, err_len,
                      "%s 的 sha1 既不是 40 位十六进制,也不是一个 http(s) 的 .sha1 URL", what);
            return SXCL_JRE_ERR_INDEX;
        }
    }
    copy_str(dst->file, sizeof(dst->file), rel);
    dst->size = size;
    return SXCL_JRE_OK;
}

/** 读一条 "files[]" 风格的条目(自带托管形式)。 */
static int jre_parse_file_obj(const sxcl_json_value *f, const char *id, size_t index,
                              sxcl_jre_file *dst, char *err, size_t err_len)
{
    char what[128];
    const char *rel;
    const char *sha256;
    const char *sha1;
    int64_t size;
    if (f == NULL || sxcl_json_type_of(f) != SXCL_JSON_OBJECT) {
        set_textf(err, err_len, "%s 的 files[%u] 不是对象", id, (unsigned)index);
        return SXCL_JRE_ERR_INDEX;
    }
    memset(dst, 0, sizeof(*dst));
    rel = sxcl_json_get_string(f, "file", "");
    if (rel[0] == '\0') {
        rel = sxcl_json_get_string(f, "url", ""); /* url 也认(与 Java_index 的命名对齐) */
    }
    sha256 = sxcl_json_get_string(f, "sha256", "");
    sha1 = sxcl_json_get_string(f, "sha1", "");
    size = sxcl_json_get_int64(f, "size", -1);
    /* mirrors[0] -> 第二条候选(清单在 GitHub、包在云上时就是这条);侧车兜底也用它 */
    {
        const sxcl_json_value *mirrors = sxcl_json_get(f, "mirrors");
        const char *m0 = (mirrors != NULL && sxcl_json_size(mirrors) > 0)
                             ? sxcl_json_string(sxcl_json_at(mirrors, 0))
                             : NULL;
        if (m0 != NULL && m0[0] != '\0' && strlen(m0) < sizeof(dst->mirror)) {
            copy_str(dst->mirror, sizeof(dst->mirror), m0);
        }
    }
    if (size <= 0) {
        /* size 没给就退到 size_mb(老 schema 只有它);四舍五入到字节,并在文档里写明不精确 */
        const int64_t mb = sxcl_json_get_int64(f, "size_mb", -1);
        if (mb > 0) {
            size = (int64_t)((double)mb * 1048576.0 + 0.5);
        }
    }
    snprintf(what, sizeof(what), "%s 的 %s", id, rel[0] != '\0' ? rel : "files[]");
    return jre_check_file(what, rel, size, sha256, sha1, dst, err, err_len);
}

/** 解析**既有**的 SXCL/Java_index.json(versions -> platforms -> <os> -> <abi>)。
 *  只取 platforms.android.<abi>;其它平台(桌面)不是我们要分发的运行时,忽略。 */
static int jre_parse_java_index(const sxcl_json_value *root, sxcl_jre_component *out, size_t cap,
                                char *err, size_t err_len)
{
    const sxcl_json_value *versions = sxcl_json_get(root, "versions");
    const size_t vcount = versions != NULL ? sxcl_json_member_count(versions) : 0;
    size_t i;
    size_t n = 0;
    if (vcount == 0) {
        set_text(err, err_len, "Java_index.json 里没有 versions(或它是空的)");
        return SXCL_JRE_ERR_INDEX;
    }
    for (i = 0; i < vcount && n < cap; ++i) {
        const char *major_key = sxcl_json_member_key(versions, i);
        const sxcl_json_value *node = sxcl_json_member_value(versions, i);
        const sxcl_json_value *platforms;
        const sxcl_json_value *android;
        size_t acount;
        size_t k;
        int major = 0;
        if (major_key == NULL || node == NULL) {
            continue;
        }
        major = (int)strtol(major_key, NULL, 10);
        if (major <= 0) {
            continue; /* 键不是主版本号(将来可能有别的键),跳过而不是报错 */
        }
        platforms = sxcl_json_get(node, "platforms");
        android = platforms != NULL ? sxcl_json_get(platforms, "android") : NULL;
        if (android == NULL || sxcl_json_type_of(android) != SXCL_JSON_OBJECT) {
            continue; /* 这个主版本还没上安卓那一档 */
        }
        acount = sxcl_json_member_count(android);
        for (k = 0; k < acount && n < cap; ++k) {
            const char *abi = sxcl_json_member_key(android, k);
            const sxcl_json_value *p = sxcl_json_member_value(android, k);
            sxcl_jre_component *c = &out[n];
            const char *id;
            const char *version;
            const char *url;
            const char *sha256;
            const char *sha1;
            int64_t size;
            char what[160];
            if (abi == NULL || p == NULL || sxcl_json_type_of(p) != SXCL_JSON_OBJECT) {
                continue;
            }
            memset(c, 0, sizeof(*c));
            id = sxcl_json_get_string(p, "id", "");
            if (id[0] == '\0') {
                snprintf(c->id, sizeof(c->id), "jre%d", major);
            } else {
                copy_str(c->id, sizeof(c->id), id);
            }
            /* version:优先平台档自己的(扩展字段),退回版本节点的 name(老字段) */
            version = sxcl_json_get_string(p, "version", "");
            if (version[0] == '\0') {
                version = sxcl_json_get_string(node, "name", "");
            }
            if (version[0] == '\0') {
                set_textf(err, err_len, "versions.%s.platforms.android.%s 缺 version(重装判据)",
                          major_key, abi);
                return SXCL_JRE_ERR_INDEX;
            }
            if (strlen(version) >= sizeof(c->version)) {
                set_textf(err, err_len, "versions.%s 的 version 太长", major_key);
                return SXCL_JRE_ERR_INDEX;
            }
            c->java_major = major;
            copy_str(c->version, sizeof(c->version), version);
            url = sxcl_json_get_string(p, "url", "");
            sha256 = sxcl_json_get_string(p, "sha256", "");
            sha1 = sxcl_json_get_string(p, "sha1", "");
            size = sxcl_json_get_int64(p, "size", -1);
            if (size <= 0) {
                const double mb = sxcl_json_get_string(p, "size_mb", "")[0] != '\0'
                                      ? (double)atof(sxcl_json_get_string(p, "size_mb", "0"))
                                      : -1.0;
                if (mb > 0.0) {
                    size = (int64_t)(mb * 1048576.0 + 0.5);
                }
            }
            snprintf(what, sizeof(what), "versions.%s.platforms.android.%s", major_key, abi);
            if (jre_check_file(what, url, size, sha256, sha1, &c->files[0], err, err_len) !=
                SXCL_JRE_OK) {
                return SXCL_JRE_ERR_INDEX;
            }
            c->files[0].platform_selected = 1; /* android.<abi> 这一档就是给这个 ABI 的 */
            /* mirrors[0] -> 第二条候选(清单在 GitHub、包在 SC 时用) */
            {
                const sxcl_json_value *mirrors = sxcl_json_get(p, "mirrors");
                const char *m0 = (mirrors != NULL && sxcl_json_size(mirrors) > 0)
                                     ? sxcl_json_string(sxcl_json_at(mirrors, 0))
                                     : NULL;
                if (m0 != NULL && m0[0] != '\0') {
                    copy_str(c->files[0].mirror, sizeof(c->files[0].mirror), m0);
                }
            }
            c->file_count = 1;
            /* extra_files[]:同一个 ABI 还需要额外解一份时(例如 FCL 式的 bin-<abi>.tar.xz) */
            {
                const sxcl_json_value *extra = sxcl_json_get(p, "extra_files");
                const size_t ecount = extra != NULL ? sxcl_json_size(extra) : 0;
                size_t e;
                for (e = 0; e < ecount; ++e) {
                    sxcl_jre_file *dst;
                    const sxcl_json_value *ef = sxcl_json_at(extra, e);
                    if (c->file_count >= SXCL_JRE_MAX_FILES) {
                        set_textf(err, err_len, "%s 的文件条目超过上限 %d", what,
                                  SXCL_JRE_MAX_FILES);
                        return SXCL_JRE_ERR_INDEX;
                    }
                    dst = &c->files[c->file_count];
                    {
                        const char *erel = sxcl_json_get_string(ef, "url", "");
                        const char *ename = sxcl_json_get_string(ef, "name", "");
                        const char *esha = sxcl_json_get_string(ef, "sha256", "");
                        const char *esha1 = sxcl_json_get_string(ef, "sha1", "");
                        int64_t esize = sxcl_json_get_int64(ef, "size", -1);
                        char ewhat[192];
                        if (ename[0] != '\0' && erel[0] != '\0') {
                            snprintf(ewhat, sizeof(ewhat), "%s 的 %s", what, ename);
                        } else {
                            snprintf(ewhat, sizeof(ewhat), "%s 的 extra_files[%u]", what,
                                     (unsigned)e);
                        }
                        if (jre_check_file(ewhat, erel, esize, esha, esha1, dst, err, err_len) !=
                            SXCL_JRE_OK) {
                            return SXCL_JRE_ERR_INDEX;
                        }
                        dst->platform_selected = 1;
                        {
                            const sxcl_json_value *em = sxcl_json_get(ef, "mirrors");
                            const char *em0 = (em != NULL && sxcl_json_size(em) > 0)
                                                  ? sxcl_json_string(sxcl_json_at(em, 0))
                                                  : NULL;
                            if (em0 != NULL && em0[0] != '\0') {
                                copy_str(dst->mirror, sizeof(dst->mirror), em0);
                            }
                        }
                        /* extra_files 的 name 用来判 ABI:bin-<别的 abi> 不装到本机 */
                        if (ename[0] != '\0') {
                            sxcl_jre_file probe;
                            memset(&probe, 0, sizeof(probe));
                            copy_str(probe.file, sizeof(probe.file), ename);
                            if (sxcl_jre_file_applies(&probe, abi) == 0) {
                                dst->platform_selected = 0; /* 交给通用的 ABI 过滤去挡 */
                            }
                        }
                    }
                    ++c->file_count;
                }
            }
            ++n;
        }
    }
    if (n == 0) {
        set_text(err, err_len,
                 "Java_index.json 里没有任何 versions.*.platforms.android.* 条目"
                 "(桌面那份清单里没有安卓 JRE)");
        return SXCL_JRE_ERR_INDEX;
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return (int)n;
}

/** 把 ABI 名归一化比较:清单用 "arm64-v8a",我们内部用 "arm64";x86_64 / x64 同理。
 *  认不出就按"字面相等"比。 */
static int abi_matches(const char *a, const char *b)
{
    static const char *const groups[][3] = {
        {"arm64-v8a", "arm64", "aarch64"},
        {"armeabi-v7a", "arm", "armv7"},
        {"x86_64", "x64", "amd64"},
        {"x86", "i686", "i386"},
    };
    size_t g;
    if (a == NULL || b == NULL || *a == '\0' || *b == '\0') {
        return 0;
    }
    if (strcmp(a, b) == 0) {
        return 1;
    }
    for (g = 0; g < sizeof(groups) / sizeof(groups[0]); ++g) {
        int has_a = 0;
        int has_b = 0;
        int i;
        for (i = 0; i < 3; ++i) {
            if (strcmp(groups[g][i], a) == 0) {
                has_a = 1;
            }
            if (strcmp(groups[g][i], b) == 0) {
                has_b = 1;
            }
        }
        if (has_a && has_b) {
            return 1;
        }
    }
    return 0;
}

/** 解析 **sxcl.jre.index/1**(我们自己的安卓 JRE 来源;见头文件与 docs/19 §2.1)。
 *  只取 packages[](运行时本体);version_file / notice / manifest / source 不参与安装。 */
static int jre_parse_sxcl_index(const sxcl_json_value *root, sxcl_jre_component *out, size_t cap,
                                char *err, size_t err_len)
{
    const sxcl_json_value *comps = sxcl_json_get(root, "components");
    size_t members;
    size_t i;
    size_t n = 0;
    if (comps == NULL || sxcl_json_type_of(comps) != SXCL_JSON_OBJECT) {
        set_textf(err, err_len, "%s 的 components 不是对象(这份 schema 里它按组件名索引)",
                  SXCL_JRE_INDEX_SCHEMA_STRING);
        return SXCL_JRE_ERR_INDEX;
    }
    members = sxcl_json_member_count(comps);
    if (members == 0) {
        set_text(err, err_len, "清单的 components 是空的");
        return SXCL_JRE_ERR_INDEX;
    }
    for (i = 0; i < members && n < cap; ++i) {
        const char *key = sxcl_json_member_key(comps, i);
        const sxcl_json_value *node = sxcl_json_member_value(comps, i);
        sxcl_jre_component *c = &out[n];
        const char *component;
        const char *version;
        const char *abi;
        const char *id;
        const sxcl_json_value *pkgs;
        size_t pcount;
        size_t k;
        if (key == NULL || node == NULL || sxcl_json_type_of(node) != SXCL_JSON_OBJECT) {
            set_textf(err, err_len, "components.%s 不是对象", key != NULL ? key : "?");
            return SXCL_JRE_ERR_INDEX;
        }
        memset(c, 0, sizeof(*c));
        component = sxcl_json_get_string(node, "component", key);
        version = sxcl_json_get_string(node, "version", "");
        if (version[0] == '\0') {
            version = sxcl_json_get_string(node, "java_version", "");
        }
        abi = sxcl_json_get_string(node, "abi", "");
        id = sxcl_json_get_string(node, "id", "");
        c->java_major = (int)sxcl_json_get_int64(node, "major", 0);
        if (c->java_major <= 0) {
            /* 键或 component 里的数字兜底(例如 "jre17") */
            const char *q = key;
            while (q != NULL && *q != '\0' && !(*q >= '0' && *q <= '9')) {
                ++q;
            }
            if (q != NULL && *q != '\0') {
                c->java_major = (int)strtol(q, NULL, 10);
            }
        }
        if (component == NULL || component[0] == '\0' || version[0] == '\0' ||
            c->java_major <= 0) {
            set_textf(err, err_len, "components.%s 缺 component/version/major(三个都必填)", key);
            return SXCL_JRE_ERR_INDEX;
        }
        if (strlen(component) >= sizeof(c->id) || strlen(version) >= sizeof(c->version)) {
            set_textf(err, err_len, "components.%s 的 component/version 太长", key);
            return SXCL_JRE_ERR_INDEX;
        }
        copy_str(c->id, sizeof(c->id), component);
        copy_str(c->version, sizeof(c->version), version);
        copy_str(c->abi, sizeof(c->abi), abi);
        copy_str(c->index_id, sizeof(c->index_id), id);
        c->installed_bytes = sxcl_json_get_int64(node, "installed_bytes", 0);
        /* 落盘相对路径:<component>/<version>/<abi>(与远端 jre17/ 这个前缀对齐,
         * 多版本可以并排躺着)。清单里没有 abi 就退回 <component>/<version>。 */
        if (abi[0] != '\0') {
            snprintf(c->dir_key, sizeof(c->dir_key), "%s/%s/%s", component, version, abi);
        } else {
            snprintf(c->dir_key, sizeof(c->dir_key), "%s/%s", component, version);
        }
        if (strstr(c->dir_key, "..") != NULL || c->dir_key[0] == '/') {
            set_textf(err, err_len, "components.%s 拼出的落盘路径不合法: %s", key, c->dir_key);
            return SXCL_JRE_ERR_INDEX;
        }
        pkgs = sxcl_json_get(node, "packages");
        if (pkgs == NULL || sxcl_json_type_of(pkgs) != SXCL_JSON_ARRAY || sxcl_json_size(pkgs) == 0) {
            set_textf(err, err_len, "components.%s 没有 packages 数组(运行时本体在它里面)", key);
            return SXCL_JRE_ERR_INDEX;
        }
        pcount = sxcl_json_size(pkgs);
        if (pcount > SXCL_JRE_MAX_FILES) {
            set_textf(err, err_len, "components.%s 的 packages 有 %u 个,超过上限 %d", key,
                      (unsigned)pcount, SXCL_JRE_MAX_FILES);
            return SXCL_JRE_ERR_INDEX;
        }
        for (k = 0; k < pcount; ++k) {
            const sxcl_json_value *pk = sxcl_json_at(pkgs, k);
            sxcl_jre_file *dst = &c->files[k];
            const char *rel;
            const char *sha256;
            const char *sha1;
            int64_t size;
            char what[192];
            if (pk == NULL || sxcl_json_type_of(pk) != SXCL_JSON_OBJECT) {
                set_textf(err, err_len, "components.%s.packages[%u] 不是对象", key, (unsigned)k);
                return SXCL_JRE_ERR_INDEX;
            }
            memset(dst, 0, sizeof(*dst));
            rel = sxcl_json_get_string(pk, "url", "");
            sha256 = sxcl_json_get_string(pk, "sha256", "");
            sha1 = sxcl_json_get_string(pk, "sha1", "");
            size = sxcl_json_get_int64(pk, "size", -1);
            snprintf(what, sizeof(what), "components.%s.packages[%u](%s)", key, (unsigned)k,
                     sxcl_json_get_string(pk, "name", "?"));
            if (jre_check_file(what, rel, size, sha256, sha1, dst, err, err_len) != SXCL_JRE_OK) {
                return SXCL_JRE_ERR_INDEX;
            }
            /* packages[] 是**这个 ABI 专属**的:不必再按文件名判 ABI */
            dst->platform_selected = 1;
            {
                const sxcl_json_value *mirrors = sxcl_json_get(pk, "mirrors");
                const char *m0 = (mirrors != NULL && sxcl_json_size(mirrors) > 0)
                                     ? sxcl_json_string(sxcl_json_at(mirrors, 0))
                                     : NULL;
                if (m0 != NULL && m0[0] != '\0' && strlen(m0) < sizeof(dst->mirror)) {
                    copy_str(dst->mirror, sizeof(dst->mirror), m0);
                }
            }
        }
        c->file_count = pcount;
        ++n;
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return (int)n;
}

/** 解析"files[] 数组"形式(自带托管:一个组件带 universal + bin-<abi>)。 */
static int jre_parse_components(const sxcl_json_value *root, sxcl_jre_component *out, size_t cap,
                                char *err, size_t err_len)
{
    const sxcl_json_value *components = sxcl_json_get(root, "components");
    const size_t members = components != NULL ? sxcl_json_size(components) : 0;
    size_t i;
    size_t n = 0;
    if (members == 0) {
        set_text(err, err_len, "清单里没有 components 数组(或它是空的)");
        return SXCL_JRE_ERR_INDEX;
    }
    for (i = 0; i < members && n < cap; ++i) {
        const sxcl_json_value *item = sxcl_json_at(components, i);
        sxcl_jre_component *c = &out[n];
        const char *id;
        const char *version;
        const sxcl_json_value *files;
        size_t fcount;
        size_t k;
        if (item == NULL || sxcl_json_type_of(item) != SXCL_JSON_OBJECT) {
            set_textf(err, err_len, "components[%u] 不是对象", (unsigned)i);
            return SXCL_JRE_ERR_INDEX;
        }
        memset(c, 0, sizeof(*c));
        id = sxcl_json_get_string(item, "id", "");
        version = sxcl_json_get_string(item, "version", "");
        c->java_major = (int)sxcl_json_get_int64(item, "javaMajor", 0);
        if (!id[0] || !version[0] || c->java_major <= 0) {
            set_textf(err, err_len, "components[%u] 缺 id/javaMajor/version(三个都必填)",
                      (unsigned)i);
            return SXCL_JRE_ERR_INDEX;
        }
        if (strlen(id) >= sizeof(c->id) || strlen(version) >= sizeof(c->version)) {
            set_textf(err, err_len, "components[%u] 的 id/version 太长", (unsigned)i);
            return SXCL_JRE_ERR_INDEX;
        }
        if (c->java_major > 99) {
            set_textf(err, err_len, "%s 的 javaMajor=%d 不像个主版本号", id, c->java_major);
            return SXCL_JRE_ERR_INDEX;
        }
        copy_str(c->id, sizeof(c->id), id);
        copy_str(c->version, sizeof(c->version), version);
        files = sxcl_json_get(item, "files");
        if (files == NULL || sxcl_json_type_of(files) != SXCL_JSON_ARRAY || sxcl_json_size(files) == 0) {
            set_textf(err, err_len, "%s 没有 files 数组(至少要有 universal.tar.xz)", id);
            return SXCL_JRE_ERR_INDEX;
        }
        fcount = sxcl_json_size(files);
        if (fcount > SXCL_JRE_MAX_FILES) {
            set_textf(err, err_len, "%s 的文件条目 %u 个,超过上限 %d", id, (unsigned)fcount,
                      SXCL_JRE_MAX_FILES);
            return SXCL_JRE_ERR_INDEX;
        }
        for (k = 0; k < fcount; ++k) {
            if (jre_parse_file_obj(sxcl_json_at(files, k), id, k, &c->files[k], err, err_len) !=
                SXCL_JRE_OK) {
                return SXCL_JRE_ERR_INDEX;
            }
        }
        c->file_count = fcount;
        ++n;
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return (int)n;
}

int sxcl_jre_index_parse(const char *text, size_t len, sxcl_jre_component *out, size_t cap,
                         char *err, size_t err_len)
{
    sxcl_json *doc;
    const sxcl_json_value *root;
    int n;
    if (text == NULL || out == NULL || cap == 0) {
        set_text(err, err_len, "清单文本为空");
        return SXCL_JRE_ERR_ARG;
    }
    {
        char parse_err[256];
        parse_err[0] = '\0';
        doc = sxcl_json_parse(text, len, parse_err, sizeof(parse_err));
        if (doc == NULL) {
            set_textf(err, err_len, "清单解析失败: %s", parse_err);
            return SXCL_JRE_ERR_INDEX;
        }
    }
    root = sxcl_json_root(doc);
    if (root == NULL || sxcl_json_type_of(root) != SXCL_JSON_OBJECT) {
        sxcl_json_free(doc);
        set_text(err, err_len, "清单根节点不是对象");
        return SXCL_JRE_ERR_INDEX;
    }
    /* 三种形式都吃,判定顺序固定(见 docs/19 §2.1):
     *   1) **sxcl.jre.index/1**(字符串 schema)——这是我们自己安卓 JRE 的**正式**来源;
     *   2) 既有的 SXCL/Java_index.json(versions -> platforms -> android -> <abi>)——桌面 Oracle JDK
     *      那份保持不动,安卓档只是它的一种扩展;
     *   3) 自带托管的 {"schema":1,"components":[…]}(数组)——tools/jre_pack.py 产出的 manifest.json。
     */
    {
        const sxcl_json_value *schema_value = sxcl_json_get(root, "schema");
        if (schema_value != NULL && sxcl_json_type_of(schema_value) == SXCL_JSON_STRING) {
            const char *schema = sxcl_json_string(schema_value);
            if (strcmp(schema, SXCL_JRE_INDEX_SCHEMA_STRING) != 0) {
                sxcl_json_free(doc);
                set_textf(err, err_len, "清单 schema 是 %s,本版本只认 %s", schema,
                          SXCL_JRE_INDEX_SCHEMA_STRING);
                return SXCL_JRE_ERR_INDEX;
            }
            n = jre_parse_sxcl_index(root, out, cap, err, err_len);
            sxcl_json_free(doc);
            return n;
        }
    }
    if (sxcl_json_get(root, "versions") != NULL) {
        n = jre_parse_java_index(root, out, cap, err, err_len);
        sxcl_json_free(doc);
        return n;
    }
    {
        const sxcl_json_value *comps = sxcl_json_get(root, "components");
        const int64_t schema = sxcl_json_get_int64(root, "schema", -1);
        if (comps == NULL || sxcl_json_type_of(comps) != SXCL_JSON_ARRAY) {
            sxcl_json_free(doc);
            set_textf(err, err_len,
                      "认不出这份清单:既不是 %s(字符串 schema)、也没有 versions、"
                      "也没有 components 数组(schema=%lld)",
                      SXCL_JRE_INDEX_SCHEMA_STRING, (long long)schema);
            return SXCL_JRE_ERR_INDEX;
        }
        if (schema != SXCL_JRE_INDEX_SCHEMA) {
            sxcl_json_free(doc);
            set_textf(err, err_len, "清单的 schema=%lld,本版本只认 schema=%d",
                      (long long)schema, SXCL_JRE_INDEX_SCHEMA);
            return SXCL_JRE_ERR_INDEX;
        }
    }
    n = jre_parse_components(root, out, cap, err, err_len);
    sxcl_json_free(doc);
    return n;
}
int sxcl_jre_index_pick(const sxcl_jre_component *list, size_t count, const char *component_id,
                        int java_major, sxcl_jre_component *out, char *err, size_t err_len)
{
    size_t i;
    const sxcl_jre_component *best = NULL;
    if (list == NULL || out == NULL || count == 0) {
        set_text(err, err_len, "组件列表为空");
        return SXCL_JRE_ERR_ARG;
    }
    if (component_id != NULL && *component_id != '\0') {
        for (i = 0; i < count; ++i) {
            if (strcmp(list[i].id, component_id) == 0) {
                *out = list[i];
                return SXCL_JRE_OK;
            }
        }
        set_textf(err, err_len, "index.json 里没有组件 %s", component_id);
        return SXCL_JRE_ERR_COMPONENT;
    }
    for (i = 0; i < count; ++i) {
        if (list[i].java_major == java_major) {
            if (best == NULL || strcmp(list[i].id, best->id) < 0) {
                best = &list[i];
            }
        }
    }
    if (best == NULL) {
        set_textf(err, err_len, "index.json 里没有 javaMajor=%d 的组件", java_major);
        return SXCL_JRE_ERR_COMPONENT;
    }
    *out = *best;
    return SXCL_JRE_OK;
}

const char *sxcl_jre_host_abi(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "armeabi-v7a";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "x64";
#endif
}

int sxcl_jre_file_applies(const sxcl_jre_file *file, const char *abi)
{
    const char *name;
    if (file == NULL) {
        return 0;
    }
    /* 来自 versions/platforms/android/<abi> 的那一档:清单作者已经替我们选好了 ABI,直接用 */
    if (file->platform_selected) {
        return 1;
    }
    name = base_name(file->file);
    if (strstr(name, "universal") != NULL) {
        return 1;
    }
    if (strncmp(name, "bin-", 4) == 0) {
        /* bin-<abi>.tar.xz(或 .tar.zst 之类:后缀我们不看,只看 "bin-" 后面那一段) */
        const char *want = (abi != NULL && *abi != '\0') ? abi : sxcl_jre_host_abi();
        const size_t want_len = strlen(want);
        if (strncmp(name + 4, want, want_len) == 0) {
            const char c = name[4 + want_len];
            if (c == '.' || c == '-' || c == '\0') {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

int sxcl_jre_default_index_url_is_placeholder(void)
{
    return (strstr(SXCL_JRE_INDEX_URL_DEFAULT, "<REPO>") != NULL) ? 1 : 0;
}

int sxcl_jre_resolve_index_url(const char *explicit_url, const char *setting_value,
                               const char *env_value, char *out, size_t out_len)
{
    const char *chosen = NULL;
    size_t n;
    if (out == NULL || out_len == 0) {
        return SXCL_JRE_ERR_ARG;
    }
    out[0] = '\0';
    if (explicit_url != NULL && *explicit_url != '\0') {
        chosen = explicit_url;
    } else if (env_value != NULL && *env_value != '\0') {
        chosen = env_value;
    } else if (setting_value != NULL && *setting_value != '\0') {
        chosen = setting_value;
    } else if (!sxcl_jre_default_index_url_is_placeholder()) {
        chosen = SXCL_JRE_INDEX_URL_DEFAULT; /* GitHub raw 默认值(仓库名钉死时才有) */
    }
    if (chosen == NULL) {
        return SXCL_JRE_ERR_ARG;
    }
    n = strlen(chosen);
    if (n + 1 > out_len) {
        return SXCL_JRE_ERR_ARG; /* 装不下就报错,绝不截断出半条 URL */
    }
    memcpy(out, chosen, n + 1);
    return SXCL_JRE_OK;
}

int sxcl_jre_join_url(const char *index_url, const char *rel, char *out, size_t out_len)
{
    const char *slash;
    size_t prefix;
    int written;
    if (index_url == NULL || rel == NULL || out == NULL || out_len == 0) {
        return SXCL_JRE_ERR_ARG;
    }
    /* 绝对 URL(**用户把包放在自己的云上、清单放在 GitHub 时就是这条**):原样用 */
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        if (strlen(rel) + 1u > out_len) {
            return SXCL_JRE_ERR_ARG;
        }
        memcpy(out, rel, strlen(rel) + 1u);
        return SXCL_JRE_OK;
    }
    if (strstr(rel, "://") != NULL || strstr(rel, "..") != NULL || rel[0] == '/' ||
        rel[0] == '\\') {
        return SXCL_JRE_ERR_ARG;
    }
    slash = strrchr(index_url, '/');
    prefix = (slash != NULL) ? (size_t)(slash - index_url) + 1u : strlen(index_url);
    if (prefix + strlen(rel) + 1u > out_len) {
        return SXCL_JRE_ERR_ARG;
    }
    memcpy(out, index_url, prefix);
    written = snprintf(out + prefix, out_len - prefix, "%s", rel);
    return (written > 0 && (size_t)written < out_len - prefix) ? SXCL_JRE_OK : SXCL_JRE_ERR_ARG;
}

int sxcl_jre_target_dir(const char *target_root, const char *component_id, char *out,
                        size_t out_len)
{
    char root[SXCL_JRE_PATH_MAX];
    char err[SXCL_JRE_ERROR_MAX];
    if (component_id == NULL || *component_id == '\0' || out == NULL || out_len == 0) {
        return SXCL_JRE_ERR_ARG;
    }
    if (target_root != NULL && *target_root != '\0') {
        copy_str(root, sizeof(root), target_root);
    } else {
        err[0] = '\0';
        if (sxcl_java_runtime_default_root(root, sizeof(root), err, sizeof(err)) !=
            SXCL_JAVA_RUNTIME_OK) {
            return SXCL_JRE_ERR_IO;
        }
    }
    /* 自托管包的目录名就是 index 里的 id(jre17),不是 java_runtime.c 的 <组件>-<平台> */
    if (join_path(out, out_len, root, component_id) != 0) {
        return SXCL_JRE_ERR_ARG;
    }
    return SXCL_JRE_OK;
}

/* ── jre.json(我们自己的标记) ── */

static int json_escape(const char *text, char *out, size_t out_len)
{
    size_t n = 0;
    if (out == NULL || out_len == 0) {
        return -1;
    }
    for (; text != NULL && *text != '\0'; ++text) {
        const unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            if (n + 2 >= out_len) {
                return -1;
            }
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            if (n + 6 >= out_len) {
                return -1;
            }
            n += (size_t)snprintf(out + n, out_len - n, "\\u%04x", (unsigned)c);
        } else {
            if (n + 1 >= out_len) {
                return -1;
            }
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return 0;
}

static int write_marker(const char *target, const sxcl_jre_component *comp, const char *abi,
                        const char *index_url, const sxcl_jre_file *const *files, size_t file_count,
                        char *marker_path_out, size_t marker_path_len)
{
    char path[SXCL_JRE_PATH_MAX];
    char tmp[SXCL_JRE_PATH_MAX + 8];
    char iso[40];
    FILE *fp;
    size_t i;
    if (join_path(path, sizeof(path), target, SXCL_JRE_MARKER) != 0) {
        return -1;
    }
    if (marker_path_out != NULL && marker_path_len > 0) {
        copy_str(marker_path_out, marker_path_len, path);
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = sxcl_fs_fopen(tmp, "wb");
    if (fp == NULL) {
        return -1;
    }
    char esc[SXCL_JRE_URL_MAX * 2];
    now_iso8601(iso, sizeof(iso));
    (void)fprintf(fp, "{\n  \"schema\": %d,\n", SXCL_JRE_INDEX_SCHEMA);
    if (json_escape(comp->id, esc, sizeof(esc)) != 0) {
        (void)fclose(fp);
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    (void)fprintf(fp, "  \"component\": \"%s\",\n", esc);
    (void)fprintf(fp, "  \"javaMajor\": %d,\n", comp->java_major);
    if (json_escape(comp->version, esc, sizeof(esc)) != 0) {
        (void)fclose(fp);
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    (void)fprintf(fp, "  \"version\": \"%s\",\n", esc);
    (void)fprintf(fp, "  \"installedAt\": \"%s\",\n", iso);
    if (comp->index_id[0] != '\0') {
        if (json_escape(comp->index_id, esc, sizeof(esc)) != 0) {
            (void)fclose(fp);
            (void)sxcl_fs_remove(tmp);
            return -1;
        }
        (void)fprintf(fp, "  \"indexId\": \"%s\",\n", esc);
    }
    if (comp->installed_bytes > 0) {
        (void)fprintf(fp, "  \"installedBytes\": %lld,\n", (long long)comp->installed_bytes);
    }
    (void)fprintf(fp, "  \"abi\": \"%s\",\n", abi != NULL ? abi : "");
    if (json_escape(index_url != NULL ? index_url : "", esc, sizeof(esc)) != 0) {
        (void)fclose(fp);
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    (void)fprintf(fp, "  \"indexUrl\": \"%s\",\n", esc);
    (void)fprintf(fp, "  \"files\": [");
    for (i = 0; i < file_count; ++i) {
        char esc_file[SXCL_JRE_FILE_MAX * 2];
        if (json_escape(files[i]->file, esc_file, sizeof(esc_file)) != 0) {
            (void)fclose(fp);
            (void)sxcl_fs_remove(tmp);
            return -1;
        }
        (void)fprintf(fp,
                      "%s\n    {\"file\": \"%s\", \"size\": %lld, \"sha256\": \"%s\", "
                      "\"sha1\": \"%s\"}",
                      i == 0 ? "" : ",", esc_file, (long long)files[i]->size, files[i]->sha256,
                      files[i]->sha1);
    }
    (void)fprintf(fp, "\n  ]\n}\n");
    if (fclose(fp) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    if (sxcl_fs_rename_replace(tmp, path) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

int sxcl_jre_read_marker(const char *dir, char *version_out, size_t version_len,
                         char *json_out, size_t json_len)
{
    char path[SXCL_JRE_PATH_MAX];
    sxcl_json *doc;
    char jerr[256];
    char *text = NULL;
    FILE *fp;
    long size;
    if (dir == NULL || *dir == '\0') {
        return SXCL_JRE_ERR_ARG;
    }
    if (version_out != NULL && version_len > 0) {
        version_out[0] = '\0';
    }
    if (json_out != NULL && json_len > 0) {
        json_out[0] = '\0';
    }
    if (join_path(path, sizeof(path), dir, SXCL_JRE_MARKER) != 0) {
        return SXCL_JRE_ERR_ARG;
    }
    fp = sxcl_fs_fopen(path, "rb");
    if (fp == NULL) {
        return SXCL_JRE_ERR_IO;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) {
        (void)fclose(fp);
        return SXCL_JRE_ERR_INDEX;
    }
    text = (char *)malloc((size_t)size + 1u);
    if (text == NULL) {
        (void)fclose(fp);
        return SXCL_JRE_ERR_NOMEM;
    }
    if (size > 0 && fread(text, 1, (size_t)size, fp) != (size_t)size) {
        free(text);
        (void)fclose(fp);
        return SXCL_JRE_ERR_IO;
    }
    (void)fclose(fp);
    text[size] = '\0';
    if (json_out != NULL && json_len > 0) {
        copy_str(json_out, json_len, text);
    }
    jerr[0] = '\0';
    doc = sxcl_json_parse(text, (size_t)size, jerr, sizeof(jerr));
    free(text);
    if (doc == NULL) {
        return SXCL_JRE_ERR_INDEX;
    }
    if (version_out != NULL && version_len > 0) {
        copy_str(version_out, version_len, sxcl_json_get_string(sxcl_json_root(doc), "version", ""));
    }
    sxcl_json_free(doc);
    return SXCL_JRE_OK;
}

/** <dir>/bin/java 或 <dir>/bin/java.exe 存在?写回实际存在的那个路径(可空)。
 *  为什么两种都认:sxcl_java_runtime_java_path() 按**平台**拼后缀(Windows 给 .exe),
 *  而安卓 JRE 的归档里就叫 bin/java —— **归档里有什么才是准的**。 */
static int jre_java_present(const char *dir, char *out, size_t out_len)
{
    char cand[SXCL_JRE_PATH_MAX];
    if (join_path(cand, sizeof(cand), dir, "bin/java") == 0 && sxcl_fs_exists(cand)) {
        if (out != NULL && out_len > 0) {
            copy_str(out, out_len, cand);
        }
        return 1;
    }
    if (join_path(cand, sizeof(cand), dir, "bin/java.exe") == 0 && sxcl_fs_exists(cand)) {
        if (out != NULL && out_len > 0) {
            copy_str(out, out_len, cand);
        }
        return 1;
    }
    return 0;
}

int sxcl_jre_is_installed(const char *dir, const char *want_version)
{
    char version[SXCL_JRE_VERSION_MAX];
    if (dir == NULL || *dir == '\0') {
        return SXCL_JRE_ERR_ARG;
    }
    if (jre_java_present(dir, NULL, 0) == 0) {
        return 0;
    }
    if (sxcl_jre_read_marker(dir, version, sizeof(version), NULL, 0) != SXCL_JRE_OK) {
        return 0;
    }
    if (want_version != NULL && *want_version != '\0' && strcmp(version, want_version) != 0) {
        return 0;
    }
    return 1;
}

/* ── 安装 ── */

typedef struct jre_state {
    const sxcl_jre_request *request;
    sxcl_jre_result *out;
    sxcl_jre_stage stage;
    sxcl_jre_progress progress;
    int cancelled;
    sxcl_engine *engine;
    const sxcl_task *const *submitted;
    size_t submitted_count;
    double started;
    double last_emit;
    int64_t last_bytes;
    double last_bytes_time;
    double speed_bps;
    int last_stage_emitted;
} jre_state;

static void progress_message(jre_state *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(st->progress.message, sizeof(st->progress.message), fmt, ap);
    va_end(ap);
    st->progress.message[sizeof(st->progress.message) - 1] = '\0';
}

static void update_speed(jre_state *st, int64_t bytes)
{
    const double now = now_seconds();
    const double dt = now - st->last_bytes_time;
    if (dt >= 0.5) {
        const int64_t delta = bytes - st->last_bytes;
        const double inst = (dt > 0.0) ? (double)delta / dt : 0.0;
        st->speed_bps = (st->speed_bps <= 0.0) ? inst : (st->speed_bps * 0.7 + inst * 0.3);
        st->last_bytes = bytes;
        st->last_bytes_time = now;
    }
}

static void emit(jre_state *st, int percent, const char *current)
{
    const double now = now_seconds();
    st->progress.stage = st->stage;
    st->progress.stage_id = sxcl_jre_stage_id(st->stage);
    st->progress.stage_name = sxcl_jre_stage_name(st->stage);
    st->progress.percent = percent;
    st->progress.current = current != NULL ? current : "";
    st->progress.component = st->out->component;
    st->progress.version = st->out->version;
    update_speed(st, st->progress.bytes_done);
    st->progress.speed_bps = st->speed_bps;
    if (st->speed_bps > 1.0 && st->progress.bytes_total > st->progress.bytes_done) {
        const double remain = (double)(st->progress.bytes_total - st->progress.bytes_done);
        st->progress.eta_seconds = (int64_t)(remain / st->speed_bps);
    } else {
        st->progress.eta_seconds = -1; /* 算不出来就明说,不编一个数出来 */
    }
    if (st->progress.message[0] == '\0') {
        snprintf(st->progress.message, sizeof(st->progress.message), "%s",
                 sxcl_jre_stage_name(st->stage));
    }
    /* 节流 100ms,但**换阶段一定报**:不然极快的安装在界面上会"跳过整个阶段"
     * (实测:测试夹具几十毫秒就跑完,只收到最后一条)。 */
    if (st->request->on_progress != NULL &&
        (now - st->last_emit >= 0.1 || percent >= 100 ||
         st->last_stage_emitted != (int)st->stage)) {
        st->last_emit = now;
        st->last_stage_emitted = (int)st->stage;
        st->request->on_progress(st->request->ud, &st->progress);
    }
}

static int is_cancelled(jre_state *st)
{
    if (st->cancelled) {
        return 1;
    }
    if (st->request->is_cancelled != NULL && st->request->is_cancelled(st->request->ud) != 0) {
        st->cancelled = 1;
        return 1;
    }
    return 0;
}

static int fail_stage(jre_state *st, int code, sxcl_jre_stage stage)
{
    st->out->code = code;
    st->out->fail_stage = stage;
    st->out->fail_stage_id = sxcl_jre_stage_id(stage);
    st->stage = stage;
    return code;
}

/** 引擎回调:从工作线程来(与 engine.h 一个口径)。 */
static void engine_progress(void *ud, const sxcl_task *task)
{
    jre_state *st = (jre_state *)ud;
    int64_t done = 0;
    size_t i;
    int completed = 0;
    if (task == NULL) {
        return;
    }
    for (i = 0; i < st->submitted_count; ++i) {
        const sxcl_task *t = st->submitted[i];
        if (t == NULL) {
            continue;
        }
        if (t->bytes_done > 0) {
            done += t->bytes_done;
        }
        if (t->state == SXCL_TASK_DONE) {
            ++completed;
        }
    }
    st->progress.bytes_done = done;
    st->progress.files_done = completed;
    if (task->label != NULL) {
        progress_message(st, "下载 %s(%.1f MB/s)", task->label,
                         st->speed_bps > 0 ? st->speed_bps / (1024.0 * 1024.0) : 0.0);
    }
    {
        const int percent = (st->progress.bytes_total > 0)
                                ? (int)(10 + (done * 60) / st->progress.bytes_total)
                                : 20;
        emit(st, percent > 70 ? 70 : percent, task->label);
    }
}

/** 取一份文本(index.json)。mirror_base 非空时同一路径多一条候选。 */
static int fetch_index_text(jre_state *st, const char *url, char **out_text, size_t *out_len,
                            char *err, size_t err_len)
{
    const sxcl_jre_request *req = st->request;
    char mirror[SXCL_JRE_URL_MAX];
    const char *urls[2];
    char detail[SXCL_JRE_ERROR_MAX];
    int i;
    detail[0] = '\0';
    urls[0] = url;
    urls[1] = NULL;
    if (req->mirror_base != NULL && *req->mirror_base != '\0') {
        /* 镜像只是"同一路径的另一个前缀":index.json 自己也要能镜像 */
        const char *name = base_name(url);
        char base[SXCL_JRE_URL_MAX];
        copy_str(base, sizeof(base), req->mirror_base);
        {
            const size_t bl = strlen(base);
            if (bl > 0 && base[bl - 1] != '/') {
                snprintf(base + bl, sizeof(base) - bl, "/");
            }
        }
        if (snprintf(mirror, sizeof(mirror), "%s%s", base, name) < (int)sizeof(mirror)) {
            if (req->mirror_first) {
                urls[0] = mirror;
                urls[1] = url;
            } else {
                urls[0] = url;
                urls[1] = mirror;
            }
        }
    }
    for (i = 0; i < 2 && urls[i] != NULL; ++i) {
        sxcl_transport *tr = req->transport_factory(req->ud);
        sxcl_http_opts opts;
        char http_err[SXCL_HTTP_ERROR_MAX];
        char *text = NULL;
        size_t len = 0;
        int rc;
        if (tr == NULL) {
            set_text(err, err_len, "传输后端创建失败");
            return SXCL_JRE_ERR_NET;
        }
        memset(&opts, 0, sizeof(opts));
        opts.max_bytes = 4u * 1024u * 1024u;
        http_err[0] = '\0';
        rc = sxcl_http_get_text_ex(tr, urls[i], NULL, &opts, &text, &len, http_err,
                                   sizeof(http_err));
        if (tr->destroy != NULL) {
            tr->destroy(tr->ctx);
        }
        if (rc == SXCL_HTTP_OK) {
            *out_text = text;
            if (out_len != NULL) {
                *out_len = len;
            }
            copy_str(st->out->index_url, sizeof(st->out->index_url), urls[i]);
            return SXCL_JRE_OK;
        }
        free(text);
        if (detail[0] == '\0') {
            copy_str(detail, sizeof(detail), http_err);
        }
    }
    set_textf(err, err_len, "取 index.json 失败: %s", detail[0] ? detail : url);
    return SXCL_JRE_ERR_NET;
}

/** 取一份纯文本(sha256 那种小文件)。走与 index 同一条"创建/销毁传输后端"的路。 */
static int fetch_sha256_text(jre_state *st, const char *url, char **out_text, size_t *out_len,
                             char *err, size_t err_len)
{
    const sxcl_jre_request *req = st->request;
    sxcl_transport *tr = req->transport_factory(req->ud);
    sxcl_http_opts opts;
    char http_err[SXCL_HTTP_ERROR_MAX];
    char *text = NULL;
    size_t len = 0;
    int rc;
    (void)st;
    if (tr == NULL) {
        set_text(err, err_len, "传输后端创建失败");
        return SXCL_JRE_ERR_NET;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_bytes = 64u * 1024u; /* .sha256 文件只有几十个字节 */
    http_err[0] = '\0';
    rc = sxcl_http_get_text_ex(tr, url, NULL, &opts, &text, &len, http_err, sizeof(http_err));
    if (tr->destroy != NULL) {
        tr->destroy(tr->ctx);
    }
    if (rc != SXCL_HTTP_OK) {
        set_textf(err, err_len, "%s", http_err[0] ? http_err : "取不到");
        free(text);
        return SXCL_JRE_ERR_NET;
    }
    *out_text = text;
    if (out_len != NULL) {
        *out_len = len;
    }
    return SXCL_JRE_OK;
}

/** 去首尾空白后取**第一个连续的 hex_len 位十六进制串**。
 *  兼容三种真实写法:纯 hex(Oracle 的 .sha256,64 字节)、"<hex>  <文件名>"(sha256sum)、
 *  前面带 "SHA256 (x) = " 之类的行。找不到返回 -1。 */
static int extract_hex_run(const char *text, size_t len, size_t hex_len, char *out, size_t out_len)
{
    size_t i = 0;
    if (out_len <= hex_len) {
        return -1;
    }
    while (i < len) {
        size_t j = i;
        while (j < len && isxdigit((unsigned char)text[j])) {
            ++j;
        }
        if (j - i == hex_len) {
            memcpy(out, text + i, hex_len);
            out[hex_len] = '\0';
            return 0;
        }
        i = (j > i) ? j : i + 1;
    }
    return -1;
}

/** 取一份侧车哈希文件并解析出哈希。主 URL 取不到时,**拿 mirrors 里的同名 .sha256 再试一次**。
 *  两条都不行就返回错误(绝不"拿不到就当没有校验"——那是假装校验过)。 */
static int resolve_sidecar_hash(jre_state *st, const char *url, const char *mirror,
                                size_t hex_len, char *out, size_t out_len, char *err,
                                size_t err_len)
{
    char cand[2][SXCL_JRE_URL_MAX];
    char detail[SXCL_JRE_ERROR_MAX];
    int n = 0;
    int i;
    detail[0] = '\0';
    if (url != NULL && *url != '\0') {
        copy_str(cand[n], sizeof(cand[0]), url);
        ++n;
    }
    if (mirror != NULL && *mirror != '\0' && n < 2) {
        /* 镜像那份是"包"的地址:它的同名侧车 = 包地址 + ".sha256" */
        if (snprintf(cand[n], sizeof(cand[0]), "%s.sha256", mirror) < (int)sizeof(cand[0])) {
            ++n;
        }
    }
    for (i = 0; i < n; ++i) {
        char *text = NULL;
        size_t text_len = 0;
        char http_err[SXCL_HTTP_ERROR_MAX];
        http_err[0] = '\0';
        if (fetch_sha256_text(st, cand[i], &text, &text_len, http_err, sizeof(http_err)) !=
            SXCL_JRE_OK) {
            if (detail[0] == '\0') {
                copy_str(detail, sizeof(detail), http_err[0] ? http_err : "取不到");
            }
            continue;
        }
        if (extract_hex_run(text, text_len, hex_len, out, out_len) == 0) {
            free(text);
            return SXCL_JRE_OK;
        }
        free(text);
        if (detail[0] == '\0') {
            set_textf(detail, sizeof(detail), "%s 里没有 %u 位十六进制",
                      cand[i], (unsigned)hex_len);
        }
    }
    set_textf(err, err_len, "%s", detail[0] ? detail : "没有可用的侧车地址");
    return SXCL_JRE_ERR_NET;
}

int sxcl_jre_install(const sxcl_jre_request *request, sxcl_jre_result *out)
{
    jre_state st;
    char err[SXCL_JRE_ERROR_MAX];
    char index_url[SXCL_JRE_URL_MAX];
    char *index_text = NULL;
    size_t index_len = 0;
    int index_owned = 0;
    /* 注意:sxcl_jre_component 现在有 ~20 KiB(每个文件条目带 URL/mirror/sha256 侧车三个大缓冲),
     * 32 个就是一坨 600 KiB —— **必须堆分配**,放栈上会在 Windows 的 1 MiB 默认栈上直接爆掉
     * (实测:0xC00000FD 栈溢出)。调用方自己拿数组时也要注意这点。 */
    sxcl_jre_component *list = NULL;
    int list_count;
    sxcl_jre_component comp;
    const sxcl_jre_file *apply[SXCL_JRE_MAX_FILES];
    size_t apply_idx[SXCL_JRE_MAX_FILES];
    size_t apply_count = 0;
    char abi[24];
    char target[SXCL_JRE_PATH_MAX];
    char archive_dir[SXCL_JRE_PATH_MAX];
    char version_on_disk[SXCL_JRE_VERSION_MAX];
    int64_t want_bytes = 0;
    int rc = SXCL_JRE_OK;

    if (request == NULL || out == NULL) {
        return SXCL_JRE_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->fail_stage = SXCL_JRE_STAGE_END;
    out->fail_stage_id = sxcl_jre_stage_id(SXCL_JRE_STAGE_END);
    out->free_bytes = -1;
    err[0] = '\0';

    /* 安装**总要**下载点什么(没有 index_text 要取清单,有 index_text 也要取包),
     * 所以传输后端是硬要求 —— 缺了就当场说清楚,不要跑到一半才报"下载失败"。 */
    if (request->transport_factory == NULL) {
        out->code = SXCL_JRE_ERR_ARG;
        copy_str(out->error, sizeof(out->error),
                 "没有传输后端(transport_factory 为空):取清单与下包都要它");
        return SXCL_JRE_ERR_ARG;
    }
    if ((request->component_id == NULL || *request->component_id == '\0') &&
        request->java_major <= 0) {
        out->code = SXCL_JRE_ERR_ARG;
        copy_str(out->error, sizeof(out->error), "java_major 与 component_id 至少要给一个");
        return SXCL_JRE_ERR_ARG;
    }

    memset(&st, 0, sizeof(st));
    st.request = request;
    st.out = out;
    st.stage = SXCL_JRE_STAGE_INDEX;
    st.started = now_seconds();
    st.last_bytes_time = st.started;
    st.progress.stage = SXCL_JRE_STAGE_INDEX;
    st.progress.stage_id = sxcl_jre_stage_id(SXCL_JRE_STAGE_INDEX);
    st.progress.stage_name = sxcl_jre_stage_name(SXCL_JRE_STAGE_INDEX);
    st.progress.eta_seconds = -1;
    snprintf(st.progress.message, sizeof(st.progress.message), "%s",
             sxcl_jre_stage_name(SXCL_JRE_STAGE_INDEX));
    copy_str(abi, sizeof(abi),
             (request->abi != NULL && *request->abi != '\0') ? request->abi
                                                             : sxcl_jre_host_abi());
    copy_str(out->abi, sizeof(out->abi), abi);

    /* ── 1) index.json ── */
    emit(&st, 0, "");
    if (request->index_text != NULL) {
        index_text = (char *)request->index_text;
        index_len = strlen(request->index_text);
        copy_str(index_url, sizeof(index_url), "provided");
        copy_str(out->index_url, sizeof(out->index_url), "provided");
    } else {
        const char *env = getenv(SXCL_JRE_INDEX_URL_ENV);
        if (sxcl_jre_resolve_index_url(request->index_url, request->index_setting, env, index_url,
                                       sizeof(index_url)) != SXCL_JRE_OK) {
            out->code = SXCL_JRE_ERR_ARG;
            if (sxcl_jre_default_index_url_is_placeholder()) {
                copy_str(out->error, sizeof(out->error),
                         "没有配 JRE 来源,而且编译期默认的 GitHub 仓库名还没确认"
                         "(SXCL_JRE_GH_REPO 还是 <REPO> 占位):请填设置项 "
                         SXCL_JRE_INDEX_URL_SETTING " 或环境变量 " SXCL_JRE_INDEX_URL_ENV);
            } else {
                copy_str(out->error, sizeof(out->error),
                         "没有配 JRE 自托管来源:请在设置里填 index.json 地址,或设环境变量 "
                         SXCL_JRE_INDEX_URL_ENV);
            }
            return SXCL_JRE_ERR_ARG;
        }
        if (fetch_index_text(&st, index_url, &index_text, &index_len, err, sizeof(err)) !=
            SXCL_JRE_OK) {
            copy_str(out->error, sizeof(out->error), err);
            return fail_stage(&st, SXCL_JRE_ERR_NET, SXCL_JRE_STAGE_INDEX);
        }
        index_owned = 1;
    }
    list = (sxcl_jre_component *)calloc(JRE_MAX_COMPONENTS, sizeof(*list));
    if (list == NULL) {
        if (index_owned) {
            free(index_text);
        }
        copy_str(out->error, sizeof(out->error), "内存不足(组件表分配失败)");
        return fail_stage(&st, SXCL_JRE_ERR_NOMEM, SXCL_JRE_STAGE_INDEX);
    }
    list_count = sxcl_jre_index_parse(index_text, index_len, list, JRE_MAX_COMPONENTS, err,
                                      sizeof(err));
    if (index_owned) {
        free(index_text);
        index_owned = 0;
    }
    if (list_count < 0) {
        free(list);
        copy_str(out->error, sizeof(out->error), err);
        return fail_stage(&st, list_count, SXCL_JRE_STAGE_INDEX);
    }
    rc = sxcl_jre_index_pick(list, (size_t)list_count, request->component_id, request->java_major,
                             &comp, err, sizeof(err));
    free(list);
    list = NULL;
    if (rc != SXCL_JRE_OK) {
        copy_str(out->error, sizeof(out->error), err);
        return fail_stage(&st, rc, SXCL_JRE_STAGE_INDEX);
    }
    copy_str(out->component, sizeof(out->component), comp.id);
    copy_str(out->version, sizeof(out->version), comp.version);
    emit(&st, 10, "");

    /* ── 2) 目标目录 + 已装快路径(version 判据) ── */
    if (request->target_dir != NULL && *request->target_dir != '\0') {
        copy_str(target, sizeof(target), request->target_dir);
    } else {
        /* 清单给了 dir_key 就用它(sxcl.jre.index/1 的 <component>/<version>/<abi>);
         * 没有才退回 <root>/<组件 id>。 */
        const char *leaf = (comp.dir_key[0] != '\0') ? comp.dir_key : comp.id;
        char root[SXCL_JRE_PATH_MAX];
        if (request->target_root != NULL && *request->target_root != '\0') {
            copy_str(root, sizeof(root), request->target_root);
        } else if (sxcl_java_runtime_default_root(root, sizeof(root), err, sizeof(err)) !=
                   SXCL_JAVA_RUNTIME_OK) {
            copy_str(out->error, sizeof(out->error), "拿不到默认 runtime 根目录");
            return fail_stage(&st, SXCL_JRE_ERR_ARG, SXCL_JRE_STAGE_INDEX);
        }
        if (join_path(target, sizeof(target), root, leaf) != 0) {
            copy_str(out->error, sizeof(out->error), "拼不出安装目录(路径太长)");
            return fail_stage(&st, SXCL_JRE_ERR_ARG, SXCL_JRE_STAGE_INDEX);
        }
    }
    copy_str(out->java_home, sizeof(out->java_home), target);
    (void)sxcl_java_runtime_java_path(target, out->java_path, sizeof(out->java_path));
    (void)join_path(out->marker_path, sizeof(out->marker_path), target, SXCL_JRE_MARKER);

    if (!request->force && request->skip_if_installed) {
        version_on_disk[0] = '\0';
        (void)sxcl_jre_read_marker(target, version_on_disk, sizeof(version_on_disk), NULL, 0);
        if (sxcl_jre_is_installed(target, comp.version) == 1) {
            st.stage = SXCL_JRE_STAGE_FINISH;
            st.progress.bytes_done = 0;
            st.progress.bytes_total = 0;
            st.progress.files_done = 0;
            progress_message(&st, "已装 %s(%s),跳过", comp.id, comp.version);
            emit(&st, 100, "");
            out->skipped = 1;
            out->code = SXCL_JRE_OK;
            out->fail_stage = SXCL_JRE_STAGE_END;
            out->fail_stage_id = sxcl_jre_stage_id(SXCL_JRE_STAGE_END);
            return SXCL_JRE_OK;
        }
        if (version_on_disk[0] != '\0' && strcmp(version_on_disk, comp.version) != 0) {
            progress_message(&st, "盘上是 %s,index 上是 %s:重装", version_on_disk, comp.version);
        }
    }

    /* 清单自己分过 ABI(sxcl.jre.index/1)时先卡一道:组件 ABI 与本机不符就明确报错,
     * 不要靠文件名过滤去"猜"——那会把 universal 当成能用,解出一棵没有 bin/java 的树。 */
    if (comp.abi[0] != '\0' && !abi_matches(comp.abi, abi)) {
        set_textf(out->error, sizeof(out->error),
                  "清单里的 %s 是给 %s 的,本机是 %s", comp.id, comp.abi, abi);
        return fail_stage(&st, SXCL_JRE_ERR_NO_ABI, SXCL_JRE_STAGE_INDEX);
    }

    /* 挑出适用于本机 ABI 的文件 + 量总大小 */
    {
        size_t i;
        for (i = 0; i < comp.file_count; ++i) {
            if (sxcl_jre_file_applies(&comp.files[i], abi)) {
                apply_idx[apply_count] = i;
                apply[apply_count] = &comp.files[i];
                ++apply_count;
                want_bytes += comp.files[i].size;
            }
        }
        if (apply_count == 0) {
            copy_str(out->error, sizeof(out->error),
                     "index.json 里没有适用于本机 ABI 的文件(要 universal.tar.xz 或 bin-<abi>.tar.xz)");
            return fail_stage(&st, SXCL_JRE_ERR_NO_ABI, SXCL_JRE_STAGE_INDEX);
        }
    }
    out->files_total = apply_count;
    out->bytes_total = want_bytes;
    st.progress.files_total = apply_count;
    st.progress.bytes_total = want_bytes;
    {
        size_t i;
        for (i = 0; i < apply_count; ++i) {
            if (apply[i]->no_hash) {
                ++out->files_without_hash;
            }
        }
    }

    /* 老 schema 的坑:sha256 写的是一个 .sha256 文件的 URL(例如 Oracle 的下载页)。
     * 去取一次,取到的第一个空白分隔的 token 必须是 64 位十六进制。
     * 优先级:**能直接给的十六进制 > 去那个 URL 取**(见 docs/19)。 */
    {
        size_t i;
        for (i = 0; i < apply_count; ++i) {
            sxcl_jre_file *f = &comp.files[apply_idx[i]];
            char text_err[256];
            if (f->sha256_url[0] != '\0') {
                /* 走下面的侧车解析 */
            } else if (f->sha1_url[0] != '\0' && f->sha256[0] == '\0') {
                text_err[0] = '\0';
                if (resolve_sidecar_hash(&st, f->sha1_url, f->mirror, 40, f->sha1,
                                         sizeof(f->sha1), text_err, sizeof(text_err)) !=
                    SXCL_JRE_OK) {
                    snprintf(out->error, sizeof(out->error), "校验信息拿不到:%s(%s)",
                             f->sha1_url, text_err);
                    return fail_stage(&st, SXCL_JRE_ERR_INDEX, SXCL_JRE_STAGE_INDEX);
                }
                f->no_hash = 0;
                continue;
            } else {
                continue;
            }
            if (request->transport_factory == NULL) {
                copy_str(out->error, sizeof(out->error),
                         "清单里的 sha256 是一个 URL,但没有传输后端去取它");
                return fail_stage(&st, SXCL_JRE_ERR_ARG, SXCL_JRE_STAGE_INDEX);
            }
            text_err[0] = '\0';
            if (resolve_sidecar_hash(&st, f->sha256_url, f->mirror, 64, f->sha256,
                                     sizeof(f->sha256), text_err, sizeof(text_err)) !=
                SXCL_JRE_OK) {
                snprintf(out->error, sizeof(out->error),
                         "校验信息拿不到:%s(%s)。建议把**字面哈希**写进 GitHub 上那份清单 —— "
                         "它是信任锚;云上的 .sha256 只是便捷通路。",
                         f->sha256_url, text_err);
                return fail_stage(&st, SXCL_JRE_ERR_INDEX, SXCL_JRE_STAGE_INDEX);
            }
        }
    }

    if (sxcl_fs_mkdirs(target) != 0) {
        copy_str(out->error, sizeof(out->error), "建不了安装目录");
        return fail_stage(&st, SXCL_JRE_ERR_IO, SXCL_JRE_STAGE_INDEX);
    }
    if (join_path(archive_dir, sizeof(archive_dir), target, JRE_ARCHIVE_DIR) != 0 ||
        sxcl_fs_mkdirs(archive_dir) != 0) {
        copy_str(out->error, sizeof(out->error), "建不了安装包缓存目录");
        return fail_stage(&st, SXCL_JRE_ERR_IO, SXCL_JRE_STAGE_INDEX);
    }
    copy_str(out->archive_dir, sizeof(out->archive_dir), archive_dir);

    /* ── 3) 磁盘闸:还差多少字节,可用空间够不够(下第一个字节之前) ── */
    {
        int64_t still_need = 0;
        size_t i;
        for (i = 0; i < apply_count; ++i) {
            char dest[SXCL_JRE_PATH_MAX];
            if (join_path(dest, sizeof(dest), archive_dir, base_name(apply[i]->file)) != 0) {
                copy_str(out->error, sizeof(out->error), "安装包缓存路径太长");
                return fail_stage(&st, SXCL_JRE_ERR_ARG, SXCL_JRE_STAGE_INDEX);
            }
            if (sxcl_verify_file(dest, apply[i]->size, apply[i]->sha256, SXCL_HASH_SHA256, NULL) !=
                SXCL_VERIFY_OK) {
                still_need += apply[i]->size;
            }
        }
        {
            uint64_t free_bytes = 0;
            if (sxcl_fs_free_space(target, &free_bytes) == 1) {
                out->free_bytes = (int64_t)free_bytes;
                /* 解包后还会再占一份(压缩态 + 解开的树),留 64 MiB 余量 */
                if (still_need > 0 && (int64_t)free_bytes < still_need + (int64_t)64 * 1024 * 1024) {
                    set_textf(err, sizeof(err),
                              "磁盘空间不够:还需要 %lld 字节,可用 %lld 字节",
                              (long long)still_need, (long long)free_bytes);
                    copy_str(out->error, sizeof(out->error), err);
                    return fail_stage(&st, SXCL_JRE_ERR_DISK, SXCL_JRE_STAGE_INDEX);
                }
            }
            /* 拿不到可用空间 -> 只报"未知",不拦(与 java_runtime.c 一个口径) */
        }
    }

    /* ── 4) 下载(SHA-256 强校验;已有同 sha256 的包就跳过) ── */
    {
        sxcl_task *tasks = NULL;
        char **dest_heaps = NULL;   /* <目标>/.archives/<名字>(堆:任务活到引擎销毁之后) */
        char **url_heaps = NULL;    /* urls[0] 的堆副本(引擎也只在 run 期间读) */
        char **url2_heaps = NULL;   /* urls[1] = 镜像候选(可空) */
        sxcl_task **submitted = NULL;
        sxcl_engine *engine = NULL;
        size_t submitted_count = 0;
        size_t i;
        st.stage = SXCL_JRE_STAGE_DOWNLOAD;
        progress_message(&st, "%s %u 个安装包", sxcl_jre_stage_name(st.stage),
                         (unsigned)apply_count);
        emit(&st, 10, "");

        tasks = (sxcl_task *)calloc(apply_count, sizeof(sxcl_task));
        dest_heaps = (char **)calloc(apply_count, sizeof(char *));
        url_heaps = (char **)calloc(apply_count, sizeof(char *));
        url2_heaps = (char **)calloc(apply_count, sizeof(char *));
        submitted = (sxcl_task **)calloc(apply_count, sizeof(sxcl_task *));
        if (tasks == NULL || dest_heaps == NULL || url_heaps == NULL || url2_heaps == NULL ||
            submitted == NULL) {
            rc = SXCL_JRE_ERR_NOMEM;
        }
        for (i = 0; rc == SXCL_JRE_OK && i < apply_count; ++i) {
            char dest[SXCL_JRE_PATH_MAX];
            char url[SXCL_JRE_URL_MAX];
            char mirror_url[SXCL_JRE_URL_MAX];
            sxcl_task *task;
            if (join_path(dest, sizeof(dest), archive_dir, base_name(apply[i]->file)) != 0) {
                rc = SXCL_JRE_ERR_ARG;
                break;
            }
            if (apply[i]->sha256[0] != '\0' &&
                sxcl_verify_file(dest, apply[i]->size, apply[i]->sha256, SXCL_HASH_SHA256,
                                 NULL) == SXCL_VERIFY_OK) {
                /* 已在盘上且 sha256 相符:一个字节都不下(第二道闸) */
                ++st.progress.files_skipped;
                ++st.progress.files_done;
                continue;
            }
            if (sxcl_jre_join_url(index_url, apply[i]->file, url, sizeof(url)) != SXCL_JRE_OK) {
                rc = SXCL_JRE_ERR_INDEX;
                break;
            }
            dest_heaps[i] = (char *)malloc(strlen(dest) + 1);
            url_heaps[i] = (char *)malloc(strlen(url) + 1);
            if (dest_heaps[i] == NULL || url_heaps[i] == NULL) {
                rc = SXCL_JRE_ERR_NOMEM;
                break;
            }
            memcpy(dest_heaps[i], dest, strlen(dest) + 1);
            memcpy(url_heaps[i], url, strlen(url) + 1);
            task = &tasks[i];
            memset(task, 0, sizeof(*task));
            task->dest = dest_heaps[i];
            /* 没有校验信息时**显式给 NULL**:引擎的 verify_state 会退化成"只有 size"(弱校验),
             * 我们不去编一个哈希出来,也不假装校验过。 */
            task->sha1 = (apply[i]->sha256[0] != '\0') ? apply[i]->sha256 : NULL;
            task->algo = SXCL_HASH_SHA256;
            task->size = apply[i]->size;
            task->priority = 10;
            task->label = apply[i]->file;
            if (apply[i]->no_hash) {
                progress_message(&st, "下载 %s(**没有校验信息**,只按大小校验)",
                                 base_name(apply[i]->file));
            }
            /* 第二条候选:清单里那条 file 自己的 mirror(绝对 URL)**优先**;
             * 没给才退到请求级 mirror_base + 相对路径。 */
            if (apply[i]->mirror[0] != '\0') {
                copy_str(mirror_url, sizeof(mirror_url), apply[i]->mirror);
                url2_heaps[i] = (char *)malloc(strlen(mirror_url) + 1);
                if (url2_heaps[i] != NULL) {
                    memcpy(url2_heaps[i], mirror_url, strlen(mirror_url) + 1);
                }
            } else if (request->mirror_base != NULL && *request->mirror_base != '\0' &&
                       strncmp(apply[i]->file, "http", 4) != 0) {
                const char *mb = request->mirror_base;
                const size_t ml = strlen(mb);
                if (snprintf(mirror_url, sizeof(mirror_url), "%s%s%s", mb,
                             (ml > 0 && mb[ml - 1] == '/') ? "" : "/", apply[i]->file) <
                    (int)sizeof(mirror_url)) {
                    url2_heaps[i] = (char *)malloc(strlen(mirror_url) + 1);
                    if (url2_heaps[i] != NULL) {
                        memcpy(url2_heaps[i], mirror_url, strlen(mirror_url) + 1);
                    }
                }
            }
            if (request->mirror_first && url2_heaps[i] != NULL) {
                task->urls[0] = url2_heaps[i];
                task->urls[1] = url_heaps[i];
            } else {
                task->urls[0] = url_heaps[i];
                task->urls[1] = url2_heaps[i];
            }
            submitted[submitted_count++] = task;
        }

        if (rc == SXCL_JRE_OK && submitted_count > 0) {
            sxcl_engine_opts opts;
            if (request->engine_opts != NULL) {
                opts = *request->engine_opts;
            } else {
                memset(&opts, 0, sizeof(opts));
            }
            opts.transport_factory = request->transport_factory;
            opts.userdata = &st;
            opts.on_progress = engine_progress;
            engine = sxcl_engine_create(&opts);
            if (engine == NULL) {
                rc = SXCL_JRE_ERR_NOMEM;
            } else {
                st.engine = engine;
                st.submitted = (const sxcl_task *const *)submitted;
                st.submitted_count = submitted_count;
                for (i = 0; i < submitted_count; ++i) {
                    if (sxcl_engine_submit(engine, submitted[i]) != 0) {
                        rc = SXCL_JRE_ERR_NOMEM;
                        break;
                    }
                }
                if (rc == SXCL_JRE_OK) {
                    (void)sxcl_engine_run(engine);
                }
            }
        }

        /* 收口:失败/取消/统计(引擎跑完才数,取消也要能分开数) */
        if (rc == SXCL_JRE_OK || rc == SXCL_JRE_ERR_NOMEM) {
            int64_t done = 0;
            size_t failed = 0;
            size_t ok_count = st.progress.files_skipped;
            for (i = 0; i < submitted_count; ++i) {
                const sxcl_task *task = submitted[i];
                if (task->bytes_done > 0) {
                    done += task->bytes_done;
                }
                if (task->state == SXCL_TASK_FAILED) {
                    ++failed;
                    if (out->error[0] == '\0') {
                        set_textf(out->error, sizeof(out->error), "下载或校验失败: %s(%s)",
                                  task->label != NULL ? task->label : "?", task->error);
                    }
                } else if (task->state == SXCL_TASK_DONE) {
                    ++ok_count;
                }
            }
            st.progress.bytes_done = done;
            st.progress.files_done = ok_count;
            st.progress.files_failed = failed;
        }
        if (engine != NULL) {
            sxcl_engine_destroy(engine); /* 任务由本函数持有,引擎必须在返回前放掉(engine.h) */
            st.engine = NULL;
        }
        out->files_skipped = st.progress.files_skipped;
        out->files_done = st.progress.files_done;
        out->files_failed = st.progress.files_failed;
        out->bytes_done = st.progress.bytes_done;

        for (i = 0; i < apply_count; ++i) {
            free(dest_heaps != NULL ? dest_heaps[i] : NULL);
            free(url_heaps != NULL ? url_heaps[i] : NULL);
            free(url2_heaps != NULL ? url2_heaps[i] : NULL);
        }
        free(tasks);
        free(dest_heaps);
        free(url_heaps);
        free(url2_heaps);
        free(submitted);

        if (is_cancelled(&st)) {
            copy_str(out->error, sizeof(out->error), sxcl_lang_tr("page.progress.cancel", "取消"));
            return fail_stage(&st, SXCL_JRE_ERR_CANCELLED, SXCL_JRE_STAGE_DOWNLOAD);
        }
        if (rc != SXCL_JRE_OK) {
            if (out->error[0] == '\0') {
                copy_str(out->error, sizeof(out->error), sxcl_jre_code_name(rc));
            }
            return fail_stage(&st, rc, SXCL_JRE_STAGE_DOWNLOAD);
        }
        if (st.progress.files_failed > 0) {
            return fail_stage(&st, SXCL_JRE_ERR_DOWNLOAD, SXCL_JRE_STAGE_DOWNLOAD);
        }
    }

    /* ── 5) 解包(universal 在前,bin-<abi> 在后:后者覆盖前者) ── */
    {
        size_t i;
        st.stage = SXCL_JRE_STAGE_EXTRACT;
        progress_message(&st, "%s", sxcl_jre_stage_name(st.stage));
        emit(&st, 70, "");
        for (i = 0; i < apply_count; ++i) {
            char archive[SXCL_JRE_PATH_MAX];
            sxcl_tar_opts topts;
            char terr[256];
            int trc;
            if (is_cancelled(&st)) {
                copy_str(out->error, sizeof(out->error),
                         sxcl_lang_tr("page.progress.cancel", "取消"));
                return fail_stage(&st, SXCL_JRE_ERR_CANCELLED, SXCL_JRE_STAGE_EXTRACT);
            }
            if (join_path(archive, sizeof(archive), archive_dir, base_name(apply[i]->file)) != 0) {
                copy_str(out->error, sizeof(out->error), "安装包路径太长");
                return fail_stage(&st, SXCL_JRE_ERR_ARG, SXCL_JRE_STAGE_EXTRACT);
            }
            memset(&topts, 0, sizeof(topts));
            topts.dest_dir = target;
            terr[0] = '\0';
            progress_message(&st, "解包 %s(%lld 字节)", base_name(apply[i]->file),
                             (long long)apply[i]->size);
            emit(&st, 70 + (int)((i * 25) / apply_count), base_name(apply[i]->file));
            trc = sxcl_tar_extract_file(archive, &topts, terr, sizeof(terr));
            if (trc != SXCL_TAR_OK) {
                set_textf(out->error, sizeof(out->error), "解包 %s 失败: %s(%s)",
                          base_name(apply[i]->file), terr, sxcl_tar_code_name(trc));
                return fail_stage(&st, SXCL_JRE_ERR_EXTRACT, SXCL_JRE_STAGE_EXTRACT);
            }
        }
        emit(&st, 95, "");
    }

    /* ── 6) 收尾:核对 bin/java、补 JRE 侧库(安卓)、写 jre.json ── */
    st.stage = SXCL_JRE_STAGE_FINISH;
    if (out->files_without_hash > 0) {
        progress_message(&st, "%s(%u 个包没有校验信息,只按大小校验)",
                         sxcl_jre_stage_name(st.stage), (unsigned)out->files_without_hash);
    } else {
        progress_message(&st, "%s", sxcl_jre_stage_name(st.stage));
    }
    emit(&st, 96, "");
    if (jre_java_present(target, out->java_path, sizeof(out->java_path)) == 0) {
        set_textf(out->error, sizeof(out->error),
                  "解完了却找不到 java 可执行文件(找过 <home>/bin/java 与 <home>/bin/java.exe): %s",
                  out->java_path);
        return fail_stage(&st, SXCL_JRE_ERR_FINISH, SXCL_JRE_STAGE_FINISH);
    }
    {
        const char *native_lib_dir = getenv("SXCL_ANDROID_NATIVE_LIB_DIR");
        if (native_lib_dir != NULL && *native_lib_dir != '\0') {
            char patch_err[SXCL_JRE_ERROR_MAX];
            char patch_lib_dir[SXCL_ANDROID_JRE_PATH_MAX];
            patch_err[0] = '\0';
            patch_lib_dir[0] = '\0';
            if (sxcl_android_jre_patch_libs(out->java_home, native_lib_dir, patch_lib_dir,
                                           sizeof(patch_lib_dir), patch_err,
                                           sizeof(patch_err)) != SXCL_ANDROID_JRE_OK) {
                set_textf(out->error, sizeof(out->error), "JRE 侧共享库补不进去: %s", patch_err);
                return fail_stage(&st, SXCL_JRE_ERR_FINISH, SXCL_JRE_STAGE_FINISH);
            }
        }
    }
    if (write_marker(target, &comp, abi, out->index_url, apply, apply_count, out->marker_path,
                     sizeof(out->marker_path)) != 0) {
        copy_str(out->error, sizeof(out->error), "写不了 jre.json(版本标记),安装不算完成");
        return fail_stage(&st, SXCL_JRE_ERR_FINISH, SXCL_JRE_STAGE_FINISH);
    }
    /* 全绿之后才清安装包缓存:半路失败要留着断点续传(下一轮直接命中 sha256 快路径) */
    {
        size_t i;
        for (i = 0; i < apply_count; ++i) {
            char archive[SXCL_JRE_PATH_MAX];
            if (join_path(archive, sizeof(archive), archive_dir, base_name(apply[i]->file)) == 0) {
                (void)sxcl_fs_remove(archive);
            }
        }
        (void)sxcl_fs_remove_tree(archive_dir);
    }

    out->files_done = apply_count;
    out->files_failed = 0;
    out->fail_stage = SXCL_JRE_STAGE_END;
    out->fail_stage_id = sxcl_jre_stage_id(SXCL_JRE_STAGE_END);
    out->code = SXCL_JRE_OK;
    out->cancelled = 0;
    progress_message(&st, "%s %s 安装完成", comp.id, comp.version);
    emit(&st, 100, "");
    return SXCL_JRE_OK;
}
