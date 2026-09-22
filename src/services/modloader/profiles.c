/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include "sxcl/loader.h"

#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* 仅为了把 UTF-8 路径转宽字符:fopen 在 Windows 上按 ANSI 代码页解释路径 */
#endif

#define PROFILE_DEFAULT_KEY  "SXCL"
#define PROFILE_DEFAULT_NAME "Silent X Craft Launcher"
#define PROFILE_DEFAULT_LAST_VERSION "latest-release"   /* Python 版那条固定值 */
#define PROFILE_DEFAULT_TOKEN "23323323323323323323323323323333"   /* Python 版原样搬过来的 */

/* ── UTF-8 路径的文件 IO(与 src/core/settings.c 同一套做法) ── */

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
    FILE *fh = NULL;
    if (_wfopen_s(&fh, wide, for_write ? L"wb" : L"rb") != 0) {
        fh = NULL;
    }
    free(wide);
    return fh;
#else
    return fopen(path, for_write ? "wb" : "rb");
#endif
}

/* ── 动态文本缓冲(整份 JSON 文本) ── */

typedef struct dbuf {
    char *data;
    size_t len;
    size_t cap;
    int bad;      /* 1 = 分配失败,后面全部操作变成空转 */
} dbuf;

static void db_init(dbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->bad = 0;
}

