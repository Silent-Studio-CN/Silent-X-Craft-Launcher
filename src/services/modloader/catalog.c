/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1  /* 用了几处 C 串函数,MSVC 默认标弃用(工程惯例,不压 pragma) */

#include "sxcl/loader_catalog.h"

#include "sxcl/loader.h" /* sxcl_loader_maven_path:拼 downloads.artifact 的落盘路径 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 小工具(与 modloader/compat.c 同一套写法) ── */

/** 写一句人话原因(err 可空)。 */
static void catalog_err(char *err, size_t cap, const char *text)
{
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", text != NULL ? text : "");
    }
}

static int copy_cap(char *dst, size_t cap, const char *src)
{
    const size_t len = src ? strlen(src) : 0;
    if (!dst || cap == 0) {
        return -1;
    }
    if (len + 1 > cap) {
        memcpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static int copy_range(char *dst, size_t cap, const char *begin, size_t len)
{
    if (!dst || cap == 0) {
        return -1;
    }
    if (len + 1 > cap) {
        memcpy(dst, begin, cap - 1);
        dst[cap - 1] = '\0';
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

static int chr_is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int chr_is_name(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '_' || c == '-' || c == '.' || c == ':';
}

/* 大小写不敏感的前缀比较(needle 必须是小写)。 */
static int ci_match(const char *hay, const char *needle)
{
    for (size_t i = 0; needle[i]; ++i) {
        if (!hay[i] || chr_lower((unsigned char)hay[i]) != (unsigned char)needle[i]) {
            return 0;
        }
    }
    return 1;
}

/* 大小写不敏感的子串查找。 */
static int ci_find(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) {
        return 0;
    }
    for (const char *p = hay; *p; ++p) {
        if (ci_match(p, needle)) {
            return 1;
        }
    }
    return 0;
}

static const char *find_seq(const char *begin, const char *end, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || (size_t)(end - begin) < n) {
        return NULL;
    }
    for (const char *p = begin; p + n <= end; ++p) {
        if (memcmp(p, needle, n) == 0) {
            return p;
        }
    }
    return NULL;
}

/* ── 最小 XML 扫描器 ── */

void sxcl_xml_init(sxcl_xml_cursor *cur, const char *text, size_t len)
{
    if (!cur) {
        return;
    }
    cur->p = text ? text : "";
    cur->end = cur->p + (text ? len : 0);
}

int sxcl_xml_next(sxcl_xml_cursor *cur, sxcl_xml_tag *out)
{
    if (!cur || !out || !cur->p) {
        return 0;
    }
    for (;;) {
        const char *lt = (const char *)memchr(cur->p, '<', (size_t)(cur->end - cur->p));
        if (!lt) {
            cur->p = cur->end;
            return 0;
        }
        const size_t left = (size_t)(cur->end - lt);
        /* 注释:整段跳过(坏 XML 里没闭合就到此为止) */
        if (left >= 4 && memcmp(lt, "<!--", 4) == 0) {
            const char *close = find_seq(lt + 4, cur->end, "-->");
            cur->p = close ? close + 3 : cur->end;
            continue;
        }
        /* CDATA / 声明 / DOCTYPE:跳过(它们不是元素) */
        if (left >= 2 && (lt[1] == '!' || lt[1] == '?')) {
            const char *close = (const char *)memchr(lt + 2, '>', left - 2);
            cur->p = close ? close + 1 : cur->end;
            continue;
        }

        const char *p = lt + 1;
        int is_end = 0;
        if (p < cur->end && *p == '/') {
            is_end = 1;
            ++p;
        }
        const char *name_begin = p;
        while (p < cur->end && chr_is_name((unsigned char)*p)) {
            ++p;
        }
        const size_t name_len = (size_t)(p - name_begin);

        /* 扫到标签的 '>'(引号里的 '>' 不算);遇到下一个 '<' 说明这个标签没闭合。 */
        int quote = 0;
        const char *gt = NULL;
        for (const char *q = p; q < cur->end; ++q) {
            const char ch = *q;
            if (quote) {
                if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '>') {
                gt = q;
                break;
            } else if (ch == '<') {
                break;
            }
        }
        if (!gt || name_len == 0) {
            /* 认不出来的 '<':跳过它继续找,绝不原地打转、绝不越界 */
            cur->p = lt + 1;
            continue;
        }

        copy_range(out->name, sizeof out->name, name_begin, name_len);
        out->is_end = is_end;
        out->raw = lt;
        out->raw_len = (size_t)(gt + 1 - lt);
        out->self_closing = (!is_end && out->raw_len >= 2 && gt[-1] == '/') ? 1 : 0;

        const char *tbeg = gt + 1;
        const char *tend = (const char *)memchr(tbeg, '<', (size_t)(cur->end - tbeg));
        if (!tend) {
            tend = cur->end;
        }
        out->text = tbeg;
        out->text_len = (size_t)(tend - tbeg);
        cur->p = tend;
        return 1;
    }
}

int sxcl_xml_tag_is(const sxcl_xml_tag *tag, const char *name)
{
    if (!tag || !name) {
        return 0;
    }
    const size_t n = strlen(name);
    if (n >= sizeof tag->name || strlen(tag->name) != n) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (chr_lower((unsigned char)tag->name[i]) != chr_lower((unsigned char)name[i])) {
            return 0;
        }
    }
    return 1;
}

int sxcl_xml_attr(const sxcl_xml_tag *tag, const char *name, char *out, size_t out_len)
{
    if (!tag || !name || !out || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    const size_t nlen = strlen(name);
    const char *const end = tag->raw + tag->raw_len;
    for (const char *p = tag->raw; p + nlen < end; ++p) {
        if (!ci_match(p, name)) {
            continue;
        }
        /* 属性名前面必须是空白/'<'/'/',避免把 class 里的 "classy" 之类也算上 */
        if (p != tag->raw && !chr_is_space((unsigned char)p[-1]) && p[-1] != '<' && p[-1] != '/') {
            continue;
        }
        const char *q = p + nlen;
        while (q < end && chr_is_space((unsigned char)*q)) {
            ++q;
        }
        if (q >= end || *q != '=') {
            continue;
        }
        ++q;
        while (q < end && chr_is_space((unsigned char)*q)) {
            ++q;
        }
        if (q >= end) {
            return 0;
        }
        if (*q == '"' || *q == '\'') {
            const char qc = *q++;
            const char *vbeg = q;
            while (q < end && *q != qc) {
                ++q;
            }
            copy_range(out, out_len, vbeg, (size_t)(q - vbeg));
            return 1;
        }
        const char *vbeg = q;
        while (q < end && !chr_is_space((unsigned char)*q) && *q != '>' && *q != '/') {
            ++q;
        }
        copy_range(out, out_len, vbeg, (size_t)(q - vbeg));
        return 1;
    }
    return 0;
}

int sxcl_xml_text_plain(const char *text, size_t len, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!text) {
        return 1;
    }
    const char *p = text;
    const char *end = text + len;
    while (p < end && chr_is_space((unsigned char)*p)) {
        ++p;
    }
    while (end > p && chr_is_space((unsigned char)end[-1])) {
        --end;
    }
    size_t written = 0;
    int complete = 1;
    while (p < end) {
        char ch = *p;
        if (ch == '&') {
            /* 实体:常见五个 + 数字实体;认不出来就原样抄一个 '&' */
            if (end - p >= 5 && memcmp(p, "&amp;", 5) == 0) { ch = '&'; p += 5; }
            else if (end - p >= 4 && memcmp(p, "&lt;", 4) == 0) { ch = '<'; p += 4; }
            else if (end - p >= 4 && memcmp(p, "&gt;", 4) == 0) { ch = '>'; p += 4; }
            else if (end - p >= 6 && memcmp(p, "&quot;", 6) == 0) { ch = '"'; p += 6; }
            else if (end - p >= 6 && memcmp(p, "&apos;", 6) == 0) { ch = '\''; p += 6; }
            else if (end - p >= 4 && p[1] == '#') {
                unsigned long code = 0;
                const char *q = p + 2;
                int hex = 0;
                if (q < end && (*q == 'x' || *q == 'X')) { hex = 1; ++q; }
                const char *digits = q;
                while (q < end && *q != ';') {
                    const int c = (unsigned char)*q;
                    unsigned long digit = 0;
                    if (c >= '0' && c <= '9') { digit = (unsigned long)(c - '0'); }
                    else if (hex && c >= 'a' && c <= 'f') { digit = (unsigned long)(c - 'a' + 10); }
                    else if (hex && c >= 'A' && c <= 'F') { digit = (unsigned long)(c - 'A' + 10); }
                    else { break; }
                    code = code * (hex ? 16UL : 10UL) + digit;
                    if (code > 0x10FFFFUL) { code = 0xFFFDUL; }
                    ++q;
                }
                if (q > digits && q < end && *q == ';') {
                    p = q + 1;
                    /* 只处理 ASCII 与 Latin-1 这两档;更高的码点直接丢掉(加载器元数据里不会出现) */
                    if (code == 0 || code > 0x7FUL) { continue; }
                    ch = (char)code;
                } else {
                    ++p;
                }
            } else {
                ++p;
            }
        } else {
            ++p;
        }
        if (written + 1 >= out_len) {
            complete = 0;
            break;
        }
        out[written++] = ch;
    }
    out[written] = '\0';
    return complete;
}

/* ── 条目小工具 ── */

static void entry_clear(sxcl_catalog_entry *e)
{
    memset(e, 0, sizeof *e);
}

/* Beta/预览标记在版本串里的位置(找不到返回 SIZE_MAX)。
 * 这些字样前面必须是在开头或分隔符(- _ . +),免得误伤 "1.20.1-47.2.0" 之类;
 * "pre" 太短,尤其容易被别的词撞上,所以一律走边界判定。 */
static size_t beta_marker_pos(const char *text)
{
    static const char *const marks[] = { "beta", "alpha", "snapshot", "pre", "rc" };
    size_t best = (size_t)-1;
    if (!text) {
        return best;
    }
    for (size_t i = 0; i < sizeof marks / sizeof marks[0]; ++i) {
        for (const char *p = text; *p; ++p) {
            if (!ci_match(p, marks[i])) {
                continue;
            }
            const int at_start = (p == text);
            const char prev = at_start ? '\0' : p[-1];
            const int after_sep = (prev == '-' || prev == '_' || prev == '.' || prev == '+');
            if (!at_start && !after_sep) {
                continue;
            }
            const size_t pos = (size_t)(p - text);
            if (pos < best) {
                best = pos;
            }
            break;
        }
    }
    return best;
}

/* 版本串里带 Beta/预览标记就当 Beta(大小写不敏感)。 */
static int is_beta_text(const char *text)
{
    return beta_marker_pos(text) != (size_t)-1;
}

static void entry_set_version(sxcl_catalog_entry *e, const char *version, sxcl_loader_kind kind)
{
    sxcl_loader_version_info info;
    copy_cap(e->version, sizeof e->version, version);
    memset(&info, 0, sizeof info);
    if (sxcl_loader_parse_version(version, kind, &info) == SXCL_LOADER_OK && info.ok) {
        copy_cap(e->mc, sizeof e->mc, info.mc);
        copy_cap(e->loader, sizeof e->loader, info.loader);
    }
    if (is_beta_text(version)) {
        e->is_beta = 1;
    }
    if (e->display[0] == '\0') {
        copy_cap(e->display, sizeof e->display, version);
    }
}

/* mc 过滤:空 = 不过滤。 */
static int mc_accepts(const char *wanted, const char *mc)
{
    return (!wanted || !*wanted || (mc && strcmp(wanted, mc) == 0));
}

/* ── 解析:maven-metadata.xml(Forge / NeoForge) ── */

size_t sxcl_catalog_parse_maven_xml(const char *xml, size_t len, sxcl_loader_kind kind,
                                    const char *mc, sxcl_catalog_doc_info *info,
                                    sxcl_catalog_entry *out, size_t out_cap)
{
    sxcl_catalog_doc_info local;
    memset(&local, 0, sizeof local);
    if (!xml) {
        if (info) {
            *info = local;
        }
        return 0;
    }
    char buf[192];
    size_t kept = 0;
    size_t seen = 0;
    sxcl_xml_cursor cur;
    sxcl_xml_tag tag;
    sxcl_xml_init(&cur, xml, len);
    while (sxcl_xml_next(&cur, &tag)) {
        if (tag.is_end) {
            continue;
        }
        if (sxcl_xml_tag_is(&tag, "latest")) {
            sxcl_xml_text_plain(tag.text, tag.text_len, local.latest, sizeof local.latest);
        } else if (sxcl_xml_tag_is(&tag, "release")) {
            sxcl_xml_text_plain(tag.text, tag.text_len, local.release, sizeof local.release);
        } else if (sxcl_xml_tag_is(&tag, "lastUpdated")) {
            if (sxcl_xml_text_plain(tag.text, tag.text_len, buf, sizeof buf) && strlen(buf) >= 8) {
                /* 20260827045925 -> 2026-08-27 04:59:25(拿不到就原样留着) */
                if (strlen(buf) >= 14) {
                    snprintf(local.last_updated, sizeof local.last_updated, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                             buf, buf + 4, buf + 6, buf + 8, buf + 10, buf + 12);
                } else {
                    copy_cap(local.last_updated, sizeof local.last_updated, buf);
                }
            }
        } else if (sxcl_xml_tag_is(&tag, "groupId")) {
            sxcl_xml_text_plain(tag.text, tag.text_len, local.group, sizeof local.group);
        } else if (sxcl_xml_tag_is(&tag, "artifactId")) {
            sxcl_xml_text_plain(tag.text, tag.text_len, local.artifact, sizeof local.artifact);
        } else if (sxcl_xml_tag_is(&tag, "version") && !tag.self_closing) {
            /* maven-metadata.xml 里 <version> 只出现在 <versions> 里;这里不强制要求先见到
             * <versions> —— 真实响应被切片/截断时也要能读(测试就是喂的片段)。 */
            if (!sxcl_xml_text_plain(tag.text, tag.text_len, buf, sizeof buf) || buf[0] == '\0') {
                continue;
            }
            sxcl_catalog_entry tmp;
            entry_clear(&tmp);
            entry_set_version(&tmp, buf, kind);
            if (!mc_accepts(mc, tmp.mc)) {
                continue;   /* 不是这个 MC 的:直接丢,别占 out 的位置 */
            }
            ++seen;
            if (out && kept < out_cap) {
                out[kept] = tmp;
                if (local.latest[0] && strcmp(tmp.version, local.latest) == 0) {
                    out[kept].is_latest = 1;
                }
                if (local.release[0] && strcmp(tmp.version, local.release) == 0) {
                    out[kept].is_recommended = 1;
                }
                ++kept;
            }
        }
    }
    local.parsed = seen;
    local.matched = seen;
    if (info) {
        *info = local;
    }
    return seen;
}

/* ── 解析:Fabric / Quilt 的 meta JSON ──
 * 两家的返回体是同构的:[{ "loader": {"version": "0.19.5", "stable": true, ...}, ... }]
 * (没有 MC 的 /v2/versions/loader 那种是平铺的 {"version": ..., "stable": ...},所以两种都认)。 */
static size_t parse_meta_json(const char *json, size_t len, const char *mc,
                              sxcl_catalog_entry *out, size_t out_cap)
{
    if (!json) {
        return 0;
    }
    char err[128];
    sxcl_json *doc = sxcl_json_parse(json, len, err, sizeof err);
    if (!doc) {
        return 0;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const size_t count = sxcl_json_size(root);
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *item = sxcl_json_at(root, i);
        if (!item) {
            continue;
        }
        const sxcl_json_value *loader = sxcl_json_get(item, "loader");
        const sxcl_json_value *src = loader ? loader : item;
        const char *version = sxcl_json_get_string(src, "version", "");
        char from_maven[SXCL_CATALOG_VERSION_MAX];
        from_maven[0] = '\0';
        if (!version || !*version) {
            /* 只有 maven 坐标时("net.fabricmc:fabric-loader:0.19.5")取最后一段 */
            const char *maven = sxcl_json_get_string(src, "maven", "");
            const char *colon = maven ? strrchr(maven, ':') : NULL;
            if (colon && colon[1]) {
                copy_cap(from_maven, sizeof from_maven, colon + 1);
                version = from_maven;
            }
        }
        if (!version || !*version) {
            continue;
        }
        sxcl_catalog_entry tmp;
        entry_clear(&tmp);
        copy_cap(tmp.version, sizeof tmp.version, version);
        copy_cap(tmp.loader, sizeof tmp.loader, version);
        copy_cap(tmp.display, sizeof tmp.display, version);
        copy_cap(tmp.mc, sizeof tmp.mc, mc ? mc : "");
        if (sxcl_json_get_bool(src, "stable", 0)) {
            tmp.is_recommended = 1;
        }
        if (is_beta_text(version)) {
            tmp.is_beta = 1;
        }
        if (out && kept < out_cap) {
            out[kept++] = tmp;
        }
    }
    sxcl_json_free(doc);
    return kept;
}

size_t sxcl_catalog_parse_fabric_json(const char *json, size_t len, const char *mc,
                                      sxcl_catalog_entry *out, size_t out_cap)
{
    return parse_meta_json(json, len, mc, out, out_cap);
}

size_t sxcl_catalog_parse_quilt_json(const char *json, size_t len, const char *mc,
                                     sxcl_catalog_entry *out, size_t out_cap)
{
    return parse_meta_json(json, len, mc, out, out_cap);
}

/* ── Quilt:从 meta 直接拼「加载器版本 JSON」(不走安装器 jar) ──
 *
 * 为什么需要它:Quilt 的安装器 jar(org.quiltmc:quilt-installer,8.7MB)只有 maven.quiltmc.org
 * 一家托管,实测 32KB/s 且会停摆(BMCLAPI 与 Maven Central 都 404 —— 见 docs/22 §16),
 * 那条路在本机根本走不完。而 meta.quiltmc.org 的返回体里**本来就有**
 * launcherMeta.libraries 与 mainClass.client,照着拼就是一份完整的"加载器版本 JSON"。
 *
 * 产出**与 Fabric/Forge 安装器写出来的那份同形**:带 inheritsFrom 的"加载器层",
 * 调用方再用 sxcl_loader_flatten_json() 与原版合并成能独立启动的单层 JSON(PCL 同形)。 */
int sxcl_loader_quilt_loader_json(const char *meta_json, size_t len, const char *loader_version,
                                  const char *mc_version, char **out_text, char *err,
                                  size_t err_len)
{
    if (out_text != NULL) {
        *out_text = NULL;
    }
    if (meta_json == NULL || loader_version == NULL || loader_version[0] == '\0' || out_text == NULL) {
        catalog_err(err, err_len, "参数不全");
        return SXCL_CATALOG_ERR_ARG;
    }
    char perr[160];
    perr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(meta_json, len, perr, sizeof(perr));
    if (doc == NULL) {
        catalog_err(err, err_len, "meta 不是合法 JSON");
        return SXCL_CATALOG_ERR_FORMAT;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const size_t count = sxcl_json_size(root);
    const sxcl_json_value *picked = NULL;
    const char *main_class = NULL;
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *item = sxcl_json_at(root, i);
        if (item == NULL) {
            continue;
        }
        const sxcl_json_value *loader = sxcl_json_get(item, "loader");
        const sxcl_json_value *src = loader != NULL ? loader : item;
        const char *version = sxcl_json_get_string(src, "version", "");
        char from_maven[128];
        from_maven[0] = '\0';
        if (version[0] == '\0') {
            /* 只有 maven 坐标("org.quiltmc:quilt-loader:0.20.0-beta.9")时取最后一段 */
            const char *maven = sxcl_json_get_string(src, "maven", "");
            const char *colon = maven != NULL ? strrchr(maven, ':') : NULL;
            if (colon != NULL && colon[1] != '\0') {
                copy_cap(from_maven, sizeof(from_maven), colon + 1);
                version = from_maven;
            }
        }
        if (version[0] == '\0' || strcmp(version, loader_version) != 0) {
            continue;
        }
        const sxcl_json_value *launcher_meta = sxcl_json_get(item, "launcherMeta");
        const sxcl_json_value *classes = sxcl_json_get(launcher_meta, "mainClass");
        const char *client = sxcl_json_get_string(classes, "client", "");
        if (client[0] == '\0') {
            continue; /* 没有入口类的条目不是我们要的(有些老条目形态不同) */
        }
        picked = item;
        main_class = client;
        break;
    }
    if (picked == NULL) {
        sxcl_json_free(doc);
        catalog_err(err, err_len, "meta 里没有这个 loader 版本(或它没有 mainClass)");
        return SXCL_CATALOG_ERR_FORMAT;
    }
    const sxcl_json_value *launcher_meta = sxcl_json_get(picked, "launcherMeta");
    const sxcl_json_value *libs = sxcl_json_get(launcher_meta, "libraries");
    /* 注意:实测 Quilt 的 libraries 是**对象**(client/common/server),sxcl_json_size 只对数组有效 ——
     * 用数组长度判空会把好好的 meta 判成"空的"(踩过一次)。 */
    if (sxcl_json_size(libs) == 0 && sxcl_json_member_count(libs) == 0) {
        sxcl_json_free(doc);
        catalog_err(err, err_len, "这个 loader 版本的 launcherMeta.libraries 是空的");
        return SXCL_CATALOG_ERR_FORMAT;
    }
    /* 产出的 JSON 比 meta 小得多:按 meta 长度 + 余量分配一次就够,写不下就如实报错。 */
    size_t cap = len + 4096;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) {
        sxcl_json_free(doc);
        catalog_err(err, err_len, "内存不足");
        return SXCL_CATALOG_ERR_ARG;
    }
    size_t used = 0;
    int truncated = 0;
#define QUILT_APPEND(...)                                                                          \
    do {                                                                                           \
        const int w = snprintf(buf + used, cap - used, __VA_ARGS__);                               \
        if (w < 0 || (size_t)w >= cap - used) {                                                    \
            truncated = 1;                                                                         \
        } else {                                                                                   \
            used += (size_t)w;                                                                     \
        }                                                                                          \
    } while (0)
    QUILT_APPEND("{\n  \"id\": \"quilt-%s\",\n  \"inheritsFrom\": \"%s\",\n  \"mainClass\": \"%s\",\n",
                 loader_version, mc_version != NULL ? mc_version : "", main_class);
    QUILT_APPEND("  \"libraries\": [");
    size_t written = 0;
    /* 一条库:有哈希/大小就写成标准的 downloads.artifact 形态(带完整 URL + sha1 + size)。
     * 为什么:我们的 sxcl_loader_collect_libraries 认的是 downloads.artifact(强校验 + 落盘路径),
     * 而 Quilt 的 meta 只给 {name,url} —— 三样关键件本来就在项里带着哈希,别浪费。 */
#define QUILT_EMIT_LIB(coord, root, sha1, size)                                                     \
    do {                                                                                           \
        QUILT_APPEND("%s\n    {\"name\": \"%s\"", written > 0 ? "," : "", (coord));                 \
        if ((root) != NULL && (root)[0] != '\0') {                                                 \
            QUILT_APPEND(", \"url\": \"%s\"", (root));                                             \
        }                                                                                          \
        if ((sha1) != NULL && (sha1)[0] != '\0') {                                                 \
            char rel[420];                                                                         \
            rel[0] = '\0';                                                                         \
            if (sxcl_loader_maven_path((coord), rel, sizeof(rel)) == SXCL_LOADER_OK) {              \
                QUILT_APPEND(", \"downloads\": {\"artifact\": {\"url\": \"%s%s\", \"path\": \"%s\"", \
                             (root), rel, rel);                                                    \
                QUILT_APPEND(", \"sha1\": \"%s\"", (sha1));                                         \
                if ((size) > 0) {                                                                  \
                    QUILT_APPEND(", \"size\": %lld", (long long)(size));                            \
                }                                                                                  \
                QUILT_APPEND("}}");                                                                \
            }                                                                                      \
        }                                                                                          \
        QUILT_APPEND("}");                                                                         \
        ++written;                                                                                 \
    } while (0)

    /* ① 加载器自己 + hashed + intermediary:meta 的 libraries 里**没有**这三样,
     *    但它们是"能起来"的最小集合(HMCL 也是这么补的)。坐标在项内的 loader/hashed/intermediary。 */
    {
        struct {
            const char *key;
            const char *maven_root;
        } const extras[] = {
            {"loader", "https://maven.quiltmc.org/repository/release/"},
            {"hashed", "https://maven.quiltmc.org/repository/release/"},
            {"intermediary", "https://maven.fabricmc.net/"},
        };
        for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]); ++i) {
            const sxcl_json_value *obj = sxcl_json_get(picked, extras[i].key);
            const char *coord = sxcl_json_get_string(obj, "maven", "");
            if (coord[0] == '\0') {
                continue;
            }
            const char *sha1 = sxcl_json_get_string(sxcl_json_get(obj, "hashes"), "sha1", "");
            const int64_t size = sxcl_json_get_int64(obj, "file_size", 0);
            QUILT_EMIT_LIB(coord, extras[i].maven_root, sha1, size);
        }
    }
    /* ② launcherMeta.libraries:实测是**对象**(client / common / server 三个数组),
     *    也有版本给的是数组 —— 两种形态都认(是对象就把里面所有数组都展开)。 */
    if (sxcl_json_type_of(libs) == SXCL_JSON_OBJECT) {
        const size_t members = sxcl_json_member_count(libs);
        for (size_t m = 0; m < members; ++m) {
            const sxcl_json_value *group = sxcl_json_member_value(libs, m);
            if (sxcl_json_type_of(group) != SXCL_JSON_ARRAY) {
                continue;
            }
            const size_t n = sxcl_json_size(group);
            for (size_t i = 0; i < n; ++i) {
                const sxcl_json_value *lib = sxcl_json_at(group, i);
                const char *name = sxcl_json_get_string(lib, "name", "");
                if (name[0] == '\0') {
                    continue;
                }
                QUILT_EMIT_LIB(name, sxcl_json_get_string(lib, "url", ""),
                               sxcl_json_get_string(lib, "sha1", ""),
                               sxcl_json_get_int64(lib, "size", 0));
            }
        }
    } else {
        const size_t n = sxcl_json_size(libs);
        for (size_t i = 0; i < n; ++i) {
            const sxcl_json_value *lib = sxcl_json_at(libs, i);
            const char *name = sxcl_json_get_string(lib, "name", "");
            if (name[0] == '\0') {
                continue;
            }
            QUILT_EMIT_LIB(name, sxcl_json_get_string(lib, "url", ""),
                           sxcl_json_get_string(lib, "sha1", ""),
                           sxcl_json_get_int64(lib, "size", 0));
        }
    }
