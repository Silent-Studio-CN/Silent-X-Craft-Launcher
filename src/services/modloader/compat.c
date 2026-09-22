/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1  /* vsnprintf 之外还用了几处 C 串函数,MSVC 默认标弃用 */

#include "sxcl/loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 小工具 ── */

/* 定长拷贝:能装下返回 0,装不下(会截断)返回 -1 —— 调用方据此报 SXCL_LOADER_ERR_SPACE。 */
static int copy_cap(char *dst, size_t cap, const char *src)
{
    const size_t len = src ? strlen(src) : 0;
    if (!dst || cap == 0) {
        return -1;
    }
    if (len + 1 > cap) {
        dst[0] = '\0';
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static int copy_range(char *dst, size_t cap, const char *begin, size_t len)
{
    if (!dst || cap == 0 || len + 1 > cap) {
        if (dst && cap > 0) {
            dst[0] = '\0';
        }
        return -1;
    }
    memcpy(dst, begin, len);
    dst[len] = '\0';
    return 0;
}

static int chr_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

/* 大小写不敏感的"从 hay 的 pos 处开始是否等于 needle(全小写)"。 */
static int ci_match_at(const char *hay, size_t pos, const char *needle)
{
    for (size_t i = 0; needle[i]; ++i) {
        if (!hay[pos + i] || chr_lower((unsigned char)hay[pos + i]) != (unsigned char)needle[i]) {
            return 0;
        }
    }
    return 1;
}

/* 大小写不敏感地找 needle(全小写);找不到返回 NULL。 */
static const char *ci_find(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) {
        return NULL;
    }
    for (size_t i = 0; hay[i]; ++i) {
        if (ci_match_at(hay, i, needle)) {
            return hay + i;
        }
    }
    return NULL;
}

static int is_digit_c(char c)
{
    return c >= '0' && c <= '9';
}

static int is_alpha_c(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void trim_inplace(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' ||
                       text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
    size_t head = 0;
    while (text[head] == ' ' || text[head] == '\t') {
        ++head;
    }
    if (head > 0) {
        memmove(text, text + head, strlen(text + head) + 1);
    }
}

/* ── 加载器类型 ── */

static const char *const KIND_IDS[] = {"vanilla", "forge", "neoforge", "fabric", "quilt", "optifine"};
static const char *const KIND_NAMES[] = {"原版", "Forge", "NeoForge", "Fabric", "Quilt", "OptiFine"};

const char *sxcl_loader_kind_id(sxcl_loader_kind kind)
{
    if ((int)kind < 0 || (size_t)(int)kind >= sizeof(KIND_IDS) / sizeof(KIND_IDS[0])) {
        return KIND_IDS[0];
    }
    return KIND_IDS[(int)kind];
}

const char *sxcl_loader_kind_name(sxcl_loader_kind kind)
{
    if ((int)kind < 0 || (size_t)(int)kind >= sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0])) {
        return KIND_NAMES[0];
    }
    return KIND_NAMES[(int)kind];
}

sxcl_loader_kind sxcl_loader_kind_from_id(const char *id)
{
    if (!id) {
        return SXCL_LOADER_VANILLA;
    }
    for (size_t i = 0; i < sizeof(KIND_IDS) / sizeof(KIND_IDS[0]); ++i) {
        const char *want = KIND_IDS[i];
        size_t k = 0;
        for (; want[k]; ++k) {
            if (chr_lower((unsigned char)id[k]) != (unsigned char)want[k]) {
                break;
            }
        }
        if (want[k] == '\0' && id[k] == '\0') {
            return (sxcl_loader_kind)i;
        }
    }
    return SXCL_LOADER_VANILLA;
}

