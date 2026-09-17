/* SXCL-C 实例扫描与识别实现 —— 规则清单与两处与 Python 的有意差异见 sxcl/instance.h。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/instance.h"

#include "io_internal.h"
#include "sxcl/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INST_TMP 2048

/* ══════════════════════ 1) 小工具 ══════════════════════ */

static void inst_err(char *err, size_t err_len, const char *fmt, ...);
static void inst_copy(char *dst, size_t cap, const char *src);

static void inst_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    const size_t n = strlen(src);
    const size_t take = (n < cap - 1) ? n : cap - 1;
    (void)memcpy(dst, src, take);
    dst[take] = '\0';
}

static void inst_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static int inst_join(char *out, size_t out_len, const char *a, const char *b)
{
    const int n = sxcl_dir_join(out, out_len, a, b);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

/** <game_dir>/versions/<id> */
static int inst_version_dir(char *out, size_t out_len, const char *game_dir, const char *id)
{
    char versions[SXCL_INSTANCE_PATH_MAX + 8];
    if (inst_join(versions, sizeof(versions), game_dir, "versions") != 0) {
        return -1;
    }
    return inst_join(out, out_len, versions, id);
}

static void inst_trim(char *text)
{
    if (text == NULL) {
        return;
    }
    size_t len = strlen(text);
    while (len > 0) {
        const char c = text[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            text[--len] = '\0';
        } else {
            break;
        }
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n' ||
           text[start] == '\v' || text[start] == '\f') {
        ++start;
    }
    if (start > 0) {
        (void)memmove(text, text + start, len - start + 1);
    }
}

static char inst_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** 大小写不敏感的 ASCII 子串查找(needle 必须是小写)。 */
static const char *inst_find_ci(const char *hay, const char *needle)
{
    if (hay == NULL || needle == NULL || needle[0] == '\0') {
        return NULL;
    }
    const size_t nlen = strlen(needle);
    for (const char *p = hay; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' && inst_lower(p[i]) == needle[i]) {
            ++i;
        }
        if (i == nlen) {
            return p;
        }
    }
    return NULL;
}

/* 大小写不敏感的完全相等(Python 的 value.lower() == "unknown" / in ("true","1")) */
static int inst_ci_equal(const char *text, const char *want)
{
    if (text == NULL || want == NULL) {
        return 0;
    }
    size_t i = 0;
    for (; want[i] != '\0'; ++i) {
        if (text[i] == '\0' || inst_lower(text[i]) != inst_lower(want[i])) {
            return 0;
        }
    }
    return (text[i] == '\0') ? 1 : 0;
}

static int inst_ci_prefix(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL) {
        return 0;
    }
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (inst_lower(text[i]) != inst_lower(prefix[i])) {
            return 0;
        }
        if (text[i] == '\0') {
            return 0;
        }
    }
    return 1;
}

/** Python 的 \w(ASCII 部分)加上高位字节:用来实现 (?<![\w.\-]) 与 (?![\w.]) 的前后视。 */
static int inst_is_word_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80u;
}

/* 目录名规则里两类不同的字符集(Python 的 NAME_RULES 就是这么写的,别合并):
 *   forge/neoforge/fabric/quilt --- [0-9A-Za-z.]    (没有下划线)
 *   optifine/liteloader         --- [0-9A-Za-z_.]   (有下划线,所以 HD_U_I6 才取得全) */
static int inst_is_ver_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.';
}

static int inst_is_loose_ver_char(char c)
{
    return inst_is_ver_char(c) || c == '_';
}

/* ══════════════════════ 2) 种类与问题码的文案 ══════════════════════ */

static const char *const INST_KIND_IDS[] = {
    "vanilla", "forge", "neoforge", "fabric", "quilt", "optifine", "liteloader"
};
static const char *const INST_KIND_NAMES[] = {
    "原版", "Forge", "NeoForge", "Fabric", "Quilt", "OptiFine", "LiteLoader"
};

const char *sxcl_instance_kind_id(sxcl_instance_loader_kind kind)
{
    const int index = (int)kind;
    if (index < 0 || index >= (int)(sizeof(INST_KIND_IDS) / sizeof(INST_KIND_IDS[0]))) {
        return "vanilla";
    }
    return INST_KIND_IDS[index];
}

const char *sxcl_instance_kind_name(sxcl_instance_loader_kind kind)
{
    const int index = (int)kind;
    if (index < 0 || index >= (int)(sizeof(INST_KIND_NAMES) / sizeof(INST_KIND_NAMES[0]))) {
        return INST_KIND_NAMES[0];
    }
    return INST_KIND_NAMES[index];
}

sxcl_instance_loader_kind sxcl_instance_kind_from_id(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return SXCL_INSTANCE_VANILLA;
    }
    for (size_t i = 0; i < sizeof(INST_KIND_IDS) / sizeof(INST_KIND_IDS[0]); ++i) {
        const char *a = id;
        const char *b = INST_KIND_IDS[i];
        size_t k = 0;
        while (a[k] != '\0' && b[k] != '\0' && inst_lower(a[k]) == inst_lower(b[k])) {
            ++k;
        }
        if (a[k] == '\0' && b[k] == '\0') {
            return (sxcl_instance_loader_kind)i;
        }
    }
    return SXCL_INSTANCE_VANILLA;
}

sxcl_loader_kind sxcl_instance_kind_to_loader(sxcl_instance_loader_kind kind)
{
    if ((int)kind >= (int)SXCL_INSTANCE_LITELOADER || (int)kind < 0) {
        return SXCL_LOADER_VANILLA; /* loader.h 目前没有 LiteLoader */
    }
    return (sxcl_loader_kind)(int)kind;
}

static const char *const INST_PROBLEM_IDS[] = {
    "ok", "bad-json", "missing-parent", "no-json", "missing-jar", "id-mismatch"
};
static const char *const INST_PROBLEM_TEXTS[] = {
    "",
    "版本 JSON 损坏或缺少 mainClass",
    "需要安装前置版本",
    "缺少版本 JSON",
    "缺少客户端 jar",
    "版本目录名与版本 JSON 的 id 不一致"
};

const char *sxcl_instance_problem_id(sxcl_instance_problem problem)
{
    const int index = (int)problem;
    if (index < 0 || index >= (int)(sizeof(INST_PROBLEM_IDS) / sizeof(INST_PROBLEM_IDS[0]))) {
        return "ok";
    }
    return INST_PROBLEM_IDS[index];
}

const char *sxcl_instance_problem_default_text(sxcl_instance_problem problem)
{
    const int index = (int)problem;
    if (index < 0 || index >= (int)(sizeof(INST_PROBLEM_TEXTS) / sizeof(INST_PROBLEM_TEXTS[0]))) {
        return "";
    }
    return INST_PROBLEM_TEXTS[index];
}

/* ══════════════════════ 3) 加载器识别规则(逐条对齐 loaders.py) ══════════════════════ */

typedef struct inst_rule {
    const char *needle;
    int kind;
} inst_rule;

/* LIBRARY_RULES:NeoForge 必须排在 Forge 前面(NeoForge 的坐标里也含 minecraftforge) */
static const inst_rule INST_LIBRARY_RULES[] = {
    { "net.neoforged:neoforge", SXCL_INSTANCE_NEOFORGE },
    { "net.neoforged:forge", SXCL_INSTANCE_NEOFORGE },
    { "net.neoforge", SXCL_INSTANCE_NEOFORGE },
    { "net.minecraftforge:forge", SXCL_INSTANCE_FORGE },
    { "net.minecraftforge:fmlloader", SXCL_INSTANCE_FORGE },
    { "net.minecraftforge:minecraftforge", SXCL_INSTANCE_FORGE },
    { "net.fabricmc:fabric-loader", SXCL_INSTANCE_FABRIC },
    { "net.fabricmc:intermediary", SXCL_INSTANCE_FABRIC },
    { "org.quiltmc:quilt-loader", SXCL_INSTANCE_QUILT },
    { "org.quiltmc:quilted-fabric-loader", SXCL_INSTANCE_QUILT },
    { "optifine:OptiFine", SXCL_INSTANCE_OPTIFINE },
    { "optifine:launchwrapper-of", SXCL_INSTANCE_OPTIFINE },
    { "com.mumfrey:liteloader", SXCL_INSTANCE_LITELOADER }
};