#undef QUILT_EMIT_LIB
    QUILT_APPEND("\n  ]\n}\n");
#undef QUILT_APPEND
    sxcl_json_free(doc);
    if (truncated || written == 0) {
        free(buf);
        catalog_err(err, err_len, truncated ? "拼出来的 JSON 太长" : "没有任何可用的库条目");
        return SXCL_CATALOG_ERR_FORMAT;
    }
    *out_text = buf;
    return SXCL_CATALOG_OK;
}

/* ── 解析:OptiFine(BMCLAPI JSON) ── */

size_t sxcl_catalog_parse_optifine_json(const char *json, size_t len, const char *mc,
                                        sxcl_catalog_entry *out, size_t out_cap)
{
    if (!json) {
        return 0;
    }
    char err[128];
    sxcl_json *doc = sxcl_json_parse(json, len, err, sizeof err);
    if (!doc) {
        return 0;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const size_t count = sxcl_json_size(root);
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *item = sxcl_json_at(root, i);
        if (!item) {
            continue;
        }
        const char *type = sxcl_json_get_string(item, "type", "");
        const char *patch = sxcl_json_get_string(item, "patch", "");
        char version[SXCL_CATALOG_VERSION_MAX];
        version[0] = '\0';
        /* 拼成 PCL 那种 "HD_U_I6" / "HD_U_I5_pre4"(download_config_page.py 的 type_patch) */
        if (type[0] && patch[0]) {
            snprintf(version, sizeof version, "%s_%s", type, patch);
        } else if (patch[0]) {
            copy_cap(version, sizeof version, patch);
        } else {
            copy_cap(version, sizeof version, type);
        }
        if (version[0] == '\0') {
            continue;
        }
        sxcl_catalog_entry tmp;
        entry_clear(&tmp);
        copy_cap(tmp.version, sizeof tmp.version, version);
        copy_cap(tmp.display, sizeof tmp.display, version);
        copy_cap(tmp.file, sizeof tmp.file, sxcl_json_get_string(item, "filename", ""));
        const char *item_mc = sxcl_json_get_string(item, "mcversion", mc ? mc : "");
        copy_cap(tmp.mc, sizeof tmp.mc, item_mc);
        if (!mc_accepts(mc, tmp.mc)) {
            continue;
        }
        /* 配套 Forge:复用 loader.h 的解析("Forge 47.2.18" -> "47.2.18") */
        const char *forge = sxcl_json_get_string(item, "forge", "");
        if (forge && *forge) {
            sxcl_loader_parse_forge_requirement(forge, tmp.forge, sizeof tmp.forge);
        }
        if (is_beta_text(version) || (tmp.file[0] && ci_match(tmp.file, "preview"))) {
            tmp.is_beta = 1;
        }
        if (out && kept < out_cap) {
            out[kept++] = tmp;
        }
    }
    sxcl_json_free(doc);
    return kept;
}

