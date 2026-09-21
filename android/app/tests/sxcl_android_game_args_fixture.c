/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_android_game_args_fixture.c - 参数拼装的**离线夹具断言**(不需要设备、不需要真 JRE、不起 JVM)。
 *
 * 它做三件事:
 *   1) 在临时目录里搭一个最小可用的"游戏目录 + 实例 JSON + 夹具 JRE";
 *   2) 调 android/app/sxcl_android_game_args.c(它再调**核心库**的 instance/natives/launch 三层)
 *      拼出游戏命令行,逐条断言:-cp 里有哪些 jar / 谁被 rules 挡掉 / -Djava.library.path 指向
 *      安卓布局的私有 natives 目录 / 主类 / 关键游戏参数 / 我们追加的参数;
 *   3) 再把这条命令行喂给 include/sxcl/jvm.h 的 sxcl_jvm_build_env/build_args,断言**最终 JVM argv**
 *      (java 可执行文件 + -Djava.home + -Dos.name=Linux + -Dos.version=Android-<ver> + 游戏命令行),
 *      并把两份 argv 逐条打印出来(报告里贴的就是这段原文)。
 *
 * 返回 0 = 全部断言通过;非 0 = 有断言失败(打印失败的条目)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/jvm.h"
#include "sxcl_android_game_args.h"

#if defined(_WIN32)
#include <direct.h>
#define FIX_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define FIX_MKDIR(p) mkdir(p, 0777)
#endif

static int g_fail = 0;
static int g_checks = 0;

static void fixture_log(void *ud, const char *line)
{
    (void)ud;
    printf("  [plan] %s\n", line);
}

static void check(int ok, const char *what)
{
    ++g_checks;
    printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok)
        ++g_fail;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  [FAIL] 写不进去:%s\n", path);
        ++g_fail;
        return;
    }
    (void)fwrite(text, 1, strlen(text), f);
    (void)fclose(f);
}

static void write_empty(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f != NULL)
        (void)fclose(f);
}

/* 最小版本 JSON:一个 linux 才能用的库、一个 windows-only 的库(必须被 rules 挡掉)、
 * 一个没有 rules 的库;arguments.jvm 里带一条 windows-only 的 -D(也必须被挡掉)。 */
static const char *kVersionJson =
    "{\n"
    "  \"id\": \"1.21.4\",\n"
    "  \"type\": \"release\",\n"
    "  \"mainClass\": \"net.minecraft.client.main.Main\",\n"
    "  \"assets\": \"1.21\",\n"
    "  \"assetIndex\": {\"id\": \"1.21\", \"sha1\": \"x\", \"size\": 1, \"totalSize\": 1,"
    " \"url\": \"http://example.invalid/a.json\"},\n"
    "  \"javaVersion\": {\"component\": \"java-runtime-delta\", \"majorVersion\": 21},\n"
    "  \"libraries\": [\n"
    "    {\"name\": \"org.lwjgl:lwjgl:3.3.3\",\n"
    "     \"downloads\": {\"artifact\": {\"path\": \"org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar\","
    " \"url\": \"http://example.invalid/lwjgl.jar\", \"sha1\": \"x\", \"size\": 1}},\n"
    "     \"rules\": [{\"action\": \"allow\", \"os\": {\"name\": \"linux\"}}]},\n"
    "    {\"name\": \"com.example:windows-only:1.0\",\n"
    "     \"downloads\": {\"artifact\": {\"path\":"
    " \"com/example/windows-only/1.0/windows-only-1.0.jar\","
    " \"url\": \"http://example.invalid/w.jar\", \"sha1\": \"x\", \"size\": 1}},\n"
    "     \"rules\": [{\"action\": \"allow\", \"os\": {\"name\": \"windows\"}}]},\n"
    "    {\"name\": \"com.example:androidthing:1.0\",\n"
    "     \"downloads\": {\"artifact\": {\"path\":"
    " \"com/example/androidthing/1.0/androidthing-1.0.jar\","
    " \"url\": \"http://example.invalid/a.jar\", \"sha1\": \"x\", \"size\": 1}}\n"
    "    }\n"
    "  ],\n"
    "  \"arguments\": {\n"
    "    \"jvm\": [\n"
    "      \"-Djava.library.path=${natives_directory}\",\n"
    "      \"-Djna.tmpdir=${natives_directory}\",\n"
    "      {\"rules\": [{\"action\": \"allow\", \"os\": {\"name\": \"windows\"}}],"
    " \"value\": \"-Dos.name=Windows 10\"},\n"
    "      {\"rules\": [{\"action\": \"allow\", \"os\": {\"name\": \"linux\"}}],"
    " \"value\": \"-Dos.name=Linux\"}\n"
    "    ],\n"
    "    \"game\": [\n"
    "      \"--username\", \"${auth_player_name}\",\n"
    "      \"--version\", \"${version_name}\",\n"
    "      \"--gameDir\", \"${game_directory}\",\n"
    "      \"--assetsDir\", \"${assets_root}\",\n"
    "      \"--assetIndex\", \"${assets_index_name}\",\n"
    "      \"--uuid\", \"${auth_uuid}\",\n"
    "      \"--accessToken\", \"${auth_access_token}\",\n"
    "      \"--userType\", \"${user_type}\",\n"
    "      \"--demo\"\n"
    "    ]\n"
    "  }\n"
    "}\n";

