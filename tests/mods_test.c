/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 *
 * 模组资源来源层的单测（docs/22 的 A1）。**不联网**：
 * 夹具 JSON 是照着 Modrinth 官方响应**逐字段**缩下来的（字段名与层级不缩水），
 * 另有真机验收在 docs/22 §13。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif
#include <stdio.h>
#include <string.h>

#include "sxcl/mods.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what) {
    if (ok) { ++g_pass; } else { ++g_fail; printf("  [!!] %s\n", what); }
}
static void check_int(long got, long want, const char *what) {
    if (got == want) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: got %ld want %ld\n", what, got, want); }
}
static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)"); }
}

/* 真响应缩样（hits[] 的两条 + total_hits/offset） */
static const char *kSearchJson =
    "{\"hits\":["
    "{\"project_id\":\"AANobbMI\",\"project_type\":\"mod\",\"slug\":\"sodium\","
    "\"author\":\"jellysquid3\",\"title\":\"Sodium\","
    "\"description\":\"Modern rendering engine and client-side optimization mod\","
    "\"categories\":[\"optimization\"],\"display_categories\":[\"optimization\"],"
    "\"versions\":[\"1.21.4\",\"1.21.3\",\"1.20.1\"],\"downloads\":41234567,\"follows\":3456,"
    "\"icon_url\":\"https://cdn.modrinth.com/data/AANobbMI/icon.png\","
    "\"date_created\":\"2020-12-01T00:00:00Z\",\"date_modified\":\"2026-09-01T12:00:00Z\","
    "\"license\":\"LGPL-3.0-only\",\"client_side\":\"required\",\"server_side\":\"unsupported\"},"
    "{\"project_id\":\"P7dR8mSH\",\"slug\":\"fabric-api\",\"author\":\"modmuss50\","
    "\"title\":\"Fabric API\",\"description\":\"Lightweight and modular API\","
    "\"categories\":[\"library\"],\"display_categories\":[\"library\"],"
    "\"versions\":[\"1.21.4\"],\"downloads\":1000000,\"follows\":100,"
    "\"icon_url\":\"\",\"date_modified\":\"2026-08-30T00:00:00Z\",\"license\":\"Apache-2.0\"}"
    "],\"offset\":0,\"limit\":20,\"total_hits\":1234}";

/* 真响应缩样（版本数组两条：一条 fabric 1.21.4 且是主文件，一条 forge 1.20.1） */
static const char *kVersionsJson =
    "["
    "{\"id\":\"ver-1\",\"project_id\":\"AANobbMI\",\"name\":\"Sodium 0.6.0\","
    "\"version_number\":\"mc1.21.4-0.6.0\",\"game_versions\":[\"1.21.4\"],\"loaders\":[\"fabric\"],"
    "\"date_published\":\"2026-09-01T00:00:00Z\",\"downloads\":9000,"
    "\"files\":[{\"hashes\":{\"sha1\":\"1111111111111111111111111111111111111111\","
    "\"sha512\":\"aa\"},\"url\":\"https://cdn.modrinth.com/data/AANobbMI/versions/ver-1/sodium.jar\","
    "\"filename\":\"sodium-fabric-0.6.0.jar\",\"primary\":true,\"size\":1234567}],"
    "\"dependencies\":[{\"version_id\":null,\"project_id\":\"P7dR8mSH\","
    "\"dependency_type\":\"required\"}]},"
    "{\"id\":\"ver-2\",\"name\":\"Sodium 0.5.0\",\"version_number\":\"mc1.20.1-0.5.0\","
    "\"game_versions\":[\"1.20.1\"],\"loaders\":[\"forge\"],"
    "\"files\":[{\"hashes\":{\"sha1\":\"2222222222222222222222222222222222222222\"},"
    "\"url\":\"https://cdn.modrinth.com/data/AANobbMI/versions/ver-2/sodium-old.jar\","
    "\"filename\":\"sodium-forge-0.5.0.jar\",\"primary\":false,\"size\":999}],"
    "\"dependencies\":[]}"
    "]";