/* 从 "…?f=OptiFine_1.21.11_HD_U_J9.jar&x=…" 里抠出文件名。 */
static void pick_f_from_tag(const sxcl_xml_tag *tag, char *out, size_t out_len)
{
    if (!tag || !out || out_len == 0 || tag->is_end) {
        return;
    }
    const char *end = tag->raw + tag->raw_len;
    for (const char *p = tag->raw; p + 2 < end; ++p) {
        if (!((p[0] == 'f' || p[0] == 'F') && p[1] == '=')) {
            continue;
        }
        const char *q = p + 2;
        const char *vbeg = q;
        while (q < end && *q != '&' && *q != '"' && *q != '\'' && *q != '>' && *q != '<') {
            ++q;
        }
        if (q > vbeg && (size_t)(q - vbeg) < out_len) {
            copy_range(out, out_len, vbeg, (size_t)(q - vbeg));
            return;
        }
    }
}

/* "OptiFine_1.21.11_HD_U_J9.jar" / "preview_OptiFine_1.20.1_HD_U_I5_pre4.jar"
 * -> mc="1.21.11",版本="HD_U_J9";is_beta 由 preview_ 前缀决定。 */
static void parse_optifine_file(const char *file, char *mc_out, size_t mc_len,
                                char *ver_out, size_t ver_len, int *is_preview)
{
    if (mc_out && mc_len) {
        mc_out[0] = '\0';
    }
    if (ver_out && ver_len) {
        ver_out[0] = '\0';
    }
    if (is_preview) {
        *is_preview = 0;
    }
    if (!file || !*file) {
        return;
    }
    if (is_preview && ci_match(file, "preview")) {
        *is_preview = 1;
    }
    const char *name = strstr(file, "OptiFine_");
    if (!name) {
        return;
    }
    name += strlen("OptiFine_");
    const char *sep = strchr(name, '_');
    if (!sep) {
        return;
    }
    if (mc_out && mc_len) {
        copy_range(mc_out, mc_len, name, (size_t)(sep - name));
    }
    if (ver_out && ver_len) {
        const char *vend = strstr(sep + 1, ".jar");
        const size_t vlen = vend ? (size_t)(vend - (sep + 1)) : strlen(sep + 1);
        copy_range(ver_out, ver_len, sep + 1, vlen);
    }
}

