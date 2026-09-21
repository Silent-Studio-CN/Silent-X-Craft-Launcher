/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/loader_catalog.h"

#include "catalog_fragments.inc"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got && want && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static void check_size(size_t got, size_t want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %lu want %lu\n", what, (unsigned long)got, (unsigned long)want);
    }
}

/* 在条目表里按版本串找;找不到返回 NULL。 */
static const sxcl_catalog_entry *find_entry(const sxcl_catalog_entry *items, size_t count,
                                            const char *version)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i].version, version) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

/* ── 1) 最小 XML 扫描器 ── */

static void test_xml_scanner(void)
{
    sxcl_xml_cursor cur;
    sxcl_xml_tag tag;
    char buf[64];

    static const char *const doc = "<?xml version=\"1.0\"?>\n<!-- 注释里有个 <version>9.9</version> -->\n"
                                   "<metadata>\n  <version>1.20.1-47.2.0</version>\n"
                                   "  <download mcversion='1.20.1' forge=\"Forge 47.2.18\" />\n</metadata>";
    sxcl_xml_init(&cur, doc, strlen(doc));

    check(sxcl_xml_next(&cur, &tag) == 1 && sxcl_xml_tag_is(&tag, "metadata"), "扫描器:跳过声明/注释,第一个是 metadata");
    check(tag.is_end == 0 && tag.self_closing == 0, "  开始标签标记正确");
    check(sxcl_xml_next(&cur, &tag) == 1 && sxcl_xml_tag_is(&tag, "version"), "扫描器:第二个是 version");
    check(sxcl_xml_text_plain(tag.text, tag.text_len, buf, sizeof buf) == 1, "  取文本成功");
    check_str(buf, "1.20.1-47.2.0", "  文本内容");
    check(sxcl_xml_next(&cur, &tag) == 1 && sxcl_xml_tag_is(&tag, "version") && tag.is_end, "扫描器:第三个是 </version>");
    check(sxcl_xml_next(&cur, &tag) == 1 && sxcl_xml_tag_is(&tag, "download"), "扫描器:第四个是 download");
    check(tag.self_closing == 1, "  自闭合标签认出来了");
    check(sxcl_xml_attr(&tag, "mcversion", buf, sizeof buf) == 1, "  属性(单引号)取到");
    check_str(buf, "1.20.1", "  属性值");
    check(sxcl_xml_attr(&tag, "forge", buf, sizeof buf) == 1, "  属性(双引号)取到");
    check_str(buf, "Forge 47.2.18", "  属性值");
    check(sxcl_xml_attr(&tag, "nope", buf, sizeof buf) == 0, "  不存在的属性返回 0");
    check(sxcl_xml_next(&cur, &tag) == 1 && sxcl_xml_tag_is(&tag, "metadata") && tag.is_end, "扫描器:最后是 </metadata>");
    check(sxcl_xml_next(&cur, &tag) == 0, "扫描器:到末尾返回 0");

    /* 大小写不敏感(HTML 那边常是大写标签) */
    static const char *const upper = "<TD CLASS='colForge'>Forge 61.0.8</TD>";
    sxcl_xml_init(&cur, upper, strlen(upper));
    check(sxcl_xml_next(&cur, &tag) == 1 && sxcl_xml_tag_is(&tag, "td"), "扫描器:标签名大小写不敏感");
    check(sxcl_xml_attr(&tag, "class", buf, sizeof buf) == 1, "  属性名大小写不敏感");
    check_str(buf, "colForge", "  属性值原样");

    /* 实体解码 */
    static const char *const ent = "&amp;&lt;&gt;&quot;&#65;&#x42;&apos;";
    check(sxcl_xml_text_plain(ent, strlen(ent), buf, sizeof buf) == 1, "文本:实体解码");
    check_str(buf, "&<>\"AB'", "  解码结果");

    /* 坏 XML:一律不崩、不越界 */
    static const char *const bad[] = {
        "", "<", "<a", "<<<<", "</>", "<a href=\"没闭合>", "<a<b>", "<!-- 没闭合",
        "<![CDATA[ 没闭合", "<a href='", "text without tags", "\0tail"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        const size_t len = (i == sizeof bad / sizeof bad[0] - 1) ? 6 : strlen(bad[i]);
        sxcl_xml_init(&cur, bad[i], len);
        int guard = 0;
        while (sxcl_xml_next(&cur, &tag)) {
            (void)sxcl_xml_attr(&tag, "href", buf, sizeof buf);
            if (++guard > 64) {
                break;   /* 死循环保护:真打转的话下面的断言会挂 */
            }
        }
        check(guard <= 64, "坏 XML 扫描不死循环");
    }

    /* 缓冲区不够:截断但不越界、仍 NUL 结尾 */
    static const char *const longtext = "0123456789ABCDEF";
    char tiny[8];
    check(sxcl_xml_text_plain(longtext, strlen(longtext), tiny, sizeof tiny) == 0, "文本:超长返回 0(截断)");
    check_str(tiny, "0123456", "  截断内容");
    sxcl_xml_init(&cur, longtext, strlen(longtext));
    check(sxcl_xml_next(&cur, &tag) == 0, "扫描器:纯文本里没有标签");
}