/* 真机踩到的那个坑的夹具:一个光影文件支持一长串游戏版本（真响应里是 **76** 个,这里缩成 12 个）,
 * 目标版本 1.20.1 排在第 8 位 —— 字段被截断成前 6 个的话,pick_file 会对着一屏能装的包
 * 判"没有能用的文件"（2026-09-22 晚用 Complementary Shaders 真机复现）。 */
static const char *kManyVersionsJson =
    "[{\"id\":\"sh-1\",\"name\":\"Complementary Reimagined r5.9.3\",\"version_number\":\"r5.9.3\","
    "\"game_versions\":[\"1.7.10\",\"1.8\",\"1.8.1\",\"1.8.2\",\"1.8.3\",\"1.8.4\",\"1.8.5\","
    "\"1.20.1\",\"1.20.2\",\"1.20.3\",\"1.20.4\",\"1.20.5\"],\"loaders\":[\"iris\",\"optifine\"],"
    "\"files\":[{\"hashes\":{\"sha1\":\"4444444444444444444444444444444444444444\"},"
    "\"url\":\"https://cdn.modrinth.com/data/HVnmMxH1/versions/sh-1/x.zip\","
    "\"filename\":\"ComplementaryReimagined_r5.9.3.zip\",\"primary\":true,\"size\":553397}]}]";

static void test_search_url(void) {
    sxcl_mods_query q;
    memset(&q, 0, sizeof(q));
    char url[1024];

    q.text = "sodium";
    q.game_version = "1.20.1";
    q.loader = "fabric";
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "搜索 URL 拼得出来");
    check(strstr(url, "https://api.modrinth.com/v2/search?") == url, "  域名与路径对");
    check(strstr(url, "query=sodium") != NULL, "  关键词在");
    check(strstr(url, "limit=20") != NULL, "  默认一页 20 条");
    check(strstr(url, "index=relevance") != NULL, "  默认按相关度");
    check(strstr(url, "facets=[") != NULL, "  筛走服务端 facets");
    /* 官方文档里的实例就是原样写 facets=[["versions:1.20.1"],["categories:fabric"]]
     * （方括号/冒号/引号都不转义，上游按标准查询串解），我们照做——只把**关键词**转义。 */
    check(strstr(url, "versions:1.20.1") != NULL, "  游戏版本进了 facets");
    check(strstr(url, "categories:fabric") != NULL, "  加载器进了 facets");

    /* 资源类型 facet：模组那一栏必须传 "mod"（不传的话光影/资源包会混进模组列表） */
    memset(&q, 0, sizeof(q));
    q.project_type = "mod";
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "只筛类型也拼得出来");
    check(strstr(url, "project_type:mod") != NULL, "  project_type 进了 facets");
    q.project_type = "shader";
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "光影类型也拼得出来");
    check(strstr(url, "project_type:shader") != NULL, "  shader 进了 facets");
    q.game_version = "1.20.1";
    q.loader = "iris";
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "光影+版本+加载器一起");
    check(strstr(url, "project_type:shader") != NULL && strstr(url, "versions:1.20.1") != NULL &&
              strstr(url, "categories:iris") != NULL,
          "  三个 facet 组都在");

    /* 空条件：不拼 facets，也不拼空 query */
    memset(&q, 0, sizeof(q));
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "空条件也能拼");
    check(strstr(url, "facets") == NULL, "  没有筛选就不带 facets");
    check(strstr(url, "query=&") != NULL, "  空关键词留空(上游按空串当「不筛」)");
    q.limit = 500;
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "超大 limit 也拼得出来");
    check(strstr(url, "limit=100") != NULL, "  limit 被夹到 100(上游上限)");
    q.text = "带 空格&符号";
    check_int(sxcl_mods_modrinth_search_url(&q, url, sizeof(url)), 0, "带特殊字符也拼得出来");
    check(strstr(url, "%20") != NULL && strstr(url, "%26") != NULL, "  空格与 & 都转义了");

    check_int(sxcl_mods_modrinth_versions_url("AANobbMI", "1.20.1", "fabric", url, sizeof(url)), 1,
              "版本列表 URL 拼得出来");
    check(strstr(url, "/v2/project/AANobbMI/version?") == url + strlen("https://api.modrinth.com"),
          "  路径对");
    check(strstr(url, "game_versions=[\"1.20.1\"]") != NULL, "  版本筛在");
    check(strstr(url, "&loaders=[\"fabric\"]") != NULL, "  加载器筛在");
    check_int(sxcl_mods_modrinth_versions_url(NULL, NULL, NULL, url, sizeof(url)), -1,
              "没有工程 id = 参数错");
}

