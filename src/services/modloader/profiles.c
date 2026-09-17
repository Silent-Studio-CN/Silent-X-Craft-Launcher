/* SXCL-C launcher_profiles.json 的读取 / 合并 / 原子写 —— 官方安装器的硬性前置。
 *
 * 为什么非要有它:Forge/NeoForge 的官方安装器靠 launcher_profiles.json 判定"这是个
 * 合法的游戏目录",缺了这个文件它直接报 "There is no minecraft launcher profile"
 * 然后什么都不装(Python 版注释里记的就是这条)。
 *
 * 为什么只合并不覆盖:这个文件同时是官方启动器/PCL 的档案库,登录后还会往里补
 * authenticationDatabase —— 整份覆盖会毁掉用户的档案与登录信息(Python:
 * "只能合并,绝不能整份覆盖")。所以本模块:
 *   * 用 sxcl/json.h 解析原文件;
 *   * 序列化时**把所有我们不认识的字段、所有原有档案原样写回**(自己写的 JSON 输出,
 *     不是"重建一份" —— 重建就等于覆盖);
 *   * 同名档案已经存在时保持原样(Python: "已经有了就不动它");
 *   * 写回是原子的:先写 <路径>.tmp 再改名,任何一步失败都不留半个文件。
 *
 * 与 Python 版**故意不同**的一处:原文件解析失败时,Python 会把数据当成空字典然后
 * 覆盖写(等于丢光用户档案);这里报 SXCL_LOADER_ERR_FORMAT 并原封不动保留文件。
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

/* 序列化状态。
 *
 * 关于布尔值(json.h 只有 sxcl_json_get_bool(对象, 键, 默认值),没有"取布尔节点本身"的接口):
 *   - 对象成员的布尔值:用"父对象 + 键名"查得出来,正常写 true/false;
 *   - 数组元素、根节点位置的布尔值:读不出值。这种情况**拒绝改写**(置 unsupported_bool),
 *     而不是瞎写一个 true 把用户的数据改坏 —— 调用方会拿到 SXCL_LOADER_ERR_FORMAT 并且
 *     原文件一动不动。launcher_profiles.json 里布尔只出现在对象成员位置,正常文件不受影响。 */
typedef struct dump_state {
    dbuf *out;
    int unsupported_bool;
} dump_state;

/* 把 DOM 里的一个值原样写出来(不认识的东西照样按原结构写回)。
 * owner/owner_key:该值作为对象成员时的父对象与键名(数组元素传 NULL)。 */
static void dump_value(dump_state *st, const sxcl_json_value *value, int depth,
                       const sxcl_json_value *owner, const char *owner_key);

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
        dump_value(st, sxcl_json_member_value(value, i), depth + 1, value, key);
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
        dump_value(st, sxcl_json_at(value, i), depth + 1, NULL, NULL);
    }
    db_putn(b, "\n", 1);
    dump_indent(b, depth);
    db_putn(b, "]", 1);
}

static void dump_value(dump_state *st, const sxcl_json_value *value, int depth,
                       const sxcl_json_value *owner, const char *owner_key)
{
    dbuf *b = st->out;
    if (!value) {
        db_put(b, "null");
        return;
    }
    switch (sxcl_json_type_of(value)) {
    case SXCL_JSON_NULL:   db_put(b, "null"); break;
    case SXCL_JSON_BOOL:
        if (owner && owner_key) {
            db_put(b, sxcl_json_get_bool(owner, owner_key, 0) ? "true" : "false");
        } else {
            st->unsupported_bool = 1;
            db_put(b, "false");
        }
        break;
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
    db_put(b, "\"lastVersionId\": \"latest-release\",");
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
        dump_value(st, sxcl_json_member_value(value, i), depth + 1, value, key);
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
        dump_value(st, root, 0, NULL, NULL);
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
            dump_value(st, sxcl_json_member_value(root, i), 1, root, key);
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
    st.unsupported_bool = 0;
    dump_root(&st, root, &ctx);
    if (b.bad) {
        db_free(&b);
        if (err && err_len) {
            (void)snprintf(err, err_len, "内存不足，写不出 JSON 文本");
        }
        return SXCL_LOADER_ERR_NOMEM;
    }
    if (st.unsupported_bool) {
        db_free(&b);
        if (err && err_len) {
            (void)snprintf(err, err_len, "这份 JSON 里有数组形式的布尔值，现有 json.h 读不出它的值，拒绝改写");
        }
        return SXCL_LOADER_ERR_FORMAT;
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
                                    const char *last_used, char **out_text, int *changed,
                                    char *err, size_t err_len)
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

    dbuf b;
    db_init(&b);
    dump_state st;
    st.out = &b;
    st.unsupported_bool = 0;
    dump_root(&st, doc ? sxcl_json_root(doc) : NULL, &ctx);
    sxcl_json_free(doc);
    if (st.unsupported_bool) {
        db_free(&b);
        if (err && err_len) {
            (void)snprintf(err, err_len, "这份 launcher_profiles.json 里有数组形式的布尔值，读不出它的值，已保留原文件不覆盖");
        }
        return SXCL_LOADER_ERR_FORMAT;
    }

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
                                         const char *last_used, int *changed, char *err, size_t err_len)
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
    const int rc = sxcl_loader_merge_profiles_text(old_text, key, name, last_used, &merged, &need_write,
                                                  err, err_len);
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