/* ── 2) Forge(maven-metadata.xml) ── */

static void test_forge(void)
{
    sxcl_catalog_doc_info info;
    sxcl_catalog_entry items[64];

    const size_t head = sxcl_catalog_parse_maven_xml(k_forge_xml_head, strlen(k_forge_xml_head),
                                                     SXCL_LOADER_FORGE, NULL, &info, items, 64);
    check_size(head, 5, "Forge: 头部片段 5 条");
    check_str(info.latest, "1.19.4-45.4.5", "Forge: <latest>");
    check_str(info.release, "1.19.4-45.4.5", "Forge: <release>");
    check_str(info.group, "net.minecraftforge", "Forge: <groupId>");
    check_str(info.artifact, "forge", "Forge: <artifactId>");
    check_str(items[0].version, "1.21-51.0.33", "Forge: 第一条版本串原样");
    check_str(items[0].mc, "1.21", "Forge: 从 1.21-51.0.33 里取 MC");
    check_str(items[0].loader, "51.0.33", "Forge: 取加载器版本");
    check(items[0].is_latest == 0 && items[0].is_recommended == 0, "Forge: <latest> 指的不是这一条,不硬标");

    /* 按 MC 过滤在解析里就完成,out_cap 给 1 也只填 1 条 */
    sxcl_catalog_entry one[1];
    check_size(sxcl_catalog_parse_maven_xml(k_forge_xml_1201, strlen(k_forge_xml_1201),
                                            SXCL_LOADER_FORGE, "1.20.1", NULL, one, 1),
               8, "Forge: 1.20.1 段一共 8 条(缓冲只给 1 条也报总数)");
    check_str(one[0].version, "1.20.1-47.4.5", "  只填了第一条");

    check_size(sxcl_catalog_parse_maven_xml(k_forge_xml_1201, strlen(k_forge_xml_1201),
                                            SXCL_LOADER_FORGE, "1.20.1", NULL, NULL, 0),
               8, "Forge: 先问条数(不填表)");
    check_size(sxcl_catalog_parse_maven_xml(k_forge_xml_1201, strlen(k_forge_xml_1201),
                                            SXCL_LOADER_FORGE, NULL, NULL, items, 64),
               9, "Forge: 不过滤时 9 条(含 1.20.2-48.0.0)");
    check_size(sxcl_catalog_parse_maven_xml(k_forge_xml_1201, strlen(k_forge_xml_1201),
                                            SXCL_LOADER_FORGE, "1.19.2", NULL, items, 64),
               0, "Forge: 不存在的 MC 版本 -> 0 条");

    check_size(sxcl_catalog_parse_maven_xml(k_forge_xml_old, strlen(k_forge_xml_old),
                                            SXCL_LOADER_FORGE, "1.12.2", NULL, items, 64),
               3, "Forge: 1.12.2 段 3 条(1.12-14.21.0.2320 不算)");
    check_str(items[0].version, "1.12.2-14.23.5.2860", "  老版本原样");
    check_str(items[0].loader, "14.23.5.2860", "  老版本号解析");

    /* 真实文档里的 <release> 落在这条上 -> 直接带上 recommended */
    check_size(sxcl_catalog_parse_maven_xml(k_forge_xml_tail, strlen(k_forge_xml_tail),
                                            SXCL_LOADER_FORGE, NULL, &info, items, 64),
               2, "Forge: 尾部片段 2 条");
    check(info.release[0] == '\0', "  尾部片段里没有 <release>");
    check_str(info.last_updated, "2026-08-27 04:59:25", "Forge: <lastUpdated> 20260827045925 格式化成 ISO");
    check_str(items[0].version, "26.1.2-64.1.3", "Forge: 尾部片段第一条是 26.1.2-64.1.3");

    /* 一把梭:排序 + 标记 */
    size_t count = sxcl_catalog_prepare(SXCL_LOADER_FORGE, k_forge_xml_1201, strlen(k_forge_xml_1201),
                                        "1.20.1", &info, items, 64);
    check_size(count, 8, "Forge prepare: 8 条");
    check_str(items[0].version, "1.20.1-47.4.5", "  prepare 后最新在最前");
    check(items[0].is_latest == 1, "  第一条被标 latest");
    check(items[0].is_recommended == 1, "  第一条被标 recommended(源里没给标记时的兜底)");
    check_str(items[count - 1].version, "1.20.1-47.3.38", "  最旧的在最后");
    check(items[0].is_beta == 0 && items[count - 1].is_beta == 0, "  Forge 正式版不是 Beta");

    /* release 落在某一条上时,推荐标记跟着它走 */
    static const char *const with_release =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<metadata>\n  <versioning>\n"
        "    <latest>1.20.1-47.4.5</latest>\n    <release>1.20.1-47.2.0</release>\n    <versions>\n"
        "      <version>1.20.1-47.4.5</version>\n      <version>1.20.1-47.2.0</version>\n"
        "    </versions>\n  </versioning>\n</metadata>";
    count = sxcl_catalog_prepare(SXCL_LOADER_FORGE, with_release, strlen(with_release), "1.20.1",
                                 &info, items, 64);
    check_size(count, 2, "Forge prepare: 带 latest/release 的文档 2 条");
    const sxcl_catalog_entry *e4720 = find_entry(items, count, "1.20.1-47.2.0");
    const sxcl_catalog_entry *e4745 = find_entry(items, count, "1.20.1-47.4.5");
    check(e4745 && e4745->is_latest == 1, "  <latest> 那条带 latest");
    check(e4720 && e4720->is_recommended == 1, "  <release> 那条带 recommended");
    check(e4745 && e4745->is_recommended == 0, "  不是 release 的没有 recommended");
    check(e4745 && e4720 && strcmp(items[0].version, "1.20.1-47.4.5") == 0, "  排序仍是新的在前");
}

