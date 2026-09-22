/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/loader.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *what) {
    if (ok) { ++g_pass; } else { ++g_fail; printf("  [!!] %s\n", what); }
}
static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)"); }
}

/* 判定的顺序是定死的,但断言只关心"有没有某条结论",所以扫全表。 */
static int issues_contain(const sxcl_loader_issues *its, const char *needle) {
    for (size_t i = 0; i < its->count; ++i) {
        if (strstr(its->items[i].message, needle) != NULL) { return 1; }
    }
    return 0;
}

/* 在 [begin, end) 里数 needle 出现几次(写出来的 JSON 文本做区间断言用)。 */
static int count_in_range(const char *begin, const char *end, const char *needle) {
    int count = 0;
    const size_t n = strlen(needle);
    for (const char *p = begin; p && *p && (!end || p < end); ++p) {
        if (strncmp(p, needle, n) == 0) { ++count; p += n - 1; }
    }
    return count;
}

static int write_file(const char *path, const char *text) {
    sxcl_fs_mkdirs_for_file(path);
    FILE *fh = fopen(path, "wb");
    if (!fh) { return -1; }
    fputs(text, fh);
    fclose(fh);
    return 0;
}
static char *read_file(const char *path, char *buf, size_t n) {
    FILE *fh = fopen(path, "rb");
    if (!fh) { return NULL; }
    const size_t got = fread(buf, 1, n - 1, fh);
    buf[got] = '\0';
    fclose(fh);
    return buf;
}

