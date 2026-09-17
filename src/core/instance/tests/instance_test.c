/* 实例扫描 / 游戏目录探测 / HTTP 取文本 / 机器信息 的测试。
 *
 * 夹具:一棵**假的游戏目录树**,建在构建目录下(ctest 的 WORKING_DIRECTORY 就是本模块的构建目录,
 * 已被 .gitignore 忽略:仓库里不留任何二进制/临时夹具)。每个版本目录对应 Python 版
 * loaders.py / folders.py / pcl_compat.py 里的一条识别规则,下方注释逐条写明是哪一条。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_internal.h"
#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/http.h"
#include "sxcl/instance.h"
#include "sxcl/paths.h"
#include "sxcl/sysinfo.h"

#define FIXTURE_ROOT "sxcl_instance_fixture"
#define VD(id) FIXTURE_ROOT "/versions/" id
#define FX(rel) FIXTURE_ROOT "/" rel

static int g_pass = 0;
static int g_fail = 0;
static const sxcl_instance_list *g_list = NULL;

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
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static void check_contains(const char *hay, const char *needle, const char *what)
{
    if (hay != NULL && needle != NULL && strstr(hay, needle) != NULL) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: '%s' 里找不到 '%s'\n", what, hay ? hay : "(null)", needle ? needle : "(null)");
    }
}

static void check_int(long long got, long long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %lld want %lld\n", what, got, want);
    }
}

/* ── 夹具工具 ── */

static void put(const char *path, const char *text)
{
    sxcl_fs_mkdirs_for_file(path);
    FILE *fh = sxcl_fs_fopen(path, "wb");
    if (fh == NULL) {
        printf("  [!!] 夹具写不进去: %s\n", path);
        ++g_fail;
        return;
    }
    (void)fputs(text, fh);
    (void)fclose(fh);
}