static void test_search_parse(void) {
    sxcl_mod_page page;
    char err[160];
    err[0] = '\0';
    check_int(sxcl_mods_modrinth_search_parse(kSearchJson, strlen(kSearchJson), &page, err,
                                              sizeof(err)),
              0, "搜索结果解析成功");
    check_int((long)page.count, 2, "  两条命中");
    check_int((long)page.total, 1234, "  上游总命中数带出来了（翻页要用）");
    check_str(page.items[0].id, "AANobbMI", "  第一条 id");
    check_str(page.items[0].slug, "sodium", "  短名");
    check_str(page.items[0].title, "Sodium", "  标题");
    check_str(page.items[0].author, "jellysquid3", "  作者");
    check_str(page.items[0].source, "modrinth", "  来源标记");
    check_str(page.items[0].license, "LGPL-3.0-only", "  许可证");
    check_str(page.items[0].categories, "optimization", "  分类");
    check_str(page.items[0].versions, "1.21.4 1.21.3 1.20.1", "  支持的游戏版本(空格分隔)");
    check_int((long)page.items[0].downloads, 41234567, "  下载量");
    check_str(page.items[0].icon_url, "https://cdn.modrinth.com/data/AANobbMI/icon.png", "  图标");
    check_str(page.items[1].title, "Fabric API", "  第二条标题");
    check_str(page.items[1].icon_url, "", "  第二条没有图标(空串,不是垃圾)");
    sxcl_mods_page_free(&page);
    check(page.items == NULL && page.count == 0, "释放之后清干净了");

    err[0] = '\0';
    check_int(sxcl_mods_modrinth_search_parse("不是 JSON", 8, &page, err, sizeof(err)), -1,
              "不是 JSON = 失败");
    check(err[0] != '\0', "  有人话原因");
    err[0] = '\0';
    check_int(sxcl_mods_modrinth_search_parse("{\"hits\":[],\"total_hits\":0}", 26, &page, err,
                                              sizeof(err)),
              0, "空结果也算成功(界面显示「没找到」而不是报错)");
    check_int((long)page.count, 0, "  0 条");
    sxcl_mods_page_free(&page);
}