/* ── 3) NeoForge(maven-metadata.xml,21.1.72 -> 1.21.1) ── */

static void test_neoforge(void)
{
    sxcl_catalog_doc_info info;
    sxcl_catalog_entry items[64];

    const size_t head = sxcl_catalog_parse_maven_xml(k_neo_xml_head, strlen(k_neo_xml_head),
                                                     SXCL_LOADER_NEOFORGE, NULL, &info, items, 64);
    check_size(head, 5, "NeoForge: 头部片段 5 条");
    check_str(info.latest, "26.3.0.1-beta", "NeoForge: <latest> 就是这个 beta");
    check_str(info.release, "26.3.0.1-beta", "NeoForge: <release>");
    check_str(info.artifact, "neoforge", "NeoForge: <artifactId>");
    check_str(items[0].mc, "1.20.2", "NeoForge: 20.2.12-beta -> MC 1.20.2");
    check(items[0].is_beta == 1, "  带 -beta 的标 Beta");

    check_size(sxcl_catalog_parse_maven_xml(k_neo_xml_211, strlen(k_neo_xml_211), SXCL_LOADER_NEOFORGE,
                                            "1.21.1", NULL, items, 64),
               3, "NeoForge: 21.1.x 三条属于 1.21.1");
    check_str(items[2].version, "21.1.72", "  原样保留版本串");
    check_str(items[2].mc, "1.21.1", "  21.1.72 -> MC 1.21.1");
    check_str(items[2].loader, "21.1.72", "  加载器版本");

    size_t count = sxcl_catalog_prepare(SXCL_LOADER_NEOFORGE, k_neo_xml_211, strlen(k_neo_xml_211),
                                        "1.21.1", &info, items, 64);
    check_size(count, 3, "NeoForge prepare: 1.21.1 三条");
    check_str(items[0].version, "21.1.72", "  最新的在最前");
    check(items[0].is_latest == 1 && items[0].is_recommended == 1, "  标成 latest/recommended");
    check(items[0].is_beta == 0, "  它不是 Beta");

    count = sxcl_catalog_prepare(SXCL_LOADER_NEOFORGE, k_neo_xml_211, strlen(k_neo_xml_211), NULL,
                                 &info, items, 64);
    check_size(count, 7, "NeoForge prepare(不过滤): 7 条");
    check_str(items[0].version, "21.3.1-beta", "  21.3.1-beta 数字最大,排最前");
    check(items[0].is_beta == 1, "  它是 Beta");
    check(items[0].is_latest == 0, "  Beta 不当 latest");
    const sxcl_catalog_entry *nv = find_entry(items, count, "21.1.72");
    check(nv != NULL && nv->is_latest == 1 && nv->is_recommended == 1,
          "  latest/recommended 落到第一条非 Beta(21.1.72)");
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i].version, "21.2.0-beta") == 0) {
            check(items[i].is_beta == 1 && strcmp(items[i].mc, "1.21.2") == 0, "  21.2.0-beta -> MC 1.21.2 且是 Beta");
        }
    }
}

