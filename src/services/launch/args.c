/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/launch.h"

#include "sxcl/manifest.h"
#include "sxcl/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SXCL_ARG_PATH_MAX 1024

/* ────────────────────────── 参数数组 / 字符串缓冲 ────────────────────────── */

struct sxcl_launch_args {
    char **items; /* NULL 结尾 */
    size_t count;
    size_t cap;
};

typedef struct sxcl_launch_args arg_vec;

static int av_reserve(arg_vec *av, size_t need)
{
    size_t cap = 0;
    char **items = NULL;
    if (av->cap >= need) {
        return 0;
    }
    cap = av->cap ? av->cap : 8;
    while (cap < need) {
        cap *= 2;
    }
    items = (char **)realloc(av->items, cap * sizeof(char *));
    if (!items) {
        return -1;
    }
    av->items = items;
    av->cap = cap;
    return 0;
}

/** 接下一块已 malloc 的内存(失败则就地释放)。 */
static int av_take(arg_vec *av, char *owned)
{
    if (!owned) {
        return -1;
    }
    if (av_reserve(av, av->count + 2) != 0) {
        free(owned);
        return -1;
    }
    av->items[av->count++] = owned;
    av->items[av->count] = NULL;
    return 0;
}

static int av_push(arg_vec *av, const char *text)
{
    const size_t n = text ? strlen(text) : 0;
    char *copy = (char *)malloc(n + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, text ? text : "", n + 1);
    return av_take(av, copy);
}

/** 把 src 的元素整体搬进 dst(所有权转移,src 变成空壳)。 */
static void av_absorb(arg_vec *dst, arg_vec *src)
{
    size_t i = 0;
    for (i = 0; i < src->count; ++i) {
        (void)av_take(dst, src->items[i]); /* 失败时 av_take 已经 free 掉了 */
    }
    free(src->items);
    src->items = NULL;
    src->count = 0;
    src->cap = 0;
}

typedef struct strbuf {
    char *data;
    size_t len;
    size_t cap;
} strbuf;

static int sb_reserve(strbuf *sb, size_t extra)
{
    size_t cap = 0;
    char *data = NULL;
    if (sb->cap >= sb->len + extra + 1) {
        return 0;
    }
    cap = sb->cap ? sb->cap : 128;
    while (cap < sb->len + extra + 1) {
        cap *= 2;
    }
    data = (char *)realloc(sb->data, cap);
    if (!data) {
        return -1;
    }
    sb->data = data;
    sb->cap = cap;
    return 0;
}

