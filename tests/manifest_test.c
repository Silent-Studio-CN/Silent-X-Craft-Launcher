/* 元数据层测试:版本清单索引、rules 平台过滤、natives 分类器、下载计划生成。
 * 用内嵌的小样本(模仿真实 version JSON 的结构),不联网。 */
#include <stdio.h>
#include <string.h>

#include "sxcl/json.h"
#include "sxcl/manifest.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static const char *kManifest =
    "{\"latest\":{\"release\":\"1.21.4\",\"snapshot\":\"25w03a\"},"
    "\"versions\":["
    "{\"id\":\"1.21.4\",\"type\":\"release\",\"url\":\"https://example/1.21.4.json\","
    "\"sha1\":\"aaaa1111\",\"size\":1234},"
    "{\"id\":\"25w03a\",\"type\":\"snapshot\",\"url\":\"https://example/25w03a.json\","
    "\"sha1\":\"bbbb2222\",\"size\":2345}]}";

/* 版本 JSON 样本:覆盖 无条件库 / 限平台库 / 禁止平台库 / natives 分类器 */
static const char *kVersionJson =
    "{"
    "\"id\":\"1.21.4\","
    "\"downloads\":{\"client\":{\"url\":\"https://example/client.jar\",\"sha1\":\"cccc\",\"size\":100}},"
    "\"assetIndex\":{\"id\":\"17\",\"url\":\"https://example/17.json\",\"sha1\":\"dddd\",\"size\":200},"
    "\"libraries\":["
    " {\"name\":\"always:lib:1.0\",\"downloads\":{\"artifact\":{\"path\":\"always/lib/1.0/lib-1.0.jar\","
    "   \"url\":\"https://example/lib-1.0.jar\",\"sha1\":\"e1\",\"size\":10}}},"
    " {\"name\":\"winonly:lib:1.0\",\"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"windows\"}}],"
    "  \"downloads\":{\"artifact\":{\"path\":\"winonly/lib/1.0/lib-1.0.jar\",\"url\":\"https://example/w.jar\","
    "   \"sha1\":\"e2\",\"size\":20}}},"
    " {\"name\":\"notwin:lib:1.0\",\"rules\":[{\"action\":\"allow\"},"
    "  {\"action\":\"disallow\",\"os\":{\"name\":\"windows\"}}],"
    "  \"downloads\":{\"artifact\":{\"path\":\"notwin/lib/1.0/lib-1.0.jar\",\"url\":\"https://example/nw.jar\","
    "   \"sha1\":\"e3\",\"size\":30}}},"
    " {\"name\":\"native:lib:2.9.1\","
    "  \"downloads\":{\"artifact\":{\"path\":\"native/lib/2.9.1/lib-2.9.1.jar\",\"url\":\"https://example/n.jar\","
    "   \"sha1\":\"e4\",\"size\":40},"
    "   \"classifiers\":{\"natives-windows\":{\"path\":\"native/lib/2.9.1/lib-natives-windows.jar\","
    "     \"url\":\"https://example/n-win.jar\",\"sha1\":\"e5\",\"size\":50},"
    "    \"natives-linux\":{\"path\":\"native/lib/2.9.1/lib-natives-linux.jar\","
    "     \"url\":\"https://example/n-lin.jar\",\"sha1\":\"e6\",\"size\":60},"
    "    \"natives-osx\":{\"path\":\"native/lib/2.9.1/lib-natives-osx.jar\","
    "     \"url\":\"https://example/n-osx.jar\",\"sha1\":\"e7\",\"size\":70}}},"
    "  \"natives\":{\"windows\":\"natives-windows\",\"linux\":\"natives-linux\",\"osx\":\"natives-osx\"}},"
    " {\"name\":\"featured:lib:1.0\",\"rules\":[{\"action\":\"allow\",\"features\":{\"is_demo_user\":true}}],"
    "  \"downloads\":{\"artifact\":{\"path\":\"featured/lib/1.0/lib-1.0.jar\",\"url\":\"https://example/f.jar\","
    "   \"sha1\":\"e8\",\"size\":80}}}"
    "]"
    "}";