/* ── 4) Fabric / Quilt(meta JSON) ── */

static void test_meta_json(void)
{
    sxcl_catalog_entry items[8];

    size_t count = sxcl_catalog_parse_fabric_json(k_fabric_json_mc, strlen(k_fabric_json_mc), "1.20.1",
                                                  items, 8);
    check_size(count, 1, "Fabric: 真片段 1 条");
    check_str(items[0].version, "0.19.5", "  版本串");
    check_str(items[0].loader, "0.19.5", "  加载器版本");
    check_str(items[0].mc, "1.20.1", "  接口是按 MC 查的,MC 用调用方给的");
    check(items[0].is_recommended == 1, "  stable=true -> recommended");
    check(items[0].is_beta == 0, "  正式版不是 Beta");

    count = sxcl_catalog_parse_fabric_json(k_fabric_json_mc_old, strlen(k_fabric_json_mc_old), "1.20.1",
                                           items, 8);
    check_size(count, 1, "Fabric: 老版本片段 1 条");
    check_str(items[0].version, "0.19.3", "  版本串");
    check(items[0].is_recommended == 0, "  stable=false -> 不是 recommended");
    check(items[0].is_beta == 0, "  stable=false 不等于 Beta");

    count = sxcl_catalog_prepare(SXCL_LOADER_FABRIC, k_fabric_json_mc_old, strlen(k_fabric_json_mc_old),
                                 "1.20.1", NULL, items, 8);
    check_size(count, 1, "Fabric prepare: 1 条");
    check(items[0].is_latest == 1 && items[0].is_recommended == 1, "  源里没给标记时兜底标上");

    /* 平铺形态(meta.fabricmc.net/v2/versions/loader,元素里没有 loader 子对象) */
    static const char *const flat =
        "[{\"separator\":\".\",\"build\":5,\"maven\":\"net.fabricmc:fabric-loader:0.19.5\","
        "\"version\":\"0.19.5\",\"stable\":true},"
        "{\"separator\":\".\",\"build\":4,\"maven\":\"net.fabricmc:fabric-loader:0.19.4\","
        "\"version\":\"0.19.4\",\"stable\":false}]";
    count = sxcl_catalog_prepare(SXCL_LOADER_FABRIC, flat, strlen(flat), "1.20.1", NULL, items, 8);
    check_size(count, 2, "Fabric: 平铺形态也能读");
    check_str(items[0].version, "0.19.5", "  排序:新的在前");
    check(items[0].is_recommended == 1 && items[1].is_recommended == 0, "  stable 标记跟着元素走");

    /* 只有 maven 坐标时取最后一段 */
    static const char *const maven_only = "[{\"loader\":{\"maven\":\"net.fabricmc:fabric-loader:0.16.9\"}}]";
    count = sxcl_catalog_parse_fabric_json(maven_only, strlen(maven_only), "1.20.1", items, 8);
    check_size(count, 1, "Fabric: 只有 maven 坐标");
    check_str(items[0].version, "0.16.9", "  从坐标末段取版本");

    count = sxcl_catalog_parse_quilt_json(k_quilt_json_mc, strlen(k_quilt_json_mc), "1.20.1", items, 8);
    check_size(count, 1, "Quilt: 真片段 1 条");
    check_str(items[0].version, "0.20.0-beta.9", "  版本串原样");
    check_str(items[0].mc, "1.20.1", "  MC");
    check(items[0].is_beta == 1, "  带 beta 的标 Beta");
    check(items[0].is_recommended == 0, "  Quilt 接口没有 stable 字段");

    count = sxcl_catalog_prepare(SXCL_LOADER_QUILT, k_quilt_json_mc2, strlen(k_quilt_json_mc2), "1.20.1",
                                 NULL, items, 8);
    check_size(count, 2, "Quilt prepare: 真片段 2 条");
    check_str(items[0].version, "0.20.0-beta.8", "  数字相同时按字符串倒序(beta.8 > beta.7)");
    check(items[0].is_latest == 1 && items[0].is_recommended == 1, "  全是 Beta 时第一条也顶上");
}

/* ── 5) OptiFine(官方网页 + BMCLAPI JSON) ── */