sxcl_loader_installer_format sxcl_loader_installer_format_of(const char *profile_json, size_t len)
{
    if (profile_json == NULL || len == 0) {
        return SXCL_LOADER_INSTALLER_UNKNOWN;
    }
    char perr[128];
    perr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(profile_json, len, perr, sizeof(perr));
    if (doc == NULL) {
        return SXCL_LOADER_INSTALLER_UNKNOWN;   /* 不是 JSON = 判不出来(调用方会当"没法装") */
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    int format = SXCL_LOADER_INSTALLER_UNKNOWN;
    if (root != NULL && sxcl_json_type_of(root) == SXCL_JSON_OBJECT) {
        const int modern = sxcl_json_get_int64(root, "spec", 0) >= 1 ||
                           sxcl_json_size(sxcl_json_get(root, "processors")) > 0 ||
                           sxcl_json_get_string(root, "json", "")[0] != '\0';
        const int legacy = sxcl_json_get(root, "install") != NULL ||
                           sxcl_json_get(root, "versionInfo") != NULL;
        if (modern) {
            format = SXCL_LOADER_INSTALLER_MODERN;
        } else if (legacy) {
            format = SXCL_LOADER_INSTALLER_LEGACY;
        }
    }
    sxcl_json_free(doc);
    return (sxcl_loader_installer_format)format;
}

const char *sxcl_loader_installer_format_name(sxcl_loader_installer_format format)
{
    switch (format) {
    case SXCL_LOADER_INSTALLER_MODERN:
        return "1.13+ 新格式(spec/processors)";
    case SXCL_LOADER_INSTALLER_LEGACY:
        return "1.12- 老格式(install/versionInfo)";
    default:
        return "认不出来(没有 install_profile.json)";
    }
}

int sxcl_loader_kind_implemented(sxcl_loader_kind kind)
{
    /* 2026-09-22 晚:**Quilt 放开了**(docs/22 的 B7)。它本来就只差这一道门:
     * 类型/元数据(catalog 走 meta.quiltmc.org)/命令行(与 Fabric 同一条 -mcversion/-loader/-dir)
     * 全都在,Quilt 官方安装器是 Fabric 安装器的 fork,参数一模一样。
     * 真机验收见 docs/22 §10(装了 1.20.1 + Quilt,版本 JSON 能独立启动那条口径照旧)。
     * 原版(VANILLA)仍然不走"静默安装加载器"这条路。 */
    return kind == SXCL_LOADER_FORGE || kind == SXCL_LOADER_NEOFORGE ||
           kind == SXCL_LOADER_FABRIC || kind == SXCL_LOADER_QUILT ||
           kind == SXCL_LOADER_OPTIFINE;
}

/* ── 版本串解析 ── */

/* 匹配一段"像原版版本号"的文本(等价于 loaders.py 的
 *   MC_VERSION_RE = (?<![\w.\-])((?:1\.\d+|[ab]\d+\.\d+)(?:\.\d+)?)(?![\w.]) ):
 *   1.<数字>[.<数字>]   或   [ab]<数字>.<数字>[.<数字>]
 * 成功返回 1 并把结束位置写进 *end。 */
static int mc_token_at(const char *s, size_t pos, size_t *end)
{
    size_t p = pos;
    if (s[p] == 'a' || s[p] == 'b' || s[p] == 'A' || s[p] == 'B') {
        size_t q = p + 1;
        const size_t d0 = q;
        while (is_digit_c(s[q])) {
            ++q;
        }
        if (q == d0 || s[q] != '.') {
            return 0;
        }
        ++q;
        const size_t d1 = q;
        while (is_digit_c(s[q])) {
            ++q;
        }
        if (q == d1) {
            return 0;
        }
        if (s[q] == '.' && is_digit_c(s[q + 1])) {
            ++q;
            while (is_digit_c(s[q])) {
                ++q;
            }
        }
        *end = q;
        return 1;
    }
    if (s[p] == '1' && s[p + 1] == '.') {
        size_t q = p + 2;
        const size_t d0 = q;
        while (is_digit_c(s[q])) {
            ++q;
        }
        if (q == d0) {
            return 0;
        }
        if (s[q] == '.' && is_digit_c(s[q + 1])) {
            ++q;
            while (is_digit_c(s[q])) {
                ++q;
            }
        }
        *end = q;
        return 1;
    }
    return 0;
}

/* 前边界:串首,或前一个字符不是 [A-Za-z0-9._]。
 * 这里比 Python 的正则**宽松一点**:它的 lookbehind (?<![\w.\-]) 会把 '-' 排除,
 * 于是 "forge-1.20.1-47.2.0" 这种带前缀的串根本认不出来;我们要认(任务要求覆盖前缀/后缀)。 */
static int boundary_before(const char *s, size_t pos)
{
    if (pos == 0) {
        return 1;
    }
    const char c = s[pos - 1];
    return !(is_digit_c(c) || is_alpha_c(c) || c == '_' || c == '.');
}

/* 后边界:不能紧跟字母/数字/下划线/点(否则 "1.20.1b"、"1.20.1.5" 会被当成版本号)。 */
static int boundary_after(const char *s, size_t end)
{
    const char c = s[end];
    return !(is_digit_c(c) || is_alpha_c(c) || c == '_' || c == '.');
}

static int loader_token_char(char c)
{
    return is_digit_c(c) || is_alpha_c(c) || c == '.' || c == '_' || c == '+';
}

int sxcl_loader_parse_version(const char *text, sxcl_loader_kind kind, sxcl_loader_version_info *out)
{
    if (!out) {
        return SXCL_LOADER_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!text || !text[0]) {
        return SXCL_LOADER_OK;
    }

    /* 1) 找出原版版本号那一段(整串里第一段"像 1.x / a1.x / b1.x"的文本)。
     *    找到之后,MC 段之后**第一个数字开头的 token** 就是加载器自己的版本:
     *      "1.20.1-47.2.0"       -> mc=1.20.1 loader=47.2.0
     *      "1.12.2-14.23.5.2860" -> mc=1.12.2 loader=14.23.5.2860
     *      "1.20.1-OptiFine_HD_U_I6" -> mc=1.20.1 loader=(空,OptiFine 的版本不是数字开头的 token)
     */
    for (size_t i = 0; text[i]; ++i) {
        size_t end = 0;
        if (!mc_token_at(text, i, &end)) {
            continue;
        }
        if (!boundary_before(text, i) || !boundary_after(text, end)) {
            continue;
        }
        if (copy_range(out->mc, sizeof(out->mc), text + i, end - i) != 0) {
            return SXCL_LOADER_ERR_SPACE;
        }
        for (size_t p = end; text[p];) {
            if (!loader_token_char(text[p])) {
                ++p;
                continue;
            }
            const size_t t0 = p;
            while (loader_token_char(text[p])) {
                ++p;
            }
            if (is_digit_c(text[t0])) {
                (void)copy_range(out->loader, sizeof(out->loader), text + t0, p - t0);
                break;
            }
        }
        out->ok = 1;
        return SXCL_LOADER_OK;
    }

    /* 2) 整串里没有 MC 段:只有 NeoForge 才有另一套写法(它用 <大版本>.<次版本>... 当版本号)。
     *    Python: NeoForgeAPI._parse_version -> "21.1.72" 属于 MC 1.21.1,而 mc_version 用
     *    _mc_key 的 "21.1" 形式;这里直接给人类看的 "1.<major>.<minor>"。
     */
    if (kind == SXCL_LOADER_NEOFORGE) {
        size_t p = 0;
        while (is_alpha_c(text[p]) || text[p] == '-' || text[p] == '_') {
            ++p;   /* 允许 "neoforge-21.1.72" 这种前缀 */
        }
        const size_t m0 = p;
        while (is_digit_c(text[p])) {
            ++p;
        }
        const size_t major_len = p - m0;
        if (major_len == 0 || major_len > 4 || text[p] != '.') {
            return SXCL_LOADER_OK;
        }
        ++p;
        const size_t n0 = p;
        while (is_digit_c(text[p])) {
            ++p;
        }
        const size_t minor_len = p - n0;
        if (minor_len == 0 || minor_len > 4 || text[p] != '.') {
            return SXCL_LOADER_OK;
        }
        /* ★ 特例(实测踩过,不是猜):NeoForge 1.20.1 那一代**从 Forge 47.x 分叉**,
         *   版本号就是 47.1.x —— 按下面那条 "1.<大>.<次>" 规则会被推成 "1.47.1",
         *   于是"47.1.5 对应的是 Minecraft 1.47.1"这种判决就出来了(加载器兼容判定直接判 error)。
         *   47.x 这一个前缀有确定含义:NeoForge for Minecraft 1.20.1。
         *   其余世代(20.2/20.4/21.0/21.1/26.3…)仍走通用规则。 */
        char mc[32];
        const int legacy_47 = (major_len == 2 && text[m0] == '4' && text[m0 + 1] == '7');
        const int written = legacy_47
                                ? snprintf(mc, sizeof(mc), "1.20.1")
                                : snprintf(mc, sizeof(mc), "1.%.*s.%.*s", (int)major_len,
                                           text + m0, (int)minor_len, text + n0);
        if (written <= 0 || (size_t)written >= sizeof(mc)) {
            return SXCL_LOADER_ERR_SPACE;
        }
        (void)copy_cap(out->mc, sizeof(out->mc), mc);
        (void)copy_cap(out->loader, sizeof(out->loader), text + m0);
        out->ok = 1;
    }
    return SXCL_LOADER_OK;
}

int sxcl_loader_parse_forge_requirement(const char *text, char *out, size_t out_len)
{
    /* Python: _FORGE_REQ = re.compile(r"forge\s*([0-9][0-9A-Za-z.\-]*)", re.I)
     * "Forge 61.0.6" -> "61.0.6";取不到给空串(不算错误)。 */
    if (!out || out_len == 0) {
        return SXCL_LOADER_ERR_ARG;
    }
    out[0] = '\0';
    const char *hit = ci_find(text, "forge");
    if (!hit) {
        return SXCL_LOADER_OK;
    }
    const size_t pos = (size_t)(hit - text) + 5;
    size_t p = pos;
    while (text[p] == ' ' || text[p] == '\t') {
        ++p;
    }
    if (!is_digit_c(text[p])) {
        return SXCL_LOADER_OK;
    }
    const size_t start = p;
    while (is_digit_c(text[p]) || is_alpha_c(text[p]) || text[p] == '.' || text[p] == '-') {
        ++p;
    }
    (void)copy_range(out, out_len, text + start, p - start);
    return SXCL_LOADER_OK;
}

/* ── 兼容性判定 ── */

const char *sxcl_loader_issue_level_name(sxcl_loader_issue_level level)
{
    return level == SXCL_LOADER_ISSUE_ERROR ? "error" : "warn";
}

int sxcl_loader_issues_has_error(const sxcl_loader_issues *issues)
{
    if (!issues) {
        return 0;
    }
    for (size_t i = 0; i < issues->count; ++i) {
        if (issues->items[i].level == SXCL_LOADER_ISSUE_ERROR) {
            return 1;
        }
    }
    return 0;
}

static void add_issue(sxcl_loader_issues *out, sxcl_loader_issue_level level,
                      const char *fix, const char *fmt, ...)
{
    if (out->count >= SXCL_LOADER_MAX_ISSUES) {
        ++out->dropped;
        return;
    }
    sxcl_loader_issue *item = &out->items[out->count++];
    item->level = level;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(item->message, sizeof(item->message), fmt, ap);
    va_end(ap);
    item->fix[0] = '\0';
    if (fix) {
        (void)copy_cap(item->fix, sizeof(item->fix), fix);
    }
}

int sxcl_loader_check_selection(const sxcl_loader_selection *sel, sxcl_loader_issues *out)
{
    if (!sel || !out) {
        return SXCL_LOADER_ERR_ARG;
    }
    out->count = 0;
    out->dropped = 0;

    /* Python: "没选加载器 = 只装原版:加载器列表取没取到都不影响,别拦着用户装原版"。 */
    if (sel->kind == SXCL_LOADER_VANILLA) {
        return SXCL_LOADER_OK;
    }

    const char *name = sxcl_loader_kind_name(sel->kind);
    const char *base = sel->base_version ? sel->base_version : "";
    const char *want = sel->loader_version ? sel->loader_version : "";
    const char *self_id = sxcl_loader_kind_id(sel->kind);

    /* 1) 选中的那个加载器,列表没取到 -> 状态未知,不许装。 */
    if (sel->list_failed) {
        add_issue(out, SXCL_LOADER_ISSUE_ERROR, "检查网络后点右上角的刷新重试",
                  "%s 的版本列表没取到，无法确认能不能装", name);
    }

    /* 2) 没实现的加载器(Quilt / LiteLoader)。 */
    if (!sxcl_loader_kind_implemented(sel->kind)) {
        add_issue(out, SXCL_LOADER_ISSUE_ERROR, "先选 Forge / NeoForge / Fabric / OptiFine",
                  "还不支持安装 %s", name);
    }

    /* 3) 这个原版版本下该加载器一个版本都没有(例如 1.12.1 没有 NeoForge)。 */
    if (sel->available_count == 0) {
        add_issue(out, SXCL_LOADER_ISSUE_ERROR, "换一个游戏版本，或换别的加载器",
                  "%s 没有可用的 %s 版本", base, name);
    }

    /* 4) 选了版本但不在列表里(列表刷新过 / 手改过)。
     *    Python 用"整份列表 JSON 序列化后做包含判断";这里列表已经是版本串数组,
     *    所以对每个条目做子串包含 —— 对 Fabric 那种嵌套结构同样是"列表里出现过就算数"。 */
    if (want[0] && sel->available_count > 0 && sel->available_versions) {
        int found = 0;
        for (size_t i = 0; i < sel->available_count; ++i) {
            const char *entry = sel->available_versions[i];
            if (entry && strstr(entry, want)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            add_issue(out, SXCL_LOADER_ISSUE_ERROR, "重新选一个版本",
                      "选中的 %s 版本（%s）不在可用列表里", name, want);
        }
    }

    /* 5) 加载器版本串里写明了它配套的 MC 版本,却和选中的原版不一致
     *    (Forge 的 "1.20.1-47.2.0";NeoForge 的 "21.1.72")。
     *    列表取不到时这条也能兜住"版本与游戏版本不匹配"。 */
    if (want[0] && (sel->kind == SXCL_LOADER_FORGE || sel->kind == SXCL_LOADER_NEOFORGE)) {
        sxcl_loader_version_info info;
        if (sxcl_loader_parse_version(want, sel->kind, &info) == SXCL_LOADER_OK &&
            info.ok && info.mc[0] && base[0] && strcmp(info.mc, base) != 0) {
            add_issue(out, SXCL_LOADER_ISSUE_ERROR, "换成与它配套的原版版本，或者换一个该版本下的加载器",
                      "加载器版本 %s 对应的是 Minecraft %s，不是 %s", want, info.mc, base);
        }
    }

    /* 6) 同名实例已经存在:同一个加载器 = 覆盖提醒(warn);别的加载器/原版 = 别覆盖(error)。 */
    if (sel->instance_name && sel->instance_name[0] && sel->installed) {
        for (size_t i = 0; i < sel->installed_count; ++i) {
            const sxcl_loader_installed *it = &sel->installed[i];
            if (!it->instance_id || strcmp(it->instance_id, sel->instance_name) != 0) {
                continue;
            }
            const char *their_id = (it->loader_id && it->loader_id[0]) ? it->loader_id : "vanilla";
            const char *their_name = sxcl_loader_kind_name(sxcl_loader_kind_from_id(their_id));
            const char *their_ver = it->loader_version ? it->loader_version : "";
            if (strcmp(their_id, self_id) == 0) {
                add_issue(out, SXCL_LOADER_ISSUE_WARN, "想留着它就换一个实例名",
                          "同名版本 %s 已经存在（%s %s），继续装会覆盖它",
                          sel->instance_name, their_name, their_ver);
            } else {
                add_issue(out, SXCL_LOADER_ISSUE_ERROR, "换一个实例名",
                          "同名版本 %s 已经存在，而且它是 %s %s；继续装会把它覆盖掉",
                          sel->instance_name, their_name, their_ver);
            }
            break;
        }
    }

    /* 7) 同一个原版下已经装了别的加载器:Fabric 与 Forge 不能共用一个实例,
     *    我们只会另建一个目录,所以是提醒而不是拦截。
     *  8) 同一个原版 + 同一个加载器版本已经装过:提醒一句(重复装)。 */
    if (sel->installed && base[0]) {
        const sxcl_loader_installed *other = NULL;
        const char *same_ver = NULL;
        for (size_t i = 0; i < sel->installed_count; ++i) {
            const sxcl_loader_installed *it = &sel->installed[i];
            if (!it->loader_id || !it->loader_id[0]) {
                continue;
            }
            if (!it->base_version || strcmp(it->base_version, base) != 0) {
                continue;
            }
            if (strcmp(it->loader_id, self_id) != 0) {
                /* OptiFine 与 Forge/其它加载器是能叠在一起用的经典组合(Python 版专门有一条
                 * "已装有配套的 Forge" 的提示),所以它不算"不能共用一个实例"。 */
                if (!other && strcmp(it->loader_id, "optifine") != 0 &&
                    sel->kind != SXCL_LOADER_OPTIFINE) {
                    other = it;
                }
                continue;
            }
            if (!same_ver && want[0] && it->loader_version && strcmp(it->loader_version, want) == 0) {
                same_ver = it->instance_id ? it->instance_id : "";
            }
        }
        if (other) {
            add_issue(out, SXCL_LOADER_ISSUE_WARN, "我们会另外建一个独立实例，不会动它",
                      "%s 上已经装了 %s %s；它和 %s 不能共用一个实例",
                      base, sxcl_loader_kind_name(sxcl_loader_kind_from_id(other->loader_id)),
                      other->loader_version ? other->loader_version : "", name);
        }
        if (same_ver) {
            add_issue(out, SXCL_LOADER_ISSUE_WARN, "不用重复装；确实要装就换个实例名",
                      "%s 上已经装过 %s %s 了（%s）", base, name, want, same_ver);
        }
    }

    /* 9) OptiFine 的配套 Forge:提示而不是拦截(实测 OptiFine 可以独立安装)。 */
    if (sel->kind == SXCL_LOADER_OPTIFINE && sel->optifine_forge_hint && sel->optifine_forge_hint[0]) {
        char needed[48];
        if (sxcl_loader_parse_forge_requirement(sel->optifine_forge_hint, needed, sizeof(needed)) ==
                SXCL_LOADER_OK && needed[0]) {
            int matched = 0;
            for (size_t i = 0; i < sel->installed_count && sel->installed; ++i) {
                const sxcl_loader_installed *it = &sel->installed[i];
                if (!it->loader_id || strcmp(it->loader_id, "forge") != 0) {
                    continue;
                }
                if (it->loader_version && strncmp(it->loader_version, needed, strlen(needed)) == 0) {
                    matched = 1;
                    break;
                }
            }
            if (matched) {
                add_issue(out, SXCL_LOADER_ISSUE_WARN, "",
                          "已装有配套的 Forge %s，OptiFine 会跟它一起工作", needed);
            } else {
                add_issue(out, SXCL_LOADER_ISSUE_WARN,
                          "想搭配 Forge 用，先装对应版本的 Forge；只装 OptiFine 也可以，它会独立运行",
                          "这个 OptiFine 是配合 Forge %s 的（预览版信息）", needed);
            }
        }
    }

    return SXCL_LOADER_OK;
}

/* ── Maven 坐标 ── */

/* 有界字符串拼接:溢出只置标记,绝不越界(最后统一报 SXCL_LOADER_ERR_SPACE)。 */
typedef struct sbuf {
    char *data;
    size_t cap;
    size_t len;
    int overflow;
} sbuf;

static void sb_init(sbuf *b, char *data, size_t cap)
{
    b->data = data;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
    if (cap > 0) {
        data[0] = '\0';
    }
}

static void sb_put(sbuf *b, const char *text)
{
    const size_t n = text ? strlen(text) : 0;
    if (b->overflow || b->len + n + 1 > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_put_n(sbuf *b, const char *text, size_t n)
{
    if (b->overflow || b->len + n + 1 > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* group 里的 '.' 换成 '/' —— 坐标的目录形态。 */
static void sb_put_group(sbuf *b, const char *text, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const char one = (text[i] == '.') ? '/' : text[i];
        sb_put_n(b, &one, 1);
    }
}

int sxcl_loader_maven_path(const char *coord, char *out, size_t out_len)
{
    /* Python: LoaderAnalyzer._coord_to_path / _parse_maven_coordinate */
    if (!coord || !out || out_len == 0) {
        return SXCL_LOADER_ERR_ARG;
    }
    out[0] = '\0';

    const char *at = strchr(coord, '@');
    const size_t coord_len = at ? (size_t)(at - coord) : strlen(coord);
    char ext[16];
    if (at && at[1]) {
        if (copy_cap(ext, sizeof(ext), at + 1) != 0) {
            return SXCL_LOADER_ERR_SPACE;
        }
        trim_inplace(ext);
    } else {
        copy_cap(ext, sizeof(ext), "jar");
    }
    if (!ext[0]) {
        copy_cap(ext, sizeof(ext), "jar");
    }

    const char *c1 = memchr(coord, ':', coord_len);
    if (!c1) {
        return SXCL_LOADER_ERR_ARG;
    }
    const size_t p1 = (size_t)(c1 - coord);
    const char *c2 = memchr(coord + p1 + 1, ':', coord_len - p1 - 1);
    if (!c2) {
        return SXCL_LOADER_ERR_ARG;
    }
    const size_t p2 = (size_t)(c2 - coord);
    const char *c3 = memchr(coord + p2 + 1, ':', coord_len - p2 - 1);

    const char *group = coord;
    const size_t group_len = p1;
    const char *artifact = coord + p1 + 1;
    const size_t artifact_len = p2 - p1 - 1;
    const char *version = coord + p2 + 1;
    const size_t version_len = (c3 ? (size_t)(c3 - coord) : coord_len) - p2 - 1;
    const char *classifier = c3 ? c3 + 1 : NULL;
    const size_t classifier_len = c3 ? coord_len - (size_t)(c3 - coord) - 1 : 0;
    if (group_len == 0 || artifact_len == 0 || version_len == 0) {
        return SXCL_LOADER_ERR_ARG;
    }

    sbuf b;
    sb_init(&b, out, out_len);
    sb_put_group(&b, group, group_len);
    sb_put(&b, "/");
    sb_put_n(&b, artifact, artifact_len);
    sb_put(&b, "/");
    sb_put_n(&b, version, version_len);
    sb_put(&b, "/");
    sb_put_n(&b, artifact, artifact_len);
    sb_put(&b, "-");
    sb_put_n(&b, version, version_len);
    if (classifier && classifier_len > 0) {
        sb_put(&b, "-");
        sb_put_n(&b, classifier, classifier_len);
    }
    sb_put(&b, ".");
    sb_put(&b, ext);
    if (b.overflow) {
        out[0] = '\0';
        return SXCL_LOADER_ERR_SPACE;
    }
    return SXCL_LOADER_OK;
}

/* ── 依赖库收集 ── */

static int library_same(const sxcl_loader_library *lib, const char *name)
{
    return strcmp(lib->name, name) == 0;
}

/* Python: _ForgeLikeAnalyzer._section —— "老格式把 libraries/processors/minecraft 放在
 * install 下面,新格式放在顶层"。所以先看 install 里的,再看顶层的。 */
static const sxcl_json_value *profile_section(const sxcl_json_value *root, const char *key)
{
    const sxcl_json_value *install = sxcl_json_get(root, "install");
    if (install && sxcl_json_type_of(install) == SXCL_JSON_OBJECT) {
        const sxcl_json_value *inner = sxcl_json_get(install, key);
        if (inner && sxcl_json_type_of(inner) != SXCL_JSON_NULL) {
            return inner;
        }
    }
    return sxcl_json_get(root, key);
}

/* 已经写出的那几条里有没有这个坐标(容量满了之后不再去重,件数仍然继续数)。 */
static int library_seen(const sxcl_loader_library *out, size_t total, size_t out_cap,
                        const char *coord)
{
    const size_t written = total < out_cap ? total : out_cap;
    for (size_t i = 0; i < written; ++i) {
        if (library_same(&out[i], coord)) {
            return 1;
        }
    }
    return 0;
}

/* 把一条 libraries[] 条目扒成"下载要用的三样东西"(坐标 / 落盘相对路径 / URL)。
 * **权威顺序**:条目自带的 downloads.artifact.path 与 .url 优先,其次才是"按 maven 根地址拼"。
 * 只按根地址拼会漏掉分类器与扩展名,更会漏掉"同一份清单里不同条目落在不同主机"这件事 ——
 * 实测 Forge 1.20.1 的 46 条安装期依赖:com.google.code.findbugs:jsr305 在
 * libraries.minecraft.net,其余在 maven.minecraftforge.net,全靠这个字段。
 * lib 传 NULL 表示"这条不是 libraries[] 里的对象"(processors[].classpath / .jar 那两处)。 */
static void fill_library(sxcl_loader_library *dst, const sxcl_json_value *lib, const char *coord,
                         const char *fallback)
{
    dst->name[0] = '\0';
    dst->path[0] = '\0';
    dst->url[0] = '\0';
    dst->url_full[0] = '\0';
    dst->sha1[0] = '\0';
    dst->size = 0;
    (void)copy_cap(dst->name, sizeof(dst->name), coord);
    (void)copy_cap(dst->url, sizeof(dst->url),
                   lib ? sxcl_json_get_string(lib, "url", fallback) : fallback);
    const sxcl_json_value *downloads = lib ? sxcl_json_get(lib, "downloads") : NULL;
    const sxcl_json_value *artifact = downloads ? sxcl_json_get(downloads, "artifact") : NULL;
    if (artifact && sxcl_json_type_of(artifact) == SXCL_JSON_OBJECT) {
        (void)copy_cap(dst->url_full, sizeof(dst->url_full),
                       sxcl_json_get_string(artifact, "url", ""));
        (void)copy_cap(dst->path, sizeof(dst->path),
                       sxcl_json_get_string(artifact, "path", ""));
        (void)copy_cap(dst->sha1, sizeof(dst->sha1), sxcl_json_get_string(artifact, "sha1", ""));
        dst->size = sxcl_json_get_int64(artifact, "size", 0);
    }
    if (!dst->path[0] &&
        sxcl_loader_maven_path(coord, dst->path, sizeof(dst->path)) != SXCL_LOADER_OK) {
        dst->path[0] = '\0';
    }
}

size_t sxcl_loader_collect_libraries(const sxcl_json *version_json, const char *default_maven,
                                     sxcl_loader_library *out, size_t out_cap)
{
    if (!version_json) {
        return 0;
    }
    const sxcl_json_value *root = sxcl_json_root(version_json);
    if (!root || sxcl_json_type_of(root) != SXCL_JSON_OBJECT) {
        return 0;
    }
    const char *fallback = default_maven ? default_maven : "";
    size_t total = 0;

    const sxcl_json_value *libraries = profile_section(root, "libraries");
    const size_t lib_count = sxcl_json_size(libraries);
    for (size_t i = 0; i < lib_count; ++i) {
        const sxcl_json_value *lib = sxcl_json_at(libraries, i);
        if (!lib || sxcl_json_type_of(lib) != SXCL_JSON_OBJECT) {
            continue;
        }
        const char *coord = sxcl_json_get_string(lib, "name", "");
        if (!coord[0]) {
            continue;
        }
        if (library_seen(out, total, out_cap, coord)) {
            continue;
        }
        if (total < out_cap && out) {
            fill_library(&out[total], lib, coord, fallback);
        }
        ++total;
    }

    /* processors[] 里的两处也都要下:classpath[](处理器依赖)与 jar(处理器自己)。
     * 为什么要收 processors[].jar:清单偶尔漏写它(漏了就是"处理器起不来"),补一道便宜。 */
    const sxcl_json_value *processors = profile_section(root, "processors");
    const size_t proc_count = sxcl_json_size(processors);
    for (size_t i = 0; i < proc_count; ++i) {
        const sxcl_json_value *proc = sxcl_json_at(processors, i);
        if (!proc || sxcl_json_type_of(proc) != SXCL_JSON_OBJECT) {
            continue;
        }
        const char *jar_coord = sxcl_json_get_string(proc, "jar", "");
        if (jar_coord[0] && !library_seen(out, total, out_cap, jar_coord)) {
            if (total < out_cap && out) {
                fill_library(&out[total], NULL, jar_coord, fallback);
            }
            ++total;
        }
        const sxcl_json_value *classpath = sxcl_json_get(proc, "classpath");
        const size_t cp_count = sxcl_json_size(classpath);
        for (size_t k = 0; k < cp_count; ++k) {
            const char *coord = sxcl_json_string(sxcl_json_at(classpath, k));
            if (!coord || !coord[0] || library_seen(out, total, out_cap, coord)) {
                continue;
            }
            if (total < out_cap && out) {
                fill_library(&out[total], NULL, coord, fallback);
            }
            ++total;
        }
    }
    return total;
}