/* MAIN_CLASS_RULES */
static const inst_rule INST_MAIN_CLASS_RULES[] = {
    { "net.neoforged", SXCL_INSTANCE_NEOFORGE },
    { "net.minecraftforge.bootstrap", SXCL_INSTANCE_FORGE },
    { "cpw.mods.modlauncher", SXCL_INSTANCE_FORGE },
    { "net.minecraftforge", SXCL_INSTANCE_FORGE },
    { "net.fabricmc.loader", SXCL_INSTANCE_FABRIC },
    { "org.quiltmc.loader", SXCL_INSTANCE_QUILT },
    { "net.optifine", SXCL_INSTANCE_OPTIFINE },
    { "com.mumfrey.liteloader", SXCL_INSTANCE_LITELOADER }
};

/* TEXT_RULES(PCL 的判据,ModMinecraft.vb:756-780);needle 一律小写,对着小写化后的 JSON 文本找 */
static const inst_rule INST_TEXT_RULES[] = {
    { "net.neoforge", SXCL_INSTANCE_NEOFORGE },
    { "minecraftforge", SXCL_INSTANCE_FORGE },
    { "net.fabricmc:fabric-loader", SXCL_INSTANCE_FABRIC },
    { "org.quiltmc:quilt-loader", SXCL_INSTANCE_QUILT },
    { "optifine", SXCL_INSTANCE_OPTIFINE },
    { "liteloader", SXCL_INSTANCE_LITELOADER }
};

/* Setup.ini 的键 -> 种类 */
static const inst_rule INST_SETUP_RULES[] = {
    { "VersionFabric", SXCL_INSTANCE_FABRIC },
    { "VersionForge", SXCL_INSTANCE_FORGE },
    { "VersionNeoForge", SXCL_INSTANCE_NEOFORGE },
    { "VersionOptiFine", SXCL_INSTANCE_OPTIFINE },
    { "VersionLiteLoader", SXCL_INSTANCE_LITELOADER }
};

/* 返回顺序(Loaders.py 的 LOADER_ORDER 加上了 Quilt —— 见 instance.h 的说明) */
static const int INST_LOADER_ORDER[] = {
    SXCL_INSTANCE_FABRIC, SXCL_INSTANCE_QUILT, SXCL_INSTANCE_FORGE,
    SXCL_INSTANCE_NEOFORGE, SXCL_INSTANCE_LITELOADER, SXCL_INSTANCE_OPTIFINE
};

#define INST_KIND_COUNT 7

typedef struct inst_found {
    int has[INST_KIND_COUNT];
    char version[INST_KIND_COUNT][SXCL_INSTANCE_VERSION_MAX];
    int from_json[INST_KIND_COUNT];
} inst_found;

/* ── 坐标 ── */

/* Python: _coord_of —— name 优先,其次 downloads.artifact.path */
static void inst_coord_of(const sxcl_json_value *lib, char *out, size_t out_len)
{
    inst_copy(out, out_len, "");
    const char *name = sxcl_json_get_string(lib, "name", NULL);
    if (name != NULL && name[0] != '\0') {
        inst_copy(out, out_len, name);
        return;
    }
    const sxcl_json_value *downloads = sxcl_json_get(lib, "downloads");
    const sxcl_json_value *artifact = (downloads != NULL) ? sxcl_json_get(downloads, "artifact") : NULL;
    const char *path = (artifact != NULL) ? sxcl_json_get_string(artifact, "path", NULL) : NULL;
    if (path != NULL) {
        inst_copy(out, out_len, path);
    }
}

/* Python: _version_from_coord —— "group:artifact:version" 的第 3 段(分类器之前那段) */
static void inst_coord_version(const char *coord, char *out, size_t out_len)
{
    inst_copy(out, out_len, "");
    const char *first = strchr(coord, ':');
    if (first == NULL) {
        return;
    }
    const char *second = strchr(first + 1, ':');
    if (second == NULL) {
        return;
    }
    const char *value = second + 1;
    const char *end = strchr(value, ':');
    const size_t len = (end != NULL) ? (size_t)(end - value) : strlen(value);
    if (len == 0) {
        return;
    }
    const size_t take = (len < out_len - 1) ? len : out_len - 1;
    (void)memcpy(out, value, take);
    out[take] = '\0';
}

/* ── 文本线索里的版本号 ── */

/* 去掉所有的 "+build"(Python: version.replace("+build", "")) */
static void inst_strip_build(char *text)
{
    char *read = text;
    char *write = text;
    while (*read != '\0') {
        if (strncmp(read, "+build", 6) == 0) {
            read += 6;
            continue;
        }
        *write++ = *read++;
    }
    *write = '\0';
}

/* 在 JSON 文本里找 "<key>" 后面那个字符串值(NeoForge 的 neoForgeVersion/forgeVersion 规则)。
 * 同时接受 JSON 风格的 "key": "value" 与 Python 正则实际要求的 "key", "value" 两种写法。 */
static int inst_text_key_value(const char *text, const char *key, char *out, size_t out_len)
{
    const size_t klen = strlen(key);
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + klen;
        if (*q != '"') {
            /* 键名后面必须是引号:避免 "xxxForgeVersionx" 这种长名字被误命中 */
            if (*q == '\0') {
                return 0;
            }
            p += klen;
            continue;
        }
        ++q;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
            ++q;
        }
        if (*q == ':') {
            ++q;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                ++q;
            }
        }
        if (*q == '"') {
            ++q;
            size_t n = 0;
            while (q[n] != '\0' && q[n] != '"' && n + 1 < out_len) {
                out[n] = q[n];
                ++n;
            }
            out[n] = '\0';
            if (n > 0) {
                return 1;
            }
        }
        p += klen;
    }
    return 0;
}

/* (?<=fabric-loader:)[0-9.]+ (?<=quilt-loader:)[0-9.]+ */
static int inst_text_loader_version(const char *text, char *out, size_t out_len)
{
    static const char *const needles[] = { "fabric-loader:", "quilt-loader:" };
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        const char *p = strstr(text, needles[i]);
        if (p == NULL) {
            continue;
        }
        p += strlen(needles[i]);
        size_t n = 0;
        while ((p[n] >= '0' && p[n] <= '9') || p[n] == '.') {
            if (n + 1 < out_len) {
                out[n] = p[n];
            }
            ++n;
        }
        out[(n < out_len) ? n : out_len - 1] = '\0';
        if (n > 0) {
            return 1;
        }
    }
    return 0;
}

/* (?<=HD_U_)[^":/]+ */
static int inst_text_optifine_version(const char *text, char *out, size_t out_len)
{
    const char *p = strstr(text, "HD_U_");
    if (p == NULL) {
        return 0;
    }
    p += 5;
    size_t n = 0;
    while (p[n] != '\0' && p[n] != '"' && p[n] != ':' && p[n] != '/' && n + 1 < out_len) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
    return (n > 0) ? 1 : 0;
}

/* ── 目录名线索(NAME_RULES) ── */

/* [0-9][0-9A-Za-z.]* (首字符必须是数字) */
static int inst_take_digit_version(const char *s, char *out, size_t out_len)
{
    if (s[0] < '0' || s[0] > '9') {
        return 0;
    }
    size_t n = 0;
    while (inst_is_ver_char(s[n]) && n + 1 < out_len) {
        out[n] = s[n];
        ++n;
    }
    out[n] = '\0';
    return (n > 0) ? 1 : 0;
}