static void test_optifine(void)
{
    sxcl_catalog_entry items[32];

    /* 官方网页:HD U J9 那行(colForge = Forge 61.0.8) */
    size_t count = sxcl_catalog_prepare(SXCL_LOADER_OPTIFINE, k_optifine_html, strlen(k_optifine_html),
                                        "1.21.11", NULL, items, 32);
    check_size(count, 2, "OptiFine 网页: J9/J8 两条(页面里是 main + more 两个表)");
    check_str(items[0].version, "HD_U_J9", "  版本串(文件名里那段)");
    check_str(items[0].mc, "1.21.11", "  MC 从文件名里取");
    check_str(items[0].forge, "61.0.8", "  配套 Forge 从 colForge 里取(Forge 61.0.8)");
    check_str(items[0].released, "2026-02-05", "  colDate 05.02.2026 归一成 ISO");
    check_str(items[0].display, "OptiFine HD U J9", "  显示名用 colFile 那列");
    check_str(items[0].file, "OptiFine_1.21.11_HD_U_J9.jar", "  jar 文件名从下载链接里抠");
    check(items[0].is_beta == 0, "  正式版不是 Beta");
    const sxcl_catalog_entry *j8 = find_entry(items, count, "HD_U_J8");
    check(j8 != NULL, "  第二行 HD_U_J8 也在");
    if (j8) {
        check_str(j8->forge, "61.0.7", "    J8 的配套 Forge");
        check_str(j8->released, "2026-01-21", "    J8 的日期");
        check_str(j8->file, "OptiFine_1.21.11_HD_U_J8.jar", "    J8 的文件名");
    }

    check_size(sxcl_catalog_parse_optifine_html(k_optifine_html, strlen(k_optifine_html), "1.20.1",
                                                items, 32),
               0, "OptiFine 网页: 不是这个 MC 的就过滤掉");

    /* 官方网页:HD U K1 pre2(Forge N/A 的预览版) */
    count = sxcl_catalog_prepare(SXCL_LOADER_OPTIFINE, k_optifine_html_preview,
                                 strlen(k_optifine_html_preview), "26.1.2", NULL, items, 32);
    check_size(count, 1, "OptiFine 网页: 预览版一条");
    check_str(items[0].version, "HD_U_K1_pre2", "  预览版版本串");
    check_str(items[0].mc, "26.1.2", "  MC");
    check_str(items[0].forge, "", "  Forge N/A -> 空(不能写成 'N/A')");
    check_str(items[0].released, "2026-06-22", "  日期");
    check(items[0].is_beta == 1, "  预览版标 Beta");

    /* BMCLAPI:唯一的 OptiFine 结构化接口(带 forge 字段) */
    count = sxcl_catalog_prepare(SXCL_LOADER_OPTIFINE, k_optifine_json, strlen(k_optifine_json), "1.20.1",
                                 NULL, items, 32);
    check_size(count, 14, "OptiFine JSON: 1.20.1 共 14 条");
    check_str(items[0].version, "HD_U_I6", "  最新的正式版在最前");
    check(items[0].is_latest == 1 && items[0].is_recommended == 1, "  标成 latest/recommended");
    check(items[0].is_beta == 0, "  正式版");
    check_str(items[0].forge, "47.2.18", "  forge 字段 'Forge 47.2.18' -> '47.2.18'");
    check_str(items[0].file, "OptiFine_1.20.1_HD_U_I6.jar", "  filename");
    const sxcl_catalog_entry *pre = find_entry(items, count, "HD_U_I5_pre4");
    check(pre != NULL, "  预览版 HD_U_I5_pre4 在列表里");
    if (pre) {
        check(pre->is_beta == 1, "    预览版标 Beta");
        check_str(pre->forge, "47.0.3", "    它配套的 Forge");
        check_str(pre->file, "preview_OptiFine_1.20.1_HD_U_I5_pre4.jar", "    文件名");
    }
    const sxcl_catalog_entry *i5 = find_entry(items, count, "HD_U_I5");
    check(i5 != NULL && i5->is_beta == 0, "  正式版 HD_U_I5 不是 Beta");
    check(i5 != NULL && i5->is_recommended == 0, "  它不是 recommended");
    check_size(sxcl_catalog_parse_optifine_json(k_optifine_json, strlen(k_optifine_json), "1.19.4",
                                                items, 32),
               0, "OptiFine JSON: 别的 MC 过滤掉");

    /* 镜像/第三方可能给的 XML 形态。**注意**:optifine.net 官方只有网页,
     * downloads.xml 实测 404 —— 这一段是按任务描述构造的形态,不是实测响应。 */
    static const char *const of_xml =
        "<downloads>\n"
        "  <download mcversion=\"1.20.1\" type=\"HD_U\" patch=\"I6\" forge=\"Forge 47.2.18\"\n"
        "            filename=\"OptiFine_1.20.1_HD_U_I6.jar\" />\n"
        "  <download mcversion=\"1.20.1\" type=\"HD_U_I5\" patch=\"pre4\" forge=\"Forge 47.0.3\"\n"
        "            filename=\"preview_OptiFine_1.20.1_HD_U_I5_pre4.jar\" />\n"
        "</downloads>";
    count = sxcl_catalog_prepare(SXCL_LOADER_OPTIFINE, of_xml, strlen(of_xml), "1.20.1", NULL, items, 32);
    check_size(count, 2, "OptiFine XML 形态: 2 条");
    check_str(items[0].version, "HD_U_I6", "  正式版在前");
    check_str(items[0].forge, "47.2.18", "  forge 属性");
    check_str(items[0].mc, "1.20.1", "  mcversion 属性");
    check(strcmp(items[1].version, "HD_U_I5_pre4") == 0 && items[1].is_beta == 1, "  预览版在后且标 Beta");
}