/* ── 1) 版本串解析 ── */
static void test_parse_version(void) {
    sxcl_loader_version_info v;

    check(sxcl_loader_parse_version("1.20.1-47.2.0", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && v.ok,
          "解析 1.20.1-47.2.0");
    check_str(v.mc, "1.20.1", "  它要求的 MC 版本");
    check_str(v.loader, "47.2.0", "  加载器版本");

    check(sxcl_loader_parse_version("1.12.2-14.23.5.2860", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && v.ok,
          "解析 1.12.2-14.23.5.2860");
    check_str(v.mc, "1.12.2", "  它要求的 MC 版本");
    check_str(v.loader, "14.23.5.2860", "  加载器版本");

    check(sxcl_loader_parse_version("1.20.1", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && v.ok,
          "只有 MC 版本也算解析成功");
    check_str(v.mc, "1.20.1", "  MC 版本");
    check_str(v.loader, "", "  没有加载器版本");

    check(sxcl_loader_parse_version("forge-1.20.1-47.2.0", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && v.ok,
          "带前缀也能解析");
    check_str(v.mc, "1.20.1", "  前缀被跳过");
    check_str(v.loader, "47.2.0", "  加载器版本");

    check(sxcl_loader_parse_version("1.20.1-47.2.0-lts", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && v.ok,
          "带后缀也能解析");
    check_str(v.mc, "1.20.1", "  后缀不影响 MC 版本");
    check_str(v.loader, "47.2.0", "  后缀不影响加载器版本");

    check(sxcl_loader_parse_version("1.20.1-OptiFine_HD_U_I6", SXCL_LOADER_OPTIFINE, &v) == SXCL_LOADER_OK && v.ok,
          "OptiFine 版本串");
    check_str(v.mc, "1.20.1", "  MC 版本");
    check_str(v.loader, "", "  OptiFine 的版本不是数字开头的 token");

    check(sxcl_loader_parse_version("1.7.10-10.13.4.1614-1.7.10", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && v.ok,
          "老 Forge 的三段串");
    check_str(v.mc, "1.7.10", "  取第一段 MC 版本");
    check_str(v.loader, "10.13.4.1614", "  第一段加载器版本");

    /* NeoForge:21.1.72 属于 MC 1.21.1(与 Python _mc_key / _parse_version 同一个换算)。 */
    check(sxcl_loader_parse_version("21.1.72", SXCL_LOADER_NEOFORGE, &v) == SXCL_LOADER_OK && v.ok,
          "NeoForge 21.1.72");
    check_str(v.mc, "1.21.1", "  换算成人类看的 MC 版本");
    check_str(v.loader, "21.1.72", "  加载器版本");
    check(sxcl_loader_parse_version("neoforge-21.1.72", SXCL_LOADER_NEOFORGE, &v) == SXCL_LOADER_OK && v.ok,
          "NeoForge 带前缀");
    check_str(v.mc, "1.21.1", "  前缀被跳过");
    check(sxcl_loader_parse_version("20.4.237", SXCL_LOADER_NEOFORGE, &v) == SXCL_LOADER_OK && v.ok,
          "NeoForge 20.4.237");
    check_str(v.mc, "1.20.4", "  换算成 1.20.4");

    /* Forge 的加载器版本单独给出来(47.2.0)时不能瞎认出一个 MC 版本。 */
    check(sxcl_loader_parse_version("47.2.0", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok,
          "Forge 的 47.2.0 认不出 MC 版本(避免误判不兼容)");
    check(sxcl_loader_parse_version("21.1.72", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok,
          "Forge 不走 NeoForge 的换算");

    /* 非法串 */
    check(sxcl_loader_parse_version("", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok, "空串非法");
    check(sxcl_loader_parse_version("abc", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok, "纯字母非法");
    check(sxcl_loader_parse_version("1.20.1.5", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok,
          "四段版本号非法(原版没有这种写法)");
    check(sxcl_loader_parse_version("1.20.1b", SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok,
          "后面紧跟字母非法");
    check(sxcl_loader_parse_version(NULL, SXCL_LOADER_FORGE, &v) == SXCL_LOADER_OK && !v.ok, "NULL 安全");
    check(sxcl_loader_parse_version("1.20.1", SXCL_LOADER_FORGE, NULL) == SXCL_LOADER_ERR_ARG,
          "out 为 NULL 报参数错");

    /* 类型与名字互转 */
    check_str(sxcl_loader_kind_id(SXCL_LOADER_NEOFORGE), "neoforge", "kind id");
    check_str(sxcl_loader_kind_name(SXCL_LOADER_OPTIFINE), "OptiFine", "kind 名字");
    check(sxcl_loader_kind_from_id("FABRIC") == SXCL_LOADER_FABRIC, "id 反查大小写不敏感");
    check(sxcl_loader_kind_from_id("不认识") == SXCL_LOADER_VANILLA, "认不出的 id = 原版");
    check(sxcl_loader_kind_implemented(SXCL_LOADER_FORGE) &&
          sxcl_loader_kind_implemented(SXCL_LOADER_NEOFORGE) &&
          sxcl_loader_kind_implemented(SXCL_LOADER_FABRIC) &&
          sxcl_loader_kind_implemented(SXCL_LOADER_OPTIFINE), "四种已实现的加载器");
    check(!sxcl_loader_kind_implemented(SXCL_LOADER_QUILT) &&
          !sxcl_loader_kind_implemented(SXCL_LOADER_VANILLA), "Quilt/原版没有静默安装实现");

    /* OptiFine 条目的 forge 字段 */
    {
        char needed[48];
        check(sxcl_loader_parse_forge_requirement("Forge 61.0.6", needed, sizeof(needed)) == SXCL_LOADER_OK,
              "取 OptiFine 配套 Forge");
        check_str(needed, "61.0.6", "  Forge 版本号");
        check(sxcl_loader_parse_forge_requirement("forge61.0.6", needed, sizeof(needed)) == SXCL_LOADER_OK,
              "无空格也认");
        check_str(needed, "61.0.6", "  版本号");
        check(sxcl_loader_parse_forge_requirement("", needed, sizeof(needed)) == SXCL_LOADER_OK,
              "空串不算错");
        check_str(needed, "", "  取不到就是空串");
        check(sxcl_loader_parse_forge_requirement("OptiFine only", needed, sizeof(needed)) == SXCL_LOADER_OK,
              "没有 forge 字样");
        check_str(needed, "", "  空串");
    }
}

/* ── 2) 兼容冲突检测 ── */
static void test_check_selection(void) {
    sxcl_loader_issues issues;
    const char *forge_versions[] = {"47.2.0", "47.1.0", "47.3.0"};
    const char *fabric_versions[] = {"0.15.11", "0.16.0"};
    sxcl_loader_installed installed[4];

    /* 2a) 只装原版:什么都不拦 */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_VANILLA;
        sel.list_failed = 1;
        check(sxcl_loader_check_selection(&sel, &issues) == SXCL_LOADER_OK, "原版的选择也能判定");
        check(issues.count == 0, "只装原版时列表取没取到都不拦");
    }

    /* 2b) 一切正常 */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_FORGE;
        sel.loader_version = "47.2.0";
        sel.instance_name = "1.20.1-Forge_47.2.0";
        sel.available_versions = forge_versions;
        sel.available_count = 3;
        check(sxcl_loader_check_selection(&sel, &issues) == SXCL_LOADER_OK, "正常组合能判定");
        check(issues.count == 0, "没有问题的组合不给结论");
        check(!sxcl_loader_issues_has_error(&issues), "没有 error");
    }

    /* 2c) 版本列表没取到 -> error(状态未知不许装) */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_FORGE;
        sel.list_failed = 1;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "列表没取到 = error");
        check(issues.count >= 1 && strstr(issues.items[0].message, "列表没取到") != NULL,
              "原因说清楚了");
    }

    /* 2d) 还没实现的加载器 */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_QUILT;
        sel.available_versions = fabric_versions;
        sel.available_count = 2;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "Quilt 还没有安装实现 = error");
        check(strstr(issues.items[0].message, "Quilt") != NULL, "  (说的是 Quilt)");
        check_str(sxcl_loader_issue_level_name(issues.items[0].level), "error", "级别名");
    }

    /* 2e) 这个原版下该加载器一个版本都没有 */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.12.1";
        sel.kind = SXCL_LOADER_NEOFORGE;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "1.12.1 没有 NeoForge = error");
        check(strstr(issues.items[0].message, "1.12.1") != NULL, "  (带上原版版本号)");
    }

    /* 2f) 选的版本不在可用列表里 */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_FORGE;
        sel.loader_version = "40.0.0";
        sel.available_versions = forge_versions;
        sel.available_count = 3;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "选了不在列表里的版本 = error");
        check(issues_contain(&issues, "40.0.0"), "  (带上选中版本号)");
    }

    /* 2g) 加载器版本与 MC 版本不匹配(版本串里写明了的)。
     *     这里给一份"包含它"的可用列表,好让 2f 那条规则不掺和进来,单独看 MC 版本这一条。 */
    {
        const char *forge_full[] = {"1.20.1-47.2.0"};
        const char *neoforge_full[] = {"21.1.72"};
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.19.2";
        sel.kind = SXCL_LOADER_FORGE;
        sel.loader_version = "1.20.1-47.2.0";
        sel.available_versions = forge_full;
        sel.available_count = 1;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "Forge 1.20.1-47.2.0 配 1.19.2 = error");
        check(issues_contain(&issues, "Minecraft 1.20.1"), "  (说的是 1.20.1)");
        check(issues_contain(&issues, "1.19.2"), "  (也说清楚了选的是 1.19.2)");

        /* 换成配套的就没问题 */
        sel.base_version = "1.20.1";
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(issues.count == 0, "配套的 1.20.1 没意见");

        /* NeoForge 的 21.1.72 属于 1.21.1(它自己那套版本号要换算) */
        sel.kind = SXCL_LOADER_NEOFORGE;
        sel.loader_version = "21.1.72";
        sel.available_versions = neoforge_full;
        sel.available_count = 1;
        sel.base_version = "1.21.1";
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(issues.count == 0, "NeoForge 21.1.72 配 1.21.1 没问题");
        sel.base_version = "1.20.1";
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "NeoForge 21.1.72 配 1.20.1 = error");
    }

    /* 2h) 同名版本已存在:别的加载器 -> error;同一个加载器 -> warn */
    memset(installed, 0, sizeof(installed));
    installed[0].instance_id = "1.20.1-Forge_47.2.0";
    installed[0].base_version = "1.20.1";
    installed[0].loader_id = "forge";
    installed[0].loader_version = "47.2.0";
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_FABRIC;
        sel.loader_version = "0.15.11";
        sel.available_versions = fabric_versions;
        sel.available_count = 2;
        sel.instance_name = "1.20.1-Forge_47.2.0";   /* 撞上已经装好的 Forge */
        sel.installed = installed;
        sel.installed_count = 1;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(sxcl_loader_issues_has_error(&issues), "同名版本是别的加载器 = error(别覆盖)");
        check(strstr(issues.items[0].message, "同名版本") != NULL, "  (说的是同名版本)");

        /* 同一个实例名的同一个加载器:只提醒 */
        sel.kind = SXCL_LOADER_FORGE;
        sel.loader_version = "47.2.0";
        sel.available_versions = forge_versions;
        sel.available_count = 3;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(!sxcl_loader_issues_has_error(&issues), "同名版本是同一个加载器 = 只警告");
        check(issues.count >= 1 && strstr(issues.items[0].message, "覆盖") != NULL, "  (说清楚会覆盖)");
    }

    /* 2i) 选中 Fabric 但该原版已经装了 Forge -> warn(不拦,会另建实例);
     *     同一加载器同一版本已经装过 -> warn。 */
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_FABRIC;
        sel.loader_version = "0.15.11";
        sel.available_versions = fabric_versions;
        sel.available_count = 2;
        sel.instance_name = "1.20.1-Fabric 0.15.11";
        sel.installed = installed;
        sel.installed_count = 1;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(!sxcl_loader_issues_has_error(&issues), "装了 Forge 再装 Fabric = 只警告");
        check(issues.count >= 1 && strstr(issues.items[0].message, "Forge") != NULL,
              "  (提醒已有的 Forge)");
        check(strstr(issues.items[0].fix, "另外建") != NULL, "  (说明会另建实例)");
    }
    {
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_FORGE;
        sel.loader_version = "47.2.0";
        sel.available_versions = forge_versions;
        sel.available_count = 3;
        sel.instance_name = "1.20.1-Forge_47.2.0-2";
        sel.installed = installed;
        sel.installed_count = 1;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(!sxcl_loader_issues_has_error(&issues), "重复装同一个加载器版本 = 只警告");
        check(strstr(issues.items[0].message, "已经装过") != NULL, "  (提醒重复)");
    }

    /* 2j) OptiFine 配套 Forge 的提示(给一份可用的 OptiFine 版本列表,好让其它规则别掺和) */
    {
        const char *optifine_versions[] = {"HD_U_I6"};
        sxcl_loader_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.base_version = "1.20.1";
        sel.kind = SXCL_LOADER_OPTIFINE;
        sel.loader_version = "HD_U_I6";
        sel.available_versions = optifine_versions;
        sel.available_count = 1;
        sel.optifine_forge_hint = "Forge 47.2.0";
        sel.installed = installed;
        sel.installed_count = 1;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(issues.count == 1 && !sxcl_loader_issues_has_error(&issues), "已装配套 Forge = 一条警告");
        check(strstr(issues.items[0].message, "配套的 Forge 47.2.0") != NULL, "  (说清楚配套版本)");

        sel.installed = NULL;
        sel.installed_count = 0;
        (void)sxcl_loader_check_selection(&sel, &issues);
        check(issues.count == 1 && !sxcl_loader_issues_has_error(&issues), "没装配套 Forge 也只是警告");
        check(strstr(issues.items[0].message, "配合 Forge 47.2.0") != NULL, "  (提示先装 Forge)");
    }

    check(sxcl_loader_check_selection(NULL, &issues) == SXCL_LOADER_ERR_ARG, "NULL 选择报参数错");
}

