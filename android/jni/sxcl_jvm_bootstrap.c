/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_jvm_bootstrap.c - 安卓侧的进程内 JVM 自举(Android only;对核心库 sxcl/jvm.h 的薄封装)。
 *
 * 为什么需要它:真机实测(2026-09-21,docs/18 §4.2)证明应用自己的 untrusted_app 域**不能**
 * exec 私有目录里的文件(SELinux avc denied { execute_no_trans },permissive=0),
 * 所以 fork+exec <jre>/bin/java 这条路是死的。剩下的活路是**进程内**:
 *
 *     dlopen("<jre>/lib/libjli.so") -> dlsym("JLI_Launch") -> 调用
 *
 * 思路参考 Boardwalk / PojavLauncher 一系(FCL 走的也是同一条);**本文件的代码是我们自己写的**,
 * 没有从 FCL/Pojav 抄任何源码文本 —— 只有"dlopen + JLI_Launch"这条公开做法。
 *
 * 这里**不重复**核心库已经做的事(环境变量、-D 系统属性、argv、输出重定向、预加载),
 * 只补安卓特有的一步:把 LD_LIBRARY_PATH 交给 linker 的私有入口(拿不到就退回预加载)。
 */

#include "sxcl_jvm_bootstrap.h"

#include <android/log.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/jvm.h"

#define BOOT_TAG "sxcl"
#define BOOT_PATH_MAX 1024

static void boot_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    __android_log_print(ANDROID_LOG_INFO, BOOT_TAG, "[jvm-boot] %s", buf);
}

/** 把 LD_LIBRARY_PATH 交给 linker 的私有入口。bionic 在进程启动时就读了它,
 *  运行期只 setenv 不一定生效 —— 这两个符号是安卓上唯一的"运行期改搜索路径"入口。 */
static int boot_update_ld_path(const char *paths)
{
    void *handle = dlopen("libdl.so", RTLD_LAZY);
    typedef void (*update_fn)(const char *);
    update_fn fn = NULL;
    const char *via = NULL;
    if (handle != NULL) {
        fn = (update_fn)(void (*)(void))dlsym(handle, "android_update_LD_LIBRARY_PATH");
        via = "android_update_LD_LIBRARY_PATH";
        if (fn == NULL) {
            fn = (update_fn)(void (*)(void))dlsym(
                handle, "__loader_android_update_LD_LIBRARY_PATH");
            via = "__loader_android_update_LD_LIBRARY_PATH";
        }
    }
    if (fn == NULL) {
        boot_log("LD_LIBRARY_PATH:linker 私有入口没拿到,改走 RTLD_GLOBAL 预加载兜底");
        return -1;
    }
    fn(paths);
    boot_log("LD_LIBRARY_PATH 已更新(经 %s):%s", via, paths);
    return 0;
}

int sxcl_android_jvm_probe(const char *java_home, const char *native_lib_dir,
                           const char *android_version)
{
    sxcl_jvm_opts opts;
    sxcl_jvm_env env;
    sxcl_jvm_result res;
    char err[SXCL_JVM_ERROR_MAX];
    const char *extra[2];
    int rc;

    if (java_home == NULL || java_home[0] == '\0') {
        boot_log("探针没拿到 JRE 路径,放弃");
        return SXCL_JVM_ERR_NO_HOME;
    }
    extra[0] = (native_lib_dir != NULL && native_lib_dir[0] != '\0') ? native_lib_dir : NULL;
    extra[1] = NULL;
    memset(&opts, 0, sizeof(opts));
    opts.java_home = java_home;
    opts.extra_lib_dirs = extra;
    opts.android_version = android_version;
    opts.apply_env = 1;
    opts.preload_libs = 1;
    err[0] = '\0';
    if (sxcl_jvm_build_env(&opts, &env, err, sizeof(err)) != SXCL_JVM_OK) {
        boot_log("算环境变量失败: %s", err);
        return SXCL_JVM_ERR_ARG;
    }
    boot_log("JAVA_HOME=%s", env.java_home);
    boot_log("LD_LIBRARY_PATH=%s", env.ld_library_path);
    boot_log("TMPDIR=%s", env.tmp_dir);
    boot_log("HOME=%s", env.home);
    /* 安卓特有:先让 linker 知道新的搜索路径,再让核心库去 dlopen */
    (void)boot_update_ld_path(env.ld_library_path);
    memset(&res, 0, sizeof(res));
    rc = sxcl_jvm_probe(&opts, &res);
    boot_log("dlopen 探针: %s(libjli=%s)%s%s", sxcl_jvm_code_name(rc), res.jli_path,
             res.error[0] != '\0' ? " / " : "", res.error);
    return rc;
}