/* 在计划里找目标路径(以 rel 结尾),返回是否找到 */
static int plan_has(sxcl_version_plan *plan, const char *needle) {
    const size_t n = sxcl_version_plan_count(plan);
    for (size_t i = 0; i < n; ++i) {
        const sxcl_task *t = sxcl_version_plan_task(plan, i);
        if (t && t->dest && strstr(t->dest, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    char err[256];

    /* 1) 版本清单索引 */
    sxcl_json *doc = sxcl_json_parse(kManifest, strlen(kManifest), err, sizeof(err));
    check(doc != NULL, "解析清单样本");
    if (!doc) {
        printf("  解析失败: %s\n", err);
        return 1;
    }
    sxcl_version_list *list = sxcl_version_list_build(doc);
    check(list != NULL, "建立版本索引");
    check(sxcl_version_list_count(list) == 2, "版本数 = 2");
    check_str(sxcl_version_list_latest_release(list), "1.21.4", "latest.release");
    check_str(sxcl_version_list_latest_snapshot(list), "25w03a", "latest.snapshot");
    const sxcl_version_entry *e = sxcl_version_list_find(list, "25w03a");
    check(e != NULL && e->size == 2345, "按 id 查找并读到 size");
    check(sxcl_version_list_find(list, "not-exist") == NULL, "查不到的版本返回 NULL");

    /* 2) rules 求值:用固定平台名测,跨平台结果一致 */
    sxcl_json *vdoc = sxcl_json_parse(kVersionJson, strlen(kVersionJson), err, sizeof(err));
    check(vdoc != NULL, "解析版本 JSON 样本");
    if (!vdoc) {
        printf("  解析失败: %s\n", err);
        return 1;
    }
    const sxcl_json_value *libs = sxcl_json_get(sxcl_json_root(vdoc), "libraries");
    check(sxcl_json_size(libs) == 5, "样本里 5 个库条目");

    const sxcl_json_value *winonly = sxcl_json_at(libs, 1);
    const sxcl_json_value *win_rules = sxcl_json_get(winonly, "rules");
    check(sxcl_rules_allow(win_rules, "windows", "x64") == 1, "仅 windows 的库在 windows 上允许");
    check(sxcl_rules_allow(win_rules, "linux", "x64") == 0, "仅 windows 的库在 linux 上被过滤");

    /* 真实元数据的写法:先一条 allow 兜底,再按平台 disallow;命中顺序里最后一条生效 */
    const sxcl_json_value *notwin = sxcl_json_at(libs, 2);
    const sxcl_json_value *notwin_rules = sxcl_json_get(notwin, "rules");
    check(sxcl_rules_allow(notwin_rules, "linux", "x64") == 1, "allow 兜底 + disallow windows:linux 上允许");
    check(sxcl_rules_allow(notwin_rules, "windows", "x64") == 0, "同一条库在 windows 上被 disallow");

    /* 官方语义:有 rules 时默认不允许,且"没有任何规则命中"也是不允许
       (原生启动器如此;只有 {disallow, os:windows} 一条规则时,在 linux 上同样被过滤) */
    {
        const char *only_disallow =
            "[{\"action\":\"disallow\",\"os\":{\"name\":\"windows\"}}]";
        sxcl_json *tmp = sxcl_json_parse(only_disallow, strlen(only_disallow), err, sizeof(err));
        check(tmp != NULL, "解析单条 disallow 规则样本");
        if (tmp) {
            check(sxcl_rules_allow(sxcl_json_root(tmp), "linux", "x64") == 0,
                  "无规则命中 => 默认不允许(与原生启动器一致)");
            check(sxcl_rules_allow(sxcl_json_root(tmp), "windows", "x64") == 0,
                  "命中 disallow => 不允许");
            sxcl_json_free(tmp);
        }
    }

    const sxcl_json_value *featured = sxcl_json_at(libs, 4);
    check(sxcl_rules_allow(sxcl_json_get(featured, "rules"), "windows", "x64") == 0,
          "带 features 的规则一律不匹配");
    check(sxcl_rules_allow(NULL, "windows", "x64") == 1, "没有 rules = 允许");

    /* 3) 下载计划:按本机平台生成 */
    const char *os_name = sxcl_platform_os_name();
    sxcl_version_plan *plan = sxcl_version_plan_build(vdoc, "C:/mc", "1.21.4", err, sizeof(err));
    check(plan != NULL, "生成下载计划");
    if (plan) {
        check(plan_has(plan, "versions/1.21.4/1.21.4.jar"), "计划含客户端 jar");
        check(plan_has(plan, "assets/indexes/17.json"), "计划含资源索引");
        check(plan_has(plan, "always/lib/1.0/lib-1.0.jar"), "计划含无条件库");
        check(plan_has(plan, "native/lib/2.9.1/lib-natives-windows.jar") == (strcmp(os_name, "windows") == 0),
              "natives 分类器按平台选取");
        check(plan_has(plan, "winonly/") == (strcmp(os_name, "windows") == 0),
              "限 windows 的库按平台过滤");
        check(plan_has(plan, "notwin/") == (strcmp(os_name, "windows") != 0),
              "禁 windows 的库按平台过滤");
        check(plan_has(plan, "featured/") == 0, "带 features 的库不进计划");

        /* 每个任务都要有 URL 与目标路径,且 sha1 来自元数据(强校验不允许丢) */
        int all_ok = 1;
        const size_t n = sxcl_version_plan_count(plan);
        for (size_t i = 0; i < n; ++i) {
            const sxcl_task *t = sxcl_version_plan_task(plan, i);
            if (!t->dest || !t->urls[0] || !t->sha1 || t->size <= 0) {
                all_ok = 0;
                printf("  [!!] 任务 %zu 字段不全: dest=%s url=%s sha1=%s size=%lld\n", i,
                       t->dest ? t->dest : "(null)", t->urls[0] ? t->urls[0] : "(null)",
                       t->sha1 ? t->sha1 : "(null)", (long long)t->size);
            }
        }
        check(all_ok, "计划里每个任务都有 目标/URL/SHA-1/大小");

        /* 期望值必须按平台分别算:不同平台的 natives 分类器大小不同(osx=70,linux=60,windows=50),
         * 而且 winonly/notwin 两个库按规则只进一个。写死一个数只会在别的平台挂 —— CI 上就是这么挂的。 */
        int64_t expected_total;
        if (strcmp(os_name, "windows") == 0) {
            expected_total = 100 + 200 + 10 + 20 + 40 + 50; /* 客户端+索引+always+winonly+native+natives-windows */
        } else if (strcmp(os_name, "osx") == 0) {
            expected_total = 100 + 200 + 10 + 30 + 40 + 70; /* ...+notwin+native+natives-osx */
        } else {
            expected_total = 100 + 200 + 10 + 30 + 40 + 60; /* ...+notwin+native+natives-linux */
        }
        const int64_t total = sxcl_version_plan_total_bytes(plan);
        if (total != expected_total) {
            printf("  [!!] 总字节数合计: 平台=%s 实际=%lld 期望=%lld\n", os_name, (long long)total,
                   (long long)expected_total);
            ++g_fail;
        } else {
            ++g_pass;
        }
        sxcl_version_plan_free(plan);
    }

    /* 4) 资源对象展开:按哈希去重,落盘路径 = 官方哈希 */
    {
        const char *kAssetIndex =
            "{\"objects\":{"
            "\"minecraft/sounds/a.ogg\":{\"hash\":\"1111111111111111111111111111111111111111\",\"size\":10},"
            "\"minecraft/sounds/b.ogg\":{\"hash\":\"2222222222222222222222222222222222222222\",\"size\":20},"
            "\"minecraft/sounds/c.ogg\":{\"hash\":\"1111111111111111111111111111111111111111\",\"size\":10}}}";
        sxcl_json *adoc = sxcl_json_parse(kAssetIndex, strlen(kAssetIndex), err, sizeof(err));
        check(adoc != NULL, "解析资源索引样本");
        if (adoc) {
            sxcl_version_plan *p2 = sxcl_version_plan_build(vdoc, "C:/mc", "1.21.4", err, sizeof(err));
            check(p2 != NULL, "为资源展开建计划");
            if (p2) {
                const size_t before = sxcl_version_plan_count(p2);
                const int added = sxcl_version_plan_add_asset_objects(p2, adoc, "C:/mc", NULL, NULL,
                                                                     err, sizeof(err));
                check(added == 2, "同一哈希被两个名字引用时只入队一次");
                check(sxcl_version_plan_count(p2) == before + 2, "计划条目数按去重后增加");

                const char *want_path = "assets/objects/11/1111111111111111111111111111111111111111";
                check(plan_has(p2, want_path), "对象落盘路径 = assets/objects/<前2位>/<哈希>");

                /* 找到那个任务,检查 URL/摘要/优先级/大小 */
                int found = 0;
                for (size_t i = 0; i < sxcl_version_plan_count(p2); ++i) {
                    const sxcl_task *t = sxcl_version_plan_task(p2, i);
                    if (t->dest && strstr(t->dest, want_path) != NULL) {
                        found = 1;
                        check(t->sha1 && strcmp(t->sha1, "1111111111111111111111111111111111111111") == 0,
                              "对象任务的期望摘要 = 文件名");
                        check(t->priority == SXCL_ASSET_OBJECTS_PRIORITY, "对象任务优先级 = 20(排在库之后)");
                        check(t->size == 10, "对象任务大小来自索引");
                        check(t->urls[0] && strstr(t->urls[0], "resources.download.minecraft.net/11/") != NULL,
                              "对象 URL 用官方 CDN 规则");
                        check(t->urls[1] == NULL, "未指定镜像时没有第二候选");
                    }
                }
                check(found == 1, "能在计划里找到该对象任务");
                sxcl_version_plan_free(p2);
            }
            sxcl_json_free(adoc);
        }
    }

    sxcl_json_free(vdoc);
    sxcl_version_list_free(list);
    sxcl_json_free(doc);

    printf("元数据层测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("[PASS] 版本清单/rules 过滤/natives/下载计划 全部符合预期\n");
    }
    return g_fail == 0 ? 0 : 1;
}