static void db_free(dbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void db_putn(dbuf *b, const char *text, size_t n)
{
    if (b->bad || n == 0) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t next = b->cap ? b->cap : 256;
        while (next < b->len + n + 1) {
            next *= 2;
        }
        char *grown = (char *)realloc(b->data, next);
        if (!grown) {
            b->bad = 1;
            return;
        }
        b->data = grown;
        b->cap = next;
    }
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void db_put(dbuf *b, const char *text)
{
    db_putn(b, text ? text : "", text ? strlen(text) : 0);
}

/* ── JSON 输出 ── */

static void dump_indent(dbuf *b, int depth)
{
    for (int i = 0; i < depth * 4; ++i) {
        db_putn(b, " ", 1);
    }
}

static void dump_string(dbuf *b, const char *text)
{
    db_putn(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        char tmp[8];
        switch (*p) {
        case '"':  db_put(b, "\\\""); break;
        case '\\': db_put(b, "\\\\"); break;
        case '\n': db_put(b, "\\n"); break;
        case '\r': db_put(b, "\\r"); break;
        case '\t': db_put(b, "\\t"); break;
        case '\b': db_put(b, "\\b"); break;
        case '\f': db_put(b, "\\f"); break;
        default:
            if (*p < 0x20) {
                (void)snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
                db_put(b, tmp);
            } else {
                db_putn(b, (const char *)p, 1);   /* UTF-8 字节原样透传 */
            }
            break;
        }
    }
    db_putn(b, "\"", 1);
}

static void dump_number(dbuf *b, double value)
{
    char tmp[48];
    if (value == (double)(long long)value && value > -1.0e15 && value < 1.0e15) {
        (void)snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    } else {
        (void)snprintf(tmp, sizeof(tmp), "%.17g", value);
    }
    db_put(b, tmp);
}

/* 序列化状态。布尔值走 json.h 的节点级接口 sxcl_json_bool —— 对象成员、数组元素、
 * 根位置的布尔都能正确读写,不用再"拒绝改写"或者靠父对象+键名兜圈子。 */
typedef struct dump_state {
    dbuf *out;
} dump_state;

/* 把 DOM 里的一个值原样写出来(不认识的东西照样按原结构写回)。 */
static void dump_value(dump_state *st, const sxcl_json_value *value, int depth);

static void dump_object(dump_state *st, const sxcl_json_value *value, int depth)
{
    dbuf *b = st->out;
    const size_t count = sxcl_json_member_count(value);
    if (count == 0) {
        db_put(b, "{}");
        return;
    }
    db_put(b, "{");
    for (size_t i = 0; i < count; ++i) {
        const char *key = sxcl_json_member_key(value, i);
        if (i > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_string(b, key);
        db_put(b, ": ");
        dump_value(st, sxcl_json_member_value(value, i), depth + 1);
    }
    db_putn(b, "\n", 1);
    dump_indent(b, depth);
    db_putn(b, "}", 1);
}

static void dump_array(dump_state *st, const sxcl_json_value *value, int depth)
{
    dbuf *b = st->out;
    const size_t count = sxcl_json_size(value);
    if (count == 0) {
        db_put(b, "[]");
        return;
    }
    db_putn(b, "[", 1);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_value(st, sxcl_json_at(value, i), depth + 1);
    }
    db_putn(b, "\n", 1);
    dump_indent(b, depth);
    db_putn(b, "]", 1);
}

static void dump_value(dump_state *st, const sxcl_json_value *value, int depth)
{
    dbuf *b = st->out;
    if (!value) {
        db_put(b, "null");
        return;
    }
    switch (sxcl_json_type_of(value)) {
    case SXCL_JSON_NULL:   db_put(b, "null"); break;
    case SXCL_JSON_BOOL:   db_put(b, sxcl_json_bool(value) ? "true" : "false"); break;
    case SXCL_JSON_NUMBER: dump_number(b, sxcl_json_number(value)); break;
    case SXCL_JSON_STRING: dump_string(b, sxcl_json_string(value)); break;
    case SXCL_JSON_ARRAY:  dump_array(st, value, depth); break;
    case SXCL_JSON_OBJECT: dump_object(st, value, depth); break;
    default:               db_put(b, "null"); break;
    }
}

/* ── 顶层合并上下文 ── */

typedef struct merge_ctx {
    const sxcl_loader_json_override *overrides;
    size_t override_count;
    int override_seen[SXCL_LOADER_MAX_LIBRARIES > 16 ? 16 : 16];
    /* profiles 合并(profile_key 非空时启用) */
    const char *profile_key;
    const char *profile_name;
    const char *profile_last_version;   /* 档案指向的版本 id(默认 "latest-release") */
    const char *profile_last_used;
    int had_profiles;
    int had_selected;
    int had_token;
    int profile_exists;   /* 原文件里已经有同名档案 */
    int inserted;         /* 这次写入了我们自己的档案条目 */
} merge_ctx;

static const char *find_override(const merge_ctx *ctx, const char *key, size_t *index)
{
    if (!ctx->overrides) {
        return NULL;
    }
    for (size_t i = 0; i < ctx->override_count; ++i) {
        if (ctx->overrides[i].key && strcmp(ctx->overrides[i].key, key) == 0) {
            if (index) {
                *index = i;
            }
            return ctx->overrides[i].value ? ctx->overrides[i].value : "";
        }
    }
    return NULL;
}

/* 我们自己的档案条目(Python 版 merge_launcher_profile 里那份)。 */
static void dump_own_profile(dbuf *b, const merge_ctx *ctx, int depth)
{
    db_put(b, "{");
    db_putn(b, "\n", 1);
    dump_indent(b, depth + 1);
    db_put(b, "\"icon\": \"Grass\",");
    db_putn(b, "\n", 1);
    dump_indent(b, depth + 1);
    db_put(b, "\"name\": ");
    dump_string(b, ctx->profile_name);
    db_put(b, ",");
    db_putn(b, "\n", 1);
    dump_indent(b, depth + 1);
    db_put(b, "\"lastVersionId\": ");
    dump_string(b, ctx->profile_last_version);
    db_put(b, ",");
    db_putn(b, "\n", 1);
    dump_indent(b, depth + 1);
    db_put(b, "\"type\": \"latest-release\",");
    db_putn(b, "\n", 1);
    dump_indent(b, depth + 1);
    db_put(b, "\"lastUsed\": ");
    dump_string(b, ctx->profile_last_used);
    db_putn(b, "\n", 1);
    dump_indent(b, depth);
    db_putn(b, "}", 1);
}

/* profiles 对象:原有档案逐条原样写回,我们那条只有不存在时才插进去。 */
static void dump_profiles(dump_state *st, const sxcl_json_value *value, int depth, merge_ctx *ctx)
{
    dbuf *b = st->out;
    const int is_object = value && sxcl_json_type_of(value) == SXCL_JSON_OBJECT;
    const size_t count = is_object ? sxcl_json_member_count(value) : 0;
    db_putn(b, "{", 1);
    int written = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *key = sxcl_json_member_key(value, i);
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_string(b, key);
        db_put(b, ": ");
        if (key && strcmp(key, ctx->profile_key) == 0) {
            ctx->profile_exists = 1;   /* Python: 已经有了就不动它 */
        }
        dump_value(st, sxcl_json_member_value(value, i), depth + 1);
        ++written;
    }
    if (!ctx->profile_exists) {
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_string(b, ctx->profile_key);
        db_put(b, ": ");
        dump_own_profile(b, ctx, depth + 1);
        ctx->inserted = 1;
        ++written;
    }
    if (written > 0) {
        db_putn(b, "\n", 1);
        dump_indent(b, depth);
    }
    db_putn(b, "}", 1);
}