/* "05.02.2026" -> "2026-02-05";认不出来就原样留着。 */
static void normalize_date(const char *text, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || !*text) {
        return;
    }
    unsigned d = 0, m = 0, y = 0;
    if (sscanf(text, "%u.%u.%u", &d, &m, &y) == 3 && y > 1900 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        snprintf(out, out_len, "%04u-%02u-%02u", y, m, d);
        return;
    }
    if (sscanf(text, "%u-%u-%u", &y, &m, &d) == 3 && y > 1900) {
        snprintf(out, out_len, "%04u-%02u-%02u", y, m, d);
        return;
    }
    copy_cap(out, out_len, text);
}

/* ── 解析:OptiFine(网页表格 + <download .../> 形态) ── */

size_t sxcl_catalog_parse_optifine_html(const char *html, size_t len, const char *mc,
                                        sxcl_catalog_entry *out, size_t out_cap)
{
    if (!html) {
        return 0;
    }
    sxcl_xml_cursor cur;
    sxcl_xml_tag tag;
    sxcl_xml_init(&cur, html, len);

    int in_row = 0;
    char cell[24];
    char row_file[SXCL_CATALOG_FILE_MAX];
    char row_forge[64];
    char row_date[32];
    char row_display[SXCL_CATALOG_DISPLAY_MAX];
    size_t kept = 0;
    cell[0] = '\0';
    row_file[0] = '\0';
    row_forge[0] = '\0';
    row_date[0] = '\0';
    row_display[0] = '\0';

    while (sxcl_xml_next(&cur, &tag)) {
        /* 1) 镜像/第三方给的 XML 形态:<download mcversion=… type=… patch=… forge=… /> */
        if (sxcl_xml_tag_is(&tag, "download")) {
            if (tag.is_end) {
                continue;
            }
            char a_mc[SXCL_CATALOG_MC_MAX] = "";
            char a_type[64] = "";
            char a_patch[64] = "";
            char a_forge[64] = "";
            char a_file[SXCL_CATALOG_FILE_MAX] = "";
            char a_version[SXCL_CATALOG_VERSION_MAX] = "";
            sxcl_catalog_entry tmp;
            entry_clear(&tmp);
            sxcl_xml_attr(&tag, "mcversion", a_mc, sizeof a_mc);
            sxcl_xml_attr(&tag, "type", a_type, sizeof a_type);
            sxcl_xml_attr(&tag, "patch", a_patch, sizeof a_patch);
            sxcl_xml_attr(&tag, "forge", a_forge, sizeof a_forge);
            sxcl_xml_attr(&tag, "filename", a_file, sizeof a_file);
            if (a_file[0]) {
                int preview = 0;
                parse_optifine_file(a_file, tmp.mc, sizeof tmp.mc, a_version, sizeof a_version, &preview);
                tmp.is_beta = preview;
                copy_cap(tmp.file, sizeof tmp.file, a_file);
            }
            if (a_version[0] == '\0') {
                if (a_type[0] && a_patch[0]) {
                    snprintf(a_version, sizeof a_version, "%s_%s", a_type, a_patch);
                } else {
                    copy_cap(a_version, sizeof a_version, a_patch[0] ? a_patch : a_type);
                }
            }
            if (a_version[0] == '\0') {
                continue;
            }
            copy_cap(tmp.version, sizeof tmp.version, a_version);
            copy_cap(tmp.display, sizeof tmp.display, a_version);
            if (!tmp.mc[0]) {
                copy_cap(tmp.mc, sizeof tmp.mc, a_mc);
            }
            if (a_forge[0]) {
                sxcl_loader_parse_forge_requirement(a_forge, tmp.forge, sizeof tmp.forge);
            }
            if (is_beta_text(a_version) || ci_match(a_patch, "pre")) {
                tmp.is_beta = 1;
            }
            if (!mc_accepts(mc, tmp.mc)) {
                continue;
            }
            if (out && kept < out_cap) {
                out[kept++] = tmp;
            }
            continue;
        }

        /* 2) optifine.net/downloads 的 <tr class="downloadLine …"> … </tr> */
        if (sxcl_xml_tag_is(&tag, "tr")) {
            if (tag.is_end) {
                if (in_row) {
                    char ver[SXCL_CATALOG_VERSION_MAX];
                    int preview = 0;
                    sxcl_catalog_entry tmp;
                    entry_clear(&tmp);
                    parse_optifine_file(row_file, tmp.mc, sizeof tmp.mc, ver, sizeof ver, &preview);
                    if (ver[0] == '\0') {
                        /* 没有文件名就退而求其次:用 colFile 那列里的版本号 */
                        const char *p = row_display;
                        while (*p && !((*p >= '0' && *p <= '9') && p[1] == '.')) {
                            ++p;
                        }
                        copy_cap(ver, sizeof ver, p);
                    }
                    if (ver[0] != '\0') {
                        copy_cap(tmp.version, sizeof tmp.version, ver);
                        copy_cap(tmp.display, sizeof tmp.display, row_display[0] ? row_display : ver);
                        copy_cap(tmp.file, sizeof tmp.file, row_file);
                        if (tmp.mc[0] == '\0') {
                            copy_cap(tmp.mc, sizeof tmp.mc, mc ? mc : "");
                        }
                        if (row_forge[0]) {
                            sxcl_loader_parse_forge_requirement(row_forge, tmp.forge, sizeof tmp.forge);
                        }
                        normalize_date(row_date, tmp.released, sizeof tmp.released);
                        if (preview || is_beta_text(ver) || ci_find(row_display, "preview")) {
                            tmp.is_beta = 1;
                        }
                        if (mc_accepts(mc, tmp.mc) && out && kept < out_cap) {
                            out[kept++] = tmp;
                        }
                    }
                }
                in_row = 0;
                cell[0] = '\0';
                row_file[0] = '\0';
                row_forge[0] = '\0';
                row_date[0] = '\0';
                row_display[0] = '\0';
                continue;
            }
            char cls[64];
            cls[0] = '\0';
            sxcl_xml_attr(&tag, "class", cls, sizeof cls);
            in_row = ci_find(cls, "downloadline") ? 1 : 0;
            cell[0] = '\0';
            row_file[0] = '\0';
            row_forge[0] = '\0';
            row_date[0] = '\0';
            row_display[0] = '\0';
            continue;
        }
        if (!in_row) {
            continue;
        }
        if (sxcl_xml_tag_is(&tag, "td")) {
            if (tag.is_end) {
                cell[0] = '\0';
                continue;
            }
            char cls[64];
            cls[0] = '\0';
            sxcl_xml_attr(&tag, "class", cls, sizeof cls);
            copy_cap(cell, sizeof cell, cls);
            for (char *p = cell; *p; ++p) {
                *p = (char)chr_lower((unsigned char)*p);
            }
            char text[256];
            sxcl_xml_text_plain(tag.text, tag.text_len, text, sizeof text);
            if (strcmp(cell, "colfile") == 0) {
                copy_cap(row_display, sizeof row_display, text);
            } else if (strcmp(cell, "colforge") == 0) {
                copy_cap(row_forge, sizeof row_forge, text);
            } else if (strcmp(cell, "coldate") == 0) {
                copy_cap(row_date, sizeof row_date, text);
            }
            continue;
        }
        /* 下载/镜像单元格里的 <a href="…f=<jar>">:文件名只在这里 */
        if (strcmp(cell, "coldownload") == 0 || strcmp(cell, "colmirror") == 0) {
            pick_f_from_tag(&tag, row_file, sizeof row_file);
        }
    }
    return kept;
}

