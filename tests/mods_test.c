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

int main(void) {
    test_search_url();
    test_search_parse();
    test_versions_and_pick();
    test_mods_dir();
    printf("mods 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