static int argv_has(const char *const *av, const char *text)
{
    int i = 0;
    for (i = 0; av != NULL && av[i] != NULL; ++i) {
        if (strcmp(av[i], text) == 0)
            return 1;
    }
    return 0;
}

/* 某个参数后面紧跟的那个值(--username 的下一个是 TestPlayer) */
static const char *argv_value_of(const char *const *av, const char *key)
{
    int i = 0;
    for (i = 0; av != NULL && av[i] != NULL; ++i) {
        if (strcmp(av[i], key) == 0 && av[i + 1] != NULL)
            return av[i + 1];
    }
    return NULL;
}

static int argv_contains_substr(const char *const *av, const char *needle)
{
    int i = 0;
    for (i = 0; av != NULL && av[i] != NULL; ++i) {
        if (strstr(av[i], needle) != NULL)
            return 1;
    }
    return 0;
}

static void dump_argv(const char *title, const char *const *av)
{
    int i = 0;
    printf("\n%s\n", title);
    for (i = 0; av != NULL && av[i] != NULL; ++i)
        printf("  [%2d] %s\n", i, av[i]);
}

int main(void)
{
    char dir[600];
    char path[700];
    sxcl_android_game_spec spec;
    sxcl_android_game_plan plan;
    sxcl_jvm_opts jopts;
    sxcl_jvm_env jenv;
    sxcl_jvm_args jargs;
    char err[512];
    const char *extra_jvm[2];
    const char *extra_game[5];
    int rc = 0;

#if defined(_WIN32)
    (void)system("rmdir /s /q sxcl_args_fixture_tmp 2>nul");
#else
    (void)system("rm -rf sxcl_args_fixture_tmp");
#endif
    (void)FIX_MKDIR("sxcl_args_fixture_tmp");

    /* ── 夹具目录树 ── */
    (void)snprintf(dir, sizeof(dir), "%s/game/versions/1.21.4", "sxcl_args_fixture_tmp");
    (void)sxcl_fs_mkdirs(dir);
    (void)snprintf(path, sizeof(path), "%s/1.21.4.json", dir);
    write_file(path, kVersionJson);
    (void)snprintf(path, sizeof(path), "%s/1.21.4.jar", dir);
    write_empty(path);

    (void)snprintf(dir, sizeof(dir), "%s/game/libraries/org/lwjgl/lwjgl/3.3.3", "sxcl_args_fixture_tmp");
    (void)sxcl_fs_mkdirs(dir);
    (void)snprintf(path, sizeof(path), "%s/lwjgl-3.3.3.jar", dir);
    write_empty(path);

    (void)snprintf(dir, sizeof(dir), "%s/game/libraries/com/example/androidthing/1.0",
                   "sxcl_args_fixture_tmp");
    (void)sxcl_fs_mkdirs(dir);
    (void)snprintf(path, sizeof(path), "%s/androidthing-1.0.jar", dir);
    write_empty(path);

    (void)snprintf(dir, sizeof(dir), "%s/game/libraries/com/example/windows-only/1.0",
                   "sxcl_args_fixture_tmp");
    (void)sxcl_fs_mkdirs(dir);
    (void)snprintf(path, sizeof(path), "%s/windows-only-1.0.jar", dir);
    write_empty(path);

    (void)sxcl_fs_mkdirs("sxcl_args_fixture_tmp/files");
    (void)sxcl_fs_mkdirs("sxcl_args_fixture_tmp/jre");

    sxcl_android_game_set_log(fixture_log, NULL);

    /* ── 装配(核心库的 instance/natives/launch 三层 + 我们的安卓布局) ── */
    (void)memset(&spec, 0, sizeof(spec));
    (void)memset(&plan, 0, sizeof(plan));
    spec.files_dir = "sxcl_args_fixture_tmp/files";
    spec.game_dir = "sxcl_args_fixture_tmp/game";
    spec.instance_id = "1.21.4";
    spec.jre_home = "sxcl_args_fixture_tmp/jre";
    spec.player_name = "TestPlayer";
    spec.uuid = "11111111-2222-3333-4444-555555555555";
    spec.access_token = "0";
    spec.user_type = "legacy";
    spec.memory_mb = 4096;
    spec.java_major = 21;
    spec.is_64bit = 1;
    spec.android_version = "16";
    spec.renderer = "egl";
    extra_jvm[0] = "-Dsxcl.android.renderer=egl";
    extra_jvm[1] = NULL;
    spec.extra_jvm_args = extra_jvm;
    extra_game[0] = "--width";
    extra_game[1] = "1280";
    extra_game[2] = "--height";
    extra_game[3] = "720";
    extra_game[4] = NULL;
    spec.extra_game_args = extra_game;

    err[0] = '\0';
    rc = sxcl_android_game_plan_build(&spec, &plan, err, sizeof(err));
    printf("plan_build rc=%d err=%s\n", rc, err);
    check(rc == 0, "装配成功(sxcl_android_game_plan_build == 0)");
    if (rc != 0) {
        printf("FIXTURE FAILED(build)\n");
        return 1;
    }

    /* ── 断言 1:classpath ── */
    check(plan.classpath[0] != '\0', "-cp 在命令行里(核心库拼的)");
    check(strstr(plan.classpath, "1.21.4.jar") != NULL, "-cp 含客户端 jar(versions/1.21.4/1.21.4.jar)");
    check(strstr(plan.classpath, "lwjgl-3.3.3.jar") != NULL, "-cp 含 linux 规则允许的库(lwjgl)");
    check(strstr(plan.classpath, "androidthing-1.0.jar") != NULL, "-cp 含没有 rules 的库(androidthing)");
    check(strstr(plan.classpath, "windows-only-1.0.jar") == NULL,
          "-cp 不含 windows-only 的库(rules 按 linux 求值)");
    check(strchr(plan.classpath, ':') != NULL, "-cp 用 ':' 分隔(安卓/linux 口径)");

    /* ── 断言 2:-Djava.library.path 指向安卓布局的私有 natives ── */
    check(argv_has(plan.argv,
                   "-Djava.library.path=sxcl_args_fixture_tmp/files/natives/1.21.4"),
          "-Djava.library.path=<files>/natives/1.21.4(安卓布局)");
    check(argv_contains_substr(plan.argv, "os.name=Windows 10") == 0,
          "windows-only 的 -Dos.name=Windows 10 被 rules 挡掉");
    check(argv_contains_substr(plan.argv, "-Dsxcl.android.renderer=egl") != 0,
          "我们这层补的安卓 -D 进了命令行");

    /* ── 断言 3:主类与游戏参数 ── */
    check(strcmp(plan.main_class, "net.minecraft.client.main.Main") == 0,
          "主类 = net.minecraft.client.main.Main");
    check(argv_has(plan.argv, "net.minecraft.client.main.Main"), "主类在命令行里");
    check(argv_value_of(plan.argv, "--username") != NULL &&
              strcmp(argv_value_of(plan.argv, "--username"), "TestPlayer") == 0,
          "--username TestPlayer");
    check(argv_value_of(plan.argv, "--version") != NULL &&
              strcmp(argv_value_of(plan.argv, "--version"), "1.21.4") == 0,
          "--version 1.21.4");
    check(argv_value_of(plan.argv, "--gameDir") != NULL &&
              strcmp(argv_value_of(plan.argv, "--gameDir"), "sxcl_args_fixture_tmp/game") == 0,
          "--gameDir <game>");
    check(argv_value_of(plan.argv, "--assetIndex") != NULL &&
              strcmp(argv_value_of(plan.argv, "--assetIndex"), "1.21") == 0,
          "--assetIndex 1.21(来自版本 JSON 的 assetIndex.id)");
    check(argv_has(plan.argv, "--demo"), "版本 JSON 里的 --demo 在命令行里");
    check(argv_has(plan.argv, "1280") && argv_has(plan.argv, "720"),
          "夹具追加的游戏参数(--width 1280 --height 720)在命令行里");

    dump_argv("核心库拼出的游戏命令行(sxcl_launch_build_args,不含 java 可执行文件):", plan.argv);

    /* ── 断言 4:最终 JVM argv(把游戏命令行当 extra_args 交给 jvm.h 那层) ── */
    (void)memset(&jopts, 0, sizeof(jopts));
    jopts.java_home = spec.jre_home;
    jopts.android_version = spec.android_version;
    jopts.java_library_path = plan.java_library_path; /* 与命令行里的 -Djava.library.path 同一个值 */
    jopts.extra_args = plan.argv;                     /* 整条游戏命令行原样传下去 */
    jopts.apply_env = 0;                              /* 夹具只算不设,不动这个进程的环境变量 */
    jopts.preload_libs = 0;
    memset(&jenv, 0, sizeof(jenv));
    memset(&jargs, 0, sizeof(jargs));
    err[0] = '\0';
    rc = sxcl_jvm_build_env(&jopts, &jenv, err, sizeof(err));
    check(rc == SXCL_JVM_OK, "sxcl_jvm_build_env ok");
    rc = sxcl_jvm_build_args(&jopts, &jenv, 0, &jargs, err, sizeof(err));
    check(rc == SXCL_JVM_OK, "sxcl_jvm_build_args ok");
    if (rc == SXCL_JVM_OK) {
        check(strstr(jargs.argv[0], "bin/java") != NULL,
              "argv[0] = <jre>/bin/java(存在即可,不需要可执行)");
        check(argv_has(jargs.argv, "-Djava.home=sxcl_args_fixture_tmp/jre"),
              "-Djava.home=<jre>(进程内 dlopen 必须显式给)");
        check(argv_has(jargs.argv, "-Dos.name=Linux"), "-Dos.name=Linux(jvm 层补的)");
        check(argv_has(jargs.argv, "-Dos.version=Android-16"), "-Dos.version=Android-16(jvm 层补的)");
        check(argv_has(jargs.argv, "-Djava.library.path=sxcl_args_fixture_tmp/files/natives/1.21.4"),
              "-Djava.library.path 指向安卓私有 natives(与命令行一致)");
        check(argv_has(jargs.argv, "net.minecraft.client.main.Main"), "最终 argv 里有主类");
        check(argv_has(jargs.argv, "--demo"), "最终 argv 里有游戏参数");
        dump_argv("最终 JVM argv(sxcl_jvm_build_args;进程内 JLI_Launch 直接吃这一串):", jargs.argv);
    }

    sxcl_android_game_plan_free(&plan);
    printf("\n夹具断言:%d 项检查,%d 项失败\n", g_checks, g_fail);
    printf(g_fail == 0 ? "FIXTURE OK\n" : "FIXTURE FAILED\n");
    return g_fail == 0 ? 0 : 1;
}