/* ── 6) 纯函数:过滤 / 排序 / 标记 ── */

static void test_pure_helpers(void)
{
    sxcl_catalog_entry items[8];
    memset(items, 0, sizeof items);
    const char *const versions[4] = { "1.20.1-47.2.0", "1.20.1-47.4.0", "1.12.2-14.23.5.2860",
                                      "1.20.1-47.10.0" };
    const char *const mcs[4] = { "1.20.1", "1.20.1", "1.12.2", "1.20.1" };
    for (size_t i = 0; i < 4; ++i) {
        snprintf(items[i].version, sizeof items[i].version, "%s", versions[i]);
        snprintf(items[i].mc, sizeof items[i].mc, "%s", mcs[i]);
    }
    const size_t kept = sxcl_catalog_filter_mc(items, 4, "1.20.1", items, 8);
    check_size(kept, 3, "过滤: 1.20.1 三条");
    check(strcmp(items[2].version, "1.20.1-47.10.0") == 0, "  过滤是原地做的,顺序不变");
    check_size(sxcl_catalog_filter_mc(items, kept, NULL, items, 8), 3, "过滤: 空条件 = 全留");
    check_size(sxcl_catalog_filter_mc(items, kept, "", items, 8), 3, "过滤: 空串 = 全留");

    sxcl_catalog_sort_desc(items, kept);
    check_str(items[0].version, "1.20.1-47.10.0", "排序: 47.10.0 比 47.4.0 新(数字比大小,不是字符串)");
    check_str(items[1].version, "1.20.1-47.4.0", "排序: 第二");
    check_str(items[2].version, "1.20.1-47.2.0", "排序: 第三");

    sxcl_catalog_mark_flags(items, kept);
    check(items[0].is_latest == 1 && items[0].is_recommended == 1, "标记: 第一条非 Beta 顶上");
    check(items[1].is_latest == 0 && items[2].is_latest == 0, "标记: 别的没有");

    /* 源里已经给标记时不许被覆盖 */
    memset(items, 0, sizeof items);
    snprintf(items[0].version, sizeof items[0].version, "%s", "1.20.1-47.2.0");
    snprintf(items[1].version, sizeof items[1].version, "%s", "1.20.1-47.4.0");
    items[1].is_latest = 1;
    items[0].is_recommended = 1;
    sxcl_catalog_sort_desc(items, 2);
    sxcl_catalog_mark_flags(items, 2);
    const sxcl_catalog_entry *old = find_entry(items, 2, "1.20.1-47.2.0");
    const sxcl_catalog_entry *newer = find_entry(items, 2, "1.20.1-47.4.0");
    check(old && old->is_recommended == 1, "标记: 源给的 recommended 保留");
    check(newer && newer->is_latest == 1, "标记: 源给的 latest 保留");

    /* 全是 Beta 时也得有可默认选中的一条 */
    memset(items, 0, sizeof items);
    for (size_t i = 0; i < 3; ++i) {
        items[i].is_beta = 1;
    }
    snprintf(items[0].version, sizeof items[0].version, "%s", "0.1.0-beta");
    sxcl_catalog_mark_flags(items, 3);
    check(items[0].is_latest == 1 && items[0].is_recommended == 1, "标记: 全 Beta 时第一条顶上");
}

/* ── 7) 地址与格式 ── */