static void build_fixture_tree(void)
{
    (void)sxcl_dir_remove_tree(FIXTURE_ROOT);

    /* ① 纯原版:JSON + jar 都在 -> 可启动 */
    put(VD("1.20.1") "/1.20.1.json",
        "{ \"id\": \"1.20.1\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [ { \"name\": \"com.mojang:logging:1.0\" } ] }");
    put(VD("1.20.1") "/1.20.1.jar", "jar");

    /* ② PCL 拍平版 Forge:clientVersion 认原版 + libraries 的 net.minecraftforge:forge 认加载器 */
    put(VD("1.20.1-Forge_47.2.0") "/1.20.1-Forge_47.2.0.json",
        "{ \"id\": \"1.20.1-Forge_47.2.0\", \"type\": \"release\", \"clientVersion\": \"1.20.1\","
        " \"mainClass\": \"cpw.mods.modlauncher.Launcher\","
        " \"libraries\": [ { \"name\": \"net.minecraftforge:forge:1.20.1-47.2.0\" } ] }");

    /* ③ NeoForge:坐标 net.neoforged:neoforge 必须比 Forge 规则先命中(否则会认成 Forge) */
    put(VD("1.21.1-NeoForge_21.1.72") "/1.21.1-NeoForge_21.1.72.json",
        "{ \"id\": \"1.21.1-NeoForge_21.1.72\", \"type\": \"release\", \"clientVersion\": \"1.21.1\","
        " \"mainClass\": \"net.neoforged.fml.startup.Client\","
        " \"libraries\": [ { \"name\": \"net.neoforged:neoforge:21.1.72\" } ] }");

    /* ④ Fabric:inheritsFrom 认原版 + net.fabricmc:fabric-loader 认加载器(没有自己的 jar) */
    put(VD("fabric-loader-0.15.11-1.20.1") "/fabric-loader-0.15.11-1.20.1.json",
        "{ \"id\": \"fabric-loader-0.15.11-1.20.1\", \"type\": \"release\", \"inheritsFrom\": \"1.20.1\","
        " \"mainClass\": \"net.fabricmc.loader.impl.launch.knot.KnotClient\","
        " \"libraries\": [ { \"name\": \"net.fabricmc:fabric-loader:0.15.11\" } ] }");

    /* ⑤ mainClass 线索(org.quiltmc.loader):同时验证 Quilt 会出现在结果里(Python 的 LOADER_ORDER 漏了它) */
    put(VD("1.20.1-LoaderTest") "/1.20.1-LoaderTest.json",
        "{ \"id\": \"1.20.1-LoaderTest\", \"type\": \"release\", \"inheritsFrom\": \"1.20.1\","
        " \"mainClass\": \"org.quiltmc.loader.impl.launch.knot.KnotClient\", \"libraries\": [] }");

    /* ⑥ 目录名线索 forge(JSON 里完全没有任何加载器痕迹) */
    put(VD("1.16.5-Forge_36.2.34") "/1.16.5-Forge_36.2.34.json",
        "{ \"id\": \"1.16.5-Forge_36.2.34\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑦ 目录名线索 fabric(PCL 的 "Fabric_0.14.21" 形态;带空格的那种靠 JSON 认,见 ④) */
    put(VD("1.19.4-Fabric_0.14.21") "/1.19.4-Fabric_0.14.21.json",
        "{ \"id\": \"1.19.4-Fabric_0.14.21\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑧ 目录名线索 quilt */
    put(VD("1.20.1-Quilt_0.23.1") "/1.20.1-Quilt_0.23.1.json",
        "{ \"id\": \"1.20.1-Quilt_0.23.1\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑨ 文本线索(整段 JSON 文本,连 id 一起搜):真实的 PCL OptiFine 实例就是这样被认出来的
     *    (id 里带 OptiFine_HD_U_I6),版本号取 HD_U_ 后面那段 -> "I6"(与 Python 完全一致) */
    put(VD("1.20.1-OptiFine_HD_U_I6") "/1.20.1-OptiFine_HD_U_I6.json",
        "{ \"id\": \"1.20.1-OptiFine_HD_U_I6\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑩ 文本线索 liteloader:文本里能认出来,但版本号提取规则只认 fabric-loader:/quilt-loader:
     *    这两种写法,所以版本号是空串(Python 同;想要版本号得靠 Setup.ini 或目录名,见 ⑩b) */
    put(VD("1.12.2-LiteLoader1.12.2") "/1.12.2-LiteLoader1.12.2.json",
        "{ \"id\": \"1.12.2-LiteLoader1.12.2\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑨b⑩b 目录名线索:JSON 里连 id 都没有(手改/整合包版本),只能靠目录名认 */
    put(VD("1.20.1-OptiFine_HD_U_M5") "/1.20.1-OptiFine_HD_U_M5.json",
        "{ \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");
    put(VD("1.12.2-LiteLoader_1.12.2") "/1.12.2-LiteLoader_1.12.2.json",
        "{ \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑪ 整段 JSON 文本线索(PCL 的判据):optifine 关键字 + HD_U_ 后面的版本号 */
    put(VD("1.20.1-T1") "/1.20.1-T1.json",
        "{ \"id\": \"1.20.1-T1\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [], \"comment\": \"内置 optifine HD_U_I6: 兼容包\" }");

    /* ⑫ 文本线索 + PCL 的互斥判据:文本里同时有 minecraftforge 与 net.neoforge 时只认 NeoForge,
     *    版本号从 neoForgeVersion 键取 */
    put(VD("1.21.1-T2") "/1.21.1-T2.json",
        "{ \"id\": \"1.21.1-T2\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [], \"neoForgeVersion\": \"21.1.72\","
        " \"comment\": \"net.minecraftforge:forge:1.21.1-51.0.0 的替代品 net.neoforge\" }");

    /* ⑬ HMCL 补丁认原版:patches 里 id=game 的 version(目录名里故意不带版本号) */
    put(VD("HMCL-Patched") "/HMCL-Patched.json",
        "{ \"id\": \"HMCL-Patched\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [], \"patches\": [ { \"id\": \"game\", \"version\": \"1.12.2\" } ] }");
    put(VD("HMCL-Patched") "/HMCL-Patched.jar", "jar");

    /* ⑭ 顶层有 "time" 时 patches 那条不生效(PCL 的例外),退回目录名里的 1.11.2,而不是 patches 里的 9.9.9 */
    put(VD("1.11.2-HMCLTime") "/1.11.2-HMCLTime.json",
        "{ \"id\": \"1.11.2-HMCLTime\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [], \"time\": \"2018-01-01T00:00:00+00:00\","
        " \"patches\": [ { \"id\": \"game\", \"version\": \"9.9.9\" } ] }");
    put(VD("1.11.2-HMCLTime") "/1.11.2-HMCLTime.jar", "jar");

    /* ⑮ --fml.mcVersion 认原版(目录名里故意不带版本号) */
    put(VD("Custom-Forge-Pack") "/Custom-Forge-Pack.json",
        "{ \"id\": \"Custom-Forge-Pack\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [],"
        " \"arguments\": { \"game\": [ \"--fml.mcVersion\", \"1.20.1\" ] } }");
    put(VD("Custom-Forge-Pack") "/Custom-Forge-Pack.jar", "jar");

    /* ⑯ jar 字段认原版(LiteLoader 常靠它) */
    put(VD("MCP-Pack") "/MCP-Pack.json",
        "{ \"id\": \"MCP-Pack\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [], \"jar\": \"1.12.2\" }");
    put(VD("MCP-Pack") "/MCP-Pack.jar", "jar");

    /* ⑰ 目录名里任意 x.y 兜底(MC_VERSION_RE 认不出来时用 ANY_VERSION_RE;前面必须是"词/点/横线"以外的字符) */
    put(VD("Snapshot 23.5") "/Snapshot 23.5.json",
        "{ \"id\": \"Snapshot 23.5\", \"type\": \"snapshot\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");
    put(VD("Snapshot 23.5") "/Snapshot 23.5.jar", "jar");

    /* ⑱ 缺父版本(继承 1.17.1,但 versions/1.17.1 不在) */
    put(VD("1.17.1-OrphanFabric") "/1.17.1-OrphanFabric.json",
        "{ \"id\": \"1.17.1-OrphanFabric\", \"type\": \"release\", \"inheritsFrom\": \"1.17.1\","
        " \"mainClass\": \"net.fabricmc.loader.impl.launch.knot.KnotClient\","
        " \"libraries\": [ { \"name\": \"net.fabricmc:fabric-loader:0.14.21\" } ] }");

    /* ⑲ 原版实例缺自己的 jar(没有继承、没有加载器)*/
    put(VD("1.19.2-VanillaNoJar") "/1.19.2-VanillaNoJar.json",
        "{ \"id\": \"1.19.2-VanillaNoJar\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑳ 只有 jar、没有 JSON */
    put(VD("1.18.2-NoJson") "/1.18.2-NoJson.jar", "jar");

    /* ㉑ JSON 损坏(读不出来) */
    put(VD("1.20.1-BrokenJson") "/1.20.1-BrokenJson.json", "{ 这不是 JSON ");

    /* ㉒ 目录名与 JSON id 不一致(靠 PCL 的兜底规则认出 other.json) */
    put(VD("1.16.5-MismatchId") "/other.json",
        "{ \"id\": \"other\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [] }");

    /* ㉓ PCL 的版本级缓存:Setup.ini 的 Version* 键(Unknown 不算) */
    put(VD("1.18.2-SetupIni") "/1.18.2-SetupIni.json",
        "{ \"id\": \"1.18.2-SetupIni\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");
    put(VD("1.18.2-SetupIni") "/PCL/Setup.ini",
        "State:Forge\r\nVersionForge:47.2.0\r\nVersionFabric:Unknown\r\n");

    /* ㉔ PCL 自定义图标(LogoCustom + PCL/Logo.png 成对才算) */
    put(VD("1.18.2-PclLogo") "/1.18.2-PclLogo.json",
        "{ \"id\": \"1.18.2-PclLogo\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");
    put(VD("1.18.2-PclLogo") "/1.18.2-PclLogo.jar", "jar");
    put(VD("1.18.2-PclLogo") "/PCL/Setup.ini", "LogoCustom:True\r\nLogo:PCL\\\\Logo.png\r\n");
    put(VD("1.18.2-PclLogo") "/PCL/Logo.png", "png");

    /* ㉕㉖ PCL 的 InstanceForcedJava(启动页选 Java 的第一优先级):一个文件在,一个文件没了 */
    put(VD("1.18.2-PclJava") "/1.18.2-PclJava.json",
        "{ \"id\": \"1.18.2-PclJava\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");
    put(VD("1.18.2-PclJava") "/1.18.2-PclJava.jar", "jar");
    put(VD("1.18.2-PclJavaGone") "/1.18.2-PclJavaGone.json",
        "{ \"id\": \"1.18.2-PclJavaGone\", \"type\": \"release\","
        " \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");
    put(VD("1.18.2-PclJavaGone") "/1.18.2-PclJavaGone.jar", "jar");
    put(FX("java/bin/java.exe"), "fake java");
    {
        char text[512];
        (void)snprintf(text, sizeof(text), "{ \"InstanceForcedJava\": \"%s\" }", FX("java/bin/java.exe"));
        put(VD("1.18.2-PclJava") "/PCL/config.json", text);
        (void)snprintf(text, sizeof(text), "{ \"InstanceForcedJava\": \"%s\" }", FX("java/java-gone.exe"));
        put(VD("1.18.2-PclJavaGone") "/PCL/config.json", text);
    }

    /* ⑨c 目录名线索 neoforge,顺带验证 (?<!neo)forge 的前后视:NeoForge 绝不会被同时认成 Forge */
    put(VD("1.21.1-NeoForge_21.1.72b") "/1.21.1-NeoForge_21.1.72b.json",
        "{ \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\", \"libraries\": [] }");

    /* ⑫b 文本线索里的版本号要去掉 "+build"(Python: version.replace("+build","")) */
    put(VD("1.20.1-T3") "/1.20.1-T3.json",
        "{ \"id\": \"1.20.1-T3\", \"type\": \"release\", \"mainClass\": \"net.minecraft.client.main.Main\","
        " \"libraries\": [], \"neoForgeVersion\": \"21.1.72+build.5\", \"comment\": \"net.neoforge\" }");

    /* ㉗ 一个实例带两个加载器:返回顺序按 PCL 的拼接顺序(Forge 在 OptiFine 前面) */
    put(VD("1.20.1-Forge_47.2.0-OptiFine_HD_U_I6") "/1.20.1-Forge_47.2.0-OptiFine_HD_U_I6.json",
        "{ \"id\": \"1.20.1-Forge_47.2.0-OptiFine_HD_U_I6\", \"type\": \"release\","
        " \"clientVersion\": \"1.20.1\", \"mainClass\": \"cpw.mods.modlauncher.Launcher\","
        " \"libraries\": [ { \"name\": \"net.minecraftforge:forge:1.20.1-47.2.0\" },"
        " { \"name\": \"optifine:OptiFine:1.20.1_HD_U_I6\" } ] }");

    /* ㉘ 空文件夹:PCL 也跳过,不算版本 */
    (void)sxcl_fs_mkdirs(VD("EmptyDir"));

    /* ㉙ PCL 会跳过的目录名 cache(有 jar 没有 JSON 时跳过) */
    put(VD("cache") "/cache.jar", "jar");
}

/* ── 一个实例的断言工具 ── */

static const sxcl_instance *pick(const char *id)
{
    const sxcl_instance *item = sxcl_instance_find(g_list, id);
    if (item == NULL) {
        ++g_fail;
        printf("  [!!] 扫描结果里没有实例 %s\n", id);
    }
    return item;
}

static void check_loader(const sxcl_instance *inst, size_t index, sxcl_instance_loader_kind kind,
                         const char *version, int from_json, const char *what)
{
    char label[256];
    (void)snprintf(label, sizeof(label), "%s(第 %llu 个加载器)", what, (unsigned long long)(index + 1));
    if (inst == NULL) {
        return;
    }
    if (index >= inst->loader_count) {
        ++g_fail;
        printf("  [!!] %s: loader_count=%llu,取不到第 %llu 个\n", label, (unsigned long long)inst->loader_count,
               (unsigned long long)(index + 1));
        return;
    }
    check(inst->loaders[index].kind == kind, label);
    check_str(inst->loaders[index].version, version, label);
    check_int(inst->loaders[index].from_json, from_json, label);
}

/* ══════════════════════ 1) 实例扫描与识别 ══════════════════════ */

static void test_instance_scan(void)
{
    sxcl_instance_list list;
    char err[SXCL_INSTANCE_ERROR_MAX];
    err[0] = '\0';
    int rc = sxcl_instance_scan(FIXTURE_ROOT, NULL, &list, err, sizeof(err));
    check_int(rc, SXCL_INSTANCE_OK, "扫一次给全部:返回成功");
    check_int((long long)list.count, 31, "实例条数(33 个版本目录里 cache/EmptyDir 不算)");
    check_int((long long)list.dropped, 0, "没有被丢掉的实例");
    check_int(list.truncated, 0, "没有截断");
    check_int(list.game_dir_exists, 1, "游戏目录存在");
    check_int((long long)list.launchable_count, 26, "可启动的实例数");
    check_int((long long)list.problem_count, 5, "不能启动的实例数");
    check_int((long long)list.vanilla_count, 13, "纯原版实例数");
    check_int((long long)list.with_loader_count, 18, "带加载器的实例数");
    g_list = &list;

    /* ① 纯原版 */
    const sxcl_instance *inst = pick("1.20.1");
    if (inst != NULL) {
        check_str(inst->base_version, "1.20.1", "①原版:原版版本号");
        check_int(inst->base_reliable, 0, "①原版:JSON 里没有继承关系,版本号靠目录名兜底(不可信,Python 同)");
        check_int((long long)inst->loader_count, 0, "①原版:没有加载器");
        check_int(inst->has_jar, 1, "①原版:有 jar");
        check_int(inst->has_json, 1, "①原版:有 JSON");
        check_int(inst->launchable, 1, "①原版:可启动");
        check_int(inst->problem_code, SXCL_INSTANCE_PROBLEM_NONE, "①原版:没有问题");
        check_str(inst->problem, "", "①原版:没有问题文案");
        check_str(inst->version_type, "release", "①原版:版本类型");
        check_str(inst->summary, "原版", "①原版:一句话摘要");
        check_str(inst->describe, "1.20.1（1.20.1，原版，可启动）", "①原版:人话描述");
        check_contains(inst->json_path, "1.20.1.json", "①原版:JSON 路径");
        check_str(inst->launcher, "", "①原版:不是 PCL/HMCL 装的");
    }

    /* ② PCL 拍平的 Forge */
    inst = pick("1.20.1-Forge_47.2.0");
    if (inst != NULL) {
        check_str(inst->base_version, "1.20.1", "②PCL Forge:clientVersion 认原版");
        check_int(inst->base_reliable, 1, "②PCL Forge:版本号可信");
        check_int((long long)inst->loader_count, 1, "②PCL Forge:一个加载器");
        check_loader(inst, 0, SXCL_INSTANCE_FORGE, "47.2.0", 1, "②PCL Forge:坐标认加载器 + 去掉原版前缀");
        check_int(inst->launchable, 1, "②PCL Forge:没有 jar 也可启动(靠继承/加载器)");
        check_str(inst->summary, "Forge 47.2.0", "②PCL Forge:摘要");
        check_str(inst->describe, "1.20.1-Forge_47.2.0（1.20.1，Forge 47.2.0，可启动）", "②PCL Forge:人话描述");
    }

    /* ③ NeoForge(neoforge 规则必须排在 forge 前面) */
    inst = pick("1.21.1-NeoForge_21.1.72");
    if (inst != NULL) {
        check_str(inst->base_version, "1.21.1", "③NeoForge:clientVersion");
        check_int((long long)inst->loader_count, 1, "③NeoForge:只认出一个加载器(没被认成 Forge)");
        check_loader(inst, 0, SXCL_INSTANCE_NEOFORGE, "21.1.72", 1, "③NeoForge:坐标认加载器");
        check_str(inst->summary, "NeoForge 21.1.72", "③NeoForge:摘要");
    }

    /* ④ Fabric(inheritsFrom + fabric-loader 坐标) */
    inst = pick("fabric-loader-0.15.11-1.20.1");
    if (inst != NULL) {
        check_str(inst->base_version, "1.20.1", "④Fabric:inheritsFrom 认原版");
        check_int(inst->base_reliable, 1, "④Fabric:版本号可信");
        check_int((long long)inst->loader_count, 1, "④Fabric:一个加载器");
        check_loader(inst, 0, SXCL_INSTANCE_FABRIC, "0.15.11", 1, "④Fabric:坐标认加载器");
        check_str(inst->missing_parent, "", "④Fabric:前置版本在,不算缺");
        check_int(inst->launchable, 1, "④Fabric:可启动");
    }

    /* ⑤ mainClass 线索 + Quilt 要出现在结果里(两处与 Python 不同的第 1 处) */
    inst = pick("1.20.1-LoaderTest");
    if (inst != NULL) {
        check_int((long long)inst->loader_count, 1, "⑤Quilt:一个加载器");
        check_loader(inst, 0, SXCL_INSTANCE_QUILT, "", 1, "⑤Quilt:mainClass 只给种类不给版本号");
        check_str(inst->summary, "Quilt", "⑤Quilt:摘要");
    }

    /* ⑥⑦⑧⑨⑩ 目录名线索(JSON 里没有加载器痕迹 -> from_json=0) */
    inst = pick("1.16.5-Forge_36.2.34");
    if (inst != NULL) {
        check_str(inst->base_version, "1.16.5", "⑥目录名 Forge:版本号靠目录名");
        check_int(inst->base_reliable, 0, "⑥目录名 Forge:版本号不可信");
        check_loader(inst, 0, SXCL_INSTANCE_FORGE, "36.2.34", 0, "⑥目录名 Forge");
    }
    inst = pick("1.19.4-Fabric_0.14.21");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_FABRIC, "0.14.21", 0, "⑦目录名 Fabric");
    }
    inst = pick("1.20.1-Quilt_0.23.1");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_QUILT, "0.23.1", 0, "⑧目录名 Quilt");
    }
    inst = pick("1.20.1-OptiFine_HD_U_I6");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_OPTIFINE, "I6", 1, "⑨文本线索 OptiFine(id 里带 OptiFine,版本取 HD_U_ 后面那段)");
    }
    inst = pick("1.12.2-LiteLoader1.12.2");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_LITELOADER, "", 1, "⑩文本线索 LiteLoader(版本号提取规则不认它)");
        check_str(sxcl_instance_kind_id(inst->loaders[0].kind), "liteloader", "⑩LiteLoader 的字符串 id");
        check_int((long long)sxcl_instance_kind_to_loader(inst->loaders[0].kind), SXCL_LOADER_VANILLA,
                  "⑩LiteLoader 转 loader.h 的种类(目前没有,退回 vanilla)");
    }
    inst = pick("1.20.1-OptiFine_HD_U_M5");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_OPTIFINE, "HD_U_M5", 0, "⑨b目录名 OptiFine(JSON 里没有线索)");
    }
    inst = pick("1.12.2-LiteLoader_1.12.2");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_LITELOADER, "1.12.2", 0, "⑩b目录名 LiteLoader");
    }
    inst = pick("1.21.1-NeoForge_21.1.72b");
    if (inst != NULL) {
        check_int((long long)inst->loader_count, 1, "⑨c目录名 NeoForge:(?<!neo) 前后视,不会同时认成 Forge");
        check_loader(inst, 0, SXCL_INSTANCE_NEOFORGE, "21.1.72b", 0, "⑨c目录名 NeoForge");
    }

    /* ⑪⑫ 整段 JSON 文本线索 */
    inst = pick("1.20.1-T1");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_OPTIFINE, "I6", 1, "⑪文本线索 OptiFine(HD_U_ 后面取版本号)");
    }
    inst = pick("1.21.1-T2");
    if (inst != NULL) {
        check_int((long long)inst->loader_count, 1, "⑫文本线索:有 net.neoforge 时不再认 Forge");
        check_loader(inst, 0, SXCL_INSTANCE_NEOFORGE, "21.1.72", 1, "⑫文本线索 NeoForge(neoForgeVersion 键)");
    }
    inst = pick("1.20.1-T3");
    if (inst != NULL) {
        check_loader(inst, 0, SXCL_INSTANCE_NEOFORGE, "21.1.72.5", 1, "⑫b文本线索的版本号去掉 +build");
    }

    /* ⑬⑭ HMCL 补丁 */
    inst = pick("HMCL-Patched");
    if (inst != NULL) {
        check_str(inst->base_version, "1.12.2", "⑬HMCL:patches 里 id=game 的 version 认原版");
        check_int(inst->base_reliable, 1, "⑬HMCL:版本号可信");
        check_str(inst->launcher, "hmcl", "⑬HMCL:识别出是 HMCL 装的");
    }
    inst = pick("1.11.2-HMCLTime");
    if (inst != NULL) {
        check_str(inst->base_version, "1.11.2", "⑭HMCL:顶层有 time 时 patches 不生效");
        check_int(inst->base_reliable, 0, "⑭HMCL:退回目录名,不可信");
    }

    /* ⑮⑯ --fml.mcVersion 与 jar 字段 */
    inst = pick("Custom-Forge-Pack");
    if (inst != NULL) {
        check_str(inst->base_version, "1.20.1", "⑮--fml.mcVersion 认原版");
        check_int(inst->base_reliable, 1, "⑮--fml.mcVersion:版本号可信");
        check_int((long long)inst->loader_count, 0, "⑮目录名里的 Forge-Pack 不触发目录名规则");
    }
    inst = pick("MCP-Pack");
    if (inst != NULL) {
        check_str(inst->base_version, "1.12.2", "⑯jar 字段认原版");
        check_int(inst->base_reliable, 1, "⑯jar 字段:版本号可信");
    }

    /* ⑰ 任意 x.y 兜底 + 版本类型 */
    inst = pick("Snapshot 23.5");
    if (inst != NULL) {
        check_str(inst->base_version, "23.5", "⑰ANY_VERSION 兜底认原版");
        check_int(inst->base_reliable, 0, "⑰兜底:版本号不可信");
        check_str(inst->version_type, "snapshot", "⑰版本类型");
    }

    /* ⑱⑲⑳㉑㉒ 五种不能启动的原因 */
    inst = pick("1.17.1-OrphanFabric");
    if (inst != NULL) {
        check_str(inst->missing_parent, "1.17.1", "⑱缺父版本:记下缺哪个");
        check_int(inst->problem_code, SXCL_INSTANCE_PROBLEM_MISSING_PARENT, "⑱缺父版本:问题码");
        check_str(inst->problem, "需要安装 1.17.1 作为前置版本", "⑱缺父版本:人话");
        check_int(inst->launchable, 0, "⑱缺父版本:不可启动");
        check_str(sxcl_instance_problem_id(inst->problem_code), "missing-parent", "⑱缺父版本:问题码字符串");
        check_str(inst->describe, "1.17.1-OrphanFabric（1.17.1，Fabric 0.14.21，需要安装 1.17.1 作为前置版本）",
                  "⑱缺父版本:人话描述");
    }
    inst = pick("1.19.2-VanillaNoJar");
    if (inst != NULL) {
        check_int(inst->problem_code, SXCL_INSTANCE_PROBLEM_MISSING_JAR, "⑲缺 jar:问题码");
        check_contains(inst->problem, "缺少客户端 jar", "⑲缺 jar:人话");
        check_int(inst->launchable, 0, "⑲缺 jar:不可启动");
    }
    inst = pick("1.18.2-NoJson");
    if (inst != NULL) {
        check_int(inst->has_json, 0, "⑳缺 JSON:has_json=0");
        check_int(inst->has_jar, 1, "⑳缺 JSON:有 jar");
        check_int(inst->problem_code, SXCL_INSTANCE_PROBLEM_NO_JSON, "⑳缺 JSON:问题码");
        check_str(inst->problem, "缺少版本 JSON", "⑳缺 JSON:人话");
        check_str(inst->describe, "1.18.2-NoJson（1.18.2，原版，缺少版本 JSON）", "⑳缺 JSON:人话描述");
    }
    inst = pick("1.20.1-BrokenJson");
    if (inst != NULL) {
        check_int(inst->broken, 1, "㉑坏 JSON:broken=1");
        check_int(inst->problem_code, SXCL_INSTANCE_PROBLEM_BAD_JSON, "㉑坏 JSON:问题码");
        check_str(inst->problem, "版本 JSON 损坏或缺少 mainClass", "㉑坏 JSON:人话");
    }
    inst = pick("1.16.5-MismatchId");
    if (inst != NULL) {
        check_int(inst->json_id_mismatch, 1, "㉒目录名/JSON id 不一致:标记");
        check_str(inst->json_id, "other", "㉒目录名/JSON id 不一致:JSON 里的 id");
        check_contains(inst->json_path, "other.json", "㉒目录名/JSON id 不一致:实际用的 JSON 路径");
        check_int(inst->problem_code, SXCL_INSTANCE_PROBLEM_ID_MISMATCH, "㉒目录名/JSON id 不一致:问题码(C 特有)");
        check_contains(inst->problem, "id 不一致", "㉒目录名/JSON id 不一致:人话");
        check_int(inst->launchable, 0, "㉒目录名/JSON id 不一致:不可启动");
    }

    /* ㉓ PCL 的 Setup.ini */
    inst = pick("1.18.2-SetupIni");
    if (inst != NULL) {
        check_int((long long)inst->loader_count, 1, "㉓Setup.ini:Unknown 不算,所以只有一个加载器");
        check_loader(inst, 0, SXCL_INSTANCE_FORGE, "47.2.0", 0, "㉓Setup.ini 的 VersionForge(不算 JSON 线索)");
        check_str(inst->pcl_state, "Forge", "㉓Setup.ini 的 State");
        check_int(inst->pcl_present, 1, "㉓有 PCL 的痕迹");
        check_str(inst->launcher, "pcl", "㉓识别出是 PCL 装的");
        check_str(inst->summary, "Forge 47.2.0", "㉓摘要");
    }

    /* ㉔ PCL 自定义图标 */
    inst = pick("1.18.2-PclLogo");
    if (inst != NULL) {
        check_int(inst->has_custom_logo, 1, "㉔LogoCustom + Logo.png 成对才算");
        check_contains(inst->custom_logo, "PCL/Logo.png", "㉔自定义图标路径");
    }

    /* ㉕㉖ InstanceForcedJava */
    inst = pick("1.18.2-PclJava");
    if (inst != NULL) {
        check_int(inst->forced_java_state, SXCL_INSTANCE_JAVA_OK, "㉕PCL 钉的 Java 可用");
        check_str(inst->forced_java, FX("java/bin/java.exe"), "㉕PCL 钉的 Java 路径");
    }
    inst = pick("1.18.2-PclJavaGone");
    if (inst != NULL) {
        check_int(inst->forced_java_state, SXCL_INSTANCE_JAVA_MISSING, "㉖PCL 钉的 Java 不在了");
        check_str(inst->forced_java, "", "㉖不在了就不返回路径");
    }

    /* ㉗ 两个加载器的返回顺序 */
    inst = pick("1.20.1-Forge_47.2.0-OptiFine_HD_U_I6");
    if (inst != NULL) {
        check_int((long long)inst->loader_count, 2, "㉗两个加载器");
        check_loader(inst, 0, SXCL_INSTANCE_FORGE, "47.2.0", 1, "㉗Forge 在前");
        check_loader(inst, 1, SXCL_INSTANCE_OPTIFINE, "1.20.1_HD_U_I6", 1, "㉗OptiFine 在后");
        check_str(inst->summary, "Forge 47.2.0 + OptiFine 1.20.1_HD_U_I6", "㉗摘要按 PCL 的拼接顺序");
    }

    /* ㉘㉙ 跳过的目录 / 被丢掉的目录 */
    check(sxcl_instance_find(&list, "EmptyDir") == NULL, "㉘空文件夹不算版本");
    check(sxcl_instance_find(&list, "cache") == NULL, "㉙PCL 会跳过的 cache 目录");

    /* 分组计数(installed_for_base 等价物) */
    check_int((long long)sxcl_instance_count_for_base(&list, "1.20.1"), 12, "按原版 1.20.1 分组计数");
    check_int((long long)sxcl_instance_count_for_base(&list, "1.21.1"), 3, "按原版 1.21.1 分组计数");
    check_int((long long)sxcl_instance_count_for_base(&list, "9.9.9"), 0, "不存在的原版版本计数为 0");

    /* 喂 loader.h 的入参结构 */
    {
        sxcl_loader_installed slots[4];
        (void)memset(slots, 0, sizeof(slots));
        inst = pick("1.20.1-Forge_47.2.0");
        if (inst != NULL) {
            check_int((long long)sxcl_instance_to_loader_installed(inst, slots, 4), 1, "转 loader_installed:1 条");
            check_str(slots[0].loader_id, "forge", "转 loader_installed:loader_id");
            check_str(slots[0].loader_version, "47.2.0", "转 loader_installed:loader_version");
            check_str(slots[0].base_version, "1.20.1", "转 loader_installed:base_version");
        }
        inst = pick("1.20.1");
        if (inst != NULL) {
            check_int((long long)sxcl_instance_to_loader_installed(inst, slots, 4), 1, "转 loader_installed:原版也算一条");
            check_str(slots[0].loader_id, "vanilla", "转 loader_installed:原版 loader_id");
        }
        inst = pick("1.12.2-LiteLoader1.12.2");
        if (inst != NULL) {
            check_int((long long)sxcl_instance_to_loader_installed(inst, slots, 4), 0,
                      "转 loader_installed:LiteLoader 在 loader.h 里没有,跳过");
        }
    }

    sxcl_instance_list_free(&list);
    g_list = NULL;

    /* 上限截断:超出的记 dropped,不静默漏 */
    {
        sxcl_instance_scan_opts opts;
        (void)memset(&opts, 0, sizeof(opts));
        opts.max_instances = 5;
        sxcl_instance_list limited;
        rc = sxcl_instance_scan(FIXTURE_ROOT, &opts, &limited, err, sizeof(err));
        check_int(rc, SXCL_INSTANCE_OK, "上限截断:返回成功");
        check_int((long long)limited.count, 5, "上限截断:只给 5 条");
        check_int((long long)limited.dropped, 26, "上限截断:剩下的记在 dropped 里");
        check_int(limited.truncated, 1, "上限截断:truncated=1");
        check_int((long long)(limited.launchable_count + limited.problem_count), 5,
                  "上限截断:计数只统计返回的那几条");
        sxcl_instance_list_free(&limited);
    }

    /* 单条扫描 / 找不存在的实例 */
    {
        sxcl_instance one;
        check_int(sxcl_instance_scan_one(FIXTURE_ROOT, "1.20.1", &one, err, sizeof(err)), SXCL_INSTANCE_OK,
                  "单条扫描:成功");
        check_str(one.id, "1.20.1", "单条扫描:id");
        check_int(sxcl_instance_scan_one(FIXTURE_ROOT, "EmptyDir", &one, err, sizeof(err)),
                  SXCL_INSTANCE_ERR_IO, "单条扫描:空文件夹报 IO 错");
        check(err[0] != '\0', "单条扫描:空文件夹给了人话错误");
        check_int(sxcl_instance_scan_one(FIXTURE_ROOT, "不存在的实例", &one, err, sizeof(err)),
                  SXCL_INSTANCE_ERR_IO, "单条扫描:不存在的实例报 IO 错");
    }

    /* 版本 JSON 读取(主页"版本简要信息") */
    {
        char path[SXCL_INSTANCE_PATH_MAX];
        sxcl_json *doc = sxcl_instance_read_json(FIXTURE_ROOT, "1.20.1", path, sizeof(path), err, sizeof(err));
        check(doc != NULL, "读版本 JSON:成功");
        if (doc != NULL) {
            check_str(sxcl_json_get_string(sxcl_json_root(doc), "type", ""), "release", "读版本 JSON:type");
            check_contains(path, "1.20.1.json", "读版本 JSON:路径");
            sxcl_json_free(doc);
        }
        doc = sxcl_instance_read_json(FIXTURE_ROOT, "1.16.5-MismatchId", path, sizeof(path), err, sizeof(err));
        check(doc != NULL, "读版本 JSON:目录名不一致时走 PCL 的兜底");
        if (doc != NULL) {
            check_contains(path, "other.json", "读版本 JSON:兜底用的文件名");
            sxcl_json_free(doc);
        }
        doc = sxcl_instance_read_json(FIXTURE_ROOT, "1.18.2-NoJson", path, sizeof(path), err, sizeof(err));
        check(doc == NULL, "读版本 JSON:没有 JSON 时返回 NULL");
        check(err[0] != '\0', "读版本 JSON:没有 JSON 时给人话错误");
    }

    /* 游戏目录不存在 = 空态,不是错误 */
    {
        sxcl_instance_list empty;
        rc = sxcl_instance_scan(FX("这个目录不存在"), NULL, &empty, err, sizeof(err));
        check_int(rc, SXCL_INSTANCE_OK, "游戏目录不存在:仍返回成功(界面走空态)");
        check_int((long long)empty.count, 0, "游戏目录不存在:0 条");
        check_int(empty.game_dir_exists, 0, "游戏目录不存在:game_dir_exists=0");
        check(err[0] != '\0', "游戏目录不存在:给了人话说明");
        sxcl_instance_list_free(&empty);
    }

    /* 种类 / 问题码的字符串映射 */
    check_int(sxcl_instance_kind_from_id("NeoForge"), SXCL_INSTANCE_NEOFORGE, "kind_from_id 大小写不敏感");
    check_int(sxcl_instance_kind_from_id("不认识"), SXCL_INSTANCE_VANILLA, "kind_from_id 认不出来 = vanilla");
    check_str(sxcl_instance_kind_id(SXCL_INSTANCE_VANILLA), "vanilla", "kind_id(vanilla)");
    check_str(sxcl_instance_kind_name(SXCL_INSTANCE_OPTIFINE), "OptiFine", "kind_name(OptiFine)");
    check_str(sxcl_instance_problem_id(SXCL_INSTANCE_PROBLEM_NONE), "ok", "problem_id(ok)");
    check_str(sxcl_instance_problem_id(SXCL_INSTANCE_PROBLEM_MISSING_JAR), "missing-jar", "problem_id(missing-jar)");
}

