/** processors[] 重放（实现，声明与理由见 include/sxcl/processor.h）。 */

#include "sxcl/processor.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/loader.h"   /* sxcl_loader_maven_path */
#include "sxcl/log.h"
#include "sxcl/verify.h"   /* sxcl_hash_file */
#include "sxcl/zip.h"      /* 读处理器 jar 的 MANIFEST.MF 拿 Main-Class */

#define PROC_TIMEOUT_DEFAULT_MS (30 * 60 * 1000)

/* ── 小工具 ── */

static void copy_cap(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (!err || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* a + "/" + b（a 末尾已经有斜杠就不重复加）。 */
static int join2(char *out, size_t cap, const char *a, const char *b)
{
    const size_t alen = a ? strlen(a) : 0;
    const int need_sep = alen > 0 && a[alen - 1] != '/' && a[alen - 1] != '\\';
    const int wrote = snprintf(out, cap, "%s%s%s", a ? a : "", need_sep ? "/" : "", b ? b : "");
    return (wrote <= 0 || (size_t)wrote >= cap) ? -1 : 0;
}

/* ── processors[] 的访问器（纯函数） ── */

static const sxcl_json_value *processor_at(const sxcl_json *profile, size_t index)
{
    if (!profile) {
        return NULL;
    }
    const sxcl_json_value *root = sxcl_json_root(profile);
    const sxcl_json_value *processors = sxcl_json_get(root, "processors");
    const sxcl_json_value *proc = sxcl_json_at(processors, index);
    if (!proc || sxcl_json_type_of(proc) != SXCL_JSON_OBJECT) {
        return NULL;
    }
    return proc;
}

size_t sxcl_processor_count(const sxcl_json *profile)
{
    if (!profile) {
        return 0;
    }
    return sxcl_json_size(sxcl_json_get(sxcl_json_root(profile), "processors"));
}

int sxcl_processor_wants(const sxcl_json *profile, size_t index, const char *side)
{
    const sxcl_json_value *proc = processor_at(profile, index);
    if (!proc || !side || !side[0]) {
        return 0;
    }
    /* FCL: ForgeNewInstallProfile.Processor.isSide —— sides == null || sides.contains(side)。
     * **没有 sides 键** = 两条边都跑（实测 1.20.1 的 5 条客户端处理器就是这样）；
     * 空的 sides 数组按 FCL 的语义是"哪条边都不跑"。 */
    const sxcl_json_value *sides = sxcl_json_get(proc, "sides");
    if (!sides || sxcl_json_type_of(sides) == SXCL_JSON_NULL) {
        return 1;
    }
    const size_t n = sxcl_json_size(sides);
    for (size_t i = 0; i < n; ++i) {
        const char *text = sxcl_json_string(sxcl_json_at(sides, i));
        if (text && strcmp(text, side) == 0) {
            return 1;
        }
    }
    return 0;
}

int sxcl_processor_jar(const sxcl_json *profile, size_t index, char *out, size_t cap)
{
    const sxcl_json_value *proc = processor_at(profile, index);
    if (!proc || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    const char *coord = sxcl_json_string(sxcl_json_get(proc, "jar"));
    if (!coord || !coord[0]) {
        return -1;
    }
    copy_cap(out, cap, coord);
    return 0;
}

static const sxcl_json_value *classpath_of(const sxcl_json *profile, size_t index)
{
    const sxcl_json_value *proc = processor_at(profile, index);
    return proc ? sxcl_json_get(proc, "classpath") : NULL;
}

size_t sxcl_processor_classpath_count(const sxcl_json *profile, size_t index)
{
    return sxcl_json_size(classpath_of(profile, index));
}

int sxcl_processor_classpath_at(const sxcl_json *profile, size_t index, size_t k, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    const char *coord = sxcl_json_string(sxcl_json_at(classpath_of(profile, index), k));
    if (!coord || !coord[0]) {
        return -1;
    }
    copy_cap(out, cap, coord);
    return 0;
}

static const sxcl_json_value *args_of(const sxcl_json *profile, size_t index)
{
    const sxcl_json_value *proc = processor_at(profile, index);
    return proc ? sxcl_json_get(proc, "args") : NULL;
}

size_t sxcl_processor_arg_count(const sxcl_json *profile, size_t index)
{
    return sxcl_json_size(args_of(profile, index));
}

const char *sxcl_processor_arg_at(const sxcl_json *profile, size_t index, size_t k)
{
    return sxcl_json_string(sxcl_json_at(args_of(profile, index), k));
}

static const sxcl_json_value *outputs_of(const sxcl_json *profile, size_t index)
{
    const sxcl_json_value *proc = processor_at(profile, index);
    const sxcl_json_value *outs = proc ? sxcl_json_get(proc, "outputs") : NULL;
    if (!outs || sxcl_json_type_of(outs) != SXCL_JSON_OBJECT) {
        return NULL;
    }
    return outs;
}

size_t sxcl_processor_output_count(const sxcl_json *profile, size_t index)
{
    return sxcl_json_member_count(outputs_of(profile, index));
}

const char *sxcl_processor_output_key_at(const sxcl_json *profile, size_t index, size_t k)
{
    return sxcl_json_member_key(outputs_of(profile, index), k);
}

const char *sxcl_processor_output_value_at(const sxcl_json *profile, size_t index, size_t k)
{
    return sxcl_json_string(sxcl_json_member_value(outputs_of(profile, index), k));
}

/* ── data{} → 变量表 ── */

void sxcl_processor_vars_init(sxcl_processor_vars *vars)
{
    if (vars) {
        memset(vars, 0, sizeof(*vars));
    }
}

int sxcl_processor_vars_put(sxcl_processor_vars *vars, const char *key, const char *value)
{
    if (!vars || !key || !key[0] || !value) {
        return -1;
    }
    for (size_t i = 0; i < vars->count; ++i) {
        if (strcmp(vars->items[i].key, key) == 0) {
            copy_cap(vars->items[i].value, sizeof(vars->items[i].value), value);
            return 0;
        }
    }
    if (vars->count >= SXCL_PROCESSOR_MAX_VARS) {
        return -1;
    }
    sxcl_processor_var *slot = &vars->items[vars->count];
    copy_cap(slot->key, sizeof(slot->key), key);
    copy_cap(slot->value, sizeof(slot->value), value);
    ++vars->count;
    return 0;
}

const char *sxcl_processor_vars_get(const sxcl_processor_vars *vars, const char *key)
{
    if (!vars || !key) {
        return NULL;
    }
    for (size_t i = 0; i < vars->count; ++i) {
        if (strcmp(vars->items[i].key, key) == 0) {
            return vars->items[i].value;
        }
    }
    return NULL;
}

/* FCL: ForgeNewInstallTask.replaceTokens —— 逐行移植（含 \\ 转义与 \'{字面量}\' 两种括号）。 */
static int replace_tokens(const sxcl_processor_vars *vars, const char *value, char *out, size_t cap,
                          char *err, size_t err_len)
{
    size_t o = 0;
    const size_t n = strlen(value);
    for (size_t x = 0; x < n; ++x) {
        const char c = value[x];
        if (c == '\\') {
            if (x + 1 >= n) {
                set_err(err, err_len, "转义符后面没有字符：%s", value);
                return -3;
            }
            if (o + 1 >= cap) {
                return -2;
            }
            out[o++] = value[++x];
            continue;
        }
        if (c != '{' && c != '\'') {
            if (o + 1 >= cap) {
                return -2;
            }
            out[o++] = c;
            continue;
        }
        char key[SXCL_PROCESSOR_VAR_VALUE_MAX];
        size_t kn = 0;
        int closed = 0;
        for (size_t y = x + 1; y <= n; ++y) {
            if (y == n) {
                set_err(err, err_len, "括号没有闭合（%c）：%s", c, value);
                return -3;
            }
            const char d = value[y];
            if (d == '\\') {
                if (y + 1 >= n) {
                    set_err(err, err_len, "转义符后面没有字符：%s", value);
                    return -3;
                }
                if (kn + 1 >= sizeof(key)) {
                    return -2;
                }
                key[kn++] = value[++y];
                continue;
            }
            if (c == '{' && d == '}') {
                x = y;
                closed = 1;
                break;
            }
            if (c == '\'' && d == '\'') {
                x = y;
                closed = 1;
                break;
            }
            if (kn + 1 >= sizeof(key)) {
                return -2;
            }
            key[kn++] = d;
        }
        if (!closed) {
            set_err(err, err_len, "括号没有闭合（%c）：%s", c, value);
            return -3;
        }
        key[kn] = '\0';
        if (c == '\'') {
            if (o + kn >= cap) {
                return -2;
            }
            memcpy(out + o, key, kn);
            o += kn;
            continue;
        }
        const char *hit = sxcl_processor_vars_get(vars, key);
        if (!hit) {
            set_err(err, err_len, "没有这个键：{%s}（原文 %s）", key, value);
            return -3;
        }
        const size_t hlen = strlen(hit);
        if (o + hlen >= cap) {
            return -2;
        }
        memcpy(out + o, hit, hlen);
        o += hlen;
    }
    if (o >= cap) {
        return -2;
    }
    out[o] = '\0';
    return 0;
}

/* 去掉首尾一个字符（"[x]" -> "x"），并保证装得下。 */
static int slice_inner(const char *literal, char *out, size_t cap)
{
    const size_t n = strlen(literal);
    if (n < 2 || n - 1 >= cap) {
        return -1;
    }
    memcpy(out, literal + 1, n - 2);
    out[n - 2] = '\0';
    return 0;
}

int sxcl_processor_eval(const char *literal, const sxcl_processor_vars *vars, const char *game_dir,
                        sxcl_processor_take_file_fn take, void *ud, char *out, size_t cap,
                        char *err, size_t err_len)
{
    if (!literal || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    const size_t n = strlen(literal);
    if (n >= 2 && literal[0] == '[' && literal[n - 1] == ']') {
        /* FCL: parseLiteral 的 "[" 分支 —— gameRepository.getArtifactFile(version, Artifact)。
         * 坐标 -> libraries/ 下的相对路径由 sxcl_loader_maven_path 负责（含分类器与 @zip）。 */
        char coord[256];
        char rel[320];
        if (slice_inner(literal, coord, sizeof(coord)) != 0) {
            set_err(err, err_len, "坐标太长：%s", literal);
            return -2;
        }
        if (sxcl_loader_maven_path(coord, rel, sizeof(rel)) != SXCL_LOADER_OK) {
            set_err(err, err_len, "坐标拼不出路径：%s", coord);
            return -3;
        }
        if (join2(out, cap, game_dir, "libraries") != 0 ||
            join2(out, cap, out, rel) != 0) {
            set_err(err, err_len, "路径太长：%s", rel);
            return -2;
        }
        return 0;
    }
    if (n >= 2 && literal[0] == '\'' && literal[n - 1] == '\'') {
        if (slice_inner(literal, out, cap) != 0) {
            set_err(err, err_len, "字面量太长：%s", literal);
            return -2;
        }
        return 0;
    }
    if (n >= 2 && literal[0] == '{' && literal[n - 1] == '}') {
        char key[SXCL_PROCESSOR_VAR_KEY_MAX * 2];
        if (slice_inner(literal, key, sizeof(key)) != 0) {
            set_err(err, err_len, "键名太长：%s", literal);
            return -2;
        }
        const char *hit = sxcl_processor_vars_get(vars, key);
        if (!hit) {
            set_err(err, err_len, "没有这个键：{%s}", key);
            return -3;
        }
        copy_cap(out, cap, hit);
        return 0;
    }
    char plain[SXCL_PROCESSOR_ARG_MAX * 2];
    const int rrc = replace_tokens(vars, literal, plain, sizeof(plain), err, err_len);
    if (rrc != 0) {
        return rrc;
    }
    if (take) {
        /* data 的用法：这一档的原文是"安装器 zip 里的一个条目"，取出来落到临时文件。 */
        if (take(ud, plain, out, cap) != 0) {
            set_err(err, err_len, "安装器里取不到这个文件：%s", plain);
            return -3;
        }
        return 0;
    }
    copy_cap(out, cap, plain);
    return 0;
}

int sxcl_processor_vars_from_data(const sxcl_json *profile, const char *game_dir,
                                  sxcl_processor_take_file_fn take, void *ud,
                                  sxcl_processor_vars *vars, char *err, size_t err_len)
{
    if (!profile || !vars) {
        return -1;
    }
    const sxcl_json_value *data = sxcl_json_get(sxcl_json_root(profile), "data");
    if (!data || sxcl_json_type_of(data) != SXCL_JSON_OBJECT) {
        return 0;   /* 没有 data（老格式）—— 不是错，后面真缺键会在 eval 里报出来 */
    }
    const size_t n = sxcl_json_member_count(data);
    for (size_t i = 0; i < n; ++i) {
        const char *key = sxcl_json_member_key(data, i);
        const sxcl_json_value *value = sxcl_json_member_value(data, i);
        const char *text = NULL;
        if (sxcl_json_type_of(value) == SXCL_JSON_OBJECT) {
            /* 新格式：按 side 分档的对象，FCL 的 Datum 只认 client。 */
            text = sxcl_json_get_string(value, "client", NULL);
        } else if (sxcl_json_type_of(value) == SXCL_JSON_STRING) {
            text = sxcl_json_string(value);   /* 老格式：直接是字符串 */
        }
        if (!key || !text) {
            continue;   /* 这条没有 client 侧的值：跳过，真用到时报"没有这个键"更准确 */
        }
        /* 裸值（既不是 [坐标] / '字面量' 也不是 {键}）只能是"安装器 zip 里的一个文件"：
         * 没有取文件的实现就别装作能求值 —— 那样会把 zip 里的相对路径原样交给处理器。 */
        if (!take && text[0] != '[' && text[0] != '\'' && text[0] != '{') {
            set_err(err, err_len, "data.%s 要从安装器里取文件（%s），但没有给 take_file", key, text);
            return -1;
        }
        char evaluated[SXCL_PROCESSOR_VAR_VALUE_MAX];
        char detail[SXCL_PROCESSOR_ERR_MAX];
        detail[0] = '\0';
        /* FCL 在这里传的是**空表**（data 之间不许互相引用），我们照办：vars 传 NULL。 */
        const int rc = sxcl_processor_eval(text, NULL, game_dir, take, ud, evaluated,
                                          sizeof(evaluated), detail, sizeof(detail));
        if (rc != 0) {
            set_err(err, err_len, "data.%s 求值失败：%s", key,
                    detail[0] ? detail : "装不下或参数非法");
            return -1;
        }
        if (sxcl_processor_vars_put(vars, key, evaluated) != 0) {
            set_err(err, err_len, "变量表满了（%d 个）：%s", (int)SXCL_PROCESSOR_MAX_VARS, key);
            return -1;
        }
    }
    if (sxcl_log_enabled(SXCL_LOG_DEBUG)) {
        sxcl_log_write(SXCL_LOG_DEBUG, "processors", "data 变量 %d 条", (int)vars->count);
    }
    return 0;
}

/* ── 重放 ── */

static int zip_text(sxcl_zip *zip, const char *name, char **out)
{
    *out = NULL;
    const size_t cap = 64 * 1024;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) {
        return -1;
    }
    size_t len = 0;
    if (sxcl_zip_extract_memory(zip, name, buf, cap, &len) != 0 || len == 0 || len > cap) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    return 0;
}

/* 从 MANIFEST.MF 里取 Main-Class（jar 规范允许续行：行首一个空格表示接着上一行）。 */
static int manifest_main_class(const char *text, char *out, size_t cap)
{
    static const char kKey[] = "main-class:";
    const size_t key_len = sizeof(kKey) - 1;
    size_t value_len = 0;
    int found = 0;
    out[0] = '\0';
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            --len;
        }
        int same_key = 0;
        if (len >= key_len) {
            same_key = 1;
            for (size_t i = 0; i < key_len; ++i) {
                char a = line[i];
                if (a >= 'A' && a <= 'Z') {
                    a = (char)(a - 'A' + 'a');
                }
                if (a != kKey[i]) {
                    same_key = 0;
                    break;
                }
            }
        }
        if (same_key) {
            size_t start = key_len;
            while (start < len && (line[start] == ' ' || line[start] == '\t')) {
                ++start;
            }
            value_len = 0;
            for (size_t i = start; i < len && value_len + 1 < cap; ++i) {
                out[value_len++] = line[i];
            }
            found = 1;
        } else if (found && len > 0 && line[0] == ' ') {
            for (size_t i = 1; i < len && value_len + 1 < cap; ++i) {
                out[value_len++] = line[i];
            }
        } else if (found) {
            break;   /* Main-Class 已经收完（后面的续行才接，别的键就结束） */
        }
        line = nl ? nl + 1 : NULL;
    }
    out[value_len] = '\0';
    while (value_len > 0 && (out[value_len - 1] == ' ' || out[value_len - 1] == '\r')) {
        out[--value_len] = '\0';
    }
    return found && value_len > 0 ? 0 : -1;
}

/** 一个产物的现状：存在 + SHA-1 对得上才算"不用重跑"。 */
static int output_ready(const char *path, const char *want_sha1, int *exists)
{
    *exists = 0;
    if (!sxcl_fs_exists(path) || sxcl_fs_is_dir(path)) {
        return 0;
    }
    *exists = 1;
    if (!want_sha1 || strlen(want_sha1) != 40) {
        return 0;   /* 没给哈希（不该发生）：当作需要重跑，宁可多跑一次 */
    }
    char got[41];
    got[0] = '\0';
    if (sxcl_hash_file(path, SXCL_HASH_SHA1, got, sizeof(got)) != 0) {
        return 0;
    }
    return sxcl_hash_hex_equal(got, want_sha1) ? 1 : 0;
}

/** 收集 java 候选：调用方给的 -> %JAVA_HOME%/bin/java -> PATH 里的 java。
 *  FCL 换的是 Java 版本（8 -> 17 -> 11 -> 21，见 ForgeNewInstallTask.runJVMProcess）；
 *  我们手上只有"这台机器上有什么"，所以换的是**路径**：给的这份失败就试另外两份。 */
static size_t java_candidates(const char *first, char out[3][SXCL_PROCESSOR_ARG_MAX])
{
    size_t n = 0;
    if (first && first[0]) {
        copy_cap(out[n], SXCL_PROCESSOR_ARG_MAX, first);
        ++n;
    }
    const char *home = getenv("JAVA_HOME");
    if (home && home[0]) {
        char cand[SXCL_PROCESSOR_ARG_MAX];
#if defined(_WIN32)
        if (join2(cand, sizeof(cand), home, "bin\\java.exe") == 0) {
#else
        if (join2(cand, sizeof(cand), home, "bin/java") == 0) {
#endif
            int dup = 0;
            for (size_t i = 0; i < n; ++i) {
                if (strcmp(out[i], cand) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (!dup && n < 3) {
                copy_cap(out[n], SXCL_PROCESSOR_ARG_MAX, cand);
                ++n;
            }
        }
    }
#if defined(_WIN32)
    const char *plain = "java.exe";
#else
    const char *plain = "java";
#endif
    int dup = 0;
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(out[i], plain) == 0) {
            dup = 1;
            break;
        }
    }
    if (!dup && n < 3) {
        copy_cap(out[n], SXCL_PROCESSOR_ARG_MAX, plain);
        ++n;
    }
    return n;
}

/** 这条处理器是不是"下载任务"（FCL: patchDownloadMojangMappingsTask）。
 *  args 已经求过值：--task DOWNLOAD_MOJMAPS --side client --version X --output Y。 */
static int is_mojmaps_task(const char *const *argv, size_t argc, char *version, size_t version_cap,
                           char *output, size_t output_cap)
{
    int task_ok = 0;
    int side_ok = 0;
    version[0] = '\0';
    output[0] = '\0';
    for (size_t i = 0; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--task") == 0) {
            task_ok = strcmp(argv[i + 1], "DOWNLOAD_MOJMAPS") == 0;
        } else if (strcmp(argv[i], "--side") == 0) {
            side_ok = strcmp(argv[i + 1], "client") == 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            copy_cap(version, version_cap, argv[i + 1]);
        } else if (strcmp(argv[i], "--output") == 0) {
            copy_cap(output, output_cap, argv[i + 1]);
        }
    }
    return (task_ok && side_ok && version[0] && output[0]) ? 1 : 0;
}

static int run_one(const sxcl_json *profile, size_t index, const sxcl_processor_ctx *ctx,
                   const sxcl_processor_vars *vars, sxcl_processor_stats *stats, char *err,
                   size_t err_len)
{
    char detail[SXCL_PROCESSOR_ERR_MAX];
    detail[0] = '\0';

    /* 1) 处理器 jar 与 classpath：FCL 在 preExecute 里已经把 profile.libraries 下好了
     *    （我们的 sxcl_loader_collect_libraries 会把 processors[].jar / classpath 一起列出来），
     *    这里只核"在不在" —— 不在就点名报错，不自己去下。 */
    char jar_coord[192];
    char jar_path[SXCL_PROCESSOR_ARG_MAX];
    jar_path[0] = '\0';
    if (sxcl_processor_jar(profile, index, jar_coord, sizeof(jar_coord)) != 0) {
        set_err(err, err_len, "processors[%d] 没有 jar 字段", (int)index);
        return 1;
    }
    char rel[320];
    if (sxcl_loader_maven_path(jar_coord, rel, sizeof(rel)) != SXCL_LOADER_OK ||
        join2(jar_path, sizeof(jar_path), ctx->game_dir, "libraries") != 0 ||
        join2(jar_path, sizeof(jar_path), jar_path, rel) != 0) {
        set_err(err, err_len, "processors[%d] 的 jar 路径拼不出来：%s", (int)index, jar_coord);
        return 1;
    }

    const size_t cp_count = sxcl_processor_classpath_count(profile, index);
    char *cp = (char *)calloc(1, SXCL_PROCESSOR_ARG_MAX * 16);
    if (!cp) {
        set_err(err, err_len, "内存不足（classpath）");
        return 1;
    }
    size_t cp_len = 0;
    int missing = 0;
    char missing_name[192];
    missing_name[0] = '\0';
    for (size_t k = 0; k < cp_count; ++k) {
        char coord[192];
        char entry[SXCL_PROCESSOR_ARG_MAX];
        entry[0] = '\0';
        if (sxcl_processor_classpath_at(profile, index, k, coord, sizeof(coord)) != 0) {
            continue;
        }
        if (sxcl_loader_maven_path(coord, rel, sizeof(rel)) != SXCL_LOADER_OK ||
            join2(entry, sizeof(entry), ctx->game_dir, "libraries") != 0 ||
            join2(entry, sizeof(entry), entry, rel) != 0) {
            set_err(err, err_len, "processors[%d] 的 classpath 条目拼不出路径：%s", (int)index, coord);
            free(cp);
            return 1;
        }
        if (!missing && !sxcl_fs_exists(entry)) {
            /* 只记下来，**不当场返回** —— FCL 的顺序是"先看产物齐不齐"，产物齐了这一条直接跳过，
             * 压根不需要工具 jar 在磁盘上（重装时清过 libraries/ 也不会因此失败）。 */
            missing = 1;
            copy_cap(missing_name, sizeof(missing_name), coord);
        }
        const size_t need = strlen(entry) + 2;
        if (cp_len + need >= SXCL_PROCESSOR_ARG_MAX * 16) {
            set_err(err, err_len, "processors[%d] 的 classpath 太长", (int)index);
            free(cp);
            return 1;
        }
        memcpy(cp + cp_len, entry, strlen(entry));
        cp_len += strlen(entry);
#if defined(_WIN32)
        cp[cp_len++] = ';';
#else
        cp[cp_len++] = ':';
#endif
        cp[cp_len] = '\0';
    }
    /* jar 自己排在 classpath 最后（FCL 的顺序：依赖在前、处理器自己在后）。 */
    if (cp_len + strlen(jar_path) + 1 >= SXCL_PROCESSOR_ARG_MAX * 16) {
        set_err(err, err_len, "processors[%d] 的 classpath 太长", (int)index);
        free(cp);
        return 1;
    }
    memcpy(cp + cp_len, jar_path, strlen(jar_path) + 1);

    /* 2) 产物幂等：全在且 SHA-1 都对 -> 这一条跳过（FCL: ProcessorTask.execute 的开头）。 */
    char out_path[SXCL_PROCESSOR_MAX_OUTPUTS][SXCL_PROCESSOR_ARG_MAX];
    char out_sha[SXCL_PROCESSOR_MAX_OUTPUTS][48];
    size_t out_count = 0;
    const size_t raw_outputs = sxcl_processor_output_count(profile, index);
    for (size_t k = 0; k < raw_outputs && out_count < SXCL_PROCESSOR_MAX_OUTPUTS; ++k) {
        const char *raw_key = sxcl_processor_output_key_at(profile, index, k);
        const char *raw_val = sxcl_processor_output_value_at(profile, index, k);
        char key_buf[SXCL_PROCESSOR_ARG_MAX];
        char sha_buf[48];
        key_buf[0] = '\0';
        sha_buf[0] = '\0';
        if (sxcl_processor_eval(raw_key, vars, ctx->game_dir, NULL, NULL, key_buf, sizeof(key_buf),
                                detail, sizeof(detail)) != 0) {
            set_err(err, err_len, "processors[%d] 的产物路径求值失败：%s", (int)index, detail);
            free(cp);
            return 1;
        }
        if (sxcl_processor_eval(raw_val, vars, ctx->game_dir, NULL, NULL, sha_buf, sizeof(sha_buf),
                                detail, sizeof(detail)) != 0) {
            set_err(err, err_len, "processors[%d] 的产物哈希求值失败：%s", (int)index, detail);
            free(cp);
            return 1;
        }
        copy_cap(out_path[out_count], sizeof(out_path[0]), key_buf);
        copy_cap(out_sha[out_count], sizeof(out_sha[0]), sha_buf);
        ++out_count;
    }

    int all_ready = out_count > 0;
    for (size_t k = 0; k < out_count; ++k) {
        int exists = 0;
        if (output_ready(out_path[k], out_sha[k], &exists)) {
            continue;
        }
        all_ready = 0;
        if (exists) {
            /* 在、但内容不对：删掉重来（FCL 同样删；留着它只会让下次也判不过）。 */
            (void)sxcl_fs_remove(out_path[k]);
        }
    }
    if (!all_ready && !ctx->dry_run) {
        /* 要真跑了才核"工具在不在"（顺序与 FCL 一致，理由见上面 classpath 那处注释）。 */
        if (!sxcl_fs_exists(jar_path)) {
            set_err(err, err_len, "处理器 jar 不在（%s）：%s —— 依赖库没下齐", jar_coord, jar_path);
            free(cp);
            return -2;
        }
        if (missing) {
            set_err(err, err_len, "处理器依赖缺件：%s（processors[%d]）", missing_name, (int)index);
            free(cp);
            return -2;
        }
    }
    if (all_ready) {
        ++stats->skipped;
        if (ctx->report) {
            char line[256];
            snprintf(line, sizeof(line), "处理器 %d/%d：产物已就绪，跳过", (int)index + 1,
                     (int)sxcl_processor_count(profile));
            ctx->report(ctx->ud, line);
        }
        free(cp);
        return 0;
    }

    /* 3) argv 求值 */
    const size_t arg_count = sxcl_processor_arg_count(profile, index);
    if (arg_count + 4 > SXCL_PROCESSOR_MAX_ARGS) {
        set_err(err, err_len, "processors[%d] 的参数太多（%d 条）", (int)index, (int)arg_count);
        free(cp);
        return 1;
    }
    char arg_buf[SXCL_PROCESSOR_MAX_ARGS][SXCL_PROCESSOR_ARG_MAX];
    const char *argv[SXCL_PROCESSOR_MAX_ARGS];
    size_t argc = 0;
    for (size_t k = 0; k < arg_count; ++k) {
        const char *raw = sxcl_processor_arg_at(profile, index, k);
        if (!raw) {
            continue;
        }
        if (sxcl_processor_eval(raw, vars, ctx->game_dir, NULL, NULL, arg_buf[argc],
                                sizeof(arg_buf[0]), detail, sizeof(detail)) != 0) {
            set_err(err, err_len, "processors[%d] 第 %d 个参数求值失败：%s", (int)index, (int)k + 1,
                    detail);
            free(cp);
            return 1;
        }
        argv[argc] = arg_buf[argc];
        ++argc;
    }

    /* 4) 下载任务替代实现（FCL 的 patchDownloadMojangMappingsTask） */
    if (ctx->download_mappings) {
        char ver[64];
        char output[SXCL_PROCESSOR_ARG_MAX];
        if (is_mojmaps_task(argv, argc, ver, sizeof(ver), output, sizeof(output))) {
            detail[0] = '\0';
            if (ctx->download_mappings(ctx->ud, ver, output, detail, sizeof(detail)) == 0) {
                ++stats->downloaded;
                if (ctx->report) {
                    char line[256];
                    snprintf(line, sizeof(line), "处理器 %d/%d：客户端映射由下载引擎取回", (int)index + 1,
                             (int)sxcl_processor_count(profile));
                    ctx->report(ctx->ud, line);
                }
                free(cp);
                return 0;
            }
            if (ctx->report) {
                char line[256];
                snprintf(line, sizeof(line), "客户端映射没取回（%s），交给处理器自己下",
                         detail[0] ? detail : "原因不明");
                ctx->report(ctx->ud, line);
            }
        }
    }

    /* 5) Main-Class 来自处理器 jar 的 MANIFEST.MF（FCL 用 JarFile.getManifest）。 */
    char main_class[192];
    main_class[0] = '\0';
    if (!ctx->dry_run) {
        sxcl_zip *zip = sxcl_zip_open(jar_path);
        if (!zip) {
            set_err(err, err_len, "打不开处理器 jar：%s", jar_path);
            free(cp);
            return 1;
        }
        char *manifest = NULL;
        if (zip_text(zip, "META-INF/MANIFEST.MF", &manifest) != 0 ||
            manifest_main_class(manifest ? manifest : "", main_class, sizeof(main_class)) != 0) {
            set_err(err, err_len, "处理器 jar 里没有 Main-Class：%s", jar_path);
            free(manifest);
            sxcl_zip_close(zip);
            free(cp);
            return 1;
        }
        free(manifest);
        sxcl_zip_close(zip);
    } else {
        copy_cap(main_class, sizeof(main_class), "(dry-run)");
    }

    /* 6) 拼命令行：java -cp <classpath;jar> <MainClass> <args...> */
    const char *full_argv[SXCL_PROCESSOR_MAX_ARGS + 4];
    size_t full_argc = 0;
    full_argv[full_argc++] = "-cp";
    full_argv[full_argc++] = cp;
    full_argv[full_argc++] = main_class;
    for (size_t k = 0; k < argc; ++k) {
        full_argv[full_argc++] = argv[k];
    }
    /* **必须 NULL 结尾**（sxcl/process.h 的契约）。漏了这一行，引擎会一路读到栈上的垃圾，
     * 把上一条处理器留下的参数当成这一条的参数 —— 真机上就是这么把 MERGE_MAPPING 的
     * --classes 漏进 jarsplitter 的（joptsimple 报"classes is not a recognized option"）。
     * 单测用假回调按 argc 取值，**看不见**这种错，所以这里单独写死。 */
    full_argv[full_argc] = NULL;

    if (ctx->dry_run) {
        if (ctx->report) {
            char line[SXCL_LOADER_TEXT_MAX];
            snprintf(line, sizeof(line), "处理器 %d/%d（只算不跑）：%s", (int)index + 1,
                     (int)sxcl_processor_count(profile), jar_coord);
            ctx->report(ctx->ud, line);
        }
        free(cp);
        return 0;
    }

    /* 6.5) 把这一条的命令行报出去（FCL 同款：LOG.info("Executing external processor " + jar +
     * command line)；出了"处理器报错"时，日志里没有命令行就只能靠猜）。 */
    if (ctx->report) {
        char shown[SXCL_PROCESSOR_ARG_MAX * 2];
        size_t off = 0;
        shown[0] = '\0';
        for (size_t k = 0; k < argc; ++k) {
            /* 只报**文件名**：进度通道的缓冲只有两百多字节，全路径一塞就截断，
             * 反而看不出"这一条到底带了哪些参数"（排查时最要紧的就是这个）。 */
            const char *arg = argv[k];
            for (const char *p = arg; *p; ++p) {
                if (*p == '/' || *p == '\\') {
                    arg = p + 1;
                }
            }
            const int w = snprintf(shown + off, sizeof(shown) - off, "%s%s", k ? " " : "", arg);
            if (w <= 0 || (size_t)w >= sizeof(shown) - off) {
                break;
            }
            off += (size_t)w;
        }
        char line[SXCL_PROCESSOR_ERR_MAX * 2];
        snprintf(line, sizeof(line), "处理器 %d/%d：%s %s", (int)index + 1,
                 (int)sxcl_processor_count(profile), main_class, shown);
        ctx->report(ctx->ud, line);
    }

    /* 7) 开跑：java 路径逐个试（FCL 是换个 Java 版本重试，见 java_candidates 的说明）。 */
    char javas[3][SXCL_PROCESSOR_ARG_MAX];
    const size_t java_count = java_candidates(ctx->java_path, javas);
    const int timeout = ctx->timeout_ms > 0 ? ctx->timeout_ms : PROC_TIMEOUT_DEFAULT_MS;
    int started = 0;
    int cancelled = 0;
    for (size_t j = 0; j < java_count && !started; ++j) {
        if (ctx->is_cancelled && ctx->is_cancelled(ctx->ud)) {
            cancelled = 1;
            break;
        }
        detail[0] = '\0';
        int code = -1;
        const int rc = ctx->run(ctx->ud, javas[j], full_argv, full_argc, ctx->game_dir, timeout,
                                &code, detail, sizeof(detail));
        if (rc == 0 && code == 0) {
            started = 1;
            break;
        }
        if (j + 1 < java_count) {
            if (ctx->report) {
                char line[320];
                snprintf(line, sizeof(line), "处理器 %d/%d 用 %s 没跑成（退出码 %d），换一个 Java 再试",
                         (int)index + 1, (int)sxcl_processor_count(profile), javas[j], code);
                ctx->report(ctx->ud, line);
            }
            continue;
        }
        set_err(err, err_len, "处理器 %d/%d 失败（%s，退出码 %d）：%s", (int)index + 1,
                (int)sxcl_processor_count(profile), javas[j], code,
                detail[0] ? detail : "没有更多信息");
        free(cp);
        return 1;
    }
    if (cancelled) {
        free(cp);
        return 2;
    }
    if (!started) {
        /* 走到这儿只有一种可能：一个 java 候选都没试（java_path 为空且环境里也没有）。 */
        set_err(err, err_len, "处理器 %d/%d 起不来：没有可用的 java", (int)index + 1,
                (int)sxcl_processor_count(profile));
        free(cp);
        return 1;
    }
    ++stats->ran;

    /* 8) 跑完按 outputs 校验：缺一件或哈希不对都算这条没成（FCL 会删掉错的那件再抛）。 */
    for (size_t k = 0; k < out_count; ++k) {
        int exists = 0;
        if (output_ready(out_path[k], out_sha[k], &exists)) {
            continue;
        }
        char got[41];
        got[0] = '\0';
        if (exists) {
            (void)sxcl_hash_file(out_path[k], SXCL_HASH_SHA1, got, sizeof(got));
        }
        (void)sxcl_fs_remove(out_path[k]);
        set_err(err, err_len, "处理器 %d/%d 的产物不对（%s）：期望 %s，实际 %s", (int)index + 1,
                (int)sxcl_processor_count(profile), out_path[k], out_sha[k],
                exists ? got : "文件没生成");
        free(cp);
        return 1;
    }

    if (ctx->report) {
        char line[256];
        snprintf(line, sizeof(line), "处理器 %d/%d 完成：%s", (int)index + 1,
                 (int)sxcl_processor_count(profile), jar_coord);
        ctx->report(ctx->ud, line);
    }
    free(cp);
    return 0;
}

int sxcl_processors_run(const sxcl_json *profile, const char *side, const sxcl_processor_ctx *ctx,
                        sxcl_processor_stats *stats, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    /* stats 可空（调用方只关心成败时不给）：内部统一指向一份本地统计，省得每处都判空。 */
    sxcl_processor_stats fallback_stats;
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    } else {
        memset(&fallback_stats, 0, sizeof(fallback_stats));
        stats = &fallback_stats;
    }
    if (!profile || !ctx || !side || !side[0] || !ctx->game_dir || !ctx->installer_jar) {
        set_err(err, err_len, "processors: 参数不全（profile / side / game_dir / installer_jar 必填）");
        return -1;
    }
    if (!ctx->dry_run && !ctx->run) {
        set_err(err, err_len, "processors: 没有给跑进程的回调");
        return -1;
    }

    const size_t total = sxcl_processor_count(profile);
    if (total == 0) {
        return 0;   /* 1.12 及以前没有 processors：这条路本来就是空的 */
    }

    sxcl_processor_vars *vars = (sxcl_processor_vars *)calloc(1, sizeof(sxcl_processor_vars));
    if (!vars) {
        set_err(err, err_len, "内存不足（processors 变量表）");
        return -1;
    }
    int rc = 0;
    if (sxcl_processor_vars_from_data(profile, ctx->game_dir, ctx->take_file, ctx->ud, vars, err,
                                      err_len) != 0) {
        free(vars);
        return 1;
    }
    /* FCL: ForgeNewInstallTask.execute 里那几条固定变量（MINECRAFT_VERSION 我们给版本号 ——
     * FCL 那儿误写成了 jar 路径，实测 1.20.1 的处理器并不用它）。 */
    (void)sxcl_processor_vars_put(vars, "SIDE", side);
    if (ctx->minecraft_jar && ctx->minecraft_jar[0]) {
        (void)sxcl_processor_vars_put(vars, "MINECRAFT_JAR", ctx->minecraft_jar);
    }
    if (ctx->minecraft_version && ctx->minecraft_version[0]) {
        (void)sxcl_processor_vars_put(vars, "MINECRAFT_VERSION", ctx->minecraft_version);
    }
    (void)sxcl_processor_vars_put(vars, "ROOT", ctx->game_dir);
    (void)sxcl_processor_vars_put(vars, "INSTALLER", ctx->installer_jar);
    if (ctx->library_dir && ctx->library_dir[0]) {
        (void)sxcl_processor_vars_put(vars, "LIBRARY_DIR", ctx->library_dir);
    } else {
        char libs[SXCL_PROCESSOR_ARG_MAX];
        libs[0] = '\0';
        if (join2(libs, sizeof(libs), ctx->game_dir, "libraries") == 0) {
            (void)sxcl_processor_vars_put(vars, "LIBRARY_DIR", libs);
        }
    }

    for (size_t i = 0; i < total && rc == 0; ++i) {
        if (!sxcl_processor_wants(profile, i, side)) {
            continue;
        }
        ++stats->considered;
        if (ctx->is_cancelled && ctx->is_cancelled(ctx->ud)) {
            rc = 2;
            break;
        }
        rc = run_one(profile, i, ctx, vars, stats, err, err_len);
        if (rc == -2) {
            rc = 1;   /* "缺件"在调用方看来同样是失败，只是原因更具体 */
        }
    }
    free(vars);
    return rc;
}