/* 顶层对象:逐条写回,按需覆盖/追加 profiles 与我们缺的键。 */
static void dump_root(dump_state *st, const sxcl_json_value *root, merge_ctx *ctx)
{
    dbuf *b = st->out;
    const int is_object = root && sxcl_json_type_of(root) == SXCL_JSON_OBJECT;
    if (root && !is_object && !ctx->profile_key) {
        /* 纯序列化:根是什么就写什么(数组/标量原样输出)。 */
        dump_value(st, root, 0);
        return;
    }
    /* 根不是对象又要合并 profiles:原文件根本不是合法档案,只能按全新文件写一份
     * (Python 在这种输入上会抛异常并放弃合并)。 */
    const size_t count = is_object ? sxcl_json_member_count(root) : 0;
    db_putn(b, "{", 1);
    int written = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *key = sxcl_json_member_key(root, i);
        size_t oi = 0;
        const char *replacement = find_override(ctx, key, &oi);
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, 1);
        dump_string(b, key);
        db_put(b, ": ");
        if (replacement) {
            dump_string(b, replacement);
            if (oi < sizeof(ctx->override_seen) / sizeof(ctx->override_seen[0])) {
                ctx->override_seen[oi] = 1;
            }
        } else if (ctx->profile_key && key && strcmp(key, "profiles") == 0) {
            ctx->had_profiles = 1;
            dump_profiles(st, sxcl_json_member_value(root, i), 1, ctx);
        } else {
            dump_value(st, sxcl_json_member_value(root, i), 1);
        }
        if (ctx->profile_key && key) {
            if (strcmp(key, "selectedProfile") == 0) {
                ctx->had_selected = 1;
            } else if (strcmp(key, "clientToken") == 0) {
                ctx->had_token = 1;
            }
        }
        ++written;
    }

    /* 覆盖表里有、原文件里没有的键:追加到末尾。 */
    if (ctx->overrides) {
        for (size_t i = 0; i < ctx->override_count; ++i) {
            if (ctx->overrides[i].key && i < sizeof(ctx->override_seen) / sizeof(ctx->override_seen[0]) &&
                !ctx->override_seen[i]) {
                if (written > 0) {
                    db_putn(b, ",", 1);
                }
                db_putn(b, "\n", 1);
                dump_indent(b, 1);
                dump_string(b, ctx->overrides[i].key);
                db_put(b, ": ");
                dump_string(b, ctx->overrides[i].value ? ctx->overrides[i].value : "");
                ++written;
            }
        }
    }

    /* launcher_profiles.json:缺 profiles / selectedProfile / clientToken 时补齐。 */
    if (ctx->profile_key) {
        if (!ctx->had_profiles) {
            if (written > 0) {
                db_putn(b, ",", 1);
            }
            db_putn(b, "\n", 1);
            dump_indent(b, 1);
            db_put(b, "\"profiles\": ");
            dump_profiles(st, NULL, 1, ctx);
            ++written;
        }
        if (!ctx->had_selected) {
            db_putn(b, ",", 1);
            db_putn(b, "\n", 1);
            dump_indent(b, 1);
            db_put(b, "\"selectedProfile\": ");
            dump_string(b, ctx->profile_key);
        }
        if (!ctx->had_token) {
            db_putn(b, ",", 1);
            db_putn(b, "\n", 1);
            dump_indent(b, 1);
            db_put(b, "\"clientToken\": \"");
            db_put(b, PROFILE_DEFAULT_TOKEN);
            db_putn(b, "\"", 1);
        }
    }

    if (written > 0) {
        db_putn(b, "\n", 1);
    }
    db_putn(b, "}", 1);
}

/* ── 对外接口 ── */

int sxcl_loader_json_dump(const sxcl_json_value *root, const sxcl_loader_json_override *overrides,
                          size_t override_count, char **out_text, char *err, size_t err_len)
{
    if (!out_text) {
        return SXCL_LOADER_ERR_ARG;
    }
    *out_text = NULL;
    merge_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.overrides = overrides;
    ctx.override_count = override_count;

    dbuf b;
    db_init(&b);
    dump_state st;
    st.out = &b;
    dump_root(&st, root, &ctx);
    if (b.bad) {
        db_free(&b);
        if (err && err_len) {
            (void)snprintf(err, err_len, "内存不足，写不出 JSON 文本");
        }
        return SXCL_LOADER_ERR_NOMEM;
    }
    *out_text = b.data ? b.data : (char *)calloc(1, 1);
    if (!*out_text) {
        db_free(&b);
        return SXCL_LOADER_ERR_NOMEM;
    }
    return SXCL_LOADER_OK;
}