int sxcl_android_jvm_bootstrap(const char *java_home, const char *native_lib_dir,
                               const char *tmp_dir, const char *user_home,
                               const char *android_version, const char *capture_path,
                               int for_version)
{
    sxcl_jvm_opts opts;
    sxcl_jvm_env env;
    sxcl_jvm_result res;
    char err[SXCL_JVM_ERROR_MAX];
    const char *extra[2];
    int rc;

    if (java_home == NULL || java_home[0] == '\0') {
        boot_log("自举没拿到 JRE 路径,放弃");
        return SXCL_JVM_ERR_NO_HOME;
    }
    extra[0] = (native_lib_dir != NULL && native_lib_dir[0] != '\0') ? native_lib_dir : NULL;
    extra[1] = NULL;
    memset(&opts, 0, sizeof(opts));
    opts.java_home = java_home;
    opts.extra_lib_dirs = extra;
    opts.tmp_dir = tmp_dir;
    opts.user_home = user_home;
    opts.android_version = android_version;
    opts.apply_env = 1;
    opts.preload_libs = 1;
    opts.capture_path = capture_path;
    err[0] = '\0';
    if (sxcl_jvm_build_env(&opts, &env, err, sizeof(err)) != SXCL_JVM_OK) {
        boot_log("算环境变量失败: %s", err);
        return SXCL_JVM_ERR_ARG;
    }
    (void)boot_update_ld_path(env.ld_library_path);
    memset(&res, 0, sizeof(res));
    if (capture_path != NULL && capture_path[0] != '\0') {
        boot_log("原始输出 -> %s", capture_path);
    }
    rc = sxcl_jvm_launch(&opts, for_version, &res);
    boot_log("JLI_Launch 结束: %s(rc=%d, jli_rc=%d)%s%s", sxcl_jvm_code_name(rc), rc, res.jli_rc,
             res.error[0] != '\0' ? " / " : "", res.error);
    if (rc == SXCL_JVM_OK && res.output_bytes > 0) {
        boot_log("证词文件 %lld 字节", (long long)res.output_bytes);
    }
    return rc;
}

int sxcl_android_jvm_selfcheck(const char *java_home, const char *native_lib_dir,
                               const char *android_version, const char *capture_path, int *ok_out,
                               char *version_out, size_t version_len, char *home_out,
                               size_t home_len, char *jli_out, size_t jli_len)
{
    sxcl_jvm_opts opts;
    sxcl_jvm_env env;
    sxcl_jvm_result res;
    char err[SXCL_JVM_ERROR_MAX];
    const char *extra[2];
    int rc;

    if (ok_out != NULL) {
        *ok_out = 0;
    }
    if (version_out != NULL && version_len > 0) {
        version_out[0] = '\0';
    }
    if (home_out != NULL && home_len > 0) {
        home_out[0] = '\0';
    }
    if (jli_out != NULL && jli_len > 0) {
        jli_out[0] = '\0';
    }
    if (capture_path == NULL || capture_path[0] == '\0') {
        boot_log("自检必须给证词文件路径");
        return SXCL_JVM_ERR_ARG;
    }
    extra[0] = (native_lib_dir != NULL && native_lib_dir[0] != '\0') ? native_lib_dir : NULL;
    extra[1] = NULL;
    memset(&opts, 0, sizeof(opts));
    opts.java_home = java_home;
    opts.extra_lib_dirs = extra;
    opts.android_version = android_version;
    opts.apply_env = 1;
    opts.preload_libs = 1;
    err[0] = '\0';
    if (sxcl_jvm_build_env(&opts, &env, err, sizeof(err)) != SXCL_JVM_OK) {
        boot_log("算环境变量失败: %s", err);
        return SXCL_JVM_ERR_ARG;
    }
    (void)boot_update_ld_path(env.ld_library_path);
    memset(&res, 0, sizeof(res));
    rc = sxcl_jvm_selfcheck(&opts, capture_path, &res);
    boot_log("自举自检: %s(%s)", sxcl_jvm_code_name(rc), res.error);
    boot_log("  java.version = %s", res.java_version[0] ? res.java_version : "(没拿到)");
    boot_log("  java.home    = %s", res.reported_home[0] ? res.reported_home : "(没拿到)");
    boot_log("  java.home 与我们要的一致: %s", res.home_matches ? "是" : "否");
    boot_log("  原始输出: %s(%lld 字节)", capture_path, (long long)res.output_bytes);
    if (ok_out != NULL) {
        *ok_out = (rc == SXCL_JVM_OK) ? 1 : 0;
    }
    if (version_out != NULL && version_len > 0) {
        snprintf(version_out, version_len, "%s", res.java_version);
    }
    if (home_out != NULL && home_len > 0) {
        snprintf(home_out, home_len, "%s", res.reported_home);
    }
    if (jli_out != NULL && jli_len > 0) {
        snprintf(jli_out, jli_len, "%s", res.jli_path);
    }
    return rc;
}