/* ── 解析:BMCLAPI 镜像独有的"按 MC 分的列表"JSON ──────────────────
 * 形态(实测 2026-09-21):
 *   /forge/minecraft/1.20.1 -> [{_id,__v,build,files[],mcversion,modified,version}, …]
 *   /neoforge/list/1.20.1   -> [{_id,rawVersion,__v,mcversion,version}, …]
 *   /neoforge/list/1.21.1   -> 多一个 installerPath
 * 与 maven 那条路的关键区别:**MC 取 mcversion 字段(权威)**,不用版本串反推 ——
 * NeoForge 1.20.1 那一代是 47.1.x,反推会得到 "1.47.1"。
 */

/* "2023-06-12T19:37:00.000Z" -> "2023-06-12";不是这个形状就给空串(不编)。 */
static void list_json_date(const char *text, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (text == NULL || strlen(text) < 10) {
        return;
    }
    for (size_t i = 0; i < 10; ++i) {
        const char c = text[i];
        if (i == 4 || i == 7) {
            if (c != '-') {
                return;
            }
        } else if (c < '0' || c > '9') {
            return;
        }
    }
    copy_range(out, out_len, text, 10);
}

/* ".../neoforge-21.1.1-installer.jar" -> "neoforge-21.1.1-installer.jar"。 */
static void list_json_basename(const char *path, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (path == NULL || *path == '\0') {
        return;
    }
    const char *slash = strrchr(path, '/');
    copy_cap(out, out_len, (slash != NULL) ? slash + 1 : path);
}