/* ══════════════════════ 2) PCL 兼容读取 ══════════════════════ */

static void test_pcl_compat(void)
{
    sxcl_instance_pcl_setup setup;
    char err[SXCL_INSTANCE_ERROR_MAX];
    err[0] = '\0';

    int rc = sxcl_instance_read_pcl_setup(FIXTURE_ROOT, "1.18.2-SetupIni", &setup, err, sizeof(err));
    check_int(rc, SXCL_INSTANCE_OK, "读 Setup.ini:成功");
    check_int(setup.exists, 1, "读 Setup.ini:exists");
    check_int((long long)setup.key_count, 3, "读 Setup.ini:解析出 3 个键");
    check_str(sxcl_instance_pcl_get(&setup, "State"), "Forge", "读 Setup.ini:State");
    check_str(sxcl_instance_pcl_get(&setup, "VersionForge"), "47.2.0", "读 Setup.ini:VersionForge");
    check_str(sxcl_instance_pcl_get(&setup, "VersionFabric"), "Unknown", "读 Setup.ini:VersionFabric");
    check_str(sxcl_instance_pcl_get(&setup, "没有这个键"), "", "读 Setup.ini:没有的键给空串");
    check_str(sxcl_instance_pcl_get(NULL, "State"), "", "读 Setup.ini:setup 为空也给空串");

    rc = sxcl_instance_read_pcl_setup(FIXTURE_ROOT, "1.20.1", &setup, err, sizeof(err));
    check_int(rc, SXCL_INSTANCE_ERR_IO, "读 Setup.ini:没有这个文件时报 IO 错");
    check(err[0] != '\0', "读 Setup.ini:给了人话错误");

    check_int(sxcl_instance_pcl_present(FIXTURE_ROOT, "1.18.2-SetupIni"), 1, "PCL 痕迹:有 Setup.ini");
    check_int(sxcl_instance_pcl_present(FIXTURE_ROOT, "1.18.2-PclLogo"), 1, "PCL 痕迹:有 PCL 目录");
    check_int(sxcl_instance_pcl_present(FIXTURE_ROOT, "1.20.1"), 0, "PCL 痕迹:纯净实例没有");

    char java[SXCL_INSTANCE_PATH_MAX];
    rc = sxcl_instance_read_forced_java(FIXTURE_ROOT, "1.18.2-PclJava", java, sizeof(java), err, sizeof(err));
    check_int(rc, SXCL_INSTANCE_JAVA_OK, "InstanceForcedJava:读到了");
    check_str(java, FX("java/bin/java.exe"), "InstanceForcedJava:路径");
    rc = sxcl_instance_read_forced_java(FIXTURE_ROOT, "1.18.2-PclJavaGone", java, sizeof(java), err, sizeof(err));
    check_int(rc, SXCL_INSTANCE_JAVA_MISSING, "InstanceForcedJava:文件不在了");
    check_str(java, "", "InstanceForcedJava:文件不在时返回空串");
    rc = sxcl_instance_read_forced_java(FIXTURE_ROOT, "1.20.1", java, sizeof(java), err, sizeof(err));
    check_int(rc, SXCL_INSTANCE_JAVA_NOT_SET, "InstanceForcedJava:没配");
}