static void test_urls(void)
{
    char url[512];

    check(sxcl_catalog_url(SXCL_LOADER_FORGE, SXCL_CATALOG_SRC_OFFICIAL, NULL, url, sizeof url) == SXCL_CATALOG_OK,
          "URL: Forge 官方");
    check_str(url, "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_FORGE, SXCL_CATALOG_SRC_MIRROR, NULL, url, sizeof url) == SXCL_CATALOG_OK,
          "URL: Forge 镜像");
    check_str(url, "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/maven-metadata.xml", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_NEOFORGE, SXCL_CATALOG_SRC_OFFICIAL, NULL, url, sizeof url) == SXCL_CATALOG_OK,
          "URL: NeoForge 官方");
    check_str(url, "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_FABRIC, SXCL_CATALOG_SRC_OFFICIAL, "1.20.1", url, sizeof url) == SXCL_CATALOG_OK,
          "URL: Fabric 官方");
    check_str(url, "https://meta.fabricmc.net/v2/versions/loader/1.20.1", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_FABRIC, SXCL_CATALOG_SRC_MIRROR, "1.20.1", url, sizeof url) == SXCL_CATALOG_OK,
          "URL: Fabric 镜像");
    check_str(url, "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/1.20.1", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_QUILT, SXCL_CATALOG_SRC_OFFICIAL, "1.20.1", url, sizeof url) == SXCL_CATALOG_OK,
          "URL: Quilt 官方");
    check_str(url, "https://meta.quiltmc.org/v3/versions/loader/1.20.1", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_QUILT, SXCL_CATALOG_SRC_MIRROR, "1.20.1", url, sizeof url) == SXCL_CATALOG_OK,
          "URL: Quilt 镜像(实测 BMCLAPI 没有透传,这里也只给官方)");
    check_str(url, "https://meta.quiltmc.org/v3/versions/loader/1.20.1", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_OPTIFINE, SXCL_CATALOG_SRC_OFFICIAL, "1.20.1", url, sizeof url) == SXCL_CATALOG_OK,
          "URL: OptiFine 官方(网页)");
    check_str(url, "https://optifine.net/downloads", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_OPTIFINE, SXCL_CATALOG_SRC_MIRROR, "1.20.1", url, sizeof url) == SXCL_CATALOG_OK,
          "URL: OptiFine 镜像");
    check_str(url, "https://bmclapi2.bangbang93.com/optifine/1.20.1", "  内容");
    check(sxcl_catalog_url(SXCL_LOADER_VANILLA, SXCL_CATALOG_SRC_OFFICIAL, NULL, url, sizeof url) == SXCL_CATALOG_ERR_ARG,
          "URL: 原版没有列表地址");

    char tiny[16];
    char small[48];
    check(sxcl_catalog_url(SXCL_LOADER_OPTIFINE, SXCL_CATALOG_SRC_OFFICIAL, NULL, small, sizeof small) == SXCL_CATALOG_OK,
          "URL: 缓冲刚好够的时候成功");
    check(sxcl_catalog_url(SXCL_LOADER_OPTIFINE, SXCL_CATALOG_SRC_MIRROR, "1.20.1", tiny, sizeof tiny) == SXCL_CATALOG_ERR_SPACE,
          "URL: 缓冲不够报 ERR_SPACE");
    check_str(tiny, "", "  失败时输出被清空");

    check(sxcl_catalog_format_of(SXCL_LOADER_FORGE, SXCL_CATALOG_SRC_OFFICIAL) == SXCL_CATALOG_FMT_MAVEN_XML, "格式: Forge=maven XML");
    check(sxcl_catalog_format_of(SXCL_LOADER_NEOFORGE, SXCL_CATALOG_SRC_MIRROR) == SXCL_CATALOG_FMT_MAVEN_XML, "格式: NeoForge 镜像也是 maven XML");
    check(sxcl_catalog_format_of(SXCL_LOADER_FABRIC, SXCL_CATALOG_SRC_OFFICIAL) == SXCL_CATALOG_FMT_META_JSON, "格式: Fabric=meta JSON");
    check(sxcl_catalog_format_of(SXCL_LOADER_OPTIFINE, SXCL_CATALOG_SRC_MIRROR) == SXCL_CATALOG_FMT_OPTIFINE_JSON, "格式: OptiFine 镜像=JSON");
    check(sxcl_catalog_format_of(SXCL_LOADER_OPTIFINE, SXCL_CATALOG_SRC_OFFICIAL) == SXCL_CATALOG_FMT_OPTIFINE_HTML, "格式: OptiFine 官方=网页");
    check(sxcl_catalog_format_of(SXCL_LOADER_VANILLA, SXCL_CATALOG_SRC_OFFICIAL) == SXCL_CATALOG_FMT_NONE, "格式: 原版=NONE");
}

/* ── 8) 坏输入 ── */