/* ── 拍平(flatten):产出一份"能独立启动"的单层版本 JSON ──
 *
 * 依据(用户点名)：「PCL 与 HMCL 装出来的都是能独立启动的版本 JSON,我们肯定要学」。
 * PCL 的形态:id == 文件夹名、**没有 inheritsFrom**、没有 jar、多一个 clientVersion
 * (= 原版版本号,实例扫描最可靠的身份标记);HMCL 同样把原版的库并进同一份 JSON。
 *
 * 我们过去只写 inheritsFrom —— 而**启动层从来不解析继承**(src/services/launch 里没有任何合并):
 * 一份"只带加载器库 + inheritsFrom"的版本 JSON 启动时会缺原版库、assetIndex 退化成 legacy。
 * 这里把"合并"补上:安装时由 sxcl_loader_flatten_json 落到盘上,驱动层再用同一条规则兜底。
 *
 * 规则(逐键,与 PCL 的拍平对齐)见 include/sxcl/loader.h 的声明。
 */

typedef struct flatten_ctx {
    const sxcl_json_value *loader;   /* 加载器(子)版本 JSON 的根;NULL = 没有子版本 */
    const char *instance_name;       /* 新 id(== 目录名) */
    const char *base_version;        /* 原版版本号 -> clientVersion;空 = 保持原版自己的值 */
} flatten_ctx;