/* [0-9A-Za-z_.]+ */
static int inst_take_loose_version(const char *s, char *out, size_t out_len)
{
    size_t n = 0;
    while (inst_is_loose_ver_char(s[n]) && n + 1 < out_len) {
        out[n] = s[n];
        ++n;
    }
    out[n] = '\0';
    return (n > 0) ? 1 : 0;
}

/* Python 的 .strip("_-") */
static void inst_strip_underscore(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[0] == '_' || text[0] == '-')) {
        (void)memmove(text, text + 1, len);
        --len;
    }
    while (len > 0 && (text[len - 1] == '_' || text[len - 1] == '-')) {
        text[--len] = '\0';
    }
}

static int inst_name_neoforge(const char *id, char *out, size_t out_len)
{
    const char *p = inst_find_ci(id, "neoforge");
    if (p == NULL) {
        return 0;
    }
    p += 8;
    if (*p == '_' || *p == '-') {
        ++p;
    }
    return inst_take_digit_version(p, out, out_len);
}

/* (?<!neo)forge[_-]?<ver>:前面是 "neo" 的那次不算(NeoForge 不能被当成 Forge) */
static int inst_name_forge(const char *id, char *out, size_t out_len)
{
    const char *p = id;
    for (;;) {
        p = inst_find_ci(p, "forge");
        if (p == NULL) {
            return 0;
        }
        const int after_neo = (p >= id + 3) && inst_ci_prefix(p - 3, "neo");
        if (!after_neo) {
            const char *q = p + 5;
            if (*q == '_' || *q == '-') {
                ++q;
            }
            if (inst_take_digit_version(q, out, out_len)) {
                return 1;
            }
        }
        ++p;
    }
}

static int inst_name_fabric_like(const char *id, const char *keyword, char *out, size_t out_len)
{
    const char *p = id;
    for (;;) {
        p = inst_find_ci(p, keyword);
        if (p == NULL) {
            return 0;
        }
        const char *q = p + strlen(keyword);
        if (*q == '_' || *q == '-') {
            ++q;
        }
        if (inst_ci_prefix(q, "loader")) {
            q += 6;
            if (*q == '_' || *q == '-') {
                ++q;
            }
        }
        if (inst_take_digit_version(q, out, out_len)) {
            return 1;
        }
        ++p;
    }
}

static int inst_name_loose(const char *id, const char *keyword, char *out, size_t out_len)
{
    const char *p = id;
    for (;;) {
        p = inst_find_ci(p, keyword);
        if (p == NULL) {
            return 0;
        }
        const char *q = p + strlen(keyword);
        if (*q == '_' || *q == '-') {
            ++q;
        }
        if (inst_take_loose_version(q, out, out_len)) {
            inst_strip_underscore(out);
            return 1;
        }
        ++p;
    }
}

/** 识别一个版本带了哪些加载器(Python 的 detect_loaders)。
 *  线索优先级:libraries 坐标 > mainClass > 整段 JSON 文本 > PCL 的 Setup.ini > 目录名。 */