static void test_versions_and_pick(void) {
    sxcl_mod_file files[8];
    size_t count = 0;
    char err[160];
    err[0] = '\0';
    check_int(sxcl_mods_modrinth_versions_parse(kVersionsJson, strlen(kVersionsJson), files, 8,
                                                &count, err, sizeof(err)),
              0, "版本列表解析成功");
    check_int((long)count, 2, "  两条版本");
    check_str(files[0].version_id, "ver-1", "  版本 id");
    check_str(files[0].filename, "sodium-fabric-0.6.0.jar", "  主文件名");
    check_str(files[0].sha1, "1111111111111111111111111111111111111111", "  官方 sha1 带出来了");
    check_int((long)files[0].size, 1234567, "  文件大小");
    check_int(files[0].primary, 1, "  标记了 primary");
    check_str(files[0].loaders, "fabric", "  加载器");
    check_str(files[0].game_versions, "1.21.4", "  游戏版本");
    check_str(files[0].required_deps, "P7dR8mSH", "  required 依赖的工程 id(只展示不装)");
    check_str(files[1].required_deps, "", "  没有依赖就是空");

    /* 挑文件:加载器与游戏版本都要对上 */
    sxcl_mod_file picked;
    memset(&picked, 0, sizeof(picked));
    check_int(sxcl_mods_pick_file(files, count, "1.21.4", "fabric", &picked), 0, "挑出 fabric/1.21.4 那个");
    check_str(picked.filename, "sodium-fabric-0.6.0.jar", "  挑对了");
    check_int(sxcl_mods_pick_file(files, count, "1.20.1", "forge", &picked), 0, "挑出 forge/1.20.1 那个");
    check_str(picked.filename, "sodium-forge-0.5.0.jar", "  挑对了");
    check_int(sxcl_mods_pick_file(files, count, "1.21.4", "forge", &picked), -1,
              "版本对但加载器不对 -> 不挑（PCL:不自动换加载器）");
    check_int(sxcl_mods_pick_file(files, count, "1.19.2", "fabric", &picked), -1,
              "这一批里没有这个游戏版本 -> 不挑");
    check_int(sxcl_mods_pick_file(files, 0, "1.21.4", "fabric", &picked), -1, "空列表 -> 不挑");
    /* 不给版本/加载器时不筛（界面上"全部版本"那种） */
    check_int(sxcl_mods_pick_file(files, count, NULL, NULL, &picked), 0, "不筛时挑第一个能下的");
}

/* 支持一长串游戏版本的文件也必须挑得出来（匹配字段不许被截断成"前几个"）。 */
static void test_many_versions(void) {
    sxcl_mod_file files[4];
    size_t count = 0;
    char err[160];
    err[0] = '\0';
    check_int(sxcl_mods_modrinth_versions_parse(kManyVersionsJson, strlen(kManyVersionsJson), files, 4,
                                                &count, err, sizeof(err)),
              0, "长版本列表解析成功");
    check_int((long)count, 1, "  一条");
    check(strstr(files[0].game_versions, "1.7.10") != NULL, "  第一个版本在（匹配字段是完整列表,不是后 6 个）");
    check(strstr(files[0].game_versions, "1.20.1") != NULL, "  第 8 位的目标版本也在");
    check(strstr(files[0].game_versions, "1.20.5") != NULL, "  最后一个版本也在");
    check_str(files[0].loaders, "iris optifine", "  加载器全在");

    sxcl_mod_file picked;
    memset(&picked, 0, sizeof(picked));
    check_int(sxcl_mods_pick_file(files, count, "1.20.1", "iris", &picked), 0,
              "挑得出来（回归:前 6 个里没有 1.20.1 时不许判「没有能用的文件」）");
    check_str(picked.filename, "ComplementaryReimagined_r5.9.3.zip", "  挑对了");
    check_int(sxcl_mods_pick_file(files, count, "1.7.10", "optifine", &picked), 0, "第一个版本也能挑");
    check_int(sxcl_mods_pick_file(files, count, "1.20.1", "fabric", &picked), -1,
              "加载器不对还是不挑（口径没被放松）");
}