static size_t parse_list_json(const char *json, size_t len, sxcl_loader_kind kind,
                              const char *mc, sxcl_catalog_entry *out, size_t out_cap)
{
    if (json == NULL || len == 0) {
        return 0;
    }
    char err[128];
    sxcl_json *doc = sxcl_json_parse(json, len, err, sizeof err);
    if (doc == NULL) {
        return 0;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    /* 裸数组是实测形态;对象包一层(ex: {"versions": […]})也认,接口换个壳不至于全盲。 */
    if (root != NULL && sxcl_json_type_of(root) == SXCL_JSON_OBJECT) {
        static const char *const kWrappers[] = {"versions", "data", "list"};
        for (size_t w = 0; w < sizeof kWrappers / sizeof kWrappers[0]; ++w) {
            const sxcl_json_value *inner = sxcl_json_get(root, kWrappers[w]);
            if (inner != NULL && sxcl_json_type_of(inner) == SXCL_JSON_ARRAY) {
                root = inner;
                break;
            }
        }
    }
    if (root == NULL || sxcl_json_type_of(root) != SXCL_JSON_ARRAY) {
        sxcl_json_free(doc);
        return 0;
    }
    const size_t total = sxcl_json_size(root);
    size_t kept = 0;
    for (size_t i = 0; i < total; ++i) {
        const sxcl_json_value *item = sxcl_json_at(root, i);
        if (item == NULL || sxcl_json_type_of(item) != SXCL_JSON_OBJECT) {
            continue;
        }
        const char *version = sxcl_json_get_string(item, "version", "");
        const char *raw = sxcl_json_get_string(item, "rawVersion", "");
        const char *mcversion = sxcl_json_get_string(item, "mcversion", "");
        const int has_version = (version != NULL && version[0] != '\0');
        const int has_raw = (raw != NULL && raw[0] != '\0');
        if (!has_version && !has_raw) {
            continue;
        }
        /* 权威 MC:先 mcversion,再退回调用方给的 mc;绝不从版本串反推。 */
        char item_mc[SXCL_CATALOG_MC_MAX];
        item_mc[0] = '\0';
        if (mcversion != NULL && mcversion[0] != '\0') {
            copy_cap(item_mc, sizeof item_mc, mcversion);
        } else if (mc != NULL) {
            copy_cap(item_mc, sizeof item_mc, mc);
        }
        if (!mc_accepts(mc, item_mc)) {
            continue;
        }
        /* 加载器自己的版本:**纯字符串处理,一次规则反推都不做**(这条路的全部意义
         * 就是不信版本串里的"像 MC 的那一段")。源给的 version 本来就是权威裸版本;
         * 实测 NeoForge 1.20.1 里有 21 条写成 "1.20.1-47.1.85"、rawVersion 还多个
         * "forge-"( "1.20.1-forge-47.1.85"),所以按已知前缀切掉。 */
        const char *src_version = has_version ? version : raw;
        size_t prefix_len = 0;
        const size_t item_mc_len = strlen(item_mc);
        if (item_mc_len > 0 && strncmp(src_version, item_mc, item_mc_len) == 0 &&
            src_version[item_mc_len] == '-') {
            prefix_len = item_mc_len + 1;
        }
        if (ci_match(src_version + prefix_len, "neoforge-")) {
            prefix_len += 9;
        } else if (ci_match(src_version + prefix_len, "forge-")) {
            prefix_len += 6;
        }
        char loader[SXCL_CATALOG_LOADER_MAX];
        loader[0] = '\0';
        copy_cap(loader, sizeof loader, src_version + prefix_len);
        if (loader[0] == '\0') {
            continue;   /* 只剩前缀 = 没有版本号,丢掉 */
        }
        sxcl_catalog_entry tmp;
        entry_clear(&tmp);
        copy_cap(tmp.mc, sizeof tmp.mc, item_mc);
        copy_cap(tmp.loader, sizeof tmp.loader, loader);
        if (kind == SXCL_LOADER_FORGE) {
            /* Forge 官方那条路的版本串是 "<mc>-<forge>";镜像只给裸 "<forge>",
             * 这里统一成同一种形态(安装器直链与兼容判定两边都吃这一种)。 */
            const size_t mc_len = strlen(tmp.mc);
            if (has_version && mc_len > 0 && strncmp(version, tmp.mc, mc_len) == 0 &&
                version[mc_len] == '-') {
                copy_cap(tmp.version, sizeof tmp.version, version);
            } else if (mc_len > 0) {
                snprintf(tmp.version, sizeof tmp.version, "%s-%s", tmp.mc, loader);
            } else {
                copy_cap(tmp.version, sizeof tmp.version, loader);
            }
        } else {
            /* NeoForge 官方那条路的版本号就是裸版本("21.1.72");镜像里有 21 条写成
             * "1.20.1-47.1.85"(rawVersion 还是 "1.20.1-forge-47.1.85"),统一成裸版本 ——
             * 安装器直链就是按裸版本拼的。 */
            copy_cap(tmp.version, sizeof tmp.version, loader);
        }
        if (has_raw) {
            copy_cap(tmp.display, sizeof tmp.display, raw);
        } else {
            copy_cap(tmp.display, sizeof tmp.display, tmp.version);
        }
        list_json_basename(sxcl_json_get_string(item, "installerPath", ""), tmp.file, sizeof tmp.file);
        list_json_date(sxcl_json_get_string(item, "modified", ""), tmp.released, sizeof tmp.released);
        if ((has_version && is_beta_text(version)) || (has_raw && is_beta_text(raw))) {
            tmp.is_beta = 1;
        }
        if (out != NULL && kept < out_cap) {
            out[kept++] = tmp;
        }
    }
    sxcl_json_free(doc);
    return kept;
}

size_t sxcl_catalog_parse_list_json(const char *json, size_t len, sxcl_loader_kind kind,
                                    const char *mc, sxcl_catalog_entry *out, size_t out_cap)
{
    return parse_list_json(json, len, kind, mc, out, out_cap);
}

/* ── 解析入口 ── */

size_t sxcl_catalog_parse(sxcl_loader_kind kind, const char *text, size_t len, const char *mc,
                          sxcl_catalog_doc_info *info, sxcl_catalog_entry *out, size_t out_cap)
{
    if (info) {
        memset(info, 0, sizeof *info);
    }
    if (!text || len == 0) {
        return 0;
    }
    size_t count = 0;
    switch (kind) {
    case SXCL_LOADER_FORGE:
    case SXCL_LOADER_NEOFORGE: {
        /* 同一个加载器有两条路、两种格式:官方是 maven-metadata.xml,BMCLAPI 镜像的
         * /forge/minecraft、/neoforge/list 是 JSON 数组(每条带权威 mcversion)。
         * 看第一个非空白字符就知道该用哪个解析器 —— 与下面 OptiFine 分支同一个套路。 */
        size_t i = 0;
        while (i < len && chr_is_space((unsigned char)text[i])) {
            ++i;
        }
        if (i < len && (text[i] == '[' || text[i] == '{')) {
            count = parse_list_json(text, len, kind, mc, out, out_cap);
        } else {
            count = sxcl_catalog_parse_maven_xml(text, len, kind, mc, info, out, out_cap);
        }
        break;
    }
    case SXCL_LOADER_FABRIC:
        count = sxcl_catalog_parse_fabric_json(text, len, mc, out, out_cap);
        break;
    case SXCL_LOADER_QUILT:
        count = sxcl_catalog_parse_quilt_json(text, len, mc, out, out_cap);
        break;
    case SXCL_LOADER_OPTIFINE: {
        /* 先看第一个非空白字符:JSON 走 JSON 解析器,其余(网页/XML)走扫描器 */
        size_t i = 0;
        while (i < len && chr_is_space((unsigned char)text[i])) {
            ++i;
        }
        if (i < len && (text[i] == '[' || text[i] == '{')) {
            count = sxcl_catalog_parse_optifine_json(text, len, mc, out, out_cap);
        } else {
            count = sxcl_catalog_parse_optifine_html(text, len, mc, out, out_cap);
        }
        break;
    }
    case SXCL_LOADER_VANILLA:
    default:
        count = 0;
        break;
    }
    if (info) {
        info->parsed = count;
        info->matched = count;
    }
    return count;
}

/* ── 过滤 / 排序 / 标记 ── */

size_t sxcl_catalog_filter_mc(const sxcl_catalog_entry *in, size_t count, const char *wanted,
                              sxcl_catalog_entry *out, size_t out_cap)
{
    if (!in || !out || count == 0 || out_cap == 0) {
        return 0;
    }
    size_t kept = 0;
    for (size_t i = 0; i < count && kept < out_cap; ++i) {
        if (mc_accepts(wanted, in[i].mc)) {
            out[kept++] = in[i];   /* 原地过滤也安全:写的下标永远不大于读的下标 */
        }
    }
    return kept;
}

/* 把版本串拆成数字元组(Python _version_key 的等价物)。
 * Beta 标记之后的部分**不参与**比大小:这样 "HD_U_I6" 排在 "HD_U_I6_pre6" 前面、
 * "21.1.72" 排在 "21.1.72-beta" 前面(数字一样时再按"非 Beta 在前"收尾)。 */
static void version_parts(const char *text, unsigned long *parts, size_t cap, size_t *count)
{
    size_t n = 0;
    const char *p = text ? text : "";
    const size_t stop = beta_marker_pos(p);
    while (*p && n < cap && (size_t)(p - (text ? text : "")) < stop) {
        if (*p >= '0' && *p <= '9') {
            unsigned long value = 0;
            while (*p >= '0' && *p <= '9') {
                if (value < 100000000UL) {
                    value = value * 10UL + (unsigned long)(*p - '0');
                }
                ++p;
            }
            parts[n++] = value;
        } else {
            ++p;
        }
    }
    *count = n;
}

static int version_compare_desc(const sxcl_catalog_entry *a, const sxcl_catalog_entry *b)
{
    unsigned long pa[8];
    unsigned long pb[8];
    size_t na = 0;
    size_t nb = 0;
    version_parts(a->version, pa, 8, &na);
    version_parts(b->version, pb, 8, &nb);
    const size_t common = (na < nb) ? na : nb;
    for (size_t i = 0; i < common; ++i) {
        if (pa[i] != pb[i]) {
            return (pa[i] > pb[i]) ? -1 : 1;
        }
    }
    if (na != nb) {
        return (na > nb) ? -1 : 1;
    }
    if (a->is_beta != b->is_beta) {
        return a->is_beta ? 1 : -1;   /* 数字一样时非 Beta 在前 */
    }
    const int cmp = strcmp(a->version, b->version);
    if (cmp > 0) {
        return -1;
    }
    return (cmp < 0) ? 1 : 0;
}

static int catalog_qsort_cmp(const void *lhs, const void *rhs)
{
    return version_compare_desc((const sxcl_catalog_entry *)lhs, (const sxcl_catalog_entry *)rhs);
}

void sxcl_catalog_sort_desc(sxcl_catalog_entry *items, size_t count)
{
    if (!items || count < 2) {
        return;
    }
    qsort(items, count, sizeof items[0], catalog_qsort_cmp);
}

void sxcl_catalog_mark_flags(sxcl_catalog_entry *items, size_t count)
{
    if (!items || count == 0) {
        return;
    }
    int have_latest = 0;
    int have_recommended = 0;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].is_latest) {
            have_latest = 1;
        }
        if (items[i].is_recommended) {
            have_recommended = 1;
        }
    }
    /* 源里没给标记(Forge/NeoForge 的 <release> 常年停在老版本、Quilt 干脆没有):
     * 排序后的第一条非 Beta 顶上,界面默认就选它。 */
    for (size_t i = 0; i < count; ++i) {
        if (items[i].is_beta) {
            continue;
        }
        if (!have_latest) {
            items[i].is_latest = 1;
            have_latest = 1;
        }
        if (!have_recommended) {
            items[i].is_recommended = 1;
            have_recommended = 1;
        }
        break;
    }
    /* 整页全是 Beta 时:至少让第一条当 latest(否则界面没有可默认选中的) */
    if (!have_latest) {
        items[0].is_latest = 1;
    }
    if (!have_recommended) {
        items[0].is_recommended = 1;
    }
}