static void inst_detect_loaders(const char *version_id, const sxcl_json_value *root,
                                const char *json_text, const sxcl_instance_pcl_setup *setup,
                                sxcl_instance_loader *out, size_t *out_count)
{
    inst_found found;
    (void)memset(&found, 0, sizeof(found));

    if (root != NULL) {
        const sxcl_json_value *libraries = sxcl_json_get(root, "libraries");
        const size_t lib_count = sxcl_json_size(libraries);
        for (size_t i = 0; i < lib_count; ++i) {
            const sxcl_json_value *lib = sxcl_json_at(libraries, i);
            if (lib == NULL) {
                continue;
            }
            char coord[512];
            inst_coord_of(lib, coord, sizeof(coord));
            if (coord[0] == '\0') {
                continue;
            }
            for (size_t k = 0; k < sizeof(INST_LIBRARY_RULES) / sizeof(INST_LIBRARY_RULES[0]); ++k) {
                const inst_rule *rule = &INST_LIBRARY_RULES[k];
                if (strstr(coord, rule->needle) != NULL && !found.has[rule->kind]) {
                    found.has[rule->kind] = 1;
                    inst_coord_version(coord, found.version[rule->kind], SXCL_INSTANCE_VERSION_MAX);
                    found.from_json[rule->kind] = 1;
                    break;
                }
            }
        }
        const char *main_class = sxcl_json_get_string(root, "mainClass", "");
        if (main_class != NULL && main_class[0] != '\0') {
            for (size_t k = 0; k < sizeof(INST_MAIN_CLASS_RULES) / sizeof(INST_MAIN_CLASS_RULES[0]); ++k) {
                const inst_rule *rule = &INST_MAIN_CLASS_RULES[k];
                if (strstr(main_class, rule->needle) != NULL && !found.has[rule->kind]) {
                    found.has[rule->kind] = 1;
                    found.version[rule->kind][0] = '\0';
                    found.from_json[rule->kind] = 1;
                }
            }
        }
    }

    if (json_text != NULL && json_text[0] != '\0') {
        const size_t tlen = strlen(json_text);
        char *low = (char *)malloc(tlen + 1);
        if (low != NULL) {
            for (size_t i = 0; i < tlen; ++i) {
                low[i] = inst_lower(json_text[i]);
            }
            low[tlen] = '\0';
        }
        for (size_t k = 0; k < sizeof(INST_TEXT_RULES) / sizeof(INST_TEXT_RULES[0]); ++k) {
            const inst_rule *rule = &INST_TEXT_RULES[k];
            if (found.has[rule->kind]) {
                continue;
            }
            const int hit = (low != NULL) ? (strstr(low, rule->needle) != NULL)
                                          : (inst_find_ci(json_text, rule->needle) != NULL);
            if (!hit) {
                continue;
            }
            /* PCL 的互斥判据:NeoForge 的文本里也含 minecraftforge */
            if (rule->kind == SXCL_INSTANCE_FORGE) {
                const int neo = (low != NULL) ? (strstr(low, "net.neoforge") != NULL)
                                              : (inst_find_ci(json_text, "net.neoforge") != NULL);
                if (neo) {
                    continue;
                }
            }
            char version[SXCL_INSTANCE_VERSION_MAX];
            version[0] = '\0';
            if (rule->kind == SXCL_INSTANCE_OPTIFINE) {
                (void)inst_text_optifine_version(json_text, version, sizeof(version));
            } else if (rule->kind == SXCL_INSTANCE_NEOFORGE) {
                if (!inst_text_key_value(json_text, "neoForgeVersion", version, sizeof(version))) {
                    (void)inst_text_key_value(json_text, "forgeVersion", version, sizeof(version));
                }
            } else {
                (void)inst_text_loader_version(json_text, version, sizeof(version));
            }
            inst_strip_build(version);
            found.has[rule->kind] = 1;
            inst_copy(found.version[rule->kind], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[rule->kind] = 1;
        }
        free(low);
    }

    if (setup != NULL && setup->exists) {
        for (size_t k = 0; k < sizeof(INST_SETUP_RULES) / sizeof(INST_SETUP_RULES[0]); ++k) {
            const inst_rule *rule = &INST_SETUP_RULES[k];
            const char *value = sxcl_instance_pcl_get(setup, rule->needle);
            if (value == NULL || value[0] == '\0' || found.has[rule->kind]) {
                continue;
            }
            if (inst_ci_equal(value, "unknown")) {
                continue;
            }
            found.has[rule->kind] = 1;
            inst_copy(found.version[rule->kind], SXCL_INSTANCE_VERSION_MAX, value);
            found.from_json[rule->kind] = 0;
        }
    }

    if (version_id != NULL && version_id[0] != '\0') {
        char version[SXCL_INSTANCE_VERSION_MAX];
        if (!found.has[SXCL_INSTANCE_NEOFORGE] && inst_name_neoforge(version_id, version, sizeof(version))) {
            found.has[SXCL_INSTANCE_NEOFORGE] = 1;
            inst_copy(found.version[SXCL_INSTANCE_NEOFORGE], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[SXCL_INSTANCE_NEOFORGE] = 0;
        }
        if (!found.has[SXCL_INSTANCE_FORGE] && inst_name_forge(version_id, version, sizeof(version))) {
            found.has[SXCL_INSTANCE_FORGE] = 1;
            inst_copy(found.version[SXCL_INSTANCE_FORGE], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[SXCL_INSTANCE_FORGE] = 0;
        }
        if (!found.has[SXCL_INSTANCE_FABRIC] &&
            inst_name_fabric_like(version_id, "fabric", version, sizeof(version))) {
            found.has[SXCL_INSTANCE_FABRIC] = 1;
            inst_copy(found.version[SXCL_INSTANCE_FABRIC], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[SXCL_INSTANCE_FABRIC] = 0;
        }
        if (!found.has[SXCL_INSTANCE_QUILT] &&
            inst_name_fabric_like(version_id, "quilt", version, sizeof(version))) {
            found.has[SXCL_INSTANCE_QUILT] = 1;
            inst_copy(found.version[SXCL_INSTANCE_QUILT], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[SXCL_INSTANCE_QUILT] = 0;
        }
        if (!found.has[SXCL_INSTANCE_OPTIFINE] &&
            inst_name_loose(version_id, "optifine", version, sizeof(version))) {
            found.has[SXCL_INSTANCE_OPTIFINE] = 1;
            inst_copy(found.version[SXCL_INSTANCE_OPTIFINE], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[SXCL_INSTANCE_OPTIFINE] = 0;
        }
        if (!found.has[SXCL_INSTANCE_LITELOADER] &&
            inst_name_loose(version_id, "liteloader", version, sizeof(version))) {
            found.has[SXCL_INSTANCE_LITELOADER] = 1;
            inst_copy(found.version[SXCL_INSTANCE_LITELOADER], SXCL_INSTANCE_VERSION_MAX, version);
            found.from_json[SXCL_INSTANCE_LITELOADER] = 0;
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < sizeof(INST_LOADER_ORDER) / sizeof(INST_LOADER_ORDER[0]); ++i) {
        const int kind = INST_LOADER_ORDER[i];
        if (!found.has[kind] || count >= SXCL_INSTANCE_MAX_LOADERS) {
            continue;
        }
        (void)memset(&out[count], 0, sizeof(out[count]));
        out[count].kind = (sxcl_instance_loader_kind)kind;
        inst_copy(out[count].version, SXCL_INSTANCE_VERSION_MAX, found.version[kind]);
        out[count].from_json = found.from_json[kind];
        ++count;
    }
    *out_count = count;
}

/* ══════════════════════ 4) 原版版本号推断(_base_from_json) ══════════════════════ */

/* ((?:1\.\d+|[ab]\d+\.\d+)(?:\.\d+)?) —— 从 s[0] 开始匹配,返回匹配长度(0 = 不匹配)。
 * 前后视 ((?<![\w.\-]) / (?![\w.])) 由调用方检查前一个字符,这里检查后一个字符。 */
static size_t inst_match_mc_version_at(const char *s)
{
    size_t major = 0;
    if (s[0] == '1' && s[1] == '.') {
        size_t i = 2;
        while (s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        if (i == 2) {
            return 0;
        }
        major = i;
    } else if ((s[0] == 'a' || s[0] == 'b') && s[1] >= '0' && s[1] <= '9') {
        size_t i = 1;
        while (s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        if (s[i] != '.') {
            return 0;
        }
        ++i;
        const size_t digits = i;
        while (s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        if (i == digits) {
            return 0;
        }
        major = i;
    } else {
        return 0;
    }
    if (s[major] == '.' && s[major + 1] >= '0' && s[major + 1] <= '9') {
        size_t j = major + 1;
        while (s[j] >= '0' && s[j] <= '9') {
            ++j;
        }
        if (!inst_is_word_byte((unsigned char)s[j]) && s[j] != '.') {
            return j;
        }
    }
    if (!inst_is_word_byte((unsigned char)s[major]) && s[major] != '.') {
        return major;
    }
    return 0;
}

/* (\d+\.\d+(?:\.\d+)?) */
static size_t inst_match_any_version_at(const char *s)
{
    size_t i = 0;
    if (!(s[0] >= '0' && s[0] <= '9')) {
        return 0;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (s[i] != '.') {
        return 0;
    }
    ++i;
    const size_t digits = i;
    while (s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i == digits) {
        return 0;
    }
    const size_t major = i;
    if (s[major] == '.' && s[major + 1] >= '0' && s[major + 1] <= '9') {
        size_t j = major + 1;
        while (s[j] >= '0' && s[j] <= '9') {
            ++j;
        }
        if (!inst_is_word_byte((unsigned char)s[j]) && s[j] != '.') {
            return j;
        }
    }
    if (!inst_is_word_byte((unsigned char)s[major]) && s[major] != '.') {
        return major;
    }
    return 0;
}

static int inst_scan_version_in_name(const char *name, int mc_only, char *out, size_t out_len)
{
    const size_t len = strlen(name);
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) {
            const unsigned char prev = (unsigned char)name[i - 1];
            if (inst_is_word_byte(prev) || prev == '.' || prev == '-') {
                continue;
            }
        }
        const size_t matched = mc_only ? inst_match_mc_version_at(name + i) : inst_match_any_version_at(name + i);
        if (matched > 0) {
            const size_t take = (matched < out_len - 1) ? matched : out_len - 1;
            (void)memcpy(out, name + i, take);
            out[take] = '\0';
            return 1;
        }
    }
    return 0;
}

/* --fml.mcVersion",  \s*  "1.20.1" */
static int inst_fml_mc_version(const char *text, char *out, size_t out_len)
{
    static const char *const needle = "--fml.mcVersion\"";
    const size_t nlen = strlen(needle);
    const char *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p + nlen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
            ++q;
        }
        if (*q == ',') {
            ++q;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                ++q;
            }
            if (*q == '"') {
                ++q;
                size_t n = 0;
                while (q[n] != '\0' && q[n] != '"' && n + 1 < out_len) {
                    out[n] = q[n];
                    ++n;
                }
                out[n] = '\0';
                if (n > 0) {
                    return 1;
                }
            }
        }
        p += nlen;
    }
    return 0;
}

static int inst_patches_mc_version(const sxcl_json_value *root, char *out, size_t out_len)
{
    const sxcl_json_value *patches = sxcl_json_get(root, "patches");
    if (patches == NULL || sxcl_json_type_of(patches) != SXCL_JSON_ARRAY) {
        return 0;
    }
    const size_t count = sxcl_json_size(patches);
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *patch = sxcl_json_at(patches, i);
        if (patch == NULL || sxcl_json_type_of(patch) != SXCL_JSON_OBJECT) {
            continue;
        }
        const char *pid = sxcl_json_get_string(patch, "id", NULL);
        if (pid == NULL || strcmp(pid, "game") != 0) {
            continue;
        }
        const char *value = sxcl_json_get_string(patch, "version", NULL);
        if (value != NULL && value[0] != '\0') {
            inst_copy(out, out_len, value);
            inst_trim(out);
            if (out[0] != '\0') {
                return 1;
            }
        }
    }
    return 0;
}

/** Python 的 _base_from_json:按 PCL 的顺序推断原版版本号(第二项是"是否可靠")。 */
static void inst_base_from_json(const sxcl_json_value *root, const char *json_text, const char *version_id,
                                char *out, size_t out_len, int *reliable_out)
{
    char value[SXCL_INSTANCE_VERSION_MAX];
    inst_copy(out, out_len, "");
    *reliable_out = 0;

    if (root != NULL && sxcl_json_type_of(root) == SXCL_JSON_OBJECT) {
        /* 1) PCL 的标记:拍平后的 JSON 靠它认祖归宗 */
        const char *client_version = sxcl_json_get_string(root, "clientVersion", NULL);
        if (client_version != NULL && client_version[0] != '\0') {
            inst_copy(value, sizeof(value), client_version);
            inst_trim(value);
            if (value[0] != '\0') {
                inst_copy(out, out_len, value);
                *reliable_out = 1;
                return;
            }
        }
        /* 2) HMCL:patches 里 id=game 的 version(顶层有 "time" 时这条不生效) */
        if (sxcl_json_get(root, "time") == NULL && inst_patches_mc_version(root, value, sizeof(value))) {
            inst_copy(out, out_len, value);
            *reliable_out = 1;
            return;
        }
        /* 3) 标准继承 */
        const char *inherit = sxcl_json_get_string(root, "inheritsFrom", NULL);
        if (inherit != NULL && inherit[0] != '\0') {
            inst_copy(value, sizeof(value), inherit);
            inst_trim(value);
            if (value[0] != '\0') {
                inst_copy(out, out_len, value);
                *reliable_out = 1;
                return;
            }
        }
    }
    /* 4) 新版 Forge 会在参数里写 --fml.mcVersion */
    if (json_text != NULL && inst_fml_mc_version(json_text, value, sizeof(value))) {
        inst_copy(out, out_len, value);
        *reliable_out = 1;
        return;
    }
    /* 5) jar 字段(LiteLoader 常靠它) */
    if (root != NULL) {
        const char *jar = sxcl_json_get_string(root, "jar", NULL);
        if (jar != NULL && jar[0] != '\0') {
            inst_copy(value, sizeof(value), jar);
            inst_trim(value);
            if (value[0] != '\0') {
                inst_copy(out, out_len, value);
                *reliable_out = 1;
                return;
            }
        }
    }
    /* 6/7) 目录名兜底(不可靠) */
    if (version_id != NULL && version_id[0] != '\0') {
        if (inst_scan_version_in_name(version_id, 1, value, sizeof(value)) ||
            inst_scan_version_in_name(version_id, 0, value, sizeof(value))) {
            inst_copy(out, out_len, value);
            *reliable_out = 0;
        }
    }
}

/* ══════════════════════ 5) PCL 兼容读取 ══════════════════════ */

const char *sxcl_instance_pcl_get(const sxcl_instance_pcl_setup *setup, const char *key)
{
    if (setup == NULL || key == NULL) {
        return "";
    }
    for (size_t i = 0; i < setup->stored_count && i < SXCL_INSTANCE_PCL_KEY_MAX; ++i) {
        if (strcmp(setup->keys[i], key) == 0) {
            return setup->values[i];
        }
    }
    return "";
}

int sxcl_instance_read_pcl_setup(const char *game_dir, const char *instance_id,
                                 sxcl_instance_pcl_setup *out, char *err, size_t err_len)
{
    if (game_dir == NULL || instance_id == NULL || out == NULL) {
        inst_err(err, err_len, "参数不合法");
        return SXCL_INSTANCE_ERR_ARG;
    }
    (void)memset(out, 0, sizeof(*out));

    char vdir[SXCL_INSTANCE_PATH_MAX];
    if (inst_version_dir(vdir, sizeof(vdir), game_dir, instance_id) != 0) {
        inst_err(err, err_len, "路径太长,拼不出实例目录");
        return SXCL_INSTANCE_ERR_ARG;
    }
    char path[SXCL_INSTANCE_PATH_MAX + 32];
    if (inst_join(path, sizeof(path), vdir, "PCL/Setup.ini") != 0) {
        inst_err(err, err_len, "路径太长,拼不出 Setup.ini 的路径");
        return SXCL_INSTANCE_ERR_ARG;
    }
    if (!sxcl_fs_exists(path)) {
        inst_err(err, err_len, "这个实例没有 PCL 的 Setup.ini（%s）", path);
        return SXCL_INSTANCE_ERR_IO;
    }
    char *text = NULL;
    size_t len = 0;
    char io_err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_read_file(path, &text, &len, io_err, sizeof(io_err)) != 0) {
        inst_err(err, err_len, "读 PCL/Setup.ini 失败：%s", io_err);
        return SXCL_INSTANCE_ERR_IO;
    }
    out->exists = 1;

    char *line = text;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
            ++next;
        }
        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *key = line;
            char *value = colon + 1;
            inst_trim(key);
            inst_trim(value);
            if (key[0] != '\0') {
                ++out->key_count;
                /* dict 语义:同名键最后一次出现的为准 */
                size_t slot = out->stored_count;
                for (size_t i = 0; i < out->stored_count; ++i) {
                    if (strcmp(out->keys[i], key) == 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < SXCL_INSTANCE_PCL_KEY_MAX) {
                    if (strlen(key) < SXCL_INSTANCE_PCL_KEYLEN &&
                        strlen(value) < SXCL_INSTANCE_PCL_VALLEN) {
                        inst_copy(out->keys[slot], SXCL_INSTANCE_PCL_KEYLEN, key);
                        inst_copy(out->values[slot], SXCL_INSTANCE_PCL_VALLEN, value);
                        if (slot == out->stored_count) {
                            ++out->stored_count;
                        }
                    }
                }
            }
        }
        line = next;
    }
    free(text);
    return SXCL_INSTANCE_OK;
}

int sxcl_instance_pcl_present(const char *game_dir, const char *instance_id)
{
    if (game_dir == NULL || instance_id == NULL) {
        return 0;
    }
    char vdir[SXCL_INSTANCE_PATH_MAX];
    if (inst_version_dir(vdir, sizeof(vdir), game_dir, instance_id) != 0) {
        return 0;
    }
    char path[SXCL_INSTANCE_PATH_MAX + 32];
    if (inst_join(path, sizeof(path), vdir, "PCL/Setup.ini") == 0 && sxcl_fs_exists(path)) {
        return 1;
    }
    if (inst_join(path, sizeof(path), vdir, "PCL") == 0 && sxcl_fs_is_dir(path)) {
        return 1;
    }
    return 0;
}

int sxcl_instance_read_forced_java(const char *game_dir, const char *instance_id,
                                   char *out, size_t out_len, char *err, size_t err_len)
{
    if (game_dir == NULL || instance_id == NULL || out == NULL || out_len == 0) {
        inst_err(err, err_len, "参数不合法");
        return SXCL_INSTANCE_ERR_ARG;
    }
    out[0] = '\0';
    char vdir[SXCL_INSTANCE_PATH_MAX];
    if (inst_version_dir(vdir, sizeof(vdir), game_dir, instance_id) != 0) {
        inst_err(err, err_len, "路径太长,拼不出实例目录");
        return SXCL_INSTANCE_ERR_ARG;
    }
    char path[SXCL_INSTANCE_PATH_MAX + 32];
    if (inst_join(path, sizeof(path), vdir, "PCL/config.json") != 0) {
        inst_err(err, err_len, "路径太长,拼不出 PCL/config.json 的路径");
        return SXCL_INSTANCE_ERR_ARG;
    }
    if (!sxcl_fs_exists(path)) {
        return SXCL_INSTANCE_JAVA_NOT_SET;
    }
    char json_err[SXCL_INSTANCE_ERROR_MAX];
    sxcl_json *doc = sxcl_json_parse_file(path, json_err, sizeof(json_err));
    if (doc == NULL) {
        /* Python 的 _read_instance_config 读不出来就当空 dict:不算错误,只是没有这一项 */
        return SXCL_INSTANCE_JAVA_NOT_SET;
    }
    const char *value = sxcl_json_get_string(sxcl_json_root(doc), "InstanceForcedJava", NULL);
    int rc = SXCL_INSTANCE_JAVA_NOT_SET;
    char java[SXCL_INSTANCE_PATH_MAX];
    java[0] = '\0';
    if (value != NULL && value[0] != '\0') {
        inst_copy(java, sizeof(java), value);
        inst_trim(java);
        if (java[0] == '\0') {
            rc = SXCL_INSTANCE_JAVA_NOT_SET;
        } else if (sxcl_fs_exists(java)) {
            rc = SXCL_INSTANCE_JAVA_OK;
        } else {
            rc = SXCL_INSTANCE_JAVA_MISSING;
        }
    }
    sxcl_json_free(doc);
    if (rc == SXCL_INSTANCE_JAVA_OK) {
        inst_copy(out, out_len, java);
    }
    return rc;
}

/* ══════════════════════ 6) 单实例扫描 ══════════════════════ */

static int inst_is_skip_name(const char *id)
{
    /* PCL 会跳过这几个目录(ModMinecraft.vb:1443-1447);Python 是精确匹配 */
    return (strcmp(id, "cache") == 0 || strcmp(id, "BLClient") == 0 || strcmp(id, "PCL") == 0) ? 1 : 0;
}

static int inst_has_json_suffix(const char *name)
{
    const size_t len = strlen(name);
    if (len < 5) {
        return 0;
    }
    return (inst_lower(name[len - 5]) == '.' && inst_lower(name[len - 4]) == 'j' &&
            inst_lower(name[len - 3]) == 's' && inst_lower(name[len - 2]) == 'o' &&
            inst_lower(name[len - 1]) == 'n')
               ? 1
               : 0;
}

/** Python 的 _pick_version_json:同名 JSON 优先;没有就按 PCL 的规则找任意一个含
 *  mainClass+type+id 的 JSON(目录名与 JSON id 不一致的实例靠它认出来)。 */
static int inst_pick_version_json(const char *version_dir, const char *version_id,
                                  char *out, size_t out_len)
{
    char probe[SXCL_INSTANCE_PATH_MAX + SXCL_INSTANCE_ID_MAX + 16];
    (void)snprintf(probe, sizeof(probe), "%s/%s.json", version_dir, version_id);
    if (sxcl_fs_exists(probe)) {
        inst_copy(out, out_len, probe);
        return 1;
    }
    sxcl_dir_list list;
    char err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_list_open(version_dir, &list, err, sizeof(err)) != 0) {
        return 0;
    }
    /* Python 用 sorted(glob("*.json")),名字升序 */
    for (size_t i = 1; i < list.count; ++i) {
        sxcl_dir_entry key = list.items[i];
        size_t j = i;
        while (j > 0 && sxcl_dir_name_compare(list.items[j - 1].name, key.name) > 0) {
            list.items[j] = list.items[j - 1];
            --j;
        }
        list.items[j] = key;
    }
    int found = 0;
    for (size_t i = 0; i < list.count && !found; ++i) {
        if (list.items[i].is_dir || !inst_has_json_suffix(list.items[i].name)) {
            continue;
        }
        char candidate[SXCL_INSTANCE_PATH_MAX + 320];
        if (inst_join(candidate, sizeof(candidate), version_dir, list.items[i].name) != 0) {
            continue;
        }
        char json_err[SXCL_INSTANCE_ERROR_MAX];
        sxcl_json *doc = sxcl_json_parse_file(candidate, json_err, sizeof(json_err));
        if (doc == NULL) {
            continue;
        }
        const sxcl_json_value *root = sxcl_json_root(doc);
        if (sxcl_json_get(root, "mainClass") != NULL && sxcl_json_get(root, "type") != NULL &&
            sxcl_json_get(root, "id") != NULL) {
            inst_copy(out, out_len, candidate);
            found = 1;
        }
        sxcl_json_free(doc);
    }
    sxcl_dir_list_free(&list);
    return found;
}

/** 扫描一个版本目录并填好一个 sxcl_instance。
 *  返回 SXCL_INSTANCE_OK = 是一条实例;1 = 按规则跳过(PCL 的空目录/不算版本);负数 = 出错。 */
static int inst_scan_one_impl(const char *game_dir, const char *version_id,
                              const sxcl_instance_scan_opts *opts, sxcl_instance *out,
                              char *err, size_t err_len)
{
    (void)memset(out, 0, sizeof(*out));
    inst_copy(out->id, sizeof(out->id), version_id);

    char vdir[SXCL_INSTANCE_PATH_MAX];
    if (inst_version_dir(vdir, sizeof(vdir), game_dir, version_id) != 0) {
        inst_err(err, err_len, "路径太长,拼不出实例目录");
        return SXCL_INSTANCE_ERR_ARG;
    }
    if (!sxcl_fs_is_dir(vdir)) {
        inst_err(err, err_len, "实例目录不存在（%s）", vdir);
        return SXCL_INSTANCE_ERR_IO;
    }

    /* 空文件夹跳过(PCL 也跳过):目录里必须至少有一个普通文件 */
    sxcl_dir_list files;
    char io_err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_list_open(vdir, &files, io_err, sizeof(io_err)) != 0) {
        inst_err(err, err_len, "实例目录读不出来：%s", io_err);
        return SXCL_INSTANCE_ERR_IO;
    }
    size_t file_count = 0;
    for (size_t i = 0; i < files.count; ++i) {
        if (!files.items[i].is_dir) {
            ++file_count;
        }
    }
    sxcl_dir_list_free(&files);
    if (file_count == 0) {
        inst_err(err, err_len, "空文件夹,不算版本");
        return 1;
    }

    char jar_path[SXCL_INSTANCE_PATH_MAX + 320];
    (void)snprintf(jar_path, sizeof(jar_path), "%s/%s.jar", vdir, version_id);
    out->has_jar = sxcl_fs_exists(jar_path) ? 1 : 0;

    char json_path[SXCL_INSTANCE_PATH_MAX] = { 0 };
    const int have_json = inst_pick_version_json(vdir, version_id, json_path, sizeof(json_path));
    if (!have_json && !out->has_jar) {
        inst_err(err, err_len, "连 JSON 和 jar 都没有,不算版本");
        return 1;
    }
    if (!have_json && inst_is_skip_name(version_id)) {
        inst_err(err, err_len, "PCL 会跳过的目录名（%s）", version_id);
        return 1;
    }
    out->has_json = have_json;

    sxcl_json *doc = NULL;
    char *json_text = NULL;
    size_t json_len = 0;
    if (have_json) {
        inst_copy(out->json_path, sizeof(out->json_path), json_path);
        if (sxcl_dir_read_file(json_path, &json_text, &json_len, io_err, sizeof(io_err)) != 0) {
            out->broken = 1;
        } else {
            char parse_err[SXCL_INSTANCE_ERROR_MAX];
            doc = sxcl_json_parse(json_text, json_len, parse_err, sizeof(parse_err));
            if (doc == NULL) {
                out->broken = 1;
            }
        }
    }
    const sxcl_json_value *root = (doc != NULL) ? sxcl_json_root(doc) : NULL;
    if (doc != NULL && sxcl_json_get(root, "mainClass") == NULL) {
        out->broken = 1; /* PCL 的硬性门槛:没有 mainClass 不算版本 */
    }

    sxcl_instance_pcl_setup setup;
    (void)memset(&setup, 0, sizeof(setup));
    if (opts == NULL || opts->skip_pcl == 0) {
        char pcl_err[SXCL_INSTANCE_ERROR_MAX];
        if (sxcl_instance_read_pcl_setup(game_dir, version_id, &setup, pcl_err, sizeof(pcl_err)) !=
            SXCL_INSTANCE_OK) {
            (void)memset(&setup, 0, sizeof(setup));
        }
    }
    out->pcl_present = (setup.exists || sxcl_instance_pcl_present(game_dir, version_id)) ? 1 : 0;
    inst_copy(out->pcl_state, sizeof(out->pcl_state), sxcl_instance_pcl_get(&setup, "State"));

    if (opts == NULL || opts->skip_pcl == 0) {
        const char *logo_custom = sxcl_instance_pcl_get(&setup, "LogoCustom");
        if (logo_custom[0] != '\0' && (inst_ci_equal(logo_custom, "true") || strcmp(logo_custom, "1") == 0)) {
            char logo[SXCL_INSTANCE_PATH_MAX + 32];
            if (inst_join(logo, sizeof(logo), vdir, "PCL/Logo.png") == 0 && sxcl_fs_exists(logo)) {
                inst_copy(out->custom_logo, sizeof(out->custom_logo), logo);
                out->has_custom_logo = 1;
            }
        }
    }

    inst_base_from_json(root, json_text, version_id, out->base_version, sizeof(out->base_version),
                        &out->base_reliable);

    inst_detect_loaders(version_id, root, json_text, &setup, out->loaders, &out->loader_count);

    /* 加载器版本号去掉原版前缀:"1.20.1-47.2.0" -> "47.2.0" */
    if (out->base_version[0] != '\0') {
        const size_t base_len = strlen(out->base_version);
        for (size_t i = 0; i < out->loader_count; ++i) {
            char *version = out->loaders[i].version;
            if (strncmp(version, out->base_version, base_len) == 0 && version[base_len] == '-') {
                const char *rest = version + base_len + 1;
                (void)memmove(version, rest, strlen(rest) + 1);
            }
        }
    }

    if (root != NULL) {
        inst_copy(out->version_type, sizeof(out->version_type), sxcl_json_get_string(root, "type", ""));
        const char *inherit = sxcl_json_get_string(root, "inheritsFrom", NULL);
        if (inherit != NULL && inherit[0] != '\0') {
            inst_copy(out->inherits_from, sizeof(out->inherits_from), inherit);
            inst_trim(out->inherits_from);
        }
        const char *json_id = sxcl_json_get_string(root, "id", NULL);
        if (json_id != NULL) {
            inst_copy(out->json_id, sizeof(out->json_id), json_id);
            inst_trim(out->json_id);
            if (out->json_id[0] != '\0' && strcmp(out->json_id, version_id) != 0) {
                out->json_id_mismatch = 1;
            }
        }
        /* 前置版本在不在(PCL:需要安装 XXX 作为前置版本) */
        if (out->inherits_from[0] != '\0') {
            char parent[SXCL_INSTANCE_PATH_MAX];
            char parent_json[SXCL_INSTANCE_PATH_MAX + 320];
            if (inst_version_dir(parent, sizeof(parent), game_dir, out->inherits_from) == 0) {
                (void)snprintf(parent_json, sizeof(parent_json), "%s/%s.json", parent, out->inherits_from);
                if (!sxcl_fs_exists(parent_json)) {
                    inst_copy(out->missing_parent, sizeof(out->missing_parent), out->inherits_from);
                }
            }
        }
        /* 是哪个启动器装的:HMCL 的 patches,或 PCL 的痕迹 */
        const sxcl_json_value *patches = sxcl_json_get(root, "patches");
        if (patches != NULL && sxcl_json_type_of(patches) == SXCL_JSON_ARRAY &&
            sxcl_json_get(root, "time") == NULL) {
            inst_copy(out->launcher, sizeof(out->launcher), "hmcl");
        } else if (out->pcl_present) {
            inst_copy(out->launcher, sizeof(out->launcher), "pcl");
        }
    } else if (out->pcl_present) {
        inst_copy(out->launcher, sizeof(out->launcher), "pcl");
    }

    if ((opts == NULL || opts->skip_forced_java == 0) && out->pcl_present) {
        char java_err[SXCL_INSTANCE_ERROR_MAX];
        out->forced_java_state = sxcl_instance_read_forced_java(game_dir, version_id, out->forced_java,
                                                                sizeof(out->forced_java), java_err,
                                                                sizeof(java_err));
        if (out->forced_java_state < 0) {
            out->forced_java_state = SXCL_INSTANCE_JAVA_NOT_SET;
            out->forced_java[0] = '\0';
        }
    } else {
        out->forced_java_state = SXCL_INSTANCE_JAVA_NOT_SET;
    }

    free(json_text);
    sxcl_json_free(doc);
    (void)sxcl_instance_refresh(out);
    return SXCL_INSTANCE_OK;
}