static void test_mods_dir(void) {
    char out[512];
    check_int(sxcl_mods_dir("D:/mc", "1.20.1-fabric", "mods", 1, out, sizeof(out)), 0, "隔离时拼得出来");
    check_str(out, "D:/mc/versions/1.20.1-fabric/mods", "  隔离:versions/<实例>/mods");
    check_int(sxcl_mods_dir("D:/mc", "1.20.1-fabric", "shaderpacks", 1, out, sizeof(out)), 0, "光影也拼得出来");
    check_str(out, "D:/mc/versions/1.20.1-fabric/shaderpacks", "  光影在实例目录里");
    check_int(sxcl_mods_dir("D:/mc/", "x", "mods", 0, out, sizeof(out)), 0, "没隔离也拼得出来");
    check_str(out, "D:/mc/mods", "  没隔离:根/mods（所有版本共用,正是隔离要解决的问题）");
    check_int(sxcl_mods_dir("D:/mc", "x", NULL, 0, out, sizeof(out)), 0, "kind 为空按 mods");
    check_str(out, "D:/mc/mods", "  默认 mods");
    check_int(sxcl_mods_dir("", "x", "mods", 0, out, sizeof(out)), -1, "没有游戏目录 = 参数错");
    check_int(sxcl_mods_dir("D:/mc", "x", "mods", 1, out, 8), -1, "缓冲不够 = 参数错(不写半截)");
}


/* CurseForge（第二个源）：枚举映射 + URL + 解析。夹具字段名照官方文档（data[]/pagination/
 * logo.url/latestFiles[].gameVersions[]/authors[].name）。**不联网**。 */
static const char *kCfSearchJson =
    "{\"data\":[{\"id\":238222,\"name\":\"Just Enough Items\",\"slug\":\"jei\","
    "\"summary\":\"View Items and Recipes\",\"downloadCount\":123456789,"
    "\"logo\":{\"url\":\"https://media.forgecdn.net/avatars/29/334/jei.png\"},"
    "\"dateModified\":\"2026-09-01T00:00:00Z\","
    "\"authors\":[{\"id\":1,\"name\":\"mezz\"}],"
    "\"categories\":[{\"id\":423,\"name\":\"Map and Information\"},{\"id\":426,\"name\":\"API and Library\"}],"
    "\"latestFiles\":[{\"id\":1,\"gameVersions\":[\"1.20.1\",\"1.20.2\",\"Forge\"]}]}],"
    "\"pagination\":{\"index\":0,\"pageSize\":20,\"resultCount\":1,\"totalCount\":5432}}";