/* ══════════════════════ 3) 游戏目录探测 ══════════════════════ */

static void build_path_fixtures(void)
{
    (void)sxcl_dir_remove_tree(FX("folders"));
    (void)sxcl_fs_mkdirs(FX("folders/empty"));
    put(FX("folders/three/versions/a/a.json"), "{}");
    put(FX("folders/three/versions/b/b.json"), "{}");
    put(FX("folders/three/versions/c/c.json"), "{}");
    put(FX("folders/three/launcher_profiles.json"), "{}");
    put(FX("folders/empty/readme.txt"), "x");
}

static void test_paths(void)
{
    build_path_fixtures();

    sxcl_game_folder empty;
    sxcl_game_folder three;
    check_int(sxcl_paths_inspect(FX("folders/empty"), "空目录", &empty), SXCL_PATHS_OK, "inspect:返回成功");
    check_int(sxcl_paths_inspect(FX("folders/three"), "三个版本", &three), SXCL_PATHS_OK, "inspect:返回成功");
    check_int(empty.exists, 1, "inspect:空目录存在");
    check_int(empty.versions, 0, "inspect:空目录 0 个版本");
    check_int(empty.has_launcher_profiles, 0, "inspect:空目录没有 launcher_profiles.json");
    check_int(three.exists, 1, "inspect:三个版本的目录存在");
    check_int(three.versions, 3, "inspect:数到 3 个版本");
    check_int(three.has_launcher_profiles, 1, "inspect:认到 launcher_profiles.json");
    check_contains(three.describe, "3 个版本", "inspect:人话描述里带版本数");
    check_contains(three.describe, "三个版本", "inspect:人话描述里带来源标签");
    check(sxcl_paths_describe(&three, three.describe, sizeof(three.describe)) == SXCL_PATHS_OK,
          "describe:重新生成成功");
    check_int(three.score, 32, "inspect:分数 = 3*10 + launcher_profiles 的 2");
    check_int(empty.score, 0, "inspect:空目录分数 0");

    sxcl_game_folder missing;
    check_int(sxcl_paths_inspect(FX("folders/没有这个目录"), "不存在", &missing), SXCL_PATHS_OK,
              "inspect:目录不存在也不算错误");
    check_int(missing.exists, 0, "inspect:目录不存在 exists=0");
    check_int(missing.score, -1, "inspect:目录不存在分数 -1");
    check_contains(missing.describe, "目录不存在", "inspect:人话描述说明目录不在");

    /* 择优:三个版本的目录必须排在空目录前面(Python 的 score 排序) */
    sxcl_game_folders folders;
    (void)memset(&folders, 0, sizeof(folders));
    check_int(sxcl_paths_push(&folders, &empty), SXCL_PATHS_OK, "push:空目录");
    check_int(sxcl_paths_push(&folders, &three), SXCL_PATHS_OK, "push:三个版本");
    sxcl_paths_sort(&folders);
    check_int((long long)folders.count, 2, "sort:两条候选");
    const sxcl_game_folder *best = sxcl_paths_best(&folders);
    check(best != NULL, "best:有结果");
    if (best != NULL) {
        check_contains(best->path, "three", "best:挑了有三个版本的那个");
        check_int(best->versions, 3, "best:版本数");
    }
    sxcl_game_folders none;
    (void)memset(&none, 0, sizeof(none));
    check(sxcl_paths_best(&none) == NULL, "best:没有候选时返回 NULL");

    check_int(sxcl_paths_count_versions(FX("folders/three")), 3, "count_versions:3 个");
    check_int(sxcl_paths_count_versions(FX("folders/empty")), 0, "count_versions:0 个");
    check_int(sxcl_paths_count_versions(FX("folders/没有这个目录")), 0, "count_versions:没有的目录给 0");
    check_int(sxcl_paths_is_game_dir(FX("folders/three")), 1, "is_game_dir:是");
    check_int(sxcl_paths_is_game_dir(FX("folders/empty")), 0, "is_game_dir:不是");

    /* 平台候选 + 当前配置:配置里的目录必须出现在结果里(哪怕还有别的候选) */
    char err[SXCL_PATHS_ERROR_MAX];
    err[0] = '\0';
    check_int(sxcl_paths_detect(&folders, FX("folders/three"), err, sizeof(err)), SXCL_PATHS_OK,
              "detect:返回成功");
    int found = 0;
    for (size_t i = 0; i < folders.count; ++i) {
        if (strstr(folders.items[i].path, "folders/three") != NULL &&
            strstr(folders.items[i].path, "folders/three") != NULL) {
            found = 1;
            check_int(folders.items[i].versions, 3, "detect:当前配置目录的版本数");
            check_contains(folders.items[i].describe, "3 个版本", "detect:当前配置目录的人话描述");
            check_str(folders.items[i].label, "当前配置", "detect:当前配置目录的来源标签");
        }
    }
    check(found == 1, "detect:当前配置的目录出现在候选里");

    /* 不存在的候选不进列表(探测只列真实存在的目录) */
    check_int(sxcl_paths_detect(&folders, FX("folders/没有这个目录"), err, sizeof(err)), SXCL_PATHS_OK,
              "detect:配置目录不存在时也返回成功");
    for (size_t i = 0; i < folders.count; ++i) {
        check(strstr(folders.items[i].path, "没有这个目录") == NULL, "detect:不存在的候选不进列表");
        check_int(folders.items[i].exists, 1, "detect:进列表的都是真实存在的目录");
    }

    /* resolve_game_dir:配置优先;没配置就择优;再没有就用平台默认 */
    char resolved[SXCL_PATHS_PATH_MAX];
    check_int(sxcl_paths_resolve_game_dir(FX("folders/three"), resolved, sizeof(resolved), err, sizeof(err)),
              SXCL_PATHS_OK, "resolve:配置优先");
    check_str(resolved, FX("folders/three"), "resolve:配置里写的就是答案");
    check_int(sxcl_paths_resolve_game_dir(NULL, resolved, sizeof(resolved), err, sizeof(err)), SXCL_PATHS_OK,
              "resolve:没配置时也能给出一个");
    check(resolved[0] != '\0', "resolve:结果是非空路径");

    /* 平台默认游戏目录(Android 明确说没有默认) */
    char def[SXCL_PATHS_PATH_MAX];
    const int def_rc = sxcl_paths_default_game_dir(def, sizeof(def), err, sizeof(err));
    check(def_rc == SXCL_PATHS_OK || def_rc == SXCL_PATHS_ERR_UNSUPPORTED,
          "默认游戏目录:要么给出路径,要么明确说不支持");
    if (def_rc == SXCL_PATHS_OK) {
        check_contains(def, "minecraft", "默认游戏目录:路径里有 minecraft");
    }
    check(err[0] != '\0' || def_rc == SXCL_PATHS_OK, "默认游戏目录:失败时有人话错误");
}

