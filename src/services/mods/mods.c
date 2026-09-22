/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组资源来源层的实现（声明与口径见 sxcl/mods.h）。
//
// 只做纯逻辑：拼 URL / 解析 JSON / 挑文件 / 算落盘路径。**不联网、不碰界面**，所以能单测。

#include "sxcl/mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/json.h"

/* ── 小工具 ── */

static void copy_cap(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src != NULL) {
        for (; src[i] != '\0' && i + 1 < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void set_err(char *err, size_t err_len, const char *text)
{
    if (err != NULL && err_len > 0) {
        copy_cap(err, err_len, text);
    }
}

static int str_equal_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        char x = *a;
        char y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

/** 把 JSON 数组里的字符串用空格连起来（最多 keep 个，装不下就停）。 */
static void join_strings(const sxcl_json_value *array, char *out, size_t cap, size_t keep)
{
    size_t used = 0;
    const size_t n = sxcl_json_size(array);
    out[0] = '\0';
    for (size_t i = 0; i < n && i < keep; ++i) {
        const char *text = sxcl_json_string(sxcl_json_at(array, i));
        if (text == NULL || text[0] == '\0') {
            continue;
        }
        const size_t len = strlen(text);
        if (used + len + 2 >= cap) {
            break;
        }
        if (used > 0) {
            out[used++] = ' ';
        }
        memcpy(out + used, text, len);
        used += len;
        out[used] = '\0';
    }
}

/** 数组里有没有这个值（大小写不敏感）。 */
static int list_has(const sxcl_json_value *array, const char *want)
{
    const size_t n = sxcl_json_size(array);
    for (size_t i = 0; i < n; ++i) {
        const char *text = sxcl_json_string(sxcl_json_at(array, i));
        if (text != NULL && str_equal_ci(text, want)) {
            return 1;
        }
    }
    return 0;
}

/** 空格分隔的列表里有没有这个词（大小写不敏感）—— 版本/加载器在 sxcl_mod_file 里就是这么存的。 */
static int text_list_has(const char *list, const char *want)
{
    if (list == NULL || want == NULL || want[0] == '\0') {
        return 0;
    }
    const char *p = list;
    while (*p != '\0') {
        while (*p == ' ') {
            ++p;
        }
        const char *end = p;
        while (*end != '\0' && *end != ' ') {
            ++end;
        }
        const size_t n = (size_t)(end - p);
        const size_t w = strlen(want);
        if (n == w) {
            int same = 1;
            for (size_t i = 0; i < n; ++i) {
                char a = p[i];
                char b = want[i];
                if (a >= 'A' && a <= 'Z') {
                    a = (char)(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z') {
                    b = (char)(b - 'A' + 'a');
                }
                if (a != b) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                return 1;
            }
        }
        p = end;
    }
    return 0;
}

/* ── URL 拼装 ── */

/* URL 里的空格要写成 %20：facets 与关键词都会带 */
static void url_escape(const char *text, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t used = 0;
    if (out == NULL || cap == 0) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)text; p != NULL && *p != '\0'; ++p) {
        const unsigned char ch = *p;
        const int safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
                         ch == '~';
        if (safe) {
            if (used + 2 > cap) {
                break;
            }
            out[used++] = (char)ch;
        } else {
            if (used + 4 > cap) {
                break;
            }
            out[used++] = '%';
            out[used++] = hex[(ch >> 4) & 0xF];
            out[used++] = hex[ch & 0xF];
        }
    }
    out[used < cap ? used : cap - 1] = '\0';
}

/* Modrinth 的 facets 写法：facet 之间 AND、组内 OR，如
 *   facets=[["categories:forge"],["versions:1.20.1"]] */
static int modrinth_facets(const sxcl_mods_query *q, char *out, size_t cap)
{
    char group[5][160];
    size_t groups = 0;
    char escaped[200];
    if (q->project_type != NULL && q->project_type[0] != '\0') {
        url_escape(q->project_type, escaped, sizeof(escaped));
        (void)snprintf(group[groups], sizeof(group[0]), "[\"project_type:%s\"]", escaped);
        ++groups;
    }
    if (q->game_version != NULL && q->game_version[0] != '\0') {
        url_escape(q->game_version, escaped, sizeof(escaped));
        (void)snprintf(group[groups], sizeof(group[0]), "[\"versions:%s\"]", escaped);
        ++groups;
    }
    if (q->loader != NULL && q->loader[0] != '\0' && !str_equal_ci(q->loader, "vanilla")) {
        url_escape(q->loader, escaped, sizeof(escaped));
        (void)snprintf(group[groups], sizeof(group[0]), "[\"categories:%s\"]", escaped);
        ++groups;
    }
    if (q->category != NULL && q->category[0] != '\0') {
        url_escape(q->category, escaped, sizeof(escaped));
        (void)snprintf(group[groups], sizeof(group[0]), "[\"categories:%s\"]", escaped);
        ++groups;
    }
    if (groups == 0) {
        out[0] = '\0';
        return 0;
    }
    size_t used = 0;
    out[0] = '\0';
    if (used + 2 > cap) {
        return -1;
    }
    out[used++] = '[';
    out[used] = '\0';
    for (size_t i = 0; i < groups; ++i) {
        const int n = snprintf(out + used, cap - used, "%s%s", i > 0 ? "," : "", group[i]);
        if (n < 0 || (size_t)n >= cap - used) {
            return -1;
        }
        used += (size_t)n;
    }
    if (used + 2 > cap) {
        return -1;
    }
    out[used++] = ']';
    out[used] = '\0';
    return 0;
}

int sxcl_mods_modrinth_search_url(const sxcl_mods_query *q, char *out, size_t out_len)
{
    if (q == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    const int limit = (q->limit <= 0) ? 20 : (q->limit > 100 ? 100 : q->limit);
    const int offset = q->offset > 0 ? q->offset : 0;
    const char *sort = (q->sort != NULL && q->sort[0] != '\0') ? q->sort : "relevance";
    char facets[768];
    if (modrinth_facets(q, facets, sizeof(facets)) != 0) {
        return -1;
    }
    char escaped_text[400];
    escaped_text[0] = '\0';
    if (q->text != NULL && q->text[0] != '\0') {
        url_escape(q->text, escaped_text, sizeof(escaped_text));
    }
    const int n = snprintf(out, out_len, "https://api.modrinth.com/v2/search?query=%s&index=%s"
                                         "&limit=%d&offset=%d%s%s",
                           escaped_text, sort, limit, offset,
                           facets[0] != '\0' ? "&facets=" : "", facets);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

int sxcl_mods_modrinth_versions_url(const char *project_id, const char *game_version,
                                    const char *loader, char *out, size_t out_len)
{
    if (project_id == NULL || project_id[0] == '\0' || out == NULL || out_len == 0) {
        return -1;
    }
    char filters[512];
    filters[0] = '\0';
    if (game_version != NULL && game_version[0] != '\0') {
        char esc[120];
        url_escape(game_version, esc, sizeof(esc));
        (void)snprintf(filters, sizeof(filters), "?game_versions=[\"%s\"]", esc);
    }
    if (loader != NULL && loader[0] != '\0' && !str_equal_ci(loader, "vanilla")) {
        char esc[120];
        url_escape(loader, esc, sizeof(esc));
        const size_t used = strlen(filters);
        (void)snprintf(filters + used, sizeof(filters) - used, "%sloaders=[\"%s\"]",
                       filters[0] != '\0' ? "&" : "?", esc);
    }
    const int n = snprintf(out, out_len, "https://api.modrinth.com/v2/project/%s/version%s",
                           project_id, filters);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 1;
}

/* ── 解析：搜索结果 ── */

int sxcl_mods_modrinth_search_parse(const char *json, size_t len, sxcl_mod_page *out, char *err,
                                    size_t err_len)
{
    if (json == NULL || out == NULL) {
        set_err(err, err_len, "参数不全");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char perr[160];
    perr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(json, len, perr, sizeof(perr));
    if (doc == NULL) {
        set_err(err, err_len, "响应不是合法 JSON");
        return -1;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *hits = sxcl_json_get(root, "hits");
    const size_t n = sxcl_json_size(hits);
    size_t cap = n > SXCL_MODS_LIST_MAX ? SXCL_MODS_LIST_MAX : n;
    if (cap > 0) {
        out->items = (sxcl_mod_hit *)calloc(cap, sizeof(sxcl_mod_hit));
        if (out->items == NULL) {
            sxcl_json_free(doc);
            set_err(err, err_len, "内存不足");
            return -1;
        }
    }
    for (size_t i = 0; i < cap; ++i) {
        const sxcl_json_value *hit = sxcl_json_at(hits, i);
        sxcl_mod_hit *dst = &out->items[out->count];
        copy_cap(dst->id, sizeof(dst->id), sxcl_json_get_string(hit, "project_id", ""));
        copy_cap(dst->slug, sizeof(dst->slug), sxcl_json_get_string(hit, "slug", ""));
        copy_cap(dst->title, sizeof(dst->title), sxcl_json_get_string(hit, "title", ""));
        copy_cap(dst->author, sizeof(dst->author), sxcl_json_get_string(hit, "author", ""));
        copy_cap(dst->description, sizeof(dst->description),
                 sxcl_json_get_string(hit, "description", ""));
        copy_cap(dst->icon_url, sizeof(dst->icon_url), sxcl_json_get_string(hit, "icon_url", ""));
        copy_cap(dst->license, sizeof(dst->license), sxcl_json_get_string(hit, "license", ""));
        copy_cap(dst->updated, sizeof(dst->updated), sxcl_json_get_string(hit, "date_modified", ""));
        copy_cap(dst->source, sizeof(dst->source), "modrinth");
        dst->downloads = sxcl_json_get_int64(hit, "downloads", 0);
        dst->follows = sxcl_json_get_int64(hit, "follows", 0);
        join_strings(sxcl_json_get(hit, "display_categories"), dst->categories,
                     sizeof(dst->categories), 4);
        if (dst->categories[0] == '\0') {
            join_strings(sxcl_json_get(hit, "categories"), dst->categories,
                         sizeof(dst->categories), 4);
        }
        join_strings(sxcl_json_get(hit, "versions"), dst->versions, sizeof(dst->versions), 6);
        if (dst->id[0] == '\0' && dst->slug[0] == '\0') {
            continue;   /* 既没 id 也没短名：这一条没法用 */
        }
        ++out->count;
    }
    out->total = (size_t)sxcl_json_get_int64(root, "total_hits", (int64_t)out->count);
    out->offset = (size_t)sxcl_json_get_int64(root, "offset", 0);
    sxcl_json_free(doc);
    return 0;
}

void sxcl_mods_page_free(sxcl_mod_page *page)
{
    if (page == NULL) {
        return;
    }
    free(page->items);
    memset(page, 0, sizeof(*page));
}

/* ── 解析：版本与文件 ── */

int sxcl_mods_modrinth_versions_parse(const char *json, size_t len, sxcl_mod_file *out, size_t cap,
                                      size_t *count, char *err, size_t err_len)
{
    if (count != NULL) {
        *count = 0;
    }
    if (json == NULL || out == NULL || cap == 0) {
        set_err(err, err_len, "参数不全");
        return -1;
    }
    char perr[160];
    perr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(json, len, perr, sizeof(perr));
    if (doc == NULL) {
        set_err(err, err_len, "版本列表不是合法 JSON");
        return -1;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const size_t n = sxcl_json_size(root);
    size_t written = 0;
    for (size_t i = 0; i < n && written < cap; ++i) {
        const sxcl_json_value *ver = sxcl_json_at(root, i);
        const sxcl_json_value *files = sxcl_json_get(ver, "files");
        /* 一个版本可能有多个文件（主文件 + 附加）；主文件优先，没有标记就取第一个 */
        const sxcl_json_value *chosen = NULL;
        const size_t file_count = sxcl_json_size(files);
        for (size_t k = 0; k < file_count; ++k) {
            const sxcl_json_value *f = sxcl_json_at(files, k);
            if (chosen == NULL) {
                chosen = f;
            }
            if (sxcl_json_get_bool(f, "primary", 0)) {
                chosen = f;
                break;
            }
        }
        if (chosen == NULL) {
            continue;
        }
        sxcl_mod_file *dst = &out[written];
        copy_cap(dst->version_id, sizeof(dst->version_id), sxcl_json_get_string(ver, "id", ""));
        copy_cap(dst->version_number, sizeof(dst->version_number),
                 sxcl_json_get_string(ver, "version_number", ""));
        copy_cap(dst->title, sizeof(dst->title), sxcl_json_get_string(ver, "name", ""));
        copy_cap(dst->filename, sizeof(dst->filename),
                 sxcl_json_get_string(chosen, "filename", ""));
        copy_cap(dst->url, sizeof(dst->url), sxcl_json_get_string(chosen, "url", ""));
        dst->size = sxcl_json_get_int64(chosen, "size", 0);
        dst->primary = sxcl_json_get_bool(chosen, "primary", 0);
        copy_cap(dst->sha1, sizeof(dst->sha1),
                 sxcl_json_get_string(sxcl_json_get(chosen, "hashes"), "sha1", ""));
        join_strings(sxcl_json_get(ver, "game_versions"), dst->game_versions,
                     sizeof(dst->game_versions), 6);
        join_strings(sxcl_json_get(ver, "loaders"), dst->loaders, sizeof(dst->loaders), 4);
        /* 依赖只展示不装（PCL 口径）：把 required 的 project_id 列出来 */
        {
            const sxcl_json_value *deps = sxcl_json_get(ver, "dependencies");
            const size_t dn = sxcl_json_size(deps);
            size_t used = 0;
            dst->required_deps[0] = '\0';
            for (size_t d = 0; d < dn && used + 2 < sizeof(dst->required_deps); ++d) {
                const sxcl_json_value *dep = sxcl_json_at(deps, d);
                const char *kind = sxcl_json_get_string(dep, "dependency_type", "");
                if (!str_equal_ci(kind, "required")) {
                    continue;
                }
                const char *pid = sxcl_json_get_string(dep, "project_id", "");
                if (pid[0] == '\0') {
                    continue;
                }
                const int w = snprintf(dst->required_deps + used, sizeof(dst->required_deps) - used,
                                       "%s%s", used > 0 ? " " : "", pid);
                if (w > 0) {
                    used += (size_t)w;
                }
            }
        }
        if (dst->url[0] == '\0' || dst->filename[0] == '\0') {
            continue;   /* 没有可下载的文件：跳过 */
        }
        ++written;
    }
    *count = written;
    sxcl_json_free(doc);
    return 0;
}

int sxcl_mods_pick_file(const sxcl_mod_file *files, size_t count, const char *game_version,
                        const char *loader, sxcl_mod_file *out)
{
    if (files == NULL || count == 0 || out == NULL) {
        return -1;
    }
    int best = -1;
    int best_score = -1;
    for (size_t i = 0; i < count; ++i) {
        int score = 0;
        /* 加载器不匹配的直接归零（PCL：不自动换加载器） */
        const int loader_ok = (loader == NULL || loader[0] == '\0' ||
                               str_equal_ci(loader, "vanilla"))
                                  ? 1
                                  : text_list_has(files[i].loaders, loader);
        const int version_ok = (game_version == NULL || game_version[0] == '\0')
                                   ? 1
                                   : text_list_has(files[i].game_versions, game_version);
        if (!loader_ok || !version_ok) {
            continue;
        }
        score = 10;
        if (files[i].primary) {
            score += 2;
        }
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best < 0) {
        return -1;   /* 这一批里没有配得上的（界面如实说"没有适合这个实例的版本"） */
    }
    *out = files[best];
    return 0;
}


/* ── CurseForge（第二个源） ── */

int sxcl_mods_curseforge_loader_type(const char *loader_slug)
{
    if (loader_slug == NULL || loader_slug[0] == '\0') {
        return 0;
    }
    if (str_equal_ci(loader_slug, "forge")) {
        return 1;
    }
    if (str_equal_ci(loader_slug, "fabric")) {
        return 4;
    }
    if (str_equal_ci(loader_slug, "quilt")) {
        return 5;
    }
    if (str_equal_ci(loader_slug, "neoforge")) {
        return 6;
    }
    return 0;   /* 认不出来(比如 optifine/iris):不筛,让上游自己排 */
}

int sxcl_mods_curseforge_class_id(const char *project_type)
{
    if (project_type == NULL || project_type[0] == '\0' || str_equal_ci(project_type, "mod")) {
        return 6;   /* Mods（默认那一栏就是模组） */
    }
    if (str_equal_ci(project_type, "shader")) {
        return 6552;
    }
    if (str_equal_ci(project_type, "resourcepack")) {
        return 12;
    }
    if (str_equal_ci(project_type, "datapack")) {
        return 6945;
    }
    if (str_equal_ci(project_type, "plugin")) {
        return 5;
    }
    return 0;
}

int sxcl_mods_curseforge_search_url(const sxcl_mods_query *q, char *out, size_t out_len)
{
    if (q == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    const int limit = (q->limit <= 0) ? 20 : (q->limit > 50 ? 50 : q->limit); /* CF 上限 50 */
    const int offset = q->offset > 0 ? q->offset : 0;
    const int class_id = sxcl_mods_curseforge_class_id(q->project_type);
    const int loader_type = sxcl_mods_curseforge_loader_type(q->loader);
    char text[400];
    text[0] = '\0';
    if (q->text != NULL && q->text[0] != '\0') {
        url_escape(q->text, text, sizeof(text));
    }
    char game[120];
    game[0] = '\0';
    if (q->game_version != NULL && q->game_version[0] != '\0') {
        url_escape(q->game_version, game, sizeof(game));
    }
    /* index 0 = 按相关度（与 Modrinth 的 relevance 对齐）；sortOrder 只对其它 index 有意义 */
    const int n = snprintf(out, out_len,
                           "https://api.curseforge.com/v1/mods/search?gameId=432&index=%d"
                           "&pageSize=%d&searchFilter=%s&classId=%d&gameVersion=%s&modLoaderType=%d",
                           offset, limit, text, class_id, game, loader_type);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

int sxcl_mods_curseforge_search_parse(const char *json, size_t len, sxcl_mod_page *out, char *err,
                                      size_t err_len)
{
    if (json == NULL || out == NULL) {
        set_err(err, err_len, "参数不全");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char perr[160];
    perr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(json, len, perr, sizeof(perr));
    if (doc == NULL) {
        set_err(err, err_len, "响应不是合法 JSON");
        return -1;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *data = sxcl_json_get(root, "data");
    const size_t n = sxcl_json_size(data);
    size_t cap = n > SXCL_MODS_LIST_MAX ? SXCL_MODS_LIST_MAX : n;
    if (cap > 0) {
        out->items = (sxcl_mod_hit *)calloc(cap, sizeof(sxcl_mod_hit));
        if (out->items == NULL) {
            sxcl_json_free(doc);
            set_err(err, err_len, "内存不足");
            return -1;
        }
    }
    for (size_t i = 0; i < cap; ++i) {
        const sxcl_json_value *mod = sxcl_json_at(data, i);
        sxcl_mod_hit *dst = &out->items[out->count];
        const int64_t id = sxcl_json_get_int64(mod, "id", 0);
        (void)snprintf(dst->id, sizeof(dst->id), "%lld", (long long)id);
        copy_cap(dst->slug, sizeof(dst->slug), sxcl_json_get_string(mod, "slug", ""));
        copy_cap(dst->title, sizeof(dst->title), sxcl_json_get_string(mod, "name", ""));
        copy_cap(dst->description, sizeof(dst->description),
                 sxcl_json_get_string(mod, "summary", ""));
        copy_cap(dst->icon_url, sizeof(dst->icon_url),
                 sxcl_json_get_string(sxcl_json_get(mod, "logo"), "url", ""));
        copy_cap(dst->updated, sizeof(dst->updated), sxcl_json_get_string(mod, "dateModified", ""));
        copy_cap(dst->source, sizeof(dst->source), "curseforge");
        dst->downloads = sxcl_json_get_int64(mod, "downloadCount", 0);
        /* authors[] 只取第一个名字;categories[] 取 name(CF 给的是显示名,与 Modrinth 的 slug 不同) */
        {
            const sxcl_json_value *authors = sxcl_json_get(mod, "authors");
            const sxcl_json_value *first = sxcl_json_at(authors, 0);
            copy_cap(dst->author, sizeof(dst->author),
                     sxcl_json_get_string(first, "name", ""));
        }
        {
            const sxcl_json_value *cats = sxcl_json_get(mod, "categories");
            const size_t cn = sxcl_json_size(cats);
            size_t used = 0;
            dst->categories[0] = '\0';
            for (size_t k = 0; k < cn && k < 4; ++k) {
                const char *name = sxcl_json_get_string(sxcl_json_at(cats, k), "name", "");
                if (name[0] == '\0') {
                    continue;
                }
                const int w = snprintf(dst->categories + used, sizeof(dst->categories) - used,
                                       "%s%s", used > 0 ? " " : "", name);
                if (w > 0) {
                    used += (size_t)w;
                }
            }
        }
        /* latestFiles[].gameVersions[] 里就是支持的游戏版本（CF 只在文件上给） */
        {
            const sxcl_json_value *files = sxcl_json_get(mod, "latestFiles");
            const sxcl_json_value *first = sxcl_json_at(files, 0);
            join_strings(sxcl_json_get(first, "gameVersions"), dst->versions,
                         sizeof(dst->versions), 6);
        }
        if (dst->id[0] == '0' && dst->id[1] == '\0') {
            continue;   /* id = 0:这一条没法用 */
        }
        ++out->count;
    }
    /* CF 的分页在 pagination 里（index/pageSize/resultCount/totalCount） */
    out->total = (size_t)sxcl_json_get_int64(sxcl_json_get(root, "pagination"), "totalCount",
                                             (int64_t)out->count);
    out->offset = (size_t)sxcl_json_get_int64(sxcl_json_get(root, "pagination"), "index", 0);
    sxcl_json_free(doc);
    return 0;
}

/* ── 落盘目录 ── */

int sxcl_mods_dir(const char *game_dir, const char *instance, const char *kind, int isolated,
                  char *out, size_t out_len)
{
    if (game_dir == NULL || game_dir[0] == '\0' || out == NULL || out_len == 0) {
        return -1;
    }
    const char *leaf = (kind != NULL && kind[0] != '\0') ? kind : "mods";
    const size_t n = strlen(game_dir);
    const int need_sep = n > 0 && game_dir[n - 1] != '/' && game_dir[n - 1] != '\\';
    const char *sep = need_sep ? "/" : "";
    int w = 0;
    if (isolated && instance != NULL && instance[0] != '\0') {
        w = snprintf(out, out_len, "%s%sversions/%s/%s", game_dir, sep, instance, leaf);
    } else {
        w = snprintf(out, out_len, "%s%s%s", game_dir, sep, leaf);
    }
    return (w < 0 || (size_t)w >= out_len) ? -1 : 0;
}

#ifdef __cplusplus
}
#endif