static void test_curseforge(void) {
    check_int(sxcl_mods_curseforge_loader_type("forge"), 1, "CF:forge=1");
    check_int(sxcl_mods_curseforge_loader_type("fabric"), 4, "CF:fabric=4");
    check_int(sxcl_mods_curseforge_loader_type("quilt"), 5, "CF:quilt=5");
    check_int(sxcl_mods_curseforge_loader_type("neoforge"), 6, "CF:neoforge=6");
    check_int(sxcl_mods_curseforge_loader_type("optifine"), 0, "CF:认不出的加载器=不筛");
    check_int(sxcl_mods_curseforge_loader_type(NULL), 0, "CF:没有加载器=不筛");
    check_int(sxcl_mods_curseforge_class_id("mod"), 6, "CF:模组 classId=6");
    check_int(sxcl_mods_curseforge_class_id("shader"), 6552, "CF:光影 classId=6552");
    check_int(sxcl_mods_curseforge_class_id("resourcepack"), 12, "CF:资源包 classId=12");
    check_int(sxcl_mods_curseforge_class_id(NULL), 6, "CF:默认当模组");

    sxcl_mods_query q;
    memset(&q, 0, sizeof(q));
    char url[1024];
    q.text = "jei";
    q.game_version = "1.20.1";
    q.loader = "forge";
    q.project_type = "mod";
    q.limit = 999; /* 会被夹到 CF 的上限 50 */
    check_int(sxcl_mods_curseforge_search_url(&q, url, sizeof(url)), 0, "CF 搜索 URL 拼得出来");
    check(strstr(url, "https://api.curseforge.com/v1/mods/search?") == url, "  域名与路径对");
    check(strstr(url, "gameId=432") != NULL, "  gameId=432(我的世界)");
    check(strstr(url, "pageSize=50") != NULL, "  pageSize 夹到 50");
    check(strstr(url, "classId=6") != NULL, "  classId 在");
    check(strstr(url, "gameVersion=1.20.1") != NULL, "  游戏版本在");
    check(strstr(url, "modLoaderType=1") != NULL, "  Forge 的 modLoaderType 在");
    check(strstr(url, "key") == NULL, "  **key 绝不进 URL**(走请求头)");

    sxcl_mod_page page;
    char err[160];
    err[0] = '\0';
    check_int(sxcl_mods_curseforge_search_parse(kCfSearchJson, strlen(kCfSearchJson), &page, err,
                                                sizeof(err)),
              0, "CF 搜索结果解析成功");
    check_int((long)page.count, 1, "  一条");
    check_int((long)page.total, 5432, "  总数来自 pagination.totalCount");
    check_str(page.items[0].id, "238222", "  id 由数字转成字符串");
    check_str(page.items[0].title, "Just Enough Items", "  名字");
    check_str(page.items[0].slug, "jei", "  短名");
    check_str(page.items[0].author, "mezz", "  作者(authors[0].name)");
    check_str(page.items[0].icon_url, "https://media.forgecdn.net/avatars/29/334/jei.png", "  图标");
    check_str(page.items[0].source, "curseforge", "  来源标记");
    check_str(page.items[0].categories, "Map and Information API and Library", "  分类(显示名)");
    check_str(page.items[0].versions, "1.20.1 1.20.2 Forge", "  支持版本(latestFiles[0].gameVersions)");
    check_int((long)page.items[0].downloads, 123456789, "  下载量");
    sxcl_mods_page_free(&page);

    err[0] = '\0';
    check_int(sxcl_mods_curseforge_search_parse("{}", 2, &page, err, sizeof(err)), 0,
              "空 data 也算成功");
    check_int((long)page.count, 0, "  0 条");
    sxcl_mods_page_free(&page);
    check_int(sxcl_mods_curseforge_search_parse("nope", 4, &page, err, sizeof(err)), -1,
              "不是 JSON = 失败");
}


/* CurseForge 的文件列表（/v1/mods/<id>/files）：hashes algo==1 是 SHA-1、
 * dependencies relationType==3 是必装、gameVersions 里加载器名与游戏版本混在一起。 */
static const char *kCfFilesJson =
    "{\"data\":["
    "{\"id\":4321001,\"displayName\":\"jei-1.20.1-forge-15.2.0.27.jar\","
    "\"fileName\":\"jei-1.20.1-forge-15.2.0.27.jar\","
    "\"downloadUrl\":\"https://edge.forgecdn.net/files/4321/1/jei-1.20.1-forge-15.2.0.27.jar\","
    "\"fileLength\":1234567,\"gameVersions\":[\"1.20.1\",\"1.20.2\",\"Forge\"],"
    "\"hashes\":[{\"value\":\"aaaa\",\"algo\":2},"
    "{\"value\":\"3333333333333333333333333333333333333333\",\"algo\":1}],"
    "\"dependencies\":[{\"modId\":238222,\"relationType\":3},"
    "{\"modId\":999,\"relationType\":2}]},"
    "{\"id\":4321002,\"displayName\":\"jei-1.19.2-forge.jar\",\"fileName\":\"jei-1.19.2-forge.jar\","
    "\"downloadUrl\":\"https://edge.forgecdn.net/files/4321/2/jei-1.19.2-forge.jar\","
    "\"fileLength\":100,\"gameVersions\":[\"1.19.2\",\"Forge\"],\"hashes\":[],\"dependencies\":[]},"
    "{\"id\":4321003,\"displayName\":\"没有直链的文件\",\"fileName\":\"no-link.jar\","
    "\"downloadUrl\":null,\"fileLength\":0,\"gameVersions\":[\"1.20.1\"],\"hashes\":[],\"dependencies\":[]},"
    /* 第三条:一长串游戏版本 + 加载器名排在**最后** —— 不分堆的话 "Forge" 会被挤出缓冲 */
    "{\"id\":4321004,\"displayName\":\"many.jar\",\"fileName\":\"many.jar\","
    "\"downloadUrl\":\"https://edge.forgecdn.net/files/4321/4/many.jar\",\"fileLength\":50,"
    "\"gameVersions\":[\"1.16.5\",\"1.17\",\"1.17.1\",\"1.18\",\"1.18.1\",\"1.18.2\",\"1.19\","
    "\"1.19.1\",\"1.19.2\",\"1.20\",\"1.20.1\",\"Forge\"],\"hashes\":[],\"dependencies\":[]}"
    "] }";