static int sb_append_n(strbuf *sb, const char *text, size_t n)
{
    if (sb_reserve(sb, n) != 0) {
        return -1;
    }
    if (n) {
        memcpy(sb->data + sb->len, text, n);
    }
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

static int sb_append(strbuf *sb, const char *text)
{
    return sb_append_n(sb, text ? text : "", text ? strlen(text) : 0);
}

static void sb_free(strbuf *sb)
{
    if (!sb) {
        return;
    }
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/** 交出缓冲区所有权(调用方 free)。 */
static char *sb_take(strbuf *sb)
{
    char *data = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    if (!data) {
        data = (char *)malloc(1);
        if (data) {
            data[0] = '\0';
        }
    }
    return data;
}

/* ────────────────────────── 平台 ────────────────────────── */

static sxcl_launch_os eff_os(const sxcl_launch_ctx *ctx)
{
    if (ctx->os != SXCL_LAUNCH_OS_AUTO) {
        return ctx->os;
    }
    switch (sxcl_java_current_os()) {
    case SXCL_JAVA_OS_WINDOWS: return SXCL_LAUNCH_OS_WINDOWS;
    case SXCL_JAVA_OS_MACOS:   return SXCL_LAUNCH_OS_MACOS;
    case SXCL_JAVA_OS_ANDROID: return SXCL_LAUNCH_OS_ANDROID;
    case SXCL_JAVA_OS_LINUX:
    default:                   return SXCL_LAUNCH_OS_LINUX;
    }
}

static char os_path_sep(sxcl_launch_os os)
{
    return os == SXCL_LAUNCH_OS_WINDOWS ? '\\' : '/';
}

static const char *rules_os_name(const sxcl_launch_ctx *ctx, sxcl_launch_os os)
{
    if (ctx->os_name && *ctx->os_name) {
        return ctx->os_name;
    }
    switch (os) {
    case SXCL_LAUNCH_OS_WINDOWS: return "windows";
    case SXCL_LAUNCH_OS_MACOS:   return "osx";
    case SXCL_LAUNCH_OS_LINUX:
    case SXCL_LAUNCH_OS_ANDROID: /* 安卓在 rules 里按 linux 匹配 */
    default:                     return "linux";
    }
}

int sxcl_launch_default_memory_mb(int is_64bit)
{
    if (is_64bit == 1) {
        return 4096;
    }
    if (is_64bit == 0) {
        return 1024;
    }
    return 2048; /* 位数未知:32 位 JVM 的 -Xmx 上不去,取中间值 */
}

/* ────────────────────────── 路径/字符串小工具 ────────────────────────── */

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

static const char *pick(const char *value, const char *fallback)
{
    return (value && *value) ? value : fallback;
}

static void join_path(char *out, size_t cap, const char *a, const char *b, char sep)
{
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

/** 把路径统一成目标平台的分隔符。
 *  启动器内部一律存 UTF-8 + 正斜杠(跨平台最省事),但 Windows 上有些原生库
 *  (老 jinput/LWJGL2)只认反斜杠,而 Linux/安卓上出现反斜杠一定是错的;
 *  所以在拼参数的最后一步统一。ctx->keep_path_separator 让调用方关掉这一步。 */
static void normalize_path(char *dst, size_t cap, const char *src, char sep, int keep)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; *src && n + 1 < cap; ++src) {
        char ch = *src;
        if (!keep && (ch == '/' || ch == '\\')) {
            ch = sep;
        }
        dst[n++] = ch;
    }
    dst[n] = '\0';
}

static void set_err(char *err, size_t err_len, const char *text)
{
    if (err && err_len) {
        copy_str(err, err_len, text);
    }
}

/* ────────────────────────── 已解析的取值 ────────────────────────── */

typedef struct resolved {
    char game_dir[SXCL_ARG_PATH_MAX];
    char assets_root[SXCL_ARG_PATH_MAX];
    char natives[SXCL_ARG_PATH_MAX];
    char lib_dir[SXCL_ARG_PATH_MAX];
    char client_jar[SXCL_ARG_PATH_MAX];
    char game_assets[SXCL_ARG_PATH_MAX];
    char assets_index[128];
    char cp_sep[2];
    char *classpath; /* 堆上:几百个库拼起来可能几十 KB */
    const char *player_name;
    const char *uuid;
    const char *access_token;
    const char *user_type;
    const char *xuid;
    const char *client_id;
    const char *version_name;
    const char *version_type;
    const char *launcher_name;
    const char *launcher_version;
} resolved;

static const char *resolve_placeholder(const char *key, const resolved *res)
{
    if (strcmp(key, "auth_player_name") == 0) {
        return res->player_name;
    }
    if (strcmp(key, "auth_uuid") == 0) {
        return res->uuid;
    }
    if (strcmp(key, "auth_access_token") == 0) {
        return res->access_token;
    }
    if (strcmp(key, "auth_session") == 0) { /* 老版本用过这个名字 */
        return res->access_token;
    }
    if (strcmp(key, "user_type") == 0) {
        return res->user_type;
    }
    /* 1.20.2+ 的启动参数里有 ${auth_xuid} 与 ${clientid}:以前不认识,会把字面量
     * "${auth_xuid}" 当成 --xuid 的值传给游戏。这里补上,默认 "0" / ""(离线时也就这个值)。 */
    if (strcmp(key, "auth_xuid") == 0) {
        return res->xuid;
    }
    if (strcmp(key, "clientid") == 0) {
        return res->client_id;
    }
    if (strcmp(key, "version_name") == 0) {
        return res->version_name;
    }
    if (strcmp(key, "version_type") == 0) {
        return res->version_type;
    }
    if (strcmp(key, "game_directory") == 0) {
        return res->game_dir;
    }
    if (strcmp(key, "assets_root") == 0 || strcmp(key, "assets_directory") == 0) {
        return res->assets_root;
    }
    if (strcmp(key, "assets_index_name") == 0) {
        return res->assets_index;
    }
    if (strcmp(key, "game_assets") == 0) {
        return res->game_assets;
    }
    if (strcmp(key, "natives_directory") == 0) {
        return res->natives;
    }
    if (strcmp(key, "library_directory") == 0) {
        return res->lib_dir;
    }
    if (strcmp(key, "classpath") == 0) {
        return res->classpath ? res->classpath : "";
    }
    if (strcmp(key, "classpath_separator") == 0) {
        return res->cp_sep;
    }
    if (strcmp(key, "launcher_name") == 0) {
        return res->launcher_name;
    }
    if (strcmp(key, "launcher_version") == 0) {
        return res->launcher_version;
    }
    return NULL;
}

static int sb_expand(strbuf *sb, const char *text, const resolved *res)
{
    const char *p = text ? text : "";
    while (*p) {
        if (p[0] == '$' && p[1] == '{') {
            const char *close = strchr(p + 2, '}');
            if (close && (size_t)(close - (p + 2)) < 64) {
                char key[64];
                const size_t n = (size_t)(close - (p + 2));
                const char *value = NULL;
                memcpy(key, p + 2, n);
                key[n] = '\0';
                value = resolve_placeholder(key, res);
                if (value) {
                    if (sb_append(sb, value) != 0) {
                        return -1;
                    }
                    p = close + 1;
                    continue;
                }
                /* 不认识的占位符原样留着:日志里能一眼看出是谁没被展开 */
                if (sb_append_n(sb, p, (size_t)(close + 1 - p)) != 0) {
                    return -1;
                }
                p = close + 1;
                continue;
            }
        }
        if (sb_append_n(sb, p, 1) != 0) {
            return -1;
        }
        ++p;
    }
    return 0;
}

static char *expand_dup(const char *text, const resolved *res)
{
    strbuf sb;
    memset(&sb, 0, sizeof(sb));
    if (sb_expand(&sb, text, res) != 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_take(&sb);
}

/** 先按上下文取值,再做路径归一化。顺序不能反:归一化会动分隔符。 */
static void set_path(char *dst, size_t cap, const char *a, const char *b, char sep, int keep)
{
    char joined[SXCL_ARG_PATH_MAX * 2];
    if (b && *b) {
        join_path(joined, sizeof(joined), a, b, sep);
    } else {
        copy_str(joined, sizeof(joined), a ? a : "");
    }
    normalize_path(dst, cap, joined, sep, keep);
}

/* ────────────────────────── 上下文 -> 取值 ────────────────────────── */

static void resolve_context(const sxcl_json_value *root, const sxcl_launch_ctx *ctx, resolved *res)
{
    const sxcl_launch_os os = eff_os(ctx);
    const char sep = os_path_sep(os);
    const int keep = ctx->keep_path_separator ? 1 : 0;
    char tmp[SXCL_ARG_PATH_MAX];
    const char *asset_id = NULL;

    memset(res, 0, sizeof(*res));
    res->player_name = pick(ctx->player_name, "Player");
    res->uuid = pick(ctx->uuid, "00000000-0000-0000-0000-000000000000");
    res->access_token = pick(ctx->access_token, "0");
    res->user_type = pick(ctx->user_type, "msa");
    res->xuid = pick(ctx->xuid, "0");
    res->client_id = pick(ctx->client_id, "");
    res->version_name = pick(ctx->version_name, sxcl_json_get_string(root, "id", ""));
    res->version_type = pick(ctx->version_type, sxcl_json_get_string(root, "type", "release"));
    res->launcher_name = pick(ctx->launcher_name, "SilentXCraftLauncher");
    res->launcher_version = pick(ctx->launcher_version, sxcl_version_string());
    res->cp_sep[0] = (os == SXCL_LAUNCH_OS_WINDOWS) ? ';' : ':';
    res->cp_sep[1] = '\0';

    set_path(res->game_dir, sizeof(res->game_dir), ctx->game_directory, NULL, sep, keep);

    if (ctx->assets_root && *ctx->assets_root) {
        set_path(res->assets_root, sizeof(res->assets_root), ctx->assets_root, NULL, sep, keep);
    } else {
        set_path(res->assets_root, sizeof(res->assets_root), res->game_dir, "assets", sep, keep);
    }
    if (ctx->library_directory && *ctx->library_directory) {
        set_path(res->lib_dir, sizeof(res->lib_dir), ctx->library_directory, NULL, sep, keep);
    } else {
        set_path(res->lib_dir, sizeof(res->lib_dir), res->game_dir, "libraries", sep, keep);
    }
    if (ctx->natives_directory && *ctx->natives_directory) {
        set_path(res->natives, sizeof(res->natives), ctx->natives_directory, NULL, sep, keep);
    } else {
        char rel[SXCL_ARG_PATH_MAX];
        char sub[SXCL_ARG_PATH_MAX];
        char leaf[SXCL_ARG_PATH_MAX];
        /* 注意:叶子文件名是拼出来的,不能当路径段 join,否则会多一个分隔符(1.21.4\\.jar) */
        (void)snprintf(leaf, sizeof(leaf), "%s-natives", res->version_name);
        join_path(sub, sizeof(sub), "versions", res->version_name, sep);
        join_path(rel, sizeof(rel), sub, leaf, sep);
        set_path(res->natives, sizeof(res->natives), res->game_dir, rel, sep, keep);
    }
    if (ctx->client_jar && *ctx->client_jar) {
        set_path(res->client_jar, sizeof(res->client_jar), ctx->client_jar, NULL, sep, keep);
    } else {
        char rel[SXCL_ARG_PATH_MAX];
        char sub[SXCL_ARG_PATH_MAX];
        char leaf[SXCL_ARG_PATH_MAX];
        (void)snprintf(leaf, sizeof(leaf), "%s.jar", res->version_name);
        join_path(sub, sizeof(sub), "versions", res->version_name, sep);
        join_path(rel, sizeof(rel), sub, leaf, sep);
        set_path(res->client_jar, sizeof(res->client_jar), res->game_dir, rel, sep, keep);
    }
    /* 资源索引名:上下文优先,其次版本 JSON 的 assetIndex.id */
    asset_id = sxcl_json_get_string(sxcl_json_get(root, "assetIndex"), "id", NULL);
    copy_str(res->assets_index, sizeof(res->assets_index),
             pick(ctx->assets_index_name, pick(asset_id, "legacy")));
    /* 1.6 之前的老版本把资源虚拟映射到 assets/virtual/legacy */
    set_path(tmp, sizeof(tmp), res->assets_root, "virtual/legacy", sep, keep);
    copy_str(res->game_assets, sizeof(res->game_assets), tmp);
}

/* ────────────────────────── classpath ────────────────────────── */

typedef struct cp_entry {
    char *name;
    char *path;
    size_t path_cap;
} cp_entry;

static int cp_cmp(const void *lhs, const void *rhs)
{
    const cp_entry *x = (const cp_entry *)lhs;
    const cp_entry *y = (const cp_entry *)rhs;
    int c = strcmp(x->name, y->name);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    c = strcmp(x->path, y->path);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static void cp_list_free(cp_entry *list, size_t count)
{
    size_t i = 0;
    if (!list) {
        return;
    }
    for (i = 0; i < count; ++i) {
        free(list[i].name);
        free(list[i].path);
    }
    free(list);
}

/** 客户端 jar 在前,库按库名升序接在后面;同一路径只留一份。 */
static int build_classpath(const sxcl_json_value *root, const sxcl_launch_ctx *ctx,
                           const char *os_name, const char *arch_name, resolved *res)
{
    const sxcl_json_value *libraries = sxcl_json_get(root, "libraries");
    const size_t lib_count = sxcl_json_size(libraries);
    const char sep = os_path_sep(eff_os(ctx));
    const int keep = ctx->keep_path_separator ? 1 : 0;
    cp_entry *list = NULL;
    size_t count = 0;
    size_t capacity = lib_count ? lib_count : 1;
    size_t i = 0;
    strbuf sb;

    list = (cp_entry *)calloc(capacity, sizeof(cp_entry));
    if (!list) {
        return -1;
    }
    for (i = 0; i < lib_count; ++i) {
        const sxcl_json_value *lib = sxcl_json_at(libraries, i);
        const sxcl_json_value *artifact = NULL;
        const char *rel = NULL;
        const char *name = NULL;
        char joined[SXCL_ARG_PATH_MAX * 2];
        size_t need = 0;
        if (!lib || sxcl_json_type_of(lib) != SXCL_JSON_OBJECT) {
            continue;
        }
        if (!sxcl_rules_allow(sxcl_json_get(lib, "rules"), os_name, arch_name)) {
            continue;
        }
        artifact = sxcl_json_get(sxcl_json_get(lib, "downloads"), "artifact");
        rel = sxcl_json_get_string(artifact, "path", NULL);
        if (!rel || !*rel) {
            /* 只有 natives 分类器的老库:不占 classpath,由 natives 目录负责 */
            continue;
        }
        name = sxcl_json_get_string(lib, "name", rel);
        list[count].name = (char *)malloc(strlen(name) + 1);
        if (!list[count].name) {
            cp_list_free(list, count);
            return -1;
        }
        copy_str(list[count].name, strlen(name) + 1, name);
        join_path(joined, sizeof(joined), res->lib_dir, rel, sep);
        need = strlen(joined) + 1;
        list[count].path = (char *)malloc(need);
        if (!list[count].path) {
            cp_list_free(list, count + 1);
            return -1;
        }
        list[count].path_cap = need;
        normalize_path(list[count].path, need, joined, sep, keep);
        ++count;
    }

    qsort(list, count, sizeof(list[0]), cp_cmp);

    memset(&sb, 0, sizeof(sb));
    if (sb_append(&sb, res->client_jar) != 0) {
        cp_list_free(list, count);
        sb_free(&sb);
        return -1;
    }
    for (i = 0; i < count; ++i) {
        size_t j = 0;
        int dup = 0;
        for (j = 0; j < i; ++j) {
            if (strcmp(list[j].path, list[i].path) == 0) {
                dup = 1; /* 去重:同一路径只上一次(模组 JSON 里常出现重复库) */
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (sb_append(&sb, res->cp_sep) != 0 || sb_append(&sb, list[i].path) != 0) {
            cp_list_free(list, count);
            sb_free(&sb);
            return -1;
        }
    }
    cp_list_free(list, count);
    res->classpath = sb_take(&sb);
    return res->classpath ? 0 : -1;
}

/* ────────────────────────── 默认 JVM 参数 ────────────────────────── */

static int push_fmt(arg_vec *av, const char *fmt, const char *value)
{
    char buf[512];
    (void)snprintf(buf, sizeof(buf), fmt, value);
    return av_push(av, buf);
}

static int emit_default_jvm(arg_vec *av, const sxcl_launch_ctx *ctx, const resolved *res,
                            int is64, int have_lib_path, int have_lwjgl_path)
{
    const sxcl_launch_os os = eff_os(ctx);
    const int java_major = ctx->java_major;
    const int memory = ctx->memory_mb > 0 ? ctx->memory_mb : sxcl_launch_default_memory_mb(is64);
    int unlocked = 0;
    char buf[256];

    (void)snprintf(buf, sizeof(buf), "-Xmx%dM", memory);
    if (av_push(av, buf) != 0) {
        return -1;
    }

    /* GC:64 位走 G1;32 位或不确认位数走 Serial —— G1 在 32 位 JVM 上直接拒绝启动 */
    if (is64 == 1) {
        if (av_push(av, "-XX:+UnlockExperimentalVMOptions") != 0) {
            return -1;
        }
        unlocked = 1;
        if (av_push(av, "-XX:+UseG1GC") != 0 || av_push(av, "-XX:G1NewSizePercent=20") != 0 ||
            av_push(av, "-XX:G1ReservePercent=20") != 0 ||
            av_push(av, "-XX:G1HeapRegionSize=32M") != 0 ||
            av_push(av, "-XX:MaxGCPauseMillis=50") != 0) {
            return -1;
        }
    } else if (av_push(av, "-XX:+UseSerialGC") != 0) {
        return -1;
    }
    if (av_push(av, "-XX:MinHeapFreeRatio=25") != 0 ||
        av_push(av, "-XX:MaxHeapFreeRatio=40") != 0 ||
        av_push(av, "-XX:+PerfDisableSharedMem") != 0 ||
        av_push(av, "-XX:-OmitStackTraceInFastThrow") != 0) {
        return -1;
    }

    /* 版本相关开关只在确认主版本时才加:JDK 遇到不认识的选项会直接拒绝启动,
     * 所以门槛按各选项**真正被引入**的版本设,宁可少加。 */
    if (java_major >= 17) {
        if (av_push(av, "--enable-native-access=ALL-UNNAMED") != 0) { /* JDK 17 引入 */
            return -1;
        }
    }
    if (java_major >= 23) {
        if (av_push(av, "--sun-misc-unsafe-memory-access=allow") != 0) { /* JDK 23 引入 */
            return -1;
        }
    }
    if (java_major >= 24) {
        if (!unlocked && av_push(av, "-XX:+UnlockExperimentalVMOptions") != 0) {
            return -1;
        }
        unlocked = 1;
        if (av_push(av, "-XX:+UseCompactObjectHeaders") != 0) { /* JEP 450,JDK 24 引入 */
            return -1;
        }
    }

    if (av_push(av, "-Dfile.encoding=UTF-8") != 0 ||
        av_push(av, "-Dstdout.encoding=UTF-8") != 0 ||
        av_push(av, "-Dstderr.encoding=UTF-8") != 0) {
        return -1;
    }
    if (push_fmt(av, "-Dminecraft.launcher.brand=%s", res->launcher_name) != 0 ||
        push_fmt(av, "-Dminecraft.launcher.version=%s", res->launcher_version) != 0) {
        return -1;
    }
    /* log4j2 的 JNDI 漏洞缓解(1.7~1.18 靠它);其余是 FML/老版本常见的兼容开关 */
    if (av_push(av, "-Dlog4j2.formatMsgNoLookups=true") != 0 ||
        av_push(av, "-Djdk.lang.Process.allowAmbiguousCommands=true") != 0 ||
        av_push(av, "-Dfml.ignoreInvalidMinecraftCertificates=true") != 0 ||
        av_push(av, "-Dfml.ignorePatchDiscrepancies=true") != 0) {
        return -1;
    }

    /* 原生库目录:版本 JSON 自带就不重复加 */
    if (!have_lib_path && push_fmt(av, "-Djava.library.path=%s", res->natives) != 0) {
        return -1;
    }
    if (!have_lwjgl_path && push_fmt(av, "-Dorg.lwjgl.librarypath=%s", res->natives) != 0) {
        return -1;
    }

    if (os == SXCL_LAUNCH_OS_MACOS) {
        /* macOS 上 GLFW 要求主线程;不加会直接崩在窗口创建 */
        if (av_push(av, "-XstartOnFirstThread") != 0 ||
            av_push(av, "-Djava.awt.headless=true") != 0) {
            return -1;
        }
    } else if (os == SXCL_LAUNCH_OS_LINUX || os == SXCL_LAUNCH_OS_ANDROID) {
        if (av_push(av, "-Djava.awt.headless=true") != 0) {
            return -1;
        }
    }
    return 0;
}

/* ────────────────────────── 版本 JSON 的参数数组 ────────────────────────── */

typedef struct expand_state {
    int saw_classpath;
    int pending_cp;
    int saw_lib_path;
    int saw_lwjgl_path;
} expand_state;

static void note_arg(expand_state *st, const char *arg)
{
    if (!st || !arg) {
        return;
    }
    if (st->pending_cp) {
        st->saw_classpath = 1;
        st->pending_cp = 0;
        return;
    }
    if (strcmp(arg, "-cp") == 0 || strcmp(arg, "-classpath") == 0 ||
        strcmp(arg, "--class-path") == 0) {
        st->pending_cp = 1;
        return;
    }
    if (strncmp(arg, "-Djava.library.path=", 20) == 0) {
        st->saw_lib_path = 1;
    }
    if (strncmp(arg, "-Dorg.lwjgl.librarypath=", 25) == 0) {
        st->saw_lwjgl_path = 1;
    }
}

static int push_expanded(arg_vec *av, expand_state *st, const char *text, const resolved *res)
{
    char *value = expand_dup(text, res);
    if (!value) {
        return -1;
    }
    note_arg(st, value);
    return av_take(av, value);
}

static int expand_json_args(arg_vec *av, const sxcl_json_value *array, const resolved *res,
                            const char *os_name, const char *arch_name, expand_state *st)
{
    const size_t n = sxcl_json_size(array);
    size_t i = 0;
    for (i = 0; i < n; ++i) {
        const sxcl_json_value *item = sxcl_json_at(array, i);
        const sxcl_json_type type = item ? sxcl_json_type_of(item) : SXCL_JSON_NULL;
        if (type == SXCL_JSON_STRING) {
            if (push_expanded(av, st, sxcl_json_string(item), res) != 0) {
                return -1;
            }
        } else if (type == SXCL_JSON_OBJECT) {
            const sxcl_json_value *value = NULL;
            if (!sxcl_rules_allow(sxcl_json_get(item, "rules"), os_name, arch_name)) {
                continue;
            }
            value = sxcl_json_get(item, "value");
            if (!value) {
                continue;
            }
            if (sxcl_json_type_of(value) == SXCL_JSON_STRING) {
                if (push_expanded(av, st, sxcl_json_string(value), res) != 0) {
                    return -1;
                }
            } else if (sxcl_json_type_of(value) == SXCL_JSON_ARRAY) {
                const size_t m = sxcl_json_size(value);
                size_t j = 0;
                for (j = 0; j < m; ++j) {
                    const sxcl_json_value *sub = sxcl_json_at(value, j);
                    if (sub && sxcl_json_type_of(sub) == SXCL_JSON_STRING) {
                        if (push_expanded(av, st, sxcl_json_string(sub), res) != 0) {
                            return -1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/** 老格式 minecraftArguments:按空白切分。切完再展开,所以值里带空格(比如中文目录)不会被拆坏。 */
static int expand_legacy_args(arg_vec *av, const char *text, const resolved *res, expand_state *st)
{
    const char *p = text ? text : "";
    while (*p) {
        const char *end = NULL;
        char token[4096];
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            ++p;
        }
        if (!*p) {
            break;
        }
        end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
            ++end;
        }
        copy_n(token, sizeof(token), p, (size_t)(end - p));
        if (push_expanded(av, st, token, res) != 0) {
            return -1;
        }
        p = end;
    }
    return 0;
}

/* ────────────────────────── 主入口 ────────────────────────── */

static void free_vec(arg_vec *av)
{
    size_t i = 0;
    for (i = 0; i < av->count; ++i) {
        free(av->items[i]);
    }
    free(av->items);
    av->items = NULL;
    av->count = 0;
    av->cap = 0;
}

sxcl_launch_args *sxcl_launch_build_args(const sxcl_json *version_json,
                                         const sxcl_launch_ctx *ctx,
                                         char *err, size_t err_len)
{
    const sxcl_json_value *root = sxcl_json_root(version_json);
    const sxcl_json_value *arguments = NULL;
    const sxcl_json_value *jvm_array = NULL;
    const sxcl_json_value *game_array = NULL;
    const char *legacy = NULL;
    const char *main_class = NULL;
    const char *os_name = NULL;
    const char *arch_name = NULL;
    sxcl_launch_args *args = NULL;
    resolved res;
    arg_vec out;
    arg_vec jvm_json;
    expand_state probe;
    const int is64 = (ctx && ctx->is_64bit == 0) ? 0 : ((ctx && ctx->is_64bit == 1) ? 1 : -1);

    if (err && err_len) {
        err[0] = '\0';
    }
    if (!ctx) {
        set_err(err, err_len, "缺少启动上下文");
        return NULL;
    }
    if (!ctx->game_directory || !*ctx->game_directory) {
        set_err(err, err_len, "缺少 game_directory(游戏的运行目录)");
        return NULL;
    }

    memset(&res, 0, sizeof(res));
    memset(&out, 0, sizeof(out));
    memset(&jvm_json, 0, sizeof(jvm_json));
    memset(&probe, 0, sizeof(probe));

    resolve_context(root, ctx, &res);
    os_name = rules_os_name(ctx, eff_os(ctx));
    arch_name = (ctx->arch_name && *ctx->arch_name) ? ctx->arch_name : sxcl_platform_arch_name();

    if (build_classpath(root, ctx, os_name, arch_name, &res) != 0) {
        set_err(err, err_len, "内存不足(classpath 拼装失败)");
        goto fail;
    }

    arguments = sxcl_json_get(root, "arguments");
    jvm_array = sxcl_json_get(arguments, "jvm");
    game_array = sxcl_json_get(arguments, "game");
    legacy = sxcl_json_get_string(root, "minecraftArguments", NULL);
    main_class = sxcl_json_get_string(root, "mainClass", "net.minecraft.client.main.Main");

    /* 1) 先把版本 JSON 自带的 jvm 参数展开到临时表,顺便探测它有没有自带 -cp / 库路径 */
    if (jvm_array && expand_json_args(&jvm_json, jvm_array, &res, os_name, arch_name, &probe) != 0) {
        set_err(err, err_len, "内存不足(展开 arguments.jvm 失败)");
        goto fail;
    }

    /* 2) 默认 JVM 参数块(调用方给了 jvm_args 就整块跳过) */
    if (ctx->jvm_args) {
        const char *const *p = NULL;
        for (p = ctx->jvm_args; *p; ++p) {
            if (push_expanded(&out, &probe, *p, &res) != 0) {
                set_err(err, err_len, "内存不足");
                goto fail;
            }
        }
    } else if (emit_default_jvm(&out, ctx, &res, is64, probe.saw_lib_path,
                                probe.saw_lwjgl_path) != 0) {
        set_err(err, err_len, "内存不足(默认 JVM 参数)");
        goto fail;
    }

    /* 3) 版本 JSON 的 jvm 参数 */
    av_absorb(&out, &jvm_json);

    /* 4) classpath:版本 JSON 自己带过 -cp 就不补 */
    if (!probe.saw_classpath) {
        if (av_push(&out, "-cp") != 0 || av_push(&out, res.classpath) != 0) {
            set_err(err, err_len, "内存不足(classpath)");
            goto fail;
        }
    }

    /* 5) 调用方追加的 JVM 参数(放在主类前,所以 -Xmx 这类"后者生效"的选项能覆盖默认值) */
    if (ctx->extra_jvm_args) {
        const char *const *p = NULL;
        for (p = ctx->extra_jvm_args; *p; ++p) {
            if (push_expanded(&out, &probe, *p, &res) != 0) {
                set_err(err, err_len, "内存不足(extra_jvm_args)");
                goto fail;
            }
        }
    }

    /* 6) 主类 */
    if (push_expanded(&out, &probe, main_class, &res) != 0) {
        set_err(err, err_len, "内存不足(主类)");
        goto fail;
    }

    /* 7) 游戏参数:新格式优先,否则老格式 */
    if (game_array) {
        if (expand_json_args(&out, game_array, &res, os_name, arch_name, &probe) != 0) {
            set_err(err, err_len, "内存不足(展开 arguments.game 失败)");
            goto fail;
        }
    } else if (legacy && *legacy) {
        if (expand_legacy_args(&out, legacy, &res, &probe) != 0) {
            set_err(err, err_len, "内存不足(minecraftArguments)");
            goto fail;
        }
    }
    if (ctx->extra_game_args) {
        const char *const *p = NULL;
        for (p = ctx->extra_game_args; *p; ++p) {
            if (push_expanded(&out, &probe, *p, &res) != 0) {
                set_err(err, err_len, "内存不足(extra_game_args)");
                goto fail;
            }
        }
    }

    free(res.classpath);
    /* 句柄必须落在堆上:argv 的每一项本来就是 malloc 的,这里只是把数组头的所有权搬过去 */
    args = (sxcl_launch_args *)calloc(1, sizeof(*args));
    if (!args) {
        set_err(err, err_len, "内存不足(句柄)");
        free_vec(&out);
        return NULL;
    }
    *args = out; /* 结构体整体搬走;out 之后不再使用,也绝不能再 free_vec */
    return args;

fail:
    free(res.classpath);
    free_vec(&out);
    free_vec(&jvm_json);
    return NULL;
}

void sxcl_launch_args_free(sxcl_launch_args *args)
{
    if (!args) {
        return;
    }
    free_vec(args);
    free(args);
}

size_t sxcl_launch_arg_count(const sxcl_launch_args *args)
{
    return args ? args->count : 0;
}

const char *sxcl_launch_arg_at(const sxcl_launch_args *args, size_t index)
{
    if (!args || index >= args->count) {
        return NULL;
    }
    return args->items[index];
}

const char *const *sxcl_launch_argv(const sxcl_launch_args *args)
{
    return args ? (const char *const *)args->items : NULL;
}

size_t sxcl_launch_expand(const char *text, const sxcl_launch_ctx *ctx, char *out, size_t out_len)
{
    sxcl_launch_ctx fallback;
    resolved res;
    char *value = NULL;
    size_t written = 0;
    if (!out || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!ctx) {
        memset(&fallback, 0, sizeof(fallback));
        fallback.game_directory = ".";
        ctx = &fallback;
    }
    if (!ctx->game_directory || !*ctx->game_directory) {
        return 0;
    }
    resolve_context(NULL, ctx, &res);
    value = expand_dup(text, &res);
    free(res.classpath);
    if (!value) {
        return 0;
    }
    written = strlen(value);
    if (written >= out_len) {
        written = out_len - 1;
    }
    memcpy(out, value, written);
    out[written] = '\0';
    free(value);
    return written;
}