/* 由合并规则单独接管的键:不再走"加载器独有的键原样补写"那一遍。 */
static int flatten_rule_key(const char *key)
{
    static const char *const keys[] = {
        "id", "clientVersion", "inheritsFrom", "jar",
        "libraries", "arguments", "mainClass", "minecraftArguments",
    };
    if (!key || !key[0]) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strcmp(key, keys[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 类型不对一律当"没有"(宁可少合并,也不要照着坏数据拼出一份假版本 JSON)。 */
static const sxcl_json_value *flatten_as_array(const sxcl_json_value *value)
{
    return (value && sxcl_json_type_of(value) == SXCL_JSON_ARRAY) ? value : NULL;
}

static const sxcl_json_value *flatten_as_object(const sxcl_json_value *value)
{
    return (value && sxcl_json_type_of(value) == SXCL_JSON_OBJECT) ? value : NULL;
}

/* 库的"身份" = name 的前两段(group:artifact);读不到 name 就留空(不去重)。 */
static void flatten_lib_identity(const sxcl_json_value *lib, char *out, size_t cap)
{
    out[0] = '\0';
    const char *name = sxcl_json_get_string(lib, "name", NULL);
    if (!name || !name[0]) {
        return;
    }
    const char *first = strchr(name, ':');
    if (!first) {
        (void)snprintf(out, cap, "%s", name);
        return;
    }
    const char *second = strchr(first + 1, ':');
    const size_t len = second ? (size_t)(second - name) : strlen(name);
    if (len + 1 > cap) {
        return;
    }
    memcpy(out, name, len);
    out[len] = '\0';
}

static int flatten_libs_have(const sxcl_json_value *libs, const char *identity)
{
    const sxcl_json_value *array = flatten_as_array(libs);
    if (!array || !identity || !identity[0]) {
        return 0;
    }
    const size_t count = sxcl_json_size(array);
    for (size_t i = 0; i < count; ++i) {
        char other[256];
        flatten_lib_identity(sxcl_json_at(array, i), other, sizeof(other));
        if (other[0] && strcmp(other, identity) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 把一个节点单独序列化成文本 —— 给 arguments 的项做"整串相等"去重用。 */
static char *flatten_node_text(const sxcl_json_value *value)
{
    dbuf tmp;
    db_init(&tmp);
    dump_state st;
    st.out = &tmp;
    dump_value(&st, value, 0);
    if (tmp.bad) {
        db_free(&tmp);
        return NULL;
    }
    return tmp.data ? tmp.data : NULL;
}

/* libraries:加载器的在前,原版里没被同名覆盖的接着写。
 * 顺序有讲究:Java 的 classpath 先到先得,所以同一个 group:artifact 必须由**加载器钉住的版本**
 * 说了算(Forge 会钉自己那套 log4j/asm,原版的同名库不能挡在前面)。 */
static void flatten_dump_libraries(dump_state *st, const sxcl_json_value *base_libs,
                                   const sxcl_json_value *loader_libs, int depth)
{
    dbuf *b = st->out;
    const sxcl_json_value *base_array = flatten_as_array(base_libs);
    const sxcl_json_value *loader_array = flatten_as_array(loader_libs);
    const size_t base_count = base_array ? sxcl_json_size(base_array) : 0;
    const size_t loader_count = loader_array ? sxcl_json_size(loader_array) : 0;
    size_t written = 0;

    db_putn(b, "[", 1);
    for (size_t i = 0; i < loader_count; ++i) {
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_value(st, sxcl_json_at(loader_array, i), depth + 1);
        ++written;
    }
    for (size_t i = 0; i < base_count; ++i) {
        const sxcl_json_value *lib = sxcl_json_at(base_array, i);
        char identity[256];
        flatten_lib_identity(lib, identity, sizeof(identity));
        if (flatten_libs_have(loader_array, identity)) {
            continue;
        }
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_value(st, lib, depth + 1);
        ++written;
    }
    if (written > 0) {
        db_putn(b, "\n", 1);
        dump_indent(b, depth);
    }
    db_putn(b, "]", 1);
}

/* 参数数组:原版在前、加载器在后,整串相同的项只留一次。
 * (两边都可能写 -Djava.library.path / --add-opens;重复大多无害,但会让命令行白长一截。) */
static void flatten_dump_args(dump_state *st, const sxcl_json_value *base_args,
                              const sxcl_json_value *loader_args, int depth)
{
    dbuf *b = st->out;
    char *seen[64];
    size_t seen_count = 0;
    const size_t seen_cap = sizeof(seen) / sizeof(seen[0]);
    size_t written = 0;

    db_putn(b, "[", 1);
    for (int pass = 0; pass < 2; ++pass) {
        const sxcl_json_value *array = flatten_as_array(pass == 0 ? base_args : loader_args);
        const size_t count = array ? sxcl_json_size(array) : 0;
        for (size_t i = 0; i < count; ++i) {
            const sxcl_json_value *item = sxcl_json_at(array, i);
            char *text = flatten_node_text(item);
            int dup = 0;
            int stored = 0;
            if (text && seen_count < seen_cap) {
                for (size_t k = 0; k < seen_count; ++k) {
                    if (seen[k] && strcmp(seen[k], text) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    seen[seen_count++] = text;   /* 记下来给后面几项比;统一在函数末尾 free */
                    stored = 1;
                }
            }
            if (dup) {
                free(text);
                continue;
            }
            if (written > 0) {
                db_putn(b, ",", 1);
            }
            db_putn(b, "\n", 1);
            dump_indent(b, depth + 1);
            if (text) {
                db_put(b, text);
                if (!stored) {
                    free(text);   /* 表满了没记下来:写完就还 */
                }
            } else {
                dump_value(st, item, depth + 1);
            }
            ++written;
        }
    }
    for (size_t k = 0; k < seen_count; ++k) {
        free(seen[k]);
    }
    if (written > 0) {
        db_putn(b, "\n", 1);
        dump_indent(b, depth);
    }
    db_putn(b, "]", 1);
}

/* 能不能合并:两边至少有一边是带 jvm/game 的对象。 */
static int flatten_args_mergeable(const sxcl_json_value *value)
{
    const sxcl_json_value *object = flatten_as_object(value);
    return object && (sxcl_json_get(object, "jvm") || sxcl_json_get(object, "game"));
}

static void flatten_dump_arguments(dump_state *st, const sxcl_json_value *base_args,
                                   const sxcl_json_value *loader_args, int depth)
{
    dbuf *b = st->out;
    const sxcl_json_value *base_obj = flatten_as_object(base_args);
    const sxcl_json_value *loader_obj = flatten_as_object(loader_args);
    const sxcl_json_value *base_jvm = base_obj ? sxcl_json_get(base_obj, "jvm") : NULL;
    const sxcl_json_value *base_game = base_obj ? sxcl_json_get(base_obj, "game") : NULL;
    const sxcl_json_value *loader_jvm = loader_obj ? sxcl_json_get(loader_obj, "jvm") : NULL;
    const sxcl_json_value *loader_game = loader_obj ? sxcl_json_get(loader_obj, "game") : NULL;
    int written = 0;

    db_putn(b, "{", 1);
    if (base_jvm || loader_jvm) {
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_string(b, "jvm");
        db_put(b, ": ");
        flatten_dump_args(st, base_jvm, loader_jvm, depth + 1);
        ++written;
    }
    if (base_game || loader_game) {
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, depth + 1);
        dump_string(b, "game");
        db_put(b, ": ");
        flatten_dump_args(st, base_game, loader_game, depth + 1);
        ++written;
    }
    if (written > 0) {
        db_putn(b, "\n", 1);
        dump_indent(b, depth);
    }
    db_putn(b, "}", 1);
}

/* 拍平的根:以原版为骨架逐键写,再把加载器独有的键补在后面。 */
static void flatten_dump_root(dump_state *st, const sxcl_json_value *base, const flatten_ctx *ctx)
{
    dbuf *b = st->out;
    const sxcl_json_value *base_obj = flatten_as_object(base);
    const sxcl_json_value *loader = ctx->loader;
    const size_t count = base_obj ? sxcl_json_member_count(base_obj) : 0;
    int written = 0;
    int saw_id = 0;
    int saw_client_version = 0;

    db_putn(b, "{", 1);
    for (size_t i = 0; i < count; ++i) {
        const char *key = sxcl_json_member_key(base_obj, i);
        const sxcl_json_value *value = sxcl_json_member_value(base_obj, i);
        /* EMIT_VALUE = 原样写原版的;EMIT_TEXT = 写我们自己定的字符串;其余是特殊合并。 */
        enum { EMIT_VALUE = 0, EMIT_TEXT, EMIT_LIBS, EMIT_ARGS, EMIT_SKIP } mode = EMIT_VALUE;
        const char *text = NULL;

        if (!key) {
            continue;
        }
        if (strcmp(key, "id") == 0) {
            mode = EMIT_TEXT;
            text = ctx->instance_name ? ctx->instance_name : "";
            saw_id = 1;
        } else if (strcmp(key, "clientVersion") == 0) {
            saw_client_version = 1;
            if (ctx->base_version) {
                mode = EMIT_TEXT;
                text = ctx->base_version;
            }
        } else if (strcmp(key, "inheritsFrom") == 0 || strcmp(key, "jar") == 0) {
            mode = EMIT_SKIP;   /* 拍平:这两个键正是"不是独立版本"的标记 */
        } else if (strcmp(key, "libraries") == 0) {
            mode = EMIT_LIBS;
        } else if (strcmp(key, "arguments") == 0) {
            mode = EMIT_ARGS;
        } else if (strcmp(key, "mainClass") == 0 || strcmp(key, "minecraftArguments") == 0) {
            const char *own = loader ? sxcl_json_get_string(loader, key, NULL) : NULL;
            if (own && own[0]) {
                mode = EMIT_TEXT;   /* 加载器的说了算(主类/老式游戏参数都是它改的) */
                text = own;
            }
        }
        if (mode == EMIT_SKIP) {
            continue;
        }

        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, 1);
        dump_string(b, key);
        db_put(b, ": ");
        if (mode == EMIT_TEXT) {
            dump_string(b, text ? text : "");
        } else if (mode == EMIT_LIBS) {
            flatten_dump_libraries(st, value, loader ? sxcl_json_get(loader, "libraries") : NULL, 1);
        } else if (mode == EMIT_ARGS) {
            const sxcl_json_value *own = loader ? sxcl_json_get(loader, "arguments") : NULL;
            if (flatten_args_mergeable(value) || flatten_args_mergeable(own)) {
                flatten_dump_arguments(st, value, own, 1);
            } else {
                dump_value(st, value, 1);   /* 不是对象:原样写回,不丢信息 */
            }
        } else {
            dump_value(st, value, 1);
        }
        ++written;
    }

    /* 加载器独有的键:原版没有、也不由上面的规则接管 —— 原样带上。
     * (processors / spec / data / … 启动用不到,但"原样保留"比"猜着丢掉"安全。) */
    if (loader && sxcl_json_type_of(loader) == SXCL_JSON_OBJECT) {
        const size_t loader_count = sxcl_json_member_count(loader);
        for (size_t i = 0; i < loader_count; ++i) {
            const char *key = sxcl_json_member_key(loader, i);
            if (!key || flatten_rule_key(key)) {
                continue;
            }
            if (base_obj && sxcl_json_get(base_obj, key)) {
                continue;   /* 原版有的以原版为准(assetIndex/assets/downloads/…) */
            }
            if (written > 0) {
                db_putn(b, ",", 1);
            }
            db_putn(b, "\n", 1);
            dump_indent(b, 1);
            dump_string(b, key);
            db_put(b, ": ");
            dump_value(st, sxcl_json_member_value(loader, i), 1);
            ++written;
        }
    }

    /* id / clientVersion 兜底:原版 JSON 本来就有,这里只是把语义钉死。 */
    if (!saw_id) {
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, 1);
        dump_string(b, "id");
        db_put(b, ": ");
        dump_string(b, ctx->instance_name ? ctx->instance_name : "");
        ++written;
    }
    if (!saw_client_version && ctx->base_version && ctx->base_version[0]) {
        if (written > 0) {
            db_putn(b, ",", 1);
        }
        db_putn(b, "\n", 1);
        dump_indent(b, 1);
        dump_string(b, "clientVersion");
        db_put(b, ": ");
        dump_string(b, ctx->base_version);
        ++written;
    }

    if (written > 0) {
        db_putn(b, "\n", 1);
    }
    db_putn(b, "}", 1);
}

int sxcl_loader_flatten_json(const sxcl_json_value *loader_root, const sxcl_json_value *base_root,
                             const char *instance_name, const char *base_version, char **out_text,
                             char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!out_text) {
        return SXCL_LOADER_ERR_ARG;
    }
    *out_text = NULL;
    if (!instance_name || !instance_name[0]) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "缺少实例名,拍不出独立版本 JSON");
        }
        return SXCL_LOADER_ERR_ARG;
    }
    if (!flatten_as_object(base_root)) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "没有可合并的原版版本 JSON(拍平需要它)");
        }
        return SXCL_LOADER_ERR_ARG;
    }

    flatten_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loader = flatten_as_object(loader_root);
    ctx.instance_name = instance_name;
    ctx.base_version = (base_version && base_version[0]) ? base_version : NULL;

    dbuf b;
    db_init(&b);
    dump_state st;
    st.out = &b;
    flatten_dump_root(&st, base_root, &ctx);
    if (b.bad) {
        db_free(&b);
        if (err && err_len) {
            (void)snprintf(err, err_len, "内存不足，写不出拍平后的版本 JSON");
        }
        return SXCL_LOADER_ERR_NOMEM;
    }
    *out_text = b.data ? b.data : (char *)calloc(1, 1);
    if (!*out_text) {
        db_free(&b);
        return SXCL_LOADER_ERR_NOMEM;
    }
    return SXCL_LOADER_OK;
}