/* ── 3) launcher_profiles.json 合并 ── */
static const char *k_existing =
    "{\n"
    "  \"profiles\": {\n"
    "    \"PCL\": {\"name\": \"PCL\", \"lastVersionId\": \"1.20.1-Forge_47.2.0\", \"icon\": \"Furnace\", \"customFlag\": true},\n"
    "    \"SXCL\": {\"name\": \"用户改过的名字\", \"lastVersionId\": \"1.12.2\"}\n"
    "  },\n"
    "  \"selectedProfile\": \"PCL\",\n"
    "  \"clientToken\": \"abcdef\",\n"
    "  \"authenticationDatabase\": {\"uuid-1\": {\"accessToken\": \"tok\", \"username\": \"Steve\"}},\n"
    "  \"launcherVersion\": {\"name\": \"2.1.0\", \"format\": 21},\n"
    "  \"unknownTop\": [1, 2, 3],\n"
    "  \"flagsInArray\": [true, false, true],\n"
    "  \"nestedBool\": {\"on\": true, \"off\": false}\n"
    "}\n";

static void test_profiles_merge_text(void) {
    char err[192];
    char *text = NULL;
    int changed = 0;

    /* 3a) 全新文件 */
    check(sxcl_loader_merge_profiles_text(NULL, NULL, NULL, NULL, "2026-01-01T00:00:00.0000Z", &text,
                                          &changed, err, sizeof(err)) == SXCL_LOADER_OK, "空内容也能合并");
    check(changed == 1, "全新文件算有变化");
    check(text != NULL, "有输出文本");
    check(strstr(text, "\"selectedProfile\": \"SXCL\"") != NULL, "补了 selectedProfile");
    check(strstr(text, "\"clientToken\"") != NULL, "补了 clientToken");
    check(strstr(text, "\"lastUsed\": \"2026-01-01T00:00:00.0000Z\"") != NULL, "写上 lastUsed");
    {
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(text, strlen(text), perr, sizeof(perr));
        check(doc != NULL, "生成的文本是合法 JSON");
        if (doc) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const sxcl_json_value *profiles = sxcl_json_get(root, "profiles");
            const sxcl_json_value *own = sxcl_json_get(profiles, "SXCL");
            check(sxcl_json_member_count(profiles) == 1, "只有一个档案");
            check(own != NULL && sxcl_json_type_of(own) == SXCL_JSON_OBJECT, "SXCL 档案是对象");
            check_str(sxcl_json_get_string(own, "name", ""), "Silent X Craft Launcher", "档案名");
            check_str(sxcl_json_get_string(own, "icon", ""), "Grass", "图标(与 Python 版一致)");
            check_str(sxcl_json_get_string(own, "lastVersionId", ""), "latest-release", "lastVersionId");
            check_str(sxcl_json_get_string(root, "selectedProfile", ""), "SXCL", "selectedProfile");
            sxcl_json_free(doc);
        }
    }
    free(text);
    text = NULL;

    /* 3b) 已有档案 + 未知字段:全部原样保留;同名档案不动(所以是"没有变化") */
    check(sxcl_loader_merge_profiles_text(k_existing, NULL, NULL, NULL, "2026-01-01T00:00:00.0000Z", &text,
                                          &changed, err, sizeof(err)) == SXCL_LOADER_OK, "合并已有内容");
    check(changed == 0, "同名档案已存在且键齐全 = 不需要重写文件");
    check(text != NULL, "仍然给出合并后的文本");
    {
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(text, strlen(text), perr, sizeof(perr));
        check(doc != NULL, "合并结果仍是合法 JSON");
        if (doc) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const sxcl_json_value *profiles = sxcl_json_get(root, "profiles");
            const sxcl_json_value *pcl = sxcl_json_get(profiles, "PCL");
            const sxcl_json_value *own = sxcl_json_get(profiles, "SXCL");
            check(sxcl_json_member_count(profiles) == 2, "原有档案没有多也不能少");
            check_str(sxcl_json_get_string(pcl, "lastVersionId", ""), "1.20.1-Forge_47.2.0",
                      "原有档案原样保留");
            check(sxcl_json_get_bool(pcl, "customFlag", 0) == 1, "原有档案里的布尔值原样保留");
            check_str(sxcl_json_get_string(pcl, "icon", ""), "Furnace", "原有档案的图标");
            check_str(sxcl_json_get_string(own, "name", ""), "用户改过的名字",
                      "同名档案保持原样(Python: 已经有了就不动它)");
            check_str(sxcl_json_get_string(root, "selectedProfile", ""), "PCL", "selectedProfile 不被改");
            check_str(sxcl_json_get_string(root, "clientToken", ""), "abcdef", "clientToken 不被改");
            check_str(sxcl_json_get_string(sxcl_json_get(sxcl_json_get(root, "authenticationDatabase"),
                                                         "uuid-1"), "username", ""),
                      "Steve", "登录信息原样保留");
            check_str(sxcl_json_get_string(sxcl_json_get(root, "launcherVersion"), "name", ""),
                      "2.1.0", "未知结构的对象保留");
            check(sxcl_json_get_int64(sxcl_json_get(root, "launcherVersion"), "format", 0) == 21,
                  "未知结构里的数字保留");
            check(sxcl_json_size(sxcl_json_get(root, "unknownTop")) == 3, "未知数组原样保留");
            check(sxcl_json_number(sxcl_json_at(sxcl_json_get(root, "unknownTop"), 2)) == 3.0,
                  "数组里的数字原样写回");
            /* 数组里的布尔值:sxcl_json_bool(节点级接口)读得到、也写得回原值 */
            {
                const sxcl_json_value *flags = sxcl_json_get(root, "flagsInArray");
                check(sxcl_json_size(flags) == 3, "数组里的布尔原样保留(条数)");
                check(sxcl_json_bool(sxcl_json_at(flags, 0)) == 1, "  true 还是 true");
                check(sxcl_json_bool(sxcl_json_at(flags, 1)) == 0, "  false 还是 false");
                check(sxcl_json_bool(sxcl_json_at(flags, 2)) == 1, "  第三个也是 true");
                const char *seg = strstr(text, "\"flagsInArray\": [");
                const char *seg_end = seg ? strchr(seg, ']') : NULL;
                check(seg != NULL && seg_end != NULL, "  写出来有 flagsInArray 数组");
                if (seg && seg_end) {
                    check(count_in_range(seg, seg_end, "true") == 2 &&
                          count_in_range(seg, seg_end, "false") == 1,
                          "  写出来就是 true/false(没有被写坏成别的值)");
                }
            }
            check(sxcl_json_bool(sxcl_json_get(sxcl_json_get(root, "nestedBool"), "on")) == 1,
                  "嵌套对象里的布尔也原样保留");
            check(sxcl_json_bool(sxcl_json_get(sxcl_json_get(root, "nestedBool"), "off")) == 0,
                  "  false 没被写成 true");
            sxcl_json_free(doc);
        }
    }
    free(text);
    text = NULL;

    /* 3c) 不存在则新增(换一个 key) */
    check(sxcl_loader_merge_profiles_text(k_existing, "SXCL2", "第二个档案", NULL,
                                          "2026-03-03T00:00:00.0000Z", &text, &changed, err,
                                          sizeof(err)) == SXCL_LOADER_OK, "新增一个档案");
    check(changed == 1, "新增档案算有变化");
    {
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(text, strlen(text), perr, sizeof(perr));
        check(doc != NULL, "新增后仍是合法 JSON");
        if (doc) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const sxcl_json_value *profiles = sxcl_json_get(root, "profiles");
            check(sxcl_json_member_count(profiles) == 3, "三个档案");
            check_str(sxcl_json_get_string(sxcl_json_get(profiles, "SXCL2"), "name", ""), "第二个档案",
                      "新档案用的是传入的名字");
            check_str(sxcl_json_get_string(sxcl_json_get(profiles, "PCL"), "icon", ""), "Furnace",
                      "原有档案没被动");
            sxcl_json_free(doc);
        }
    }
    free(text);
    text = NULL;

    /* 3d) 坏 JSON:拒绝改写,绝不覆盖用户数据(与 Python 版故意不同的一处) */
    {
        char *bad_out = (char *)"哨兵";
        int bad_changed = 7;
        const int rc = sxcl_loader_merge_profiles_text("{ 这不是 JSON", NULL, NULL, NULL, NULL,
                                                       &bad_out, &bad_changed, err, sizeof(err));
        check(rc == SXCL_LOADER_ERR_FORMAT, "坏 JSON 报格式错误");
        check(bad_out == NULL, "坏 JSON 不产出文本(调用方不会拿它去覆盖)");
        check(bad_changed == 0, "坏 JSON 不算有变化");
        check(strstr(err, "保留原文件") != NULL, "错误信息里说明了原文件保留");
    }
}