size_t sxcl_catalog_prepare(sxcl_loader_kind kind, const char *text, size_t len, const char *mc,
                            sxcl_catalog_doc_info *info, sxcl_catalog_entry *out, size_t out_cap)
{
    sxcl_catalog_doc_info local;
    memset(&local, 0, sizeof local);
    const size_t found = sxcl_catalog_parse(kind, text, len, mc, &local, out, out_cap);
    if (!out || out_cap == 0) {
        if (info) {
            *info = local;
        }
        return found;
    }
    size_t filled = (found < out_cap) ? found : out_cap;
    filled = sxcl_catalog_filter_mc(out, filled, mc, out, out_cap);
    sxcl_catalog_sort_desc(out, filled);
    sxcl_catalog_mark_flags(out, filled);
    local.matched = filled;
    if (info) {
        *info = local;
    }
    return filled;
}

/* ── 地址 ── */

sxcl_catalog_format sxcl_catalog_format_of(sxcl_loader_kind kind, sxcl_catalog_source source)
{
    switch (kind) {
    case SXCL_LOADER_FORGE:
    case SXCL_LOADER_NEOFORGE:
        /* 镜像走 BMCLAPI 独有的"按 MC 分的列表"JSON(带权威 mcversion);
         * 官方没有这个接口,还是 maven-metadata.xml。 */
        return (source == SXCL_CATALOG_SRC_MIRROR) ? SXCL_CATALOG_FMT_LIST_JSON
                                                   : SXCL_CATALOG_FMT_MAVEN_XML;
    case SXCL_LOADER_FABRIC:
    case SXCL_LOADER_QUILT:
        return SXCL_CATALOG_FMT_META_JSON;
    case SXCL_LOADER_OPTIFINE:
        /* 镜像的 /optifine/<mc> 是另一种 JSON(带 mcversion + patch/type/filename),
         * 已有专门解析器;BMCLAPI 没有 /optifine/list/(实测 404),所以这里不改。 */
        return (source == SXCL_CATALOG_SRC_MIRROR) ? SXCL_CATALOG_FMT_OPTIFINE_JSON
                                                   : SXCL_CATALOG_FMT_OPTIFINE_HTML;
    case SXCL_LOADER_VANILLA:
    default:
        return SXCL_CATALOG_FMT_NONE;
    }
}