/* lastUsed 默认值:Python 用 datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.0000Z")。 */
static void default_last_used(char *out, size_t cap)
{
    const time_t now = time(NULL);
    struct tm parts;
    memset(&parts, 0, sizeof(parts));
#if defined(_WIN32)
    if (gmtime_s(&parts, &now) != 0) {
        memset(&parts, 0, sizeof(parts));
    }
#else
    if (gmtime_r(&now, &parts) == NULL) {
        memset(&parts, 0, sizeof(parts));
    }
#endif
    (void)snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.0000Z", parts.tm_year + 1900,
                   parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}

int sxcl_loader_merge_profiles_text(const char *existing_json, const char *key, const char *name,
                                    const char *last_version_id, const char *last_used, char **out_text,
                                    int *changed, char *err, size_t err_len)
{
    if (!out_text) {
        return SXCL_LOADER_ERR_ARG;
    }
    *out_text = NULL;
    if (changed) {
        *changed = 0;
    }

    const char *use_key = (key && key[0]) ? key : PROFILE_DEFAULT_KEY;
    const char *use_name = (name && name[0]) ? name : PROFILE_DEFAULT_NAME;
    char stamp[48];
    if (last_used && last_used[0]) {
        (void)snprintf(stamp, sizeof(stamp), "%s", last_used);
    } else {
        default_last_used(stamp, sizeof(stamp));
    }

    sxcl_json *doc = NULL;
    if (existing_json && existing_json[0]) {
        char perr[160];
        perr[0] = '\0';
        doc = sxcl_json_parse(existing_json, strlen(existing_json), perr, sizeof(perr));
        if (!doc) {
            if (err && err_len) {
                (void)snprintf(err, err_len, "launcher_profiles.json 解析失败（%s），已保留原文件不覆盖",
                               perr[0] ? perr : "格式不对");
            }
            return SXCL_LOADER_ERR_FORMAT;
        }
    }

    merge_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.profile_key = use_key;
    ctx.profile_name = use_name;
    ctx.profile_last_used = stamp;
    ctx.profile_last_version = (last_version_id && last_version_id[0]) ? last_version_id
                                                                      : PROFILE_DEFAULT_LAST_VERSION;

    dbuf b;
    db_init(&b);
    dump_state st;
    st.out = &b;
    dump_root(&st, doc ? sxcl_json_root(doc) : NULL, &ctx);
    sxcl_json_free(doc);
    if (b.bad) {
        db_free(&b);
        if (err && err_len) {
            (void)snprintf(err, err_len, "内存不足，写不出 launcher_profiles.json");
        }
        return SXCL_LOADER_ERR_NOMEM;
    }
    *out_text = b.data;
    if (!*out_text) {
        *out_text = (char *)calloc(1, 1);
        if (!*out_text) {
            return SXCL_LOADER_ERR_NOMEM;
        }
    }
    if (changed) {
        /* 只有真的动了内容才算"需要写回":Python 的"同名档案已存在"分支连文件都不重写。 */
        *changed = (ctx.inserted || !ctx.had_profiles || !ctx.had_selected || !ctx.had_token ||
                    !existing_json || !existing_json[0]);
    }
    return SXCL_LOADER_OK;
}