static void test_profiles_file(void) {
    char err[192];
    char buf1[4096];
    char buf2[4096];
    const char *game = "build/_loader_tmp/game";
    const char *path = "build/_loader_tmp/game/launcher_profiles.json";
    int changed = -1;

    check(sxcl_fs_mkdirs(game) == 0, "建临时游戏目录");

    /* 4a) 文件不存在:直接建一份(安装器靠它开工) */
    (void)sxcl_fs_remove(path);
    check(sxcl_loader_ensure_launcher_profiles(game, NULL, NULL, NULL, "2026-05-05T00:00:00.0000Z",
                                               &changed, err, sizeof(err)) == SXCL_LOADER_OK,
          "文件不在就建一份");
    check(changed == 1, "建文件算有变化");
    check(sxcl_fs_exists(path), "文件真的落盘了");
    check(!sxcl_fs_exists("build/_loader_tmp/game/launcher_profiles.json.tmp"), "没有留 .tmp 残渣");
    {
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse_file(path, perr, sizeof(perr));
        check(doc != NULL, "落盘的是合法 JSON");
        if (doc) {
            check(sxcl_json_get(sxcl_json_get(sxcl_json_root(doc), "profiles"), "SXCL") != NULL,
                  "里面有我们的档案");
            sxcl_json_free(doc);
        }
    }

    /* 4b) 原子写后重新读回一致 + 幂等(第二次合并不动文件) */
    check(read_file(path, buf1, sizeof(buf1)) != NULL, "读回文件");
    changed = -1;
    check(sxcl_loader_ensure_launcher_profiles(game, NULL, NULL, NULL, "2026-06-06T00:00:00.0000Z",
                                               &changed, err, sizeof(err)) == SXCL_LOADER_OK, "第二次合并");
    check(changed == 0, "同名档案已存在:不重写文件");
    check(read_file(path, buf2, sizeof(buf2)) != NULL, "再读回文件");
    check(strcmp(buf1, buf2) == 0, "再次合并后文件内容一字不差");

    /* 4c) 已有别的启动器档案:合并不覆盖(这份文件里没有 SXCL 档案,好让它真的插入一次) */
    {
        const char *without_sxcl =
            "{\"profiles\":{\"PCL\":{\"name\":\"PCL\",\"icon\":\"Furnace\"}},"
            "\"selectedProfile\":\"PCL\",\"我的自定义键\":{\"a\":1}}";
        check(write_file(path, without_sxcl) == 0, "写一份没有 SXCL 档案但字段很怪的文件");
    }
    changed = -1;
    check(sxcl_loader_ensure_launcher_profiles(game, NULL, "SXCL 测试", NULL,
                                               "2026-07-07T00:00:00.0000Z", &changed, err,
                                               sizeof(err)) == SXCL_LOADER_OK, "合并进别人家的文件");
    check(changed == 1, "这次真的插入了");
    {
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse_file(path, perr, sizeof(perr));
        check(doc != NULL, "合并后的文件是合法 JSON");
        if (doc) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const sxcl_json_value *profiles = sxcl_json_get(root, "profiles");
            check(sxcl_json_member_count(profiles) == 2, "两个档案(PCL + SXCL)");
            check_str(sxcl_json_get_string(sxcl_json_get(profiles, "PCL"), "icon", ""), "Furnace",
                      "别人家的档案原样保留");
            check_str(sxcl_json_get_string(sxcl_json_get(profiles, "SXCL"), "name", ""), "SXCL 测试",
                      "我们的档案进去了");
            check_str(sxcl_json_get_string(root, "selectedProfile", ""), "PCL",
                      "不抢别人家的 selectedProfile");
            check(sxcl_json_get(root, "我的自定义键") != NULL, "中文键名的未知字段也保留");
            sxcl_json_free(doc);
        }
    }

    /* 4d) 文件在但读不出来:保留原文件,不覆盖 */
    check(write_file(path, "{ 坏掉的 launcher_profiles") == 0, "写一份坏文件");
    changed = -1;
    check(sxcl_loader_ensure_launcher_profiles(game, NULL, NULL, NULL, NULL, &changed, err,
                                               sizeof(err)) == SXCL_LOADER_OK,
          "坏文件不影响'文件存在'这个结论");
    check(changed == 0, "坏文件不会被改写");
    check(read_file(path, buf1, sizeof(buf1)) != NULL && strcmp(buf1, "{ 坏掉的 launcher_profiles") == 0,
          "坏文件原封不动");
    check(strstr(err, "保留原文件") != NULL, "告警写进了 err");

    check(sxcl_loader_ensure_launcher_profiles(NULL, NULL, NULL, NULL, NULL, &changed, err,
                                               sizeof(err)) == SXCL_LOADER_ERR_ARG, "没有游戏目录 = 参数错");

    /* 4e) 装完登记实例:key = 实例名,lastVersionId 指向这个实例,原有档案一个不动 */
    {
        const char *with_other =
            "{\"profiles\":{\"PCL\":{\"name\":\"PCL\",\"icon\":\"Furnace\"}},"
            "\"selectedProfile\":\"PCL\",\"clientToken\":\"tok\"}";
        check(write_file(path, with_other) == 0, "写一份只有别人家档案的文件");
        changed = -1;
        check(sxcl_loader_ensure_launcher_profiles(game, "1.20.1-47.2.0", "1.20.1-47.2.0",
                                                   "1.20.1-47.2.0", "2026-08-08T00:00:00.0000Z", &changed,
                                                   err, sizeof(err)) == SXCL_LOADER_OK, "登记实例档案");
        check(changed == 1, "登记实例算有变化");
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse_file(path, perr, sizeof(perr));
        check(doc != NULL, "登记后的文件是合法 JSON");
        if (doc) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const sxcl_json_value *profiles = sxcl_json_get(root, "profiles");
            const sxcl_json_value *mine = sxcl_json_get(profiles, "1.20.1-47.2.0");
            check(sxcl_json_member_count(profiles) == 2, "两个档案(PCL + 实例)");
            check(mine != NULL, "实例档案在里面(启动器的版本列表靠它)");
            check_str(sxcl_json_get_string(mine, "lastVersionId", ""), "1.20.1-47.2.0",
                      "档案指向刚装好的那个实例");
            check_str(sxcl_json_get_string(mine, "name", ""), "1.20.1-47.2.0", "档案名");
            check_str(sxcl_json_get_string(sxcl_json_get(profiles, "PCL"), "icon", ""), "Furnace",
                      "原有档案没被动");
            check_str(sxcl_json_get_string(root, "selectedProfile", ""), "PCL", "不改别人的选中项");
            sxcl_json_free(doc);
        }
        /* 再来一次:同名档案已存在 -> 不动、不重写 */
        changed = -1;
        check(sxcl_loader_ensure_launcher_profiles(game, "1.20.1-47.2.0", "1.20.1-47.2.0",
                                                   "1.20.1-47.2.0", NULL, &changed, err,
                                                   sizeof(err)) == SXCL_LOADER_OK, "重复登记");
        check(changed == 0, "同名实例档案不重复写");
    }
}

