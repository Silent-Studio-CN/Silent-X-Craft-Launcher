/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组（MOD）与光影（Shader）的**资源来源层**（docs/22 的 A1，走 PCL 线路）。
//
// 口径（docs/22 §6「模组功能走 PCL 线路」逐条对齐）：
//   * **双源**：Modrinth（免 key，默认开）+ CurseForge（官方 API 要 key，用户自己在设置里填；
//     没填就如实不显示这一源，**不假装有**）；
//   * 上游的筛选（游戏版本 / 加载器 / 分类）交给**服务端 facets**，不在本地过滤 ——
//     本地过滤会把"装了 10 个模组、筛出来 3 个"当成"没有更多了"，用户永远翻不到后面的；
//   * **不自动换加载器**：实例是 Fabric 就只显示 Fabric 的包（PCL 同款）；
//   * **依赖只展示不装**（下载时把 required 依赖列出来，装不装用户自己决定）；
//   * 同名文件**直接覆盖**（PCL 同款；HMCL 的"拦下改名"是它 UI 层独有的做法）。
//
// 这一层**只做纯逻辑**：拼 URL、解析 JSON、挑文件、算落盘路径。网络与界面在外面
// （界面拿 HTTP 取文本 -> 这里解析 -> 引擎下载）。这样它不联网也能单测。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXCL_MODS_ID_MAX        72
#define SXCL_MODS_TEXT_MAX      256
#define SXCL_MODS_DESC_MAX      640
#define SXCL_MODS_URL_MAX       512
#define SXCL_MODS_LIST_MAX      512   /* 一页最多解析这么多条 */

/** 搜索条件（界面 -> URL）。空串 = 这一维不筛。 */
typedef struct sxcl_mods_query {
    const char *text;          /**< 关键词，可空 */
    const char *game_version;  /**< 如 "1.20.1" */
    const char *loader;        /**< "fabric" / "forge" / "neoforge" / "quilt" / 空 */
    const char *category;      /**< 分类 slug，可空 */
    /** 资源类型 facet（Modrinth 的 project_type）："mod" / "shader" / "resourcepack" /
     *  "datapack" / "plugin"；空 = 不筛（两个源混着来）。
     *  **模组那一栏要传 "mod"**：不传的话光影/资源包会混进模组列表（实测 Modrinth 默认不筛类型）。 */
    const char *project_type;
    const char *sort;          /**< "relevance" / "downloads" / "updated" / "newest"，可空 = 相关度 */
    int offset;                /**< >= 0 */
    int limit;                 /**< 1..100，<=0 取 20 */
} sxcl_mods_query;

/** 一条搜索结果（两个源的字段取并集，缺的留空）。 */
typedef struct sxcl_mod_hit {
    char id[SXCL_MODS_ID_MAX];        /**< 工程 id（Modrinth: project_id / CF: modId 的十进制） */
    char slug[SXCL_MODS_ID_MAX + 24]; /**< 短名（Modrinth 有；CF 没有） */
    char title[SXCL_MODS_TEXT_MAX];
    char author[SXCL_MODS_TEXT_MAX];
    char description[SXCL_MODS_DESC_MAX];
    char icon_url[SXCL_MODS_URL_MAX];
    char license[64];
    char source[16];                  /**< "modrinth" / "curseforge" */
    int64_t downloads;
    int64_t follows;
    char updated[40];                 /**< ISO 时间，原样带出来（界面自己格式化） */
    char categories[192];             /**< 空格分隔（分类 slug） */
    char versions[192];               /**< 空格分隔（支持的游戏版本，前若干个） */
} sxcl_mod_hit;

typedef struct sxcl_mod_page {
    sxcl_mod_hit *items;   /**< malloc 的数组（free 用 sxcl_mods_page_free） */
    size_t count;
    size_t total;          /**< 上游报的总命中数（翻页用） */
    size_t offset;
} sxcl_mod_page;