/* 原子写:先写 <path>.tmp 再改名覆盖。失败时把 .tmp 删掉,不留半个文件。 */
static int write_atomic(const char *path, const char *text, char *err, size_t err_len)
{
    char tmp[SXCL_LOADER_CMD_ARG_MAX + 8];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(tmp)) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "路径太长，写不了临时文件");
        }
        return SXCL_LOADER_ERR_SPACE;
    }
    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "建不了目录：%s", path);
        }
        return SXCL_LOADER_ERR_IO;
    }
    FILE *fh = open_utf8(tmp, 1);
    if (!fh) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "打不开临时文件：%s", tmp);
        }
        return SXCL_LOADER_ERR_IO;
    }
    const size_t len = strlen(text);
    const int ok = (fwrite(text, 1, len, fh) == len);
    if (fclose(fh) != 0 || !ok) {
        (void)sxcl_fs_remove(tmp);
        if (err && err_len) {
            (void)snprintf(err, err_len, "写临时文件失败：%s", tmp);
        }
        return SXCL_LOADER_ERR_IO;
    }
    if (sxcl_fs_rename_replace(tmp, path) != 0) {
        (void)sxcl_fs_remove(tmp);
        if (err && err_len) {
            (void)snprintf(err, err_len, "改名失败：%s", path);
        }
        return SXCL_LOADER_ERR_IO;
    }
    return SXCL_LOADER_OK;
}