/* ══════════════════════ 7) 汇总 / 描述 / 可启动判定 ══════════════════════ */

int sxcl_instance_refresh(sxcl_instance *instance)
{
    if (instance == NULL) {
        return SXCL_INSTANCE_ERR_ARG;
    }
    /* summary:Python 的 InstalledVersion.summary() */
    if (instance->loader_count == 0) {
        inst_copy(instance->summary, sizeof(instance->summary), "原版");
    } else {
        size_t used = 0;
        instance->summary[0] = '\0';
        for (size_t i = 0; i < instance->loader_count && i < SXCL_INSTANCE_MAX_LOADERS; ++i) {
            const char *name = sxcl_instance_kind_name(instance->loaders[i].kind);
            const char *version = instance->loaders[i].version;
            const int n = snprintf(instance->summary + used, sizeof(instance->summary) - used,
                                   (used == 0) ? "%s%s%s" : " + %s%s%s", name,
                                   (version[0] != '\0') ? " " : "", version);
            if (n < 0 || (size_t)n >= sizeof(instance->summary) - used) {
                break;
            }
            used += (size_t)n;
        }
    }

    /* 问题码优先级:坏 JSON -> 缺前置 -> 缺 JSON -> 目录名/JSON id 不一致 -> 原版缺 jar */
    sxcl_instance_problem problem = SXCL_INSTANCE_PROBLEM_NONE;
    if (instance->broken) {
        problem = SXCL_INSTANCE_PROBLEM_BAD_JSON;
    } else if (instance->missing_parent[0] != '\0') {
        problem = SXCL_INSTANCE_PROBLEM_MISSING_PARENT;
    } else if (!instance->has_json) {
        problem = SXCL_INSTANCE_PROBLEM_NO_JSON;
    } else if (instance->json_id_mismatch) {
        problem = SXCL_INSTANCE_PROBLEM_ID_MISMATCH;
    } else if (instance->loader_count == 0 && instance->inherits_from[0] == '\0' && !instance->has_jar) {
        problem = SXCL_INSTANCE_PROBLEM_MISSING_JAR;
    }
    instance->problem_code = problem;
    instance->launchable = (problem == SXCL_INSTANCE_PROBLEM_NONE) ? 1 : 0;
    instance->problem[0] = '\0';
    if (problem != SXCL_INSTANCE_PROBLEM_NONE) {
        if (problem == SXCL_INSTANCE_PROBLEM_MISSING_PARENT) {
            (void)snprintf(instance->problem, sizeof(instance->problem), "需要安装 %s 作为前置版本",
                           instance->missing_parent);
        } else if (problem == SXCL_INSTANCE_PROBLEM_MISSING_JAR) {
            (void)snprintf(instance->problem, sizeof(instance->problem), "缺少客户端 jar（%s.jar）",
                           instance->id);
        } else if (problem == SXCL_INSTANCE_PROBLEM_ID_MISMATCH) {
            (void)snprintf(instance->problem, sizeof(instance->problem),
                           "版本目录名与版本 JSON 的 id 不一致（JSON 里的 id 是 %s），启动器按目录名找不到它",
                           (instance->json_id[0] != '\0') ? instance->json_id : "空");
        } else {
            inst_copy(instance->problem, sizeof(instance->problem),
                      sxcl_instance_problem_default_text(problem));
        }
    }

    /* describe:"<id>（<原版>，<summary>，可启动 / 原因）" */
    (void)snprintf(instance->describe, sizeof(instance->describe), "%s（%s，%s，%s）", instance->id,
                   (instance->base_version[0] != '\0') ? instance->base_version : "原版未知",
                   instance->summary, instance->launchable ? "可启动" : instance->problem);
    return SXCL_INSTANCE_OK;
}