static void test_curseforge_files(void) {
    char url[640];
    check_int(sxcl_mods_curseforge_versions_url(238222, "1.20.1", "forge", url, sizeof(url)), 0,
              "CF 文件列表 URL 拼得出来");
    check(strstr(url, "/v1/mods/238222/files?") != NULL, "  路径对");
    check(strstr(url, "gameVersion=1.20.1") != NULL, "  游戏版本在");
    check(strstr(url, "modLoaderType=1") != NULL, "  Forge 的枚举在");
    check_int(sxcl_mods_curseforge_versions_url(0, NULL, NULL, url, sizeof(url)), -1,
              "没有 id = 参数错");

    sxcl_mod_file files[8];
    size_t count = 0;
    char err[160];
    err[0] = '\0';
    check_int(sxcl_mods_curseforge_versions_parse(kCfFilesJson, strlen(kCfFilesJson), files, 8,
                                                 &count, err, sizeof(err)),
              0, "CF 文件列表解析成功");
    check_int((long)count, 3, "  三条**有直链**的(没有直链的那条被跳过)");
    check_str(files[0].filename, "jei-1.20.1-forge-15.2.0.27.jar", "  文件名");
    check_str(files[0].version_id, "4321001", "  文件 id 转字符串");
    check_str(files[0].sha1, "3333333333333333333333333333333333333333",
              "  hashes 里 algo==1 的才是 SHA-1(不是第一个)");
    check_int((long)files[0].size, 1234567, "  文件大小");
    check_str(files[0].required_deps, "238222", "  只有 relationType==3 的算必装依赖");
    check_str(files[1].required_deps, "", "  第二条没有依赖");

    sxcl_mod_file picked;
    memset(&picked, 0, sizeof(picked));
    check_int(sxcl_mods_pick_file(files, count, "1.20.1", "forge", &picked), 0,
              "挑出 1.20.1 + forge 那条");
    check_str(picked.filename, "jei-1.20.1-forge-15.2.0.27.jar", "  挑对了");
    check_int(sxcl_mods_pick_file(files, count, "1.19.2", "fabric", &picked), -1,
              "加载器对不上 -> 不挑(与 Modrinth 同一口径)");

    /* 第三条:12 个游戏版本 + "Forge" 排在最后。分堆之后,加载器那堆只有加载器名,
     * 版本那堆只有版本号 —— 谁都不会把谁挤掉。 */
    check_str(files[2].loaders, "Forge", "CF:加载器堆里只有加载器名(没有版本号)");
    check_str(files[2].game_versions, "1.16.5 1.17 1.17.1 1.18 1.18.1 1.18.2 1.19 1.19.1 1.19.2 1.20 1.20.1",
              "CF:版本堆里只有版本号(没有加载器名)");
    check_int(sxcl_mods_pick_file(&files[2], 1, "1.20.1", "forge", &picked), 0,
              "CF:游戏版本再多也挑得出来（回归:Forge 不许被挤出缓冲）");
    check_str(picked.filename, "many.jar", "  挑对了");
}

int main(void) {
    test_search_url();
    test_search_parse();
    test_versions_and_pick();
    test_many_versions();
    test_mods_dir();
    test_curseforge();
    test_curseforge_files();
    printf("mods 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