/* ══════════════════════ 4) HTTP 取一整段文本 ══════════════════════ */

typedef struct fake_body {
    size_t pos;
} fake_body;

typedef struct fake_http {
    const char *body;
    size_t body_len;
    int status;
    int64_t content_length; /* -1 = 未知 */
    size_t chunk;           /* 每次 read 最多给多少字节 */
    int fail_after;         /* >0 = 读这么多字节后返回 -1(模拟传输中断) */
} fake_http;

static int fake_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                        sxcl_http_body **body)
{
    fake_http *fake = (fake_http *)ctx;
    if (fake == NULL || resp == NULL || body == NULL || req == NULL || req->url == NULL) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    (void)memset(resp, 0, sizeof(*resp));
    resp->status = fake->status;
    resp->content_length = fake->content_length;
    resp->total_length = fake->content_length;
    resp->range_start = -1;
    resp->range_end = -1;
    fake_body *state = (fake_body *)malloc(sizeof(fake_body));
    if (state == NULL) {
        return SXCL_NET_ERR_IO;
    }
    state->pos = 0;
    *body = (sxcl_http_body *)state;
    return SXCL_NET_OK;
}

static int64_t fake_read(void *ctx, sxcl_http_body *body, void *buf, size_t len)
{
    fake_http *fake = (fake_http *)ctx;
    fake_body *state = (fake_body *)body;
    if (fake == NULL || state == NULL || buf == NULL) {
        return -1;
    }
    if (fake->fail_after > 0 && state->pos >= (size_t)fake->fail_after) {
        return -1;
    }
    if (state->pos >= fake->body_len) {
        return 0;
    }
    size_t want = fake->body_len - state->pos;
    if (want > len) {
        want = len;
    }
    if (fake->chunk > 0 && want > fake->chunk) {
        want = fake->chunk;
    }
    if (fake->fail_after > 0 && state->pos + want > (size_t)fake->fail_after) {
        want = (size_t)fake->fail_after - state->pos;
    }
    (void)memcpy(buf, fake->body + state->pos, want);
    state->pos += want;
    return (int64_t)want;
}