/* ══════════════════════ 8) 列表扫描 ══════════════════════ */

void sxcl_instance_list_free(sxcl_instance_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->dropped = 0;
    list->truncated = 0;
    list->launchable_count = 0;
    list->problem_count = 0;
    list->vanilla_count = 0;
    list->with_loader_count = 0;
    list->game_dir_exists = 0;
}

int sxcl_instance_scan(const char *game_dir, const sxcl_instance_scan_opts *opts,
                       sxcl_instance_list *out, char *err, size_t err_len)
{
    if (out == NULL) {
        inst_err(err, err_len, "参数不合法（out 不能为空）");
        return SXCL_INSTANCE_ERR_ARG;
    }
    (void)memset(out, 0, sizeof(*out));
    if (game_dir == NULL || game_dir[0] == '\0') {
        inst_err(err, err_len, "没有指定游戏目录");
        return SXCL_INSTANCE_ERR_ARG;
    }
    if (!sxcl_fs_is_dir(game_dir)) {
        inst_err(err, err_len, "游戏目录不存在（%s）：请在设置里指定 .minecraft 目录", game_dir);
        return SXCL_INSTANCE_OK; /* 空态,不是错误 */
    }
    out->game_dir_exists = 1;

    char versions_dir[SXCL_INSTANCE_PATH_MAX + 16];
    if (inst_join(versions_dir, sizeof(versions_dir), game_dir, "versions") != 0) {
        inst_err(err, err_len, "游戏目录路径太长");
        return SXCL_INSTANCE_ERR_ARG;
    }
    if (!sxcl_fs_is_dir(versions_dir)) {
        inst_err(err, err_len, "这个游戏目录里没有 versions 文件夹（%s）", game_dir);
        return SXCL_INSTANCE_OK;
    }

    sxcl_dir_list dirs;
    char io_err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_list_open(versions_dir, &dirs, io_err, sizeof(io_err)) != 0) {
        inst_err(err, err_len, "versions 文件夹读不出来：%s", io_err);
        return SXCL_INSTANCE_ERR_IO;
    }
    /* Python: for version_dir in sorted(versions_dir.iterdir()) */
    for (size_t i = 1; i < dirs.count; ++i) {
        sxcl_dir_entry key = dirs.items[i];
        size_t j = i;
        while (j > 0 && sxcl_dir_name_compare(dirs.items[j - 1].name, key.name) > 0) {
            dirs.items[j] = dirs.items[j - 1];
            --j;
        }
        dirs.items[j] = key;
    }

    size_t cap = (opts != NULL && opts->max_instances > 0) ? opts->max_instances
                                                           : (size_t)SXCL_INSTANCE_DEFAULT_CAP;
    if (cap > 8192u) {
        cap = 8192u;
    }
    sxcl_instance *items = (sxcl_instance *)malloc(cap * sizeof(sxcl_instance));
    if (items == NULL) {
        sxcl_dir_list_free(&dirs);
        inst_err(err, err_len, "内存不足（%llu 条实例）", (unsigned long long)cap);
        return SXCL_INSTANCE_ERR_NOMEM;
    }
    size_t count = 0;
    for (size_t i = 0; i < dirs.count; ++i) {
        if (!dirs.items[i].is_dir) {
            continue;
        }
        char one_err[SXCL_INSTANCE_ERROR_MAX];
        sxcl_instance instance;
        const int rc = inst_scan_one_impl(game_dir, dirs.items[i].name, opts, &instance, one_err,
                                          sizeof(one_err));
        if (rc != SXCL_INSTANCE_OK) {
            continue; /* 跳过 / 单条读不出来:不影响其它实例(Python 也是 continue) */
        }
        if (count >= cap) {
            ++out->dropped;
            continue;
        }
        items[count++] = instance;
    }
    sxcl_dir_list_free(&dirs);

    out->items = items;
    out->count = count;
    out->truncated = (out->dropped > 0) ? 1 : 0;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].launchable) {
            ++out->launchable_count;
        } else {
            ++out->problem_count;
        }
        if (items[i].loader_count == 0) {
            ++out->vanilla_count;
        } else {
            ++out->with_loader_count;
        }
    }
    return SXCL_INSTANCE_OK;
}

