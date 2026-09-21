/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/natives.h"

#include "sxcl/fs.h"
#include "sxcl/manifest.h"
#include "sxcl/zip.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAT_STATE_NAME   ".sxcl_natives.state"
#define NAT_STATE_HEADER "SXCLNAT1"
#define NAT_MAX_EXCLUDES 8
#define NAT_MAX_JARS     64
#define NAT_EXCLUDE_MAX  64

static int g_last_count = 0;

int sxcl_natives_last_count(void)
{
    return g_last_count;
}

/* ────────────────────────── 小工具 ────────────────────────── */

static char path_sep(void)
{
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void copy_n(char *dst, size_t cap, const char *src, size_t len)
{
    size_t take = 0;
    if (cap == 0) {
        return;
    }
    take = (len < cap - 1) ? len : cap - 1;
    if (src && take) {
        memcpy(dst, src, take);
    }
    dst[take] = '\0';
}

static char *dup_str(const char *s)
{
    const size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (p) {
        memcpy(p, s ? s : "", n + 1);
    }
    return p;
}

static void join_path(char *out, size_t cap, const char *a, const char *b)
{
    const char sep = path_sep();
    size_t n = 0;
    size_t m = 0;
    size_t take = 0;
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (a && *a) {
        n = strlen(a);
        while (n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\')) {
            --n;
        }
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, a, n);
        out[n] = '\0';
    }
    if (b && *b) {
        if (n > 0 && n + 1 < cap) {
            out[n++] = sep;
            out[n] = '\0';
        }
        m = strlen(b);
        take = (n + m < cap - 1) ? m : (cap - 1 - n);
        memcpy(out + n, b, take);
        out[n + take] = '\0';
    }
}

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;
    if (!err || err_len == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* ────────────────────────── 字符串表 ────────────────────────── */

typedef struct str_list {
    char **items;
    size_t count;
    size_t cap;
} str_list;

static int sl_push(str_list *list, const char *text)
{
    char *copy = NULL;
    if (list->count + 1 > list->cap) {
        const size_t cap = list->cap ? list->cap * 2 : 8;
        char **grown = (char **)realloc(list->items, cap * sizeof(char *));
        if (!grown) {
            return -1;
        }
        list->items = grown;
        list->cap = cap;
    }
    copy = dup_str(text);
    if (!copy) {
        return -1;
    }
    list->items[list->count++] = copy;
    return 0;
}

static void sl_free(str_list *list)
{
    size_t i = 0;
    for (i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

/* ────────────────────────── 条目名安全 / 排除规则 ────────────────────────── */

/** 条目名能不能安全落到 natives 目录里。
 *  拒绝:空名、绝对路径、盘符、任何一段是 ".."(恶意 jar 不能写到目录外面)。 */
static int entry_name_ok(const char *name)
{
    const char *p = name;
    if (!name || !*name) {
        return 0;
    }
    if (name[0] == '/' || name[0] == '\\') {
        return 0;
    }
    if (name[1] == ':') {
        return 0; /* C:... */
    }
    while (*p) {
        const char *seg = p;
        size_t n = 0;
        while (seg[n] && seg[n] != '/' && seg[n] != '\\') {
            ++n;
        }
        if (n == 0) {
            return 0; /* "a//b" 这种空段:直接拒绝,免得拼出意外的路径 */
        }
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            return 0;
        }
        if (memchr(seg, ':', n) != NULL) {
            return 0; /* NTFS 数据流 / 奇怪的段 */
        }
        p = seg + n;
        while (*p == '/' || *p == '\\') {
            ++p;
        }
    }
    return 1;
}

/* ────────────────────────── 规格 ────────────────────────── */

typedef struct nat_spec {
    char jar[SXCL_NATIVES_PATH_MAX * 2];
    char lib_name[192];
    char classifier[64];
    char excludes[NAT_MAX_EXCLUDES][NAT_EXCLUDE_MAX];
    size_t exclude_count;
    int64_t size;
    int64_t mtime_ns;
    str_list files; /* 本次就绪的相对文件名(幂等跳过时来自上一次) */
    int fresh;      /* 1 = 本次真的解过 */
} nat_spec;

static int is_excluded(const nat_spec *spec, const char *name)
{
    size_t i = 0;
    for (i = 0; i < spec->exclude_count; ++i) {
        const size_t n = strlen(spec->excludes[i]);
        if (n > 0 && strncmp(name, spec->excludes[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

/** 从分类器名里认出目标系统:"natives-windows" / "natives-linux" / "natives-macos" ...
 *  返回 1 = 认出来了(out 是规范 os 名),0 = 名字里没写平台(交给 rules 决定)。 */
static int classifier_os(const char *classifier, char *out, size_t cap)
{
    /* "macos" 必须排在 "mac" 前面,否则 macos 会被 mac 抢先匹配 */
    static const struct {
        const char *token;
        const char *os;
    } kMap[] = { { "windows", "windows" }, { "linux", "linux" },   { "macos", "osx" },
                 { "osx", "osx" },         { "mac", "osx" },       { "freebsd", "freebsd" },
                 { "android", "android" } };
    const char *p = strstr(classifier, "natives-");
    size_t i = 0;
    if (!p) {
        return 0;
    }
    p += 8; /* strlen("natives-") */
    for (i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i) {
        const size_t n = strlen(kMap[i].token);
        if (strncmp(p, kMap[i].token, n) == 0 && (p[n] == '\0' || p[n] == '-')) {
            copy_str(out, cap, kMap[i].os);
            return 1;
        }
    }
    return 0;
}

/** 取 "group:artifact:version[:classifier]" 里的分类器部分。返回 1 = 有分类器。 */
static int name_classifier(const char *name, char *out, size_t cap)
{
    const char *p = name;
    int colon = 0;
    out[0] = '\0';
    for (; *p; ++p) {
        if (*p == ':') {
            ++colon;
            if (colon == 3) {
                copy_str(out, cap, p + 1);
                return out[0] ? 1 : 0;
            }
        }
    }
    return 0;
}

/** 把一条 library 解析成 natives 规格。
 *  返回 1 = 是 natives 库(已填 spec),0 = 不是 natives 库(跳过),-1 = 出错(err 有原因)。 */
static int spec_from_lib(const sxcl_json_value *lib, const char *lib_dir, const char *os_name,
                         nat_spec *spec, char *err, size_t err_len)
{
    const char *name = sxcl_json_get_string(lib, "name", "");
    const sxcl_json_value *downloads = sxcl_json_get(lib, "downloads");
    const sxcl_json_value *natives = sxcl_json_get(lib, "natives");
    const char *rel = NULL;
    char classifier[64];
    char c_os[16];

    memset(spec, 0, sizeof(*spec));
    classifier[0] = '\0';

    /* 老形态:library.natives.<os> = 分类器键,jar 在 downloads.classifiers 里 */
    {
        const char *old_key = sxcl_json_get_string(natives, os_name, NULL);
        if (old_key && *old_key) {
            const sxcl_json_value *cls = sxcl_json_get(sxcl_json_get(downloads, "classifiers"), old_key);
            copy_str(classifier, sizeof(classifier), old_key);
            rel = sxcl_json_get_string(cls, "path", NULL);
        }
    }
    /* 新形态:natives 自己就是一条带 ":natives-<os>" 分类器的库,jar 在 downloads.artifact 里 */
    if (!classifier[0]) {
        if (!name_classifier(name, classifier, sizeof(classifier)) ||
            !strstr(classifier, "natives")) {
            return 0;
        }
        if (classifier_os(classifier, c_os, sizeof(c_os)) && strcmp(c_os, os_name) != 0) {
            return 0; /* 是别家的原生库(比如 Windows 的 dll),别往这台上解 */
        }
        rel = sxcl_json_get_string(sxcl_json_get(downloads, "artifact"), "path", NULL);
    }
    if (!rel || !*rel) {
        if (classifier[0]) {
            set_err(err, err_len, "版本 JSON 里库 %s 的 natives 分类器 %s 没有下载路径", name,
                    classifier);
            return -1;
        }
        return 0;
    }

    join_path(spec->jar, sizeof(spec->jar), lib_dir, rel);
    copy_str(spec->lib_name, sizeof(spec->lib_name), name);
    copy_str(spec->classifier, sizeof(spec->classifier), classifier);

    /* extract.exclude:前缀列表;两种形态都挂在 library 上 */
    {
        const sxcl_json_value *exclude = sxcl_json_get(sxcl_json_get(lib, "extract"), "exclude");
        const size_t n = sxcl_json_size(exclude);
        size_t i = 0;
        for (i = 0; i < n && spec->exclude_count < NAT_MAX_EXCLUDES; ++i) {
            const char *item = sxcl_json_string(sxcl_json_at(exclude, i));
            if (item && *item) {
                copy_str(spec->excludes[spec->exclude_count], NAT_EXCLUDE_MAX, item);
                ++spec->exclude_count;
            }
        }
    }
    return 1;
}

/* ────────────────────────── 状态文件 ────────────────────────── */

/** 整个读进内存(NULL = 没有/读不了,一律当"没有状态")。 */
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size = 0;
    char *buf = NULL;
    size_t got = 0;
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || size > (long)(1024 * 1024)) { /* 状态文件不可能这么大,大了就当坏的 */
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    if (strncmp(buf, NAT_STATE_HEADER, strlen(NAT_STATE_HEADER)) != 0) {
        free(buf); /* 格式对不上(旧版本/被改过):当作没有状态,重解一遍就是了 */
        return NULL;
    }
    return buf;
}

/** 在状态文本里找某个 jar 的记录。找到返回它后面第一条文件行的位置,并把大小/mtime 写出来。 */
static const char *state_find(const char *text, const char *jar, int64_t *size, int64_t *mtime)
{
    const char *line = text;
    const size_t jar_len = strlen(jar);
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        const size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > 4 && strncmp(line, "JAR\t", 4) == 0) {
            const char *p = line + 4;
            const char *end = line + len;
            const char *t1 = (const char *)memchr(p, '\t', (size_t)(end - p));
            if (t1) {
                const char *t2 = (const char *)memchr(t1 + 1, '\t', (size_t)(end - t1 - 1));
                if (t2) {
                    const char *path = t2 + 1;
                    const size_t path_len = (size_t)(end - path);
                    if (path_len == jar_len && memcmp(path, jar, jar_len) == 0) {
                        char num[32];
                        copy_n(num, sizeof(num), p, (size_t)(t1 - p));
                        *size = (int64_t)strtoll(num, NULL, 10);
                        copy_n(num, sizeof(num), t1 + 1, (size_t)(t2 - t1 - 1));
                        *mtime = (int64_t)strtoll(num, NULL, 10);
                        return eol ? eol + 1 : end;
                    }
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return NULL;
}

/** 收集某个 jar 记录下面的文件行(遇到下一条 JAR 行为止)。返回 0 成功。 */
static int state_collect_files(const char *p, str_list *out)
{
    while (p && *p && strncmp(p, "JAR\t", 4) != 0) {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 2 && p[0] == 'F' && p[1] == '\t') {
            char name[SXCL_NATIVES_PATH_MAX];
            copy_n(name, sizeof(name), p + 2, len - 2);
            if (name[0] && sl_push(out, name) != 0) {
                return -1;
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}

static int state_write(const char *state_path, const nat_spec *specs, size_t count)
{
    char tmp[SXCL_NATIVES_PATH_MAX * 2 + 8];
    size_t i = 0;
    size_t j = 0;
    FILE *f = NULL;
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);
    (void)sxcl_fs_mkdirs_for_file(state_path);
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    fputs(NAT_STATE_HEADER "\n", f);
    for (i = 0; i < count; ++i) {
        (void)fprintf(f, "JAR\t%lld\t%lld\t%s\n", (long long)specs[i].size,
                      (long long)specs[i].mtime_ns, specs[i].jar);
        for (j = 0; j < specs[i].files.count; ++j) {
            (void)fprintf(f, "F\t%s\n", specs[i].files.items[j]);
        }
    }
    if (fclose(f) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    if (sxcl_fs_rename_replace(tmp, state_path) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

/* ────────────────────────── 主流程 ────────────────────────── */

static int prepare_core(const sxcl_json_value *root, const char *game_dir, const char *natives_dir,
                        char *err, size_t err_len)
{
    nat_spec specs[NAT_MAX_JARS];
    size_t spec_count = 0;
    size_t i = 0;
    size_t j = 0;
    size_t total = 0;
    int rc = -1;
    char lib_dir[SXCL_NATIVES_PATH_MAX];
    char state_path[SXCL_NATIVES_PATH_MAX];
    char *state_text = NULL;
    const char *os_name = sxcl_platform_os_name();
    const char *arch_name = sxcl_platform_arch_name();
    const sxcl_json_value *libs = NULL;
    size_t lib_count = 0;

    g_last_count = 0;
    if (err && err_len) {
        err[0] = '\0';
    }
    memset(specs, 0, sizeof(specs));
    if (!root) {
        set_err(err, err_len, "版本 JSON 是空的,读不出原生库清单");
        return -1;
    }
    if (!game_dir || !*game_dir || !natives_dir || !*natives_dir) {
        set_err(err, err_len, "原生库抽取缺少游戏目录或 natives 目录");
        return -1;
    }

    join_path(lib_dir, sizeof(lib_dir), game_dir, "libraries");

    /* 1) 收集本平台要解的 jar(判定与下载/Rules 用的是同一套 sxcl_rules_allow) */
    libs = sxcl_json_get(root, "libraries");
    lib_count = sxcl_json_size(libs);
    for (i = 0; i < lib_count; ++i) {
        const sxcl_json_value *lib = sxcl_json_at(libs, i);
        int is_nat = 0;
        if (!lib || sxcl_json_type_of(lib) != SXCL_JSON_OBJECT) {
            continue;
        }
        if (!sxcl_rules_allow(sxcl_json_get(lib, "rules"), os_name, arch_name)) {
            continue;
        }
        if (spec_count >= NAT_MAX_JARS) {
            set_err(err, err_len, "原生库条目超过 %d 个,拒绝继续(版本 JSON 不正常)", NAT_MAX_JARS);
            goto done;
        }
        is_nat = spec_from_lib(lib, lib_dir, os_name, &specs[spec_count], err, err_len);
        if (is_nat < 0) {
            goto done;
        }
        if (is_nat > 0) {
            ++spec_count;
        }
    }
    if (spec_count == 0) {
        rc = 0; /* 这个版本没有原生库:不是错误 */
        goto done;
    }

    /* 2) natives 目录 + 旧状态 */
    if (sxcl_fs_mkdirs(natives_dir) != 0) {
        set_err(err, err_len, "建不了 natives 目录:%s", natives_dir);
        goto done;
    }
    join_path(state_path, sizeof(state_path), natives_dir, NAT_STATE_NAME);
    state_text = read_text(state_path);

    /* 3) 逐个 jar:状态对得上就跳过,否则老实解 */
    for (i = 0; i < spec_count; ++i) {
        nat_spec *sp = &specs[i];
        if (sxcl_fs_stat(sp->jar, &sp->size, &sp->mtime_ns) != 0) {
            set_err(err, err_len, "原生库 jar 不存在:%s(版本 JSON 里库 %s 的 %s 分类器没下全)",
                    sp->jar, sp->lib_name, sp->classifier);
            goto done;
        }
        {
            int64_t old_size = 0;
            int64_t old_mtime = 0;
            const char *after = state_text ? state_find(state_text, sp->jar, &old_size, &old_mtime) : NULL;
            if (after && old_size == sp->size && old_mtime == sp->mtime_ns) {
                str_list old_files;
                memset(&old_files, 0, sizeof(old_files));
                if (state_collect_files(after, &old_files) == 0) {
                    int all_there = old_files.count > 0;
                    for (j = 0; j < old_files.count && all_there; ++j) {
                        char dst[SXCL_NATIVES_PATH_MAX * 2];
                        join_path(dst, sizeof(dst), natives_dir, old_files.items[j]);
                        if (!sxcl_fs_exists(dst)) {
                            all_there = 0; /* 文件被删了/被杀软吃了:重解一遍 */
                        }
                    }
                    if (all_there) {
                        for (j = 0; j < old_files.count; ++j) {
                            if (sl_push(&sp->files, old_files.items[j]) != 0) {
                                sl_free(&old_files);
                                set_err(err, err_len, "内存不足(记录原生库状态)");
                                goto done;
                            }
                        }
                    }
                }
                sl_free(&old_files);
                if (sp->files.count > 0) {
                    continue; /* 幂等:这一份 jar 一个字节都不用写 */
                }
            }
        }

        {
            sxcl_zip *zip = sxcl_zip_open(sp->jar);
            const size_t entries = zip ? sxcl_zip_count(zip) : 0;
            if (!zip) {
                set_err(err, err_len, "打不开原生库 jar:%s(版本 JSON 里库 %s)", sp->jar,
                        sp->lib_name);
                goto done;
            }
            sp->fresh = 1;
            for (j = 0; j < entries; ++j) {
                const char *entry = sxcl_zip_name_at(zip, j);
                char dst[SXCL_NATIVES_PATH_MAX * 2];
                if (!entry || !*entry) {
                    continue;
                }
                {
                    const size_t len = strlen(entry);
                    if (entry[len - 1] == '/' || entry[len - 1] == '\\') {
                        continue; /* 目录条目:自己建父目录就够了 */
                    }
                }
                if (is_excluded(sp, entry)) {
                    continue;
                }
                if (!entry_name_ok(entry)) {
                    set_err(err, err_len,
                            "原生库 jar %s 里的条目 %s 会写到 natives 目录外面,已拒绝", sp->jar,
                            entry);
                    sxcl_zip_close(zip);
                    goto done;
                }
                join_path(dst, sizeof(dst), natives_dir, entry);
                if (sxcl_zip_extract_file(zip, entry, dst) != 0) {
                    set_err(err, err_len, "解压原生库失败:%s 里的 %s", sp->jar, entry);
                    sxcl_zip_close(zip);
                    goto done;
                }
                if (sl_push(&sp->files, entry) != 0) {
                    set_err(err, err_len, "内存不足(记录解出的原生库文件名)");
                    sxcl_zip_close(zip);
                    goto done;
                }
            }
            sxcl_zip_close(zip);
        }
    }

    /* 4) 写状态(写不了不算失败:下一次顶多多解一遍,不影响能不能启动) */
    for (i = 0; i < spec_count; ++i) {
        total += specs[i].files.count;
    }
    (void)state_write(state_path, specs, spec_count);
    g_last_count = (int)total;
    rc = 0;

done:
    free(state_text);
    for (i = 0; i < spec_count; ++i) {
        sl_free(&specs[i].files);
    }
    if (rc != 0) {
        g_last_count = 0;
    }
    return rc;
}

int sxcl_natives_prepare_json(const sxcl_json *version_json, const char *game_dir,
                              const char *natives_dir, char *err, size_t err_len)
{
    return prepare_core(sxcl_json_root(version_json), game_dir, natives_dir, err, err_len);
}

int sxcl_natives_prepare(const char *version_json_path, const char *game_dir,
                         const char *natives_dir, char *err, size_t err_len)
{
    char jerr[192];
    sxcl_json *doc = NULL;
    int rc = 0;
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!version_json_path || !*version_json_path) {
        set_err(err, err_len, "原生库抽取缺少版本 JSON 路径");
        g_last_count = 0;
        return -1;
    }
    doc = sxcl_json_parse_file(version_json_path, jerr, sizeof(jerr));
    if (!doc) {
        set_err(err, err_len, "读不了版本 JSON:%s(%s)", version_json_path, jerr);
        g_last_count = 0;
        return -1;
    }
    rc = prepare_core(sxcl_json_root(doc), game_dir, natives_dir, err, err_len);
    sxcl_json_free(doc);
    return rc;
}