static void fake_close(void *ctx, sxcl_http_body *body)
{
    (void)ctx;
    free(body);
}

static void fake_cancel(void *ctx)
{
    (void)ctx;
}

static sxcl_transport make_fake_transport(fake_http *fake)
{
    sxcl_transport tr;
    (void)memset(&tr, 0, sizeof(tr));
    tr.ctx = fake;
    tr.request = fake_request;
    tr.read = fake_read;
    tr.close_body = fake_close;
    tr.cancel_all = fake_cancel;
    tr.destroy = NULL;
    return tr;
}

static void test_http(void)
{
    char err[SXCL_HTTP_ERROR_MAX];
    err[0] = '\0';
    char *out = NULL;
    size_t out_len = 0;

    /* 正常文本 */
    {
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = "hello minecraft";
        fake.body_len = strlen(fake.body);
        fake.status = 200;
        fake.content_length = (int64_t)fake.body_len;
        fake.chunk = 3; /* 强制分块读,顺便走一遍缓冲增长 */
        sxcl_transport tr = make_fake_transport(&fake);
        int status = 0;
        sxcl_http_opts opts;
        (void)memset(&opts, 0, sizeof(opts));
        opts.status_out = &status;
        const int rc = sxcl_http_get_text_ex(&tr, "https://example.invalid/version.json", NULL, &opts, &out,
                                             &out_len, err, sizeof(err));
        check_int(rc, SXCL_HTTP_OK, "HTTP:正常取回");
        check_int((long long)out_len, 15, "HTTP:字节数");
        check_str(out, "hello minecraft", "HTTP:内容");
        check_int(status, 200, "HTTP:状态码出参");
        free(out);
        out = NULL;
    }

    /* 大响应体(走缓冲增长) */
    {
        static char big[20000];
        (void)memset(big, 'x', sizeof(big));
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = big;
        fake.body_len = sizeof(big);
        fake.status = 200;
        fake.content_length = -1; /* 不知道长度,只能边读边看 */
        fake.chunk = 777;
        sxcl_transport tr = make_fake_transport(&fake);
        const int rc = sxcl_http_get_text(&tr, "https://example.invalid/big", NULL, &out, &out_len, err,
                                          sizeof(err));
        check_int(rc, SXCL_HTTP_OK, "HTTP:大响应体也能读全");
        check_int((long long)out_len, 20000, "HTTP:大响应体字节数");
        free(out);
        out = NULL;
    }

    /* 404 -> 人话 */
    {
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = "not found";
        fake.body_len = 8;
        fake.status = 404;
        fake.content_length = 9;
        sxcl_transport tr = make_fake_transport(&fake);
        int status = 0;
        sxcl_http_opts opts;
        (void)memset(&opts, 0, sizeof(opts));
        opts.status_out = &status;
        const int rc = sxcl_http_get_text_ex(&tr, "https://example.invalid/missing.json", NULL, &opts, &out,
                                             &out_len, err, sizeof(err));
        check_int(rc, SXCL_HTTP_ERR_STATUS, "HTTP:404 报状态错");
        check_contains(err, "资源不存在", "HTTP:404 翻成人话");
        check_contains(err, "404", "HTTP:错误里带状态码");
        check_int(status, 404, "HTTP:404 也把状态码给出去");
        check(out == NULL && out_len == 0, "HTTP:失败时不返回正文");
        check_contains(sxcl_http_status_text(404), "资源不存在", "HTTP:status_text(404)");
        check(sxcl_http_status_text(418) == NULL, "HTTP:没见过的状态码返回 NULL");
        check_contains(sxcl_http_status_text(429), "频繁", "HTTP:429 翻成人话");
    }

    /* 500 -> 人话 */
    {
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = "boom";
        fake.body_len = 4;
        fake.status = 503;
        fake.content_length = 4;
        sxcl_transport tr = make_fake_transport(&fake);
        const int rc = sxcl_http_get_text(&tr, "https://example.invalid/x", NULL, &out, &out_len, err,
                                          sizeof(err));
        check_int(rc, SXCL_HTTP_ERR_STATUS, "HTTP:503 报状态错");
        check_contains(err, "服务暂不可用", "HTTP:503 翻成人话");
    }

    /* 大小上限:Content-Length 就先拒 */
    {
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = "0123456789";
        fake.body_len = 10;
        fake.status = 200;
        fake.content_length = 100 * 1024 * 1024; /* 假装有 100MB */
        sxcl_transport tr = make_fake_transport(&fake);
        const int rc = sxcl_http_get_text(&tr, "https://example.invalid/huge", NULL, &out, &out_len, err,
                                          sizeof(err));
        check_int(rc, SXCL_HTTP_ERR_TOO_LARGE, "HTTP:Content-Length 超上限直接拒");
        check_contains(err, "太大", "HTTP:超上限的人话");
        check(out == NULL, "HTTP:超上限不返回正文");
    }

    /* 大小上限:长度未知时边读边掐断(自定义上限 16 字节) */
    {
        static char medium[4096];
        (void)memset(medium, 'y', sizeof(medium));
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = medium;
        fake.body_len = sizeof(medium);
        fake.status = 200;
        fake.content_length = -1;
        fake.chunk = 64;
        sxcl_transport tr = make_fake_transport(&fake);
        sxcl_http_opts opts;
        (void)memset(&opts, 0, sizeof(opts));
        opts.max_bytes = 16;
        const int rc = sxcl_http_get_text_ex(&tr, "https://example.invalid/medium", NULL, &opts, &out,
                                             &out_len, err, sizeof(err));
        check_int(rc, SXCL_HTTP_ERR_TOO_LARGE, "HTTP:长度未知时也掐断");
        check_contains(err, "上限", "HTTP:掐断的人话");
        check(out == NULL, "HTTP:掐断不返回正文");
    }

    /* SHA-1 校验(版本 JSON 那类)*/
    {
        const char *body = "{\"id\":\"1.20.1\"}";
        char digest[65];
        check_int(sxcl_hash_digest(SXCL_HASH_SHA1, body, strlen(body), digest, sizeof(digest)), 0,
                  "HTTP:先算出正确的 SHA-1");
        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = body;
        fake.body_len = strlen(body);
        fake.status = 200;
        fake.content_length = (int64_t)fake.body_len;
        sxcl_transport tr = make_fake_transport(&fake);
        const int rc = sxcl_http_get_text_sha1(&tr, "https://example.invalid/1.20.1.json", NULL, digest, &out,
                                               &out_len, err, sizeof(err));
        check_int(rc, SXCL_HTTP_OK, "HTTP:SHA-1 对得上就成功");
        check_str(out, body, "HTTP:SHA-1 成功后的内容");
        free(out);
        out = NULL;

        /* 校验失败 */
        fake.body = "{\"id\":\"1.20.2\"}";
        fake.body_len = strlen(fake.body);
        fake.content_length = (int64_t)fake.body_len;
        const int rc2 = sxcl_http_get_text_sha1(&tr, "https://example.invalid/1.20.1.json", NULL, digest, &out,
                                                &out_len, err, sizeof(err));
        check_int(rc2, SXCL_HTTP_ERR_SHA1, "HTTP:SHA-1 对不上就失败");
        check_contains(err, "SHA-1", "HTTP:SHA-1 失败的人话");
        check(out == NULL, "HTTP:SHA-1 失败不返回正文");

        /* 空期望值 = 不校验 */
        const int rc3 = sxcl_http_get_text_sha1(&tr, "https://example.invalid/x", NULL, "", &out, &out_len,
                                                err, sizeof(err));
        check_int(rc3, SXCL_HTTP_OK, "HTTP:期望 SHA-1 为空时不校验");
        free(out);
        out = NULL;
    }

    /* 传输层拿不到响应 / 读一半断了 / 参数不合法 */
    {
        sxcl_transport tr;
        (void)memset(&tr, 0, sizeof(tr));
        const int rc = sxcl_http_get_text(&tr, "https://example.invalid/x", NULL, &out, &out_len, err,
                                          sizeof(err));
        check_int(rc, SXCL_HTTP_ERR_ARG, "HTTP:没有传输后端时报参数错");
        check(err[0] != '\0', "HTTP:没有传输后端时给了人话错误");

        fake_http fake;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = "0123456789";
        fake.body_len = 10;
        fake.status = 200;
        fake.content_length = 10;
        fake.chunk = 4;
        fake.fail_after = 8;
        tr = make_fake_transport(&fake);
        const int rc2 = sxcl_http_get_text(&tr, "https://example.invalid/broken", NULL, &out, &out_len, err,
                                           sizeof(err));
        check_int(rc2, SXCL_HTTP_ERR_IO, "HTTP:读一半断了报 IO 错");
        check_contains(err, "读响应体", "HTTP:断了的人话");

        const int rc3 = sxcl_http_get_text(&tr, NULL, NULL, &out, &out_len, err, sizeof(err));
        check_int(rc3, SXCL_HTTP_ERR_ARG, "HTTP:url 为空报参数错");
    }
}