int sxcl_instance_scan_one(const char *game_dir, const char *instance_id, sxcl_instance *out,
                           char *err, size_t err_len)
{
    if (game_dir == NULL || instance_id == NULL || instance_id[0] == '\0' || out == NULL) {
        inst_err(err, err_len, "参数不合法");
        return SXCL_INSTANCE_ERR_ARG;
    }
    const int rc = inst_scan_one_impl(game_dir, instance_id, NULL, out, err, err_len);
    if (rc == 1) {
        inst_err(err, err_len, "这个目录不算一个版本（%s）", instance_id);
        return SXCL_INSTANCE_ERR_IO;
    }
    return rc;
}

const sxcl_instance *sxcl_instance_find(const sxcl_instance_list *list, const char *instance_id)
{
    if (list == NULL || instance_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].id, instance_id) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

size_t sxcl_instance_count_for_base(const sxcl_instance_list *list, const char *base_version)
{
    if (list == NULL || base_version == NULL) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < list->count; ++i) {
        const char *base = (list->items[i].base_version[0] != '\0') ? list->items[i].base_version
                                                                   : list->items[i].id;
        if (strcmp(base, base_version) == 0) {
            ++count;
        }
    }
    return count;
}

size_t sxcl_instance_to_loader_installed(const sxcl_instance *instance, sxcl_loader_installed *out,
                                         size_t out_cap)
{
    if (instance == NULL) {
        return 0;
    }
    size_t count = 0;
    if (instance->loader_count == 0) {
        if (out != NULL && count < out_cap) {
            out[count].instance_id = instance->id;
            out[count].base_version = instance->base_version;
            out[count].loader_id = "vanilla";
            out[count].loader_version = "";
        }
        return 1;
    }
    for (size_t i = 0; i < instance->loader_count; ++i) {
        if (instance->loaders[i].kind == SXCL_INSTANCE_LITELOADER) {
            continue; /* loader.h 没有 LiteLoader */
        }
        if (out != NULL && count < out_cap) {
            out[count].instance_id = instance->id;
            out[count].base_version = instance->base_version;
            out[count].loader_id = sxcl_instance_kind_id(instance->loaders[i].kind);
            out[count].loader_version = instance->loaders[i].version;
        }
        ++count;
    }
    return count;
}