/* ── 5) 命令行构造 ── */
static void test_build_commands(void) {
    sxcl_loader_cmd cmds[4];
    sxcl_loader_cmd_env env;
    memset(&env, 0, sizeof(env));
    env.java_path = "C:/Java/bin/java.exe";
    env.installer_jar = "C:/dl/forge-installer.jar";
    env.game_dir = "C:/Game Dir/.minecraft";
    env.base_version = "1.20.1";
    env.loader_version = "47.2.0";
    env.instance_name = "1.20.1-Forge_47.2.0";

    /* 5a) Forge:首选就是 --installClient <游戏目录> */
    size_t count = sxcl_loader_build_commands(SXCL_LOADER_FORGE, &env, cmds, 4);
    check(count >= 1, "Forge 至少有一组静默命令行");
    check_str(cmds[0].program, "C:/Java/bin/java.exe", "program 是 java");
    check_str(cmds[0].args[0], "-jar", "第一个参数是 -jar");
    check_str(cmds[0].args[1], "C:/dl/forge-installer.jar", "第二个参数是安装器 jar");
    check_str(cmds[0].args[2], "--installClient", "第三个参数必须是 --installClient");
    check_str(cmds[0].args[3], "C:/Game Dir/.minecraft", "--installClient 的值是游戏目录");
    check(cmds[0].argc == 4, "首选命令行正好 4 个参数");
    check_str(cmds[0].work_dir, "C:/Game Dir/.minecraft", "工作目录 = 游戏目录");
    check(cmds[0].appdata[0] == '\0', "Forge 不需要沙箱 APPDATA");
    check(count >= 2 && cmds[1].argc == 3, "第二组是 --installClient(装到当前目录)");
    {
        int all_quiet = 1;
        for (size_t i = 0; i < count; ++i) {
            if (cmds[i].argc < 3) {
                all_quiet = 0;   /* 只有 java -jar xxx 会弹图形安装器,绝不能出现 */
            }
        }
        check(all_quiet, "绝不构造不带参数的命令行(会弹 GUI 被误判成功)");
    }

    /* 5b) 镜像参数在最后 */
    env.mirror_maven = "https://bmclapi2.bangbang93.com/maven/";
    count = sxcl_loader_build_commands(SXCL_LOADER_FORGE, &env, cmds, 4);
    check(count >= 1 && cmds[0].argc == 6, "带镜像时多两个参数");
    check_str(cmds[0].args[4], "--mirror", "镜像参数名");
    check_str(cmds[0].args[5], "https://bmclapi2.bangbang93.com/maven/", "镜像地址");
    env.mirror_maven = NULL;

    /* 5c) Fabric 是另一套 CLI,而且参数是**单横线**(实测依据见 installer.c 里 Fabric 分支的注释:
     *     双横线的写法会被安装器**整段忽略**,它会退到"当前最新正式版"再报 launcher profile 找不到,
     *     退出码 1 —— 也就是双横线那一版从来没成功过)。 */
    /* 注意 -dir 是**游戏目录**,不是版本目录:安装器要在这里找 launcher_profiles.json,
     * 并把版本建到 <游戏目录>/versions/fabric-loader-<loader>-<mc>(由 adopt_generated 收编)。 */
    env.installer_jar = "C:/dl/fabric-installer.jar";
    env.loader_version = "0.15.11";
    env.instance_name = "1.20.1-Fabric 0.15.11";
    count = sxcl_loader_build_commands(SXCL_LOADER_FABRIC, &env, cmds, 4);
    check(count == 2, "Fabric 两组参数");
    check_str(cmds[0].args[2], "client", "子命令 client");
    check_str(cmds[0].args[3], "-mcversion", "参数名 -mcversion(单横线)");
    check_str(cmds[0].args[4], "1.20.1", "MC 版本");
    check_str(cmds[0].args[5], "-loader", "参数名 -loader(单横线)");
    check_str(cmds[0].args[6], "0.15.11", "加载器版本");
    check_str(cmds[0].args[7], "-dir", "参数名 -dir(单横线)");
    check_str(cmds[0].args[8], "C:/Game Dir/.minecraft", "游戏目录");
    check_str(cmds[0].args[9], "-name", "参数名 -name(单横线)");
    check_str(cmds[0].args[10], "1.20.1-Fabric 0.15.11", "实例名");
    check(cmds[0].argc == 11, "第一组 11 个参数");
    check(cmds[1].argc == 9, "第二组没有 -name");
    check_str(cmds[0].work_dir, "C:/Game Dir/.minecraft", "Fabric 的工作目录");

    /* 5d) OptiFine:沙箱里的假游戏目录 + 必须带上 APPDATA */
    env.installer_jar = "C:/dl/OptiFine_1.20.1_HD_U_I6.jar";
    env.game_dir = "C:/Temp/sxcl-optifine-1/appdata/.minecraft";
    env.fake_appdata = "C:/Temp/sxcl-optifine-1/appdata";
    count = sxcl_loader_build_commands(SXCL_LOADER_OPTIFINE, &env, cmds, 4);
    check(count == 1, "OptiFine 只有一组参数");
    check_str(cmds[0].args[2], "--installClient", "OptiFine 也用 --installClient");
    check_str(cmds[0].args[3], "C:/Temp/sxcl-optifine-1/appdata/.minecraft", "目标是沙箱里的假游戏目录");
    check_str(cmds[0].appdata, "C:/Temp/sxcl-optifine-1/appdata",
              "沙箱 APPDATA 必须带上(它只认 %APPDATA%\\.minecraft)");
    check_str(cmds[0].work_dir, "C:/Temp/sxcl-optifine-1/appdata/.minecraft", "工作目录在沙箱里");

    /* 5e) 参数非法时不给命令行 */
    check(sxcl_loader_build_commands(SXCL_LOADER_FORGE, NULL, cmds, 4) == 0, "没有上下文就没有命令行");
    check(sxcl_loader_build_commands(SXCL_LOADER_VANILLA, &env, cmds, 4) == 0, "原版没有安装器可跑");
    check(sxcl_loader_build_commands(SXCL_LOADER_FORGE, &env, cmds, 0) == 0, "容量 0 安全");
}

/* ── 6) 进度标记解析 ── */
static void test_progress(void) {
    sxcl_loader_progress p;

    check(sxcl_loader_parse_progress("Extracting json", &p) == SXCL_LOADER_OK, "解析 Extracting json");
    check(p.matched && p.stage == SXCL_LOADER_PROGRESS_EXTRACT_JSON && p.percent == 35 && !p.finished,
          "  35% / 解压版本信息");
    check_str(p.text, "解压版本信息", "  说明文字");

    check(sxcl_loader_parse_progress("[12:00:00] [main/INFO] Downloading libraries", &p) == SXCL_LOADER_OK,
          "标记是子串匹配(安装器前面有日志前缀)");
    check(p.matched && p.stage == SXCL_LOADER_PROGRESS_DOWNLOAD_LIBS && p.percent == 45, "  45%");

    check(sxcl_loader_parse_progress("Building Processors", &p) == SXCL_LOADER_OK, "解析 Building Processors");
    check(p.matched && p.stage == SXCL_LOADER_PROGRESS_BUILD_PROCESSORS && p.percent == 60, "  60%");

    check(sxcl_loader_parse_progress("Task: decompile", &p) == SXCL_LOADER_OK, "解析 Task:");
    check(p.matched && p.stage == SXCL_LOADER_PROGRESS_TASK && p.percent == 70, "  70%");
    check(strstr(p.text, "decompile") != NULL, "  带上任务名");

    /* 完成标记 */
    check(sxcl_loader_parse_progress("true", &p) == SXCL_LOADER_OK && p.finished && p.matched,
          "安装器输出 true = 完成");
    check(p.percent == 100 && p.stage == SXCL_LOADER_PROGRESS_FINISHED, "  100% / FINISHED");
    check(sxcl_loader_parse_progress("  TRUE  ", &p) == SXCL_LOADER_OK && p.finished,
          "  大小写与空白不敏感(Python 的 l.strip().lower())");
    check(sxcl_loader_parse_progress("The installation was successful", &p) == SXCL_LOADER_OK && p.finished,
          "Forge 的成功行也算完成");
    check(sxcl_loader_parse_progress("successfully installed 3 processors", &p) == SXCL_LOADER_OK &&
              p.finished, "  换一种说法也认");

    /* 没信息的行 */
    check(sxcl_loader_parse_progress("hello world", &p) == SXCL_LOADER_OK, "无关的行不报错");
    check(!p.matched && p.percent == -1 && !p.finished && p.stage == SXCL_LOADER_PROGRESS_NONE,
          "  没有进度信息");
    check(sxcl_loader_parse_progress("", &p) == SXCL_LOADER_OK && !p.matched, "空行安全");
    check(sxcl_loader_parse_progress(NULL, &p) == SXCL_LOADER_OK && !p.matched, "NULL 安全");
    check(sxcl_loader_parse_progress("true", NULL) == SXCL_LOADER_ERR_ARG, "out 为 NULL 报参数错");
}