static void test_bad_input(void)
{
    sxcl_catalog_doc_info info;
    sxcl_catalog_entry items[8];

    check_size(sxcl_catalog_parse(SXCL_LOADER_FORGE, NULL, 0, NULL, &info, items, 8), 0, "坏输入: NULL 文本");
    check_size(sxcl_catalog_parse(SXCL_LOADER_FORGE, "", 0, NULL, &info, items, 8), 0, "坏输入: 空文本");
    check_size(sxcl_catalog_parse(SXCL_LOADER_FORGE, "<metadata><versions>",
                                  strlen("<metadata><versions>"), NULL, &info, items, 8), 0,
               "坏输入: XML 截断");
    check_size(sxcl_catalog_parse(SXCL_LOADER_FABRIC, "{not json", 9, "1.20.1", &info, items, 8), 0,
               "坏输入: JSON 截断");
    check_size(sxcl_catalog_parse(SXCL_LOADER_FABRIC, "[]", 2, "1.20.1", &info, items, 8), 0, "坏输入: 空数组");
    check_size(sxcl_catalog_parse(SXCL_LOADER_FABRIC, "{\"a\":1}", 7, "1.20.1", &info, items, 8), 0,
               "坏输入: 对象不是数组");
    check_size(sxcl_catalog_parse(SXCL_LOADER_OPTIFINE, "[]", 2, "1.20.1", &info, items, 8), 0,
               "坏输入: OptiFine 空 JSON");
    check_size(sxcl_catalog_parse(SXCL_LOADER_OPTIFINE, "<html></html>", 13, "1.20.1", &info, items, 8), 0,
               "坏输入: OptiFine 空网页");
    check_size(sxcl_catalog_parse(SXCL_LOADER_VANILLA, "<metadata/>", 11, NULL, &info, items, 8), 0,
               "坏输入: 原版没有列表");
    check_size(sxcl_catalog_prepare(SXCL_LOADER_FORGE, k_forge_xml_head, strlen(k_forge_xml_head), "1.21",
                                    NULL, NULL, 0), 5, "prepare: out=NULL 时只报条数");
    check_size(sxcl_catalog_filter_mc(items, 0, "1.20.1", items, 8), 0, "过滤: 空表");
    sxcl_catalog_sort_desc(NULL, 0);
    sxcl_catalog_mark_flags(NULL, 0);
    check(1, "排序/标记: 空指针不崩");
}

/* ── 9) 各个加载器在真实数据里的"端到端"选择路径 ── */

static void test_end_to_end_pick(void)
{
    sxcl_catalog_entry items[64];
    sxcl_catalog_doc_info info;

    /* Forge */
    size_t count = sxcl_catalog_prepare(SXCL_LOADER_FORGE, k_forge_xml_1201, strlen(k_forge_xml_1201),
                                        "1.20.1", &info, items, 64);
    check(count > 0 && strcmp(items[0].version, "1.20.1-47.4.5") == 0, "端到端: Forge 1.20.1 默认选中 47.4.5");
    check_str(items[0].display, "1.20.1-47.4.5", "  非 OptiFine 的 display 就是版本串");
    check_str(items[0].forge, "", "  非 OptiFine 没有 forge 字段");
    check_str(items[0].released, "", "  maven 没有逐版本发布时间(不编)");

    /* NeoForge */
    count = sxcl_catalog_prepare(SXCL_LOADER_NEOFORGE, k_neo_xml_211, strlen(k_neo_xml_211), "1.21.1",
                                 &info, items, 64);
    check(count > 0 && strcmp(items[0].version, "21.1.72") == 0, "端到端: NeoForge 1.21.1 默认选中 21.1.72");

    /* Fabric */
    count = sxcl_catalog_prepare(SXCL_LOADER_FABRIC, k_fabric_json_mc, strlen(k_fabric_json_mc), "1.20.1",
                                 &info, items, 64);
    check(count > 0 && strcmp(items[0].version, "0.19.5") == 0, "端到端: Fabric 1.20.1 默认选中 0.19.5");

    /* Quilt */
    count = sxcl_catalog_prepare(SXCL_LOADER_QUILT, k_quilt_json_mc, strlen(k_quilt_json_mc), "1.20.1",
                                 &info, items, 64);
    check(count > 0 && strcmp(items[0].version, "0.20.0-beta.9") == 0, "端到端: Quilt 1.20.1 默认选中 0.20.0-beta.9");

    /* OptiFine */
    count = sxcl_catalog_prepare(SXCL_LOADER_OPTIFINE, k_optifine_json, strlen(k_optifine_json), "1.20.1",
                                 &info, items, 64);
    check(count > 0 && strcmp(items[0].version, "HD_U_I6") == 0, "端到端: OptiFine 1.20.1 默认选中 HD_U_I6");
    check_str(items[0].forge, "47.2.18", "  它要求配套 Forge 47.2.18");
}

int main(void)
{
    test_xml_scanner();
    test_forge();
    test_neoforge();
    test_meta_json();
    test_optifine();
    test_pure_helpers();
    test_urls();
    test_bad_input();
    test_end_to_end_pick();

    printf("加载器版本目录测试: 通过 %d 失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