int sxcl_catalog_url(sxcl_loader_kind kind, sxcl_catalog_source source, const char *mc,
                     char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return SXCL_CATALOG_ERR_ARG;
    }
    out[0] = '\0';
    const char *mc_text = mc ? mc : "";
    const int mirror = (source == SXCL_CATALOG_SRC_MIRROR);
    const char *url = NULL;
    char built[512];
    built[0] = '\0';
    switch (kind) {
    case SXCL_LOADER_FORGE:
        /* 镜像:BMCLAPI 独有的 /forge/minecraft/<mc>(按 MC 分的列表,带权威 mcversion)。
         * 它那份 maven 元数据是旧的(实测最高只到 1.18),所以**有 mc 就走列表**;
         * 没有 mc 时只能退回 maven(那是唯一"不带 MC 也能查"的形态)。 */
        if (mirror && mc_text[0] != '\0') {
            snprintf(built, sizeof built, "https://bmclapi2.bangbang93.com/forge/minecraft/%s",
                     mc_text);
        } else {
            url = mirror
                      ? "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/maven-metadata.xml"
                      : "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml";
        }
        break;
    case SXCL_LOADER_NEOFORGE:
        if (mirror && mc_text[0] != '\0') {
            snprintf(built, sizeof built, "https://bmclapi2.bangbang93.com/neoforge/list/%s",
                     mc_text);
        } else {
            url = mirror
                      ? "https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/maven-metadata.xml"
                      : "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml";
        }
        break;
    case SXCL_LOADER_FABRIC:
        snprintf(built, sizeof built, mirror
                 ? "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/%s"
                 : "https://meta.fabricmc.net/v2/versions/loader/%s", mc_text);
        break;
    case SXCL_LOADER_QUILT:
        /* 实测 BMCLAPI 没有 quilt 的透传(/quilt-meta/ 404),只有官方 */
        snprintf(built, sizeof built, "https://meta.quiltmc.org/v3/versions/loader/%s", mc_text);
        break;
    case SXCL_LOADER_OPTIFINE:
        snprintf(built, sizeof built, mirror
                 ? "https://bmclapi2.bangbang93.com/optifine/%s"
                 : "https://optifine.net/downloads", mc_text);
        break;
    case SXCL_LOADER_VANILLA:
    default:
        return SXCL_CATALOG_ERR_ARG;
    }
    if (built[0] != '\0') {
        url = built;
    }
    if (!url || !*url) {
        return SXCL_CATALOG_ERR_ARG;
    }
    if (copy_cap(out, out_len, url) != 0) {
        out[0] = '\0';
        return SXCL_CATALOG_ERR_SPACE;
    }
    return SXCL_CATALOG_OK;
}

/* ── 取文本 + 解析(唯一碰传输层的地方) ── */

int sxcl_catalog_fetch(sxcl_transport *tr, sxcl_loader_kind kind, sxcl_catalog_source source,
                       const char *mc, const char *const *headers, sxcl_catalog_doc_info *info,
                       sxcl_catalog_entry *out, size_t out_cap, size_t *count,
                       char *err, size_t err_len)
{
    if (count) {
        *count = 0;
    }
    if (err && err_len) {
        err[0] = '\0';
    }
    char url[512];
    const int rc = sxcl_catalog_url(kind, source, mc, url, sizeof url);
    if (rc != SXCL_CATALOG_OK) {
        if (err && err_len) {
            snprintf(err, err_len, "这个加载器没有可用的版本列表地址");
        }
        return SXCL_CATALOG_ERR_ARG;
    }
    char *body = NULL;
    size_t body_len = 0;
    const int http_rc = sxcl_http_get_text(tr, url, headers, &body, &body_len, err, err_len);
    if (http_rc != SXCL_HTTP_OK) {
        free(body);
        return SXCL_CATALOG_ERR_NET;
    }
    const size_t found = sxcl_catalog_prepare(kind, body, body_len, mc, info, out, out_cap);
    free(body);
    if (count) {
        *count = found;
    }
    if (found == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "列表取回来了,但一条可用的版本都没解析出来");
        }
        return SXCL_CATALOG_ERR_FORMAT;
    }
    return SXCL_CATALOG_OK;
}