int sxcl_loader_ensure_launcher_profiles(const char *game_dir, const char *key, const char *name,
                                         const char *last_version_id, const char *last_used, int *changed,
                                         char *err, size_t err_len)
{
    if (changed) {
        *changed = 0;
    }
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!game_dir || !game_dir[0]) {
        return SXCL_LOADER_ERR_ARG;
    }

    char path[SXCL_LOADER_CMD_ARG_MAX];
    int written = snprintf(path, sizeof(path), "%s/launcher_profiles.json", game_dir);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "游戏目录路径太长");
        }
        return SXCL_LOADER_ERR_SPACE;
    }

    const int exists = sxcl_fs_exists(path);
    char *old_text = NULL;
    if (exists) {
        FILE *fh = open_utf8(path, 0);
        if (fh) {
            (void)fseek(fh, 0, SEEK_END);
            const long size = ftell(fh);
            (void)fseek(fh, 0, SEEK_SET);
            if (size > 0 && size < 8 * 1024 * 1024) {
                old_text = (char *)malloc((size_t)size + 1);
                if (old_text) {
                    const size_t got = fread(old_text, 1, (size_t)size, fh);
                    old_text[got] = '\0';
                }
            }
            (void)fclose(fh);
        }
    }

    char *merged = NULL;
    int need_write = 0;
    const int rc = sxcl_loader_merge_profiles_text(old_text, key, name, last_version_id, last_used,
                                                  &merged, &need_write, err, err_len);
    free(old_text);

    if (rc != SXCL_LOADER_OK) {
        /* 读不了原文件:文件确实在(或者写不出来),按"非致命"处理交给调用方决定,
         * 但绝不用一份新内容覆盖用户已有的档案。 */
        if (exists) {
            return SXCL_LOADER_OK;
        }
        if (err && err_len) {
            (void)snprintf(err, err_len, "launcher_profiles.json 不存在，而且建不出来");
        }
        return rc == SXCL_LOADER_ERR_ARG ? SXCL_LOADER_ERR_ARG : SXCL_LOADER_ERR_IO;
    }

    int result = SXCL_LOADER_OK;
    if (need_write) {
        result = write_atomic(path, merged, err, err_len);
        if (result == SXCL_LOADER_OK && changed) {
            *changed = 1;
        }
    }
    free(merged);
    return result;
}