/* ── 7) Maven 坐标与依赖库清单 ── */
static void test_maven(void) {
    char path[320];
    check(sxcl_loader_maven_path("net.minecraftforge:forge:1.12.2-14.23.5.2863:universal", path,
                                 sizeof(path)) == SXCL_LOADER_OK, "分类器坐标");
    check_str(path, "net/minecraftforge/forge/1.12.2-14.23.5.2863/forge-1.12.2-14.23.5.2863-universal.jar",
              "  路径");

    check(sxcl_loader_maven_path("de.oceanlabs.mcp:mcp_config:1.12.2-20200226.224830@zip", path,
                                 sizeof(path)) == SXCL_LOADER_OK, "@zip 扩展名坐标");
    check_str(path, "de/oceanlabs/mcp/mcp_config/1.12.2-20200226.224830/mcp_config-1.12.2-20200226.224830.zip",
              "  不能拼成 @zip.jar(会 404)");

    check(sxcl_loader_maven_path("net.fabricmc:fabric-loader:0.15.11", path, sizeof(path)) ==
              SXCL_LOADER_OK, "三段坐标");
    check_str(path, "net/fabricmc/fabric-loader/0.15.11/fabric-loader-0.15.11.jar", "  路径");

    check(sxcl_loader_maven_path("abc", path, sizeof(path)) == SXCL_LOADER_ERR_ARG, "缺冒号 = 参数错");
    check(sxcl_loader_maven_path("net.fabricmc:fabric-loader", path, sizeof(path)) == SXCL_LOADER_ERR_ARG,
          "缺版本 = 参数错");
    check(sxcl_loader_maven_path(NULL, path, sizeof(path)) == SXCL_LOADER_ERR_ARG, "NULL 安全");

    /* 依赖库清单(版本 JSON + processors[].classpath) */
    {
        const char *vjson =
            "{\"id\":\"1.20.1-forge-47.2.0\",\"mainClass\":\"cpw.mods.bootstraplauncher.BootstrapLauncher\","
            "\"libraries\":["
            "{\"name\":\"net.minecraftforge:forge:1.20.1-47.2.0\"},"
            "{\"name\":\"com.google.code.gson:gson:2.10.1\",\"url\":\"https://libraries.minecraft.net/\"},"
            "{\"name\":\"net.minecraftforge:forge:1.20.1-47.2.0\"}],"
            "\"processors\":[{\"classpath\":[\"net.minecraftforge:installertools:1.3.2\"]}]}";
        char perr[192];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(vjson, strlen(vjson), perr, sizeof(perr));
        check(doc != NULL, "版本 JSON 能解析");
        if (doc) {
            sxcl_loader_library libs[SXCL_LOADER_MAX_LIBRARIES];
            const size_t total = sxcl_loader_collect_libraries(doc, "https://maven.minecraftforge.net/",
                                                               libs, SXCL_LOADER_MAX_LIBRARIES);
            check(total == 3, "三个库(重复的坐标去重)");
            check_str(libs[0].name, "net.minecraftforge:forge:1.20.1-47.2.0", "第一个库的坐标");
            check_str(libs[0].url, "https://maven.minecraftforge.net/", "没写 url 就用加载器的 maven");
            check_str(libs[1].url, "https://libraries.minecraft.net/", "写了 url 就用它自己的");
            check_str(libs[2].name, "net.minecraftforge:installertools:1.3.2", "处理器 classpath 也要下");
            check_str(libs[2].path, "net/minecraftforge/installertools/1.3.2/installertools-1.3.2.jar",
                      "处理器库的路径");
            /* out 为 NULL 时没有存名字的地方 -> 不去重,拿到的是原始条目数(库 3 条 + 处理器 1 条)。 */
            check(sxcl_loader_collect_libraries(doc, "x", NULL, 0) == 4, "out 为空只问条数(不去重)");
            sxcl_json_free(doc);
        }
    }
}

/* ── 8) 版本 JSON 改写(方式 B 的 id / inheritsFrom) ── */
static void test_json_dump(void) {
    const char *vjson =
        "{\"id\":\"1.20.1-forge-47.2.0\",\"mainClass\":\"cpw.mods.bootstraplauncher.BootstrapLauncher\","
        "\"libraries\":[{\"name\":\"net.minecraftforge:forge:1.20.1-47.2.0\"}],"
        "\"processors\":[{\"classpath\":[\"a:b:1\"],\"outputs\":{\"x\":\"y\"}}],\"type\":\"release\"}";
    char perr[192];
    char err[192];
    perr[0] = '\0';
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(vjson, strlen(vjson), perr, sizeof(perr));
    check(doc != NULL, "版本 JSON 解析");
    if (doc) {
        const sxcl_loader_json_override overrides[2] = {
            {"id", "1.20.1-Forge_47.2.0"},
            {"inheritsFrom", "1.20.1"},
        };
        char *text = NULL;
        check(sxcl_loader_json_dump(sxcl_json_root(doc), overrides, 2, &text, err, sizeof(err)) ==
                  SXCL_LOADER_OK, "改写并序列化");
        check(text != NULL, "有输出");
        if (text) {
            sxcl_json *back = sxcl_json_parse(text, strlen(text), perr, sizeof(perr));
            check(back != NULL, "写出来的还是合法 JSON");
            if (back) {
                const sxcl_json_value *root = sxcl_json_root(back);
                check_str(sxcl_json_get_string(root, "id", ""), "1.20.1-Forge_47.2.0", "id 被改掉了");
                check_str(sxcl_json_get_string(root, "inheritsFrom", ""), "1.20.1",
                          "inheritsFrom 不存在就补上(setdefault 语义)");
                check_str(sxcl_json_get_string(root, "mainClass", ""),
                          "cpw.mods.bootstraplauncher.BootstrapLauncher", "其它字段原样保留");
                check(sxcl_json_size(sxcl_json_get(root, "libraries")) == 1, "依赖库数组保留");
                check(sxcl_json_size(sxcl_json_get(root, "processors")) == 1,
                      "processors 原样保留(重放不在本模块里)");
                check_str(sxcl_json_get_string(
                              sxcl_json_get(sxcl_json_at(sxcl_json_get(root, "processors"), 0), "outputs"),
                              "x", ""),
                          "y", "嵌套的未知结构保留");
                sxcl_json_free(back);
            }
            free(text);
            text = NULL;
        }
        sxcl_json_free(doc);
    }
    /* 顶层不是对象时也要能输出 */
    {
        const char *arr = "[1,2,3]";
        err[0] = '\0';
        sxcl_json *doc2 = sxcl_json_parse(arr, strlen(arr), perr, sizeof(perr));
        check(doc2 != NULL, "数组文档解析");
        if (doc2) {
            char *text = NULL;
            check(sxcl_loader_json_dump(sxcl_json_root(doc2), NULL, 0, &text, err, sizeof(err)) ==
                      SXCL_LOADER_OK, "数组也能序列化");
            check(text && strstr(text, "[") != NULL, "  输出里有数组");
            free(text);
            sxcl_json_free(doc2);
        }
    }
    /* 根就是布尔/数组的情况:现在也能原样写回(以前缺节点级布尔接口时是拒绝改写的) */
    {
        const char *bool_doc = "true";
        err[0] = '\0';
        sxcl_json *doc3 = sxcl_json_parse(bool_doc, strlen(bool_doc), perr, sizeof(perr));
        check(doc3 != NULL, "根是布尔也能解析");
        if (doc3) {
            check(sxcl_json_bool(sxcl_json_root(doc3)) == 1, "根布尔读得出来");
            char *text = NULL;
            check(sxcl_loader_json_dump(sxcl_json_root(doc3), NULL, 0, &text, err, sizeof(err)) ==
                      SXCL_LOADER_OK, "根布尔能序列化");
            check_str(text, "true", "  写回来还是 true");
            free(text);
            sxcl_json_free(doc3);
        }
    }
    check(sxcl_loader_json_dump(NULL, NULL, 0, NULL, err, sizeof(err)) == SXCL_LOADER_ERR_ARG,
          "out 为 NULL 报参数错");
}