/* ══════════════════════ 5) 机器信息 ══════════════════════ */

static void test_sysinfo(void)
{
    sxcl_sysinfo info;
    char err[SXCL_SYSINFO_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_sysinfo_query(&info, err, sizeof(err));
    check_int(rc, SXCL_SYSINFO_OK, "sysinfo:查得到");
    check(info.total_mb > 0, "sysinfo:物理内存总量 > 0");
    check(info.available_mb > 0, "sysinfo:可用物理内存 > 0");
    check(info.available_mb <= info.total_mb, "sysinfo:可用 <= 总量");
    check(info.total_bytes >= info.total_mb * 1024ull * 1024ull, "sysinfo:字节与 MB 对得上");
    check(info.cpu_logical > 0, "sysinfo:cpu_logical > 0");
    check(info.cpu_physical >= 0, "sysinfo:物理核心数(拿不到是 0)");

    const uint64_t heap = sxcl_sysinfo_recommended_heap_mb();
    check(heap >= SXCL_SYSINFO_HEAP_MIN_MB && heap <= SXCL_SYSINFO_HEAP_MAX_MB,
          "sysinfo:推荐堆内存在 1024~8192 之间");
    check(heap == sxcl_sysinfo_heap_for(info.total_mb), "sysinfo:推荐堆 = f(物理内存)");

    check_int((long long)sxcl_sysinfo_heap_for(0), 2048, "堆推荐:查不出来时给 2048");
    check_int((long long)sxcl_sysinfo_heap_for(1024), 1024, "堆推荐:小内存夹到 1024");
    check_int((long long)sxcl_sysinfo_heap_for(8192), 4096, "堆推荐:16G 内存给 4096");
    check_int((long long)sxcl_sysinfo_heap_for(65536), 8192, "堆推荐:大内存夹到 8192");

    check_int(sxcl_sysinfo_workers_for(0), 4, "线程建议:核心数拿不到给 4");
    check_int(sxcl_sysinfo_workers_for(1), 2, "线程建议:下限 2");
    check_int(sxcl_sysinfo_workers_for(8), 8, "线程建议:跟核心数走");
    check_int(sxcl_sysinfo_workers_for(64), 16, "线程建议:上限 16");
    const int workers = sxcl_sysinfo_recommended_workers();
    check(workers >= SXCL_SYSINFO_WORKERS_MIN && workers <= SXCL_SYSINFO_WORKERS_MAX,
          "sysinfo:推荐线程数在 2~16 之间");

    check_int(sxcl_sysinfo_query(NULL, err, sizeof(err)), SXCL_SYSINFO_ERR_ARG, "sysinfo:out 为空报参数错");
}

int main(void)
{
    printf("== 实例扫描与识别 ==\n");
    build_fixture_tree();
    test_instance_scan();

    printf("== PCL 兼容读取 ==\n");
    test_pcl_compat();

    printf("== 游戏目录探测 ==\n");
    test_paths();

    printf("== HTTP 取文本 ==\n");
    test_http();

    printf("== 机器信息 ==\n");
    test_sysinfo();

    (void)sxcl_dir_remove_tree(FIXTURE_ROOT);

    printf("\n%s: %d 项通过, %d 项失败\n", (g_fail == 0) ? "PASS" : "FAIL", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