sxcl_json *sxcl_instance_read_json(const char *game_dir, const char *instance_id,
                                   char *json_path_out, size_t json_path_len,
                                   char *err, size_t err_len)
{
    if (json_path_out != NULL && json_path_len > 0) {
        json_path_out[0] = '\0';
    }
    if (game_dir == NULL || instance_id == NULL || instance_id[0] == '\0') {
        inst_err(err, err_len, "参数不合法");
        return NULL;
    }
    char vdir[SXCL_INSTANCE_PATH_MAX];
    if (inst_version_dir(vdir, sizeof(vdir), game_dir, instance_id) != 0) {
        inst_err(err, err_len, "路径太长,拼不出实例目录");
        return NULL;
    }
    char json_path[SXCL_INSTANCE_PATH_MAX];
    if (!inst_pick_version_json(vdir, instance_id, json_path, sizeof(json_path))) {
        inst_err(err, err_len, "找不到版本 JSON（%s/%s.json）", vdir, instance_id);
        return NULL;
    }
    if (json_path_out != NULL && json_path_len > 0) {
        inst_copy(json_path_out, json_path_len, json_path);
    }
    char parse_err[SXCL_INSTANCE_ERROR_MAX];
    sxcl_json *doc = sxcl_json_parse_file(json_path, parse_err, sizeof(parse_err));
    if (doc == NULL) {
        inst_err(err, err_len, "版本 JSON 解析失败：%s（%s）", parse_err, json_path);
        return NULL;
    }
    return doc;
}