/* ── 9) 安装入口的参数校验(不跑安装器) ── */
static void test_install_args(void) {
    sxcl_loader_install_result res;
    sxcl_loader_install_request req;
    memset(&req, 0, sizeof(req));
    req.game_dir = "build/_loader_tmp/game";
    req.instance_name = "1.20.1-Forge_47.2.0";
    req.base_version = "1.20.1";
    req.kind = SXCL_LOADER_FORGE;
    req.loader_version = "47.2.0";
    req.installer_jar = "build/_loader_tmp/不存在的安装器.jar";
    req.java_path = "java";

    check(sxcl_loader_install(NULL, &res) == SXCL_LOADER_ERR_ARG, "没有请求 = 参数错");
    check(sxcl_loader_install(&req, NULL) == SXCL_LOADER_ERR_ARG, "没有结果 = 参数错");

    req.game_dir = NULL;
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_ERR_ARG, "没有游戏目录 = 参数错");
    req.game_dir = "build/_loader_tmp/game";

    req.instance_name = NULL;
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_ERR_ARG, "没有实例名 = 参数错");
    req.instance_name = "1.20.1-Forge_47.2.0";

    req.java_path = NULL;
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_ERR_ARG, "没有 Java = 参数错");
    req.java_path = "java";

    req.kind = SXCL_LOADER_VANILLA;
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_ERR_ARG, "原版 = 参数错");
    req.kind = SXCL_LOADER_QUILT;
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_ERR_ARG, "还没有实现的加载器 = 参数错");
    req.kind = SXCL_LOADER_FORGE;

    req.installer_jar = NULL;
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_ERR_ARG, "没有安装器 jar = 参数错");
    req.installer_jar = "build/_loader_tmp/game/没有这个.jar";
    check(sxcl_loader_install(&req, &res) == SXCL_LOADER_INSTALL_FAILED, "安装器 jar 不在 = 安装失败");
    check(res.fail_stage == SXCL_LOADER_FAIL_PREPARE, "  失败阶段是 prepare");
    check(strstr(res.error, "安装器 jar 不在") != NULL, "  有给人看的原因");
    check_str(sxcl_loader_fail_stage_name(res.fail_stage), "prepare", "  阶段名");
    check_str(sxcl_loader_fail_stage_name(SXCL_LOADER_FAIL_EXTRACT), "extract", "阶段名");
}

/* ── 9) 拍平(合并原版,产出一份能独立启动的版本 JSON) ──
 * 用户点名:「PCL 与 HMCL 装出来的都是能独立启动的版本 JSON,我们肯定要学」。
 * 这里钉住的都是"拍平之后必须成立"的事实,比如:没有 inheritsFrom / 没有 jar、
 * 加载器钉的库版本不被原版的同名库挡在前面、原版的 assetIndex 与下载信息还在。 */
static const char *kBaseJson =
    "{"
    "\"id\":\"1.20.1\",\"type\":\"release\",\"time\":\"2023-06-12T13:25:51+00:00\","
    "\"mainClass\":\"net.minecraft.client.main.Main\","
    "\"assetIndex\":{\"id\":\"5\",\"sha1\":\"aaa\",\"size\":1,\"url\":\"https://piston-meta.mojang.com/5.json\"},"
    "\"assets\":\"5\","
    "\"downloads\":{\"client\":{\"sha1\":\"bbb\",\"size\":3,\"url\":\"https://piston-data.mojang.com/client.jar\"}},"
    "\"javaVersion\":{\"component\":\"java-runtime-gamma\",\"majorVersion\":17},"
    "\"libraries\":["
    "{\"name\":\"com.google.guava:guava:31.1-jre\",\"downloads\":{\"artifact\":{\"path\":\"com/google/guava/guava/31.1-jre/guava-31.1-jre.jar\",\"sha1\":\"c\",\"size\":4}}},"
    "{\"name\":\"org.ow2.asm:asm:9.3\",\"downloads\":{\"artifact\":{\"path\":\"org/ow2/asm/asm/9.3/asm-9.3.jar\",\"sha1\":\"d\",\"size\":5}}},"
    "{\"name\":\"com.mojang:logging:1.1.1\",\"downloads\":{\"artifact\":{\"path\":\"com/mojang/logging/1.1.1/logging-1.1.1.jar\",\"sha1\":\"e\",\"size\":6}}}"
    "],"
    "\"arguments\":{\"jvm\":[\"-Djava.library.path=/natives\",\"-cp\",\"/classpath\"],"
    "\"game\":[\"--username\",\"/player\"]},"
    "\"logging\":{\"client\":{\"argument\":\"-Dlog4j.configurationFile=/log4j2.xml\",\"type\":\"log4j2-xml\"}}"
    "}";

static const char *kLoaderJson =
    "{"
    "\"id\":\"1.20.1-forge-47.2.0\",\"inheritsFrom\":\"1.20.1\",\"jar\":\"1.20.1\","
    "\"type\":\"release\",\"spec\":1,\"clientVersion\":\"9.9.9\","
    "\"mainClass\":\"cpw.mods.bootstraplauncher.BootstrapLauncher\","
    "\"libraries\":["
    "{\"name\":\"org.ow2.asm:asm:9.5\",\"downloads\":{\"artifact\":{\"path\":\"org/ow2/asm/asm/9.5/asm-9.5.jar\",\"sha1\":\"11\",\"size\":8}}},"
    "{\"name\":\"net.minecraftforge:forge:1.20.1-47.2.0\",\"downloads\":{\"artifact\":{\"path\":\"net/minecraftforge/forge/forge-universal.jar\",\"sha1\":\"12\",\"size\":9}}}"
    "],"
    "\"arguments\":{\"jvm\":[\"-Djava.library.path=/natives\",\"-p\",\"/modulepath\"],"
    "\"game\":[\"--launchTarget\",\"forgeclient\"]},"
    "\"processors\":[{\"classpath\":[\"net.minecraftforge:installertools:1.3.2\"],\"outputs\":{\"a\":\"b\"}}]"
    "}";

