/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* jvm_test.c - 进程内起 JVM 的**参数与环境**单测(不需要真 JRE、不联网、不起进程)。
 *
 * 覆盖的是"起 JVM 之前必须补什么"这条清单本身(docs/18 §7.3 与 docs/19):
 *   * JAVA_HOME / LD_LIBRARY_PATH(<home>/lib + <home>/lib/server + nativeLibraryDir)/ TMPDIR / HOME
 *   * -Djava.home、-Djava.io.tmpdir、-Duser.home、-Dos.name=Linux、-Dos.version=Android-<版本>、
 *     -Djava.library.path
 *   * **绝不能有 -XstartOnFirstThread**(macOS 专用;误传的要被丢掉并记一笔)
 *   * 真 JRE 在盘上时额外做一次 dlopen 探针(没有就跳过,不算失败)
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/jvm.h"

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

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

static int argv_has(const sxcl_jvm_args *args, const char *want)
{
    size_t i;
    for (i = 0; i < args->count; ++i) {
        if (strcmp(args->argv[i], want) == 0) {
            return 1;
        }
    }
    return 0;
}

static int argv_has_prefix(const sxcl_jvm_args *args, const char *prefix)
{
    size_t i;
    for (i = 0; i < args->count; ++i) {
        if (strncmp(args->argv[i], prefix, strlen(prefix)) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_env(void)
{
    sxcl_jvm_opts opts;
    sxcl_jvm_env env;
    char err[SXCL_JVM_ERROR_MAX];
    static const char *const extra[] = {"/data/app/com.x/lib/arm64", "/sdcard/x", NULL};

    printf("== 环境变量:JAVA_HOME / LD_LIBRARY_PATH / TMPDIR / HOME\n");
    memset(&opts, 0, sizeof(opts));
    opts.java_home = "/data/user/0/com.silentstudio.sxcl/files/runtime/jre17";
    opts.extra_lib_dirs = extra;
    opts.apply_env = 0;
    err[0] = '\0';
    check_int(sxcl_jvm_build_env(&opts, &env, err, sizeof(err)), SXCL_JVM_OK, "算环境变量");
    check_str(env.java_home, "/data/user/0/com.silentstudio.sxcl/files/runtime/jre17",
              "JAVA_HOME");
    check(strstr(env.ld_library_path, "/runtime/jre17/lib") != NULL, "LD 含 <home>/lib");
#if defined(_WIN32)
    check(strchr(env.ld_library_path, ';') != NULL, "Windows 上 LD 用 ';' 分隔");
#else
    check(strchr(env.ld_library_path, ':') != NULL, "POSIX 上 LD 用 ':' 分隔");
#endif
    check(strstr(env.ld_library_path, "/runtime/jre17/lib/server") != NULL,
          "LD 含 <home>/lib/server");
    check(strstr(env.ld_library_path, "/data/app/com.x/lib/arm64") != NULL,
          "LD 含 nativeLibraryDir");
    check(strstr(env.ld_library_path, "/sdcard/x") != NULL, "LD 含第二个额外目录");
    check_str(env.tmp_dir, "/data/user/0/com.silentstudio.sxcl/files/runtime/jre17/tmp", "TMPDIR");
    check_str(env.home, "/data/user/0/com.silentstudio.sxcl/files/runtime/jre17/home", "HOME");
    check_int((long)strlen(env.ld_library_path), (long)strlen(env.java_library_path),
              "java.library.path 默认与 LD 同一份");

    /* 显式给的要赢 */
    opts.tmp_dir = "/tmp/j";
    opts.user_home = "/home/u";
    opts.java_library_path = "/only/this";
    check_int(sxcl_jvm_build_env(&opts, &env, err, sizeof(err)), SXCL_JVM_OK, "算环境变量(显式)");
    check_str(env.tmp_dir, "/tmp/j", "显式 TMPDIR");
    check_str(env.home, "/home/u", "显式 HOME");
    check_str(env.java_library_path, "/only/this", "显式 java.library.path");

    opts.java_home = NULL;
    check_int(sxcl_jvm_build_env(&opts, &env, err, sizeof(err)), SXCL_JVM_ERR_NO_HOME,
              "没有 java_home -> no_home");
    check(strstr(err, "java_home") != NULL, "错误信息点到 java_home");
    check_int(sxcl_jvm_build_env(NULL, &env, err, sizeof(err)), SXCL_JVM_ERR_ARG, "opts 为空 -> arg");
}

static void test_args(void)
{
    sxcl_jvm_opts opts;
    sxcl_jvm_env env;
    sxcl_jvm_args args;
    char err[SXCL_JVM_ERROR_MAX];
    static const char *const extra[] = {"/data/app/com.x/lib/arm64", NULL};
    static const char *const jvmargs[] = {"-Xmx2G", "-XstartOnFirstThread", "-XX:+UseSerialGC", NULL};
    static const char *const appargs[] = {"--username", "Steve", NULL};

    printf("== argv:四个 -D 系统属性 + 绝不放 -XstartOnFirstThread\n");
    memset(&opts, 0, sizeof(opts));
    opts.java_home = "/jre/jre21";
    opts.extra_lib_dirs = extra;
    opts.android_version = "16";
    opts.apply_env = 0;
    opts.extra_args = jvmargs;
    opts.main_class = "net.minecraft.client.main.Main";
    opts.app_args = appargs;
    check_int(sxcl_jvm_build_env(&opts, &env, err, sizeof(err)), SXCL_JVM_OK, "算环境变量");
    check_int(sxcl_jvm_build_args(&opts, &env, 0, &args, err, sizeof(err)), SXCL_JVM_OK, "拼 argv");
    /* 12 个:argv[0] + 6 个 -D + 2 个额外 JVM 参数 + 主类 + 2 个主类参数
     * (-XstartOnFirstThread 被丢掉了,不算在内) */
    check_int((long)args.count, 12, "参数个数");
#if defined(_WIN32)
    check_str(args.argv[0], "/jre/jre21/bin/java.exe", "argv[0] 是 bin/java.exe(Windows)");
#else
    check_str(args.argv[0], "/jre/jre21/bin/java", "argv[0] 是 bin/java(不用可执行位)");
#endif
    check_int(args.dropped_start_on_first_thread, 1, "误传的 -XstartOnFirstThread 被丢掉");
    check(!argv_has_prefix(&args, "-XstartOnFirstThread"), "argv 里确实没有它");
    check(argv_has(&args, "-Djava.home=/jre/jre21"), "-Djava.home");
    check(argv_has(&args, "-Djava.io.tmpdir=/jre/jre21/tmp"), "-Djava.io.tmpdir");
    check(argv_has(&args, "-Duser.home=/jre/jre21/home"), "-Duser.home");
    check(argv_has(&args, "-Dos.name=Linux"), "-Dos.name=Linux");
    check(argv_has(&args, "-Dos.version=Android-16"), "-Dos.version=Android-16");
    {
        /* 分隔符按平台:POSIX ':' / Windows ';'(apply_env 与 LD_LIBRARY_PATH 同一套口径) */
#if defined(_WIN32)
        static const char *const kSep = ";";
#else
        static const char *const kSep = ":";
#endif
        char want[512];
        snprintf(want, sizeof(want), "-Djava.library.path=/jre/jre21/lib%s/jre/jre21/lib/server%s"
                                     "/data/app/com.x/lib/arm64",
                 kSep, kSep);
        check(argv_has(&args, want), "-Djava.library.path 三条都在");
    }
    check(argv_has(&args, "-Xmx2G"), "额外 JVM 参数还在");
    check(argv_has(&args, "-XX:+UseSerialGC"), "另一个额外参数也在");
    check(argv_has(&args, "net.minecraft.client.main.Main"), "主类");
    check(argv_has(&args, "--username"), "主类参数");
    check(argv_has(&args, "Steve"), "主类参数值");
    check(args.argv[args.count] == NULL, "argv 以 NULL 结尾");

    /* -version 自检:不吃主类,改吃 -XshowSettings:properties -version */
    check_int(sxcl_jvm_build_args(&opts, &env, 1, &args, err, sizeof(err)), SXCL_JVM_OK,
              "拼 argv(自检)");
    check(argv_has(&args, "-version"), "自检带 -version");
    check(argv_has(&args, "-XshowSettings:properties"), "自检带 -XshowSettings:properties");
    check(!argv_has(&args, "net.minecraft.client.main.Main"), "自检不带主类");

    /* 没有 android_version 就不加 -Dos.version */
    opts.android_version = NULL;
    check_int(sxcl_jvm_build_args(&opts, &env, 1, &args, err, sizeof(err)), SXCL_JVM_OK,
              "拼 argv(无安卓版本)");
    check(!argv_has_prefix(&args, "-Dos.version"), "没给就不加 -Dos.version");
    check(argv_has(&args, "-Dos.name=Linux"), "-Dos.name 永远在");

    /* 没有主类、不是自检:就是一个纯参数向量(调用方自己负责) */
    opts.main_class = NULL;
    opts.app_args = NULL;
    check_int(sxcl_jvm_build_args(&opts, &env, 0, &args, err, sizeof(err)), SXCL_JVM_OK,
              "拼 argv(无主类)");
    check_int(sxcl_jvm_build_args(NULL, &env, 0, &args, err, sizeof(err)), SXCL_JVM_ERR_ARG,
              "opts 为空 -> arg");
    check_int(sxcl_jvm_build_args(&opts, NULL, 0, &args, err, sizeof(err)), SXCL_JVM_ERR_ARG,
              "env 为空 -> arg");
}

static void test_scan_property(void)
{
    static const char kText[] =
        "Property settings:\n"
        "    java.home = /data/user/0/com.silentstudio.sxcl/files/runtime/jre17\n"
        "    java.io.tmpdir = /data/user/0/com.silentstudio.sxcl/files/runtime/jre17/tmp\n"
        "    java.version = 17.0.9\n"
        "    os.name = Linux\n"
        "openjdk version \"17.0.9\" 2026-10-20\n";
    char out[256];
    printf("== 从 -XshowSettings:properties 的输出里抠属性\n");
    check_int(sxcl_jvm_scan_property(kText, "java.version", out, sizeof(out)), 0, "找到 java.version");
    check_str(out, "17.0.9", "java.version 的值");
    check_int(sxcl_jvm_scan_property(kText, "java.home", out, sizeof(out)), 0, "找到 java.home");
    check_str(out, "/data/user/0/com.silentstudio.sxcl/files/runtime/jre17", "java.home 的值");
    check_int(sxcl_jvm_scan_property(kText, "os.name", out, sizeof(out)), 0, "找到 os.name");
    check_str(out, "Linux", "os.name 的值");
    check_int(sxcl_jvm_scan_property(kText, "java.vendor", out, sizeof(out)), -1, "没有的键 -> -1");
    check_str(out, "", "没有的键 -> 空串");
    check_int(sxcl_jvm_scan_property(NULL, "java.version", out, sizeof(out)), -1, "文本为空 -> -1");
    /* 截断不许越界 */
    check_int(sxcl_jvm_scan_property(kText, "java.version", out, 4), 0, "缓冲很小也能用");
    check_str(out, "17.", "按缓冲截断");
}

static void test_missing_jre(void)
{
    sxcl_jvm_opts opts;
    sxcl_jvm_result res;
    printf("== 目录不存在 / 没有 libjli.so:给人话,不崩\n");
    memset(&opts, 0, sizeof(opts));
    opts.java_home = "_jvm_tmp/does-not-exist";
    opts.apply_env = 0;
    memset(&res, 0, sizeof(res));
    check_int(sxcl_jvm_probe(&opts, &res), SXCL_JVM_ERR_NO_HOME, "目录不存在 -> no_home");
    check(res.error[0] != '\0', "有人话错误");

    (void)sxcl_fs_remove_tree("_jvm_tmp");
    (void)sxcl_fs_mkdirs("_jvm_tmp/empty-jre");
    opts.java_home = "_jvm_tmp/empty-jre";
    memset(&res, 0, sizeof(res));
    check_int(sxcl_jvm_probe(&opts, &res), SXCL_JVM_ERR_NO_JLI, "空目录 -> no_jli");
    check(strstr(res.error, "libjli") != NULL || strstr(res.error, "java.dll") != NULL,
          "错误信息点到 libjli");
    check_str(sxcl_jvm_code_name(SXCL_JVM_ERR_NO_JLI), "no_jli", "返回码名字");
    (void)sxcl_fs_remove_tree("_jvm_tmp");
}

static void test_real_jre_if_any(void)
{
    const char *candidates[3];
    int i;
    printf("== 盘上真有 JRE 时做一次 dlopen 探针(没有就跳过;跳过不算失败)\n");
    candidates[0] = getenv("SXCL_TEST_JAVA_HOME");
    candidates[1] = "C:/Program Files/Java/jdk-17";
    candidates[2] = "C:/Program Files/Eclipse Adoptium";
    for (i = 0; i < 3; ++i) {
        sxcl_jvm_opts opts;
        sxcl_jvm_result res;
        if (candidates[i] == NULL || !sxcl_fs_is_dir(candidates[i])) {
            continue;
        }
        memset(&opts, 0, sizeof(opts));
        opts.java_home = candidates[i];
        opts.apply_env = 0; /* 别动测试进程的环境 */
        memset(&res, 0, sizeof(res));
        {
            const int rc = sxcl_jvm_probe(&opts, &res);
            printf("      %s -> %s(%s)\n", candidates[i], sxcl_jvm_code_name(rc), res.jli_path);
            if (rc == SXCL_JVM_OK) {
                check_int(rc, SXCL_JVM_OK, "dlopen + dlsym 成功");
                check(res.jli_path[0] != '\0', "报出了 dlopen 的文件");
            } else {
                /* 找不到 jli 也算通过:这个"JRE"可能只是个不完整的目录 */
                check(rc == SXCL_JVM_ERR_NO_JLI || rc == SXCL_JVM_ERR_DLOPEN,
                      "失败也得是说得清的那两种");
            }
        }
        return;
    }
    printf("      (盘上没有可用的 JRE,跳过)\n");
}

int main(void)
{
    check_str(sxcl_jvm_code_name(SXCL_JVM_OK), "ok", "ok");
    check_str(sxcl_jvm_code_name(SXCL_JVM_ERR_DLSYM), "dlsym", "dlsym");
    check_str(sxcl_jvm_code_name(-999), "?", "未知");
    test_env();
    test_args();
    test_scan_property();
    test_missing_jre();
    test_real_jre_if_any();
    printf("\njvm_test: 通过 %d,失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