/** Modrinth 搜索 URL（纯函数，便于单测；facets 走服务端）。 */
int sxcl_mods_modrinth_search_url(const sxcl_mods_query *q, char *out, size_t out_len);
/** 解析 Modrinth /v2/search 的响应。 */
int sxcl_mods_modrinth_search_parse(const char *json, size_t len, sxcl_mod_page *out, char *err,
                                    size_t err_len);
void sxcl_mods_page_free(sxcl_mod_page *page);

/** 一个可下载的文件（版本 + 主文件）。 */
typedef struct sxcl_mod_file {
    char version_id[SXCL_MODS_ID_MAX];
    char version_number[64];
    char title[SXCL_MODS_TEXT_MAX];
    char filename[SXCL_MODS_TEXT_MAX];
    char url[SXCL_MODS_URL_MAX];
    char sha1[48];
    int64_t size;
    int primary;                  /**< 1 = 上游标记的主文件 */
    char game_versions[192];      /**< 空格分隔 */
    char loaders[128];            /**< 空格分隔 */
    char required_deps[192];      /**< 空格分隔的 required 依赖的版本 id（**只展示不装**） */
} sxcl_mod_file;

/** Modrinth 版本列表 URL：/v2/project/<id>/version?game_versions=[...]&loaders=[...] */
int sxcl_mods_modrinth_versions_url(const char *project_id, const char *game_version,
                                    const char *loader, char *out, size_t out_len);
/** 解析 Modrinth /v2/project/<id>/version 的响应（数组）。最多写 cap 条，count 回条数。 */
int sxcl_mods_modrinth_versions_parse(const char *json, size_t len, sxcl_mod_file *out, size_t cap,
                                      size_t *count, char *err, size_t err_len);

/* ── CurseForge（第二个源；官方 API 要 key，见 docs/22 §14） ──
 *
 * 与 Modrinth 的两点不同，决定了这一层的形状：
 *   1. **必须带 x-api-key 头**（用户自己申请的那种）—— 没有 key 就**如实不查**，
 *      界面显示"未配置 CurseForge API key"，绝不假装有结果；
 *   2. 加载器与分类是**数字枚举**（modLoaderType / classId），不是 slug。
 */

/** 加载器 slug -> CurseForge 的 modLoaderType（认不出来返回 0 = 不筛）。
 *  1=Forge 4=Fabric 5=Quilt 6=NeoForge（官方文档的枚举）。 */
int sxcl_mods_curseforge_loader_type(const char *loader_slug);

/** 资源类型 -> classId：6=Mods 6552=Shaders 12=Resource Packs 6945=Data Packs 5=Bukkit Plugins。 */
int sxcl_mods_curseforge_class_id(const char *project_type);

/** CurseForge 搜索 URL（key 不进 URL，走请求头；gameId=432 是 Minecraft）。 */
int sxcl_mods_curseforge_search_url(const sxcl_mods_query *q, char *out, size_t out_len);
/** 解析 /v1/mods/search 的响应（{"data":[…]};字段名与官方文档一致）。 */
int sxcl_mods_curseforge_search_parse(const char *json, size_t len, sxcl_mod_page *out, char *err,
                                      size_t err_len);

/** 从一批文件里挑"最适合这个实例的那个"：
 *  先按 loaders 里有没有这个加载器、game_versions 里有没有这个版本打分，再优先 primary，
 *  最后取上传时间最新的那个（数组通常已按时间倒序，所以"第一个最高分"就是它）。 */
int sxcl_mods_pick_file(const sxcl_mod_file *files, size_t count, const char *game_version,
                        const char *loader, sxcl_mod_file *out);

/** 模组装到哪个目录（**版本隔离之后**这件事才有意义，见 sxcl/isolation.h）：
 *  isolated != 0 -> <game_dir>/versions/<实例名>/mods；否则 <game_dir>/mods。
 *  kind 用 "mods" / "shaderpacks" / "resourcepacks"。 */
int sxcl_mods_dir(const char *game_dir, const char *instance, const char *kind, int isolated,
                  char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