static void test_flatten(void) {
    char perr[192];
    char err[192];
    perr[0] = '\0';
    err[0] = '\0';
    sxcl_json *base = sxcl_json_parse(kBaseJson, strlen(kBaseJson), perr, sizeof(perr));
    sxcl_json *loader = sxcl_json_parse(kLoaderJson, strlen(kLoaderJson), perr, sizeof(perr));
    check(base != NULL && loader != NULL, "两份版本 JSON 都能解析");
    if (!base || !loader) {
        sxcl_json_free(base);
        sxcl_json_free(loader);
        return;
    }

    char *text = NULL;
    err[0] = '\0';
    check(sxcl_loader_flatten_json(sxcl_json_root(loader), sxcl_json_root(base),
                                   "1.20.1-Forge_47.2.0", "1.20.1", &text, err, sizeof(err)) ==
              SXCL_LOADER_OK,
          "拍平成功");
    check(text != NULL, "拍平有输出");
    if (text) {
        check(strstr(text, "inheritsFrom") == NULL, "拍平后没有 inheritsFrom(它不是独立版本的标记)");
        check(strstr(text, "\"jar\"") == NULL, "拍平后没有 jar 键");

        sxcl_json *back = sxcl_json_parse(text, strlen(text), perr, sizeof(perr));
        check(back != NULL, "拍平后还是合法 JSON");
        if (back) {
            const sxcl_json_value *root = sxcl_json_root(back);
            check_str(sxcl_json_get_string(root, "id", ""), "1.20.1-Forge_47.2.0", "id = 实例名");
            check_str(sxcl_json_get_string(root, "clientVersion", ""), "1.20.1",
                      "clientVersion = 原版版本号(PCL 的拍平标记)");
            check(sxcl_json_get(root, "inheritsFrom") == NULL, "  读不到 inheritsFrom");
            check(sxcl_json_get(root, "jar") == NULL, "  读不到 jar");
            check_str(sxcl_json_get_string(root, "mainClass", ""),
                      "cpw.mods.bootstraplauncher.BootstrapLauncher", "主类用加载器的");
            check_str(sxcl_json_get_string(root, "type", ""), "release", "其余键以原版为准");
            check(sxcl_json_get(root, "assetIndex") != NULL, "原版的 assetIndex 还在");
            check_str(sxcl_json_get_string(sxcl_json_get(root, "assetIndex"), "id", ""), "5",
                      "  资源索引还是 5");
            check_str(sxcl_json_get_string(
                          sxcl_json_get(sxcl_json_get(root, "downloads"), "client"), "url", ""),
                      "https://piston-data.mojang.com/client.jar", "原版的 client 下载信息还在");
            check(sxcl_json_get(root, "javaVersion") != NULL, "javaVersion 还在");
            check(sxcl_json_get(root, "logging") != NULL, "logging(日志配置)还在");

            /* libraries:加载器的两条在前,原版里没被同名覆盖的两条在后(asm 被加载器钉的 9.5 顶掉) */
            const sxcl_json_value *libs = sxcl_json_get(root, "libraries");
            check(sxcl_json_size(libs) == 4, "库 = 加载器 2 条 + 原版未被覆盖的 2 条");
            check_str(sxcl_json_get_string(sxcl_json_at(libs, 0), "name", ""), "org.ow2.asm:asm:9.5",
                      "  第一条是加载器钉的 asm 版本(不能被原版的 9.3 挡在前面)");
            check_str(sxcl_json_get_string(sxcl_json_at(libs, 1), "name", ""),
                      "net.minecraftforge:forge:1.20.1-47.2.0", "  第二条是 forge 本体");
            check_str(sxcl_json_get_string(sxcl_json_at(libs, 2), "name", ""),
                      "com.google.guava:guava:31.1-jre", "  原版独有的库保留");
            check_str(sxcl_json_get_string(sxcl_json_at(libs, 3), "name", ""), "com.mojang:logging:1.1.1",
                      "  原版独有的库保留(第二条)");
            check(count_in_range(text, NULL, "asm:9.3") == 0, "被覆盖的原版库不再出现");

            /* arguments:原版在前、加载器在后;两边都写过的 -Djava.library.path 只留一次 */
            check(count_in_range(text, NULL, "-Djava.library.path=/natives") == 1,
                  "重复的 JVM 参数只写一次");
            check(count_in_range(text, NULL, "--launchTarget") == 1, "加载器的游戏参数在");
            check(count_in_range(text, NULL, "--username") == 1, "原版的游戏参数在");
            check(count_in_range(text, NULL, "/modulepath") == 1, "加载器独有的 JVM 参数在");

            /* 加载器独有的键:原样带上(启动用不到,但不该猜着丢) */
            check(sxcl_json_get(root, "spec") != NULL, "加载器独有的 spec 保留");
            check(sxcl_json_size(sxcl_json_get(root, "processors")) == 1, "processors 原样保留");

            sxcl_json_free(back);
        }
        free(text);
        text = NULL;
    }

    /* 没有原版可合并:必须**报错**而不是假装拍平过(调用方据此决定退路) */
    err[0] = '\0';
    check(sxcl_loader_flatten_json(sxcl_json_root(loader), NULL, "x", "1.20.1", &text, err,
                                   sizeof(err)) == SXCL_LOADER_ERR_ARG,
          "没有原版 = 参数错");
    check(text == NULL, "  没有输出");
    check(strstr(err, "没有可合并的原版") != NULL, "  原因写清楚了");
    /* 实例名是必须的(id 要写成它) */
    check(sxcl_loader_flatten_json(sxcl_json_root(loader), sxcl_json_root(base), "", "1.20.1", &text,
                                   err, sizeof(err)) == SXCL_LOADER_ERR_ARG,
          "没有实例名 = 参数错");
    /* 原版自己就是完整 JSON(没有继承):照样能拍平,只是键值全来自它 */
    {
        char *same = NULL;
        err[0] = '\0';
        check(sxcl_loader_flatten_json(sxcl_json_root(loader), sxcl_json_root(base), "1.20.1", NULL,
                                       &same, err, sizeof(err)) == SXCL_LOADER_OK,
              "不给原版版本号也能拍(不加 clientVersion)");
        if (same) {
            check(strstr(same, "clientVersion") == NULL, "  没有 base_version 就不写 clientVersion");
            free(same);
        }
    }

    sxcl_json_free(loader);
    sxcl_json_free(base);
}

/* ── 10) 装完核对依赖库(docs/22 的 B9):说的库到底在不在磁盘上 ── */
static void test_missing_libraries(void) {
    const char *game = "build/_loader_tmp/missing-game";
    const char *inst = "build/_loader_tmp/missing-game/versions/probe";
    const char *vjson =
        "{\"id\":\"probe\",\"type\":\"release\",\"mainClass\":\"net.minecraft.client.main.Main\","
        "\"libraries\":["
        "{\"name\":\"com.example:a:1.0\",\"downloads\":{\"artifact\":{\"path\":\"com/example/a/a-1.0.jar\"}}},"
        "{\"name\":\"com.example:b:1.0\",\"downloads\":{\"artifact\":{\"path\":\"com/example/b/b-1.0.jar\"}}},"
        "{\"name\":\"com.example:c:1.0\",\"downloads\":{\"artifact\":{\"path\":\"com/example/c/c-1.0.jar\"}}}"
        "]}";
    char first[SXCL_LOADER_CMD_ARG_MAX];
    first[0] = '\0';

    check(write_file("build/_loader_tmp/missing-game/versions/probe/probe.json", vjson) == 0,
          "写下版本 JSON");
    check(write_file("build/_loader_tmp/missing-game/libraries/com/example/a/a-1.0.jar", "a") == 0,
          "摆上第一件库");
    check(write_file("build/_loader_tmp/missing-game/libraries/com/example/b/b-1.0.jar", "b") == 0,
          "摆上第二件库");
    check(sxcl_loader_count_missing_libraries(game, inst, "probe", first, sizeof(first)) == 1,
          "三件里缺一件 = 1(不去数在的那两件)");
    check_str(first, "com/example/c/c-1.0.jar", "  点名的是缺的那一件");

    check(write_file("build/_loader_tmp/missing-game/libraries/com/example/c/c-1.0.jar", "c") == 0,
          "把缺的那件补上");
    first[0] = 'x';
    check(sxcl_loader_count_missing_libraries(game, inst, "probe", first, sizeof(first)) == 0,
          "补齐之后 = 0");
    check_str(first, "", "  没有缺失就不点名(把 first 清空)");

    first[0] = '\0';
    check(sxcl_loader_count_missing_libraries(game, "build/_loader_tmp/missing-game/versions/nope",
                                              "nope", first, sizeof(first)) == 0,
          "版本 JSON 不在时返回 0(没得核对,不误报)");
}

int main(void) {
    check(sxcl_fs_mkdirs("build/_loader_tmp") == 0, "建临时目录(否则后面写文件全失败)");

    test_parse_version();
    test_check_selection();
    test_profiles_merge_text();
    test_profiles_file();
    test_build_commands();
    test_progress();
    test_maven();
    test_json_dump();
    test_flatten();
    test_missing_libraries();
    test_install_args();

    printf("loader 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
