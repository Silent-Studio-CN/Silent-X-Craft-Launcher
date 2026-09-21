/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_JVM_H
#define SXCL_JVM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 进程内起 JVM(dlopen libjli.so + JLI_Launch) ──
 *
 * 为什么只能这么做:安卓应用自己的 untrusted_app 域**不许 exec 私有目录里的文件**
 * (SELinux avc denied { execute_no_trans },permissive=0;真机原文见 docs/18 §4.2),
 * 所以 fork+exec <jre>/bin/java 这条路是死的。剩下的活路是**进程内**把 JVM 拉起来:
 *
 *     dlopen("<jre>/lib/libjli.so") -> dlsym("JLI_Launch") -> 调用
 *
 * **思路**参考 Boardwalk / PojavLauncher 一系(FCL 走的也是同一条),**代码为本项目自写**:
 * 本文件与 src/services/launch/jvm.c 没有任何一行是从 FCL/Pojav 抄来的文本,只有那一条
 * "dlopen + JLI_Launch"的公开做法,以及 OpenJDK launcher(java.c)里 JLI_Launch 的**公开签名**。
 *
 * 起 JVM 之前必须补的东西(少一样就起不来,逐条都踩过):
 *   * 环境变量:JAVA_HOME、LD_LIBRARY_PATH(含 <home>/lib 与 <home>/lib/server,
 *     再加上 APK 的 nativeLibraryDir —— 那份安卓 JRE 的 bin/java 与 libjli.so 都**没有
 *     RUNPATH**,libc++_shared.so 只在 APK 里)、TMPDIR、HOME;
 *   * 系统属性:-Djava.io.tmpdir、-Duser.home、-Dos.name=Linux、
 *     -Dos.version=Android-<版本>、-Djava.library.path;
 *   * 进程内 dlopen 时 /proc/self/exe 是**我们的 APK**,JLI 推不出 java.home,
 *     所以 -Djava.home=<home> 必须显式给,argv[0] 也要给成 <home>/bin/java(它靠这个
 *     算 application home;那个文件是解出来的真文件,存在就行,**不需要可执行**);
 *   * **绝不能加 -XstartOnFirstThread** —— 那是 macOS 专用的;安卓上加了会直接起不来。
 *     sxcl_jvm_build_args() 会把调用方误传的它**丢掉**并在结果里记一笔,而不是照抄进去。
 *
 * 不承诺的事(如实写下来):
 *   * 只做"起一个 JVM 并调 main"这一层,不做 UI/事件循环/Pojav 那套 surface 桥;
 *   * JLI_Launch 在失败路径上**可能直接 exit()**(OpenJDK 的实现如此),进程内调用要接受这一点;
 *   * Linux/macOS 上同一份代码也能 dlopen 起 JVM(便于桌面自检);Windows 走 bin\java.dll,
 *     但 Windows 本来就该直接 exec,这条路只是"能用而已"。
 */

#define SXCL_JVM_OK               0
#define SXCL_JVM_ERR_ARG        (-1) /**< 参数不合法 */
#define SXCL_JVM_ERR_NO_HOME    (-2) /**< java_home 没给 / 不存在 */
#define SXCL_JVM_ERR_NO_JLI     (-3) /**< 找不到 libjli.so(或 Windows 的 bin\java.dll) */
#define SXCL_JVM_ERR_DLOPEN     (-4) /**< dlopen 失败(人话里带 dlerror) */
#define SXCL_JVM_ERR_DLSYM      (-5) /**< dlsym(JLI_Launch) 失败 */
#define SXCL_JVM_ERR_LAUNCH     (-6) /**< JLI_Launch 返回了非 0 */
#define SXCL_JVM_ERR_UNSUPPORTED (-7)/**< 这个平台没有 dlopen 这条路 */
#define SXCL_JVM_ERR_NOMEM      (-8) /**< 内存不足 */
#define SXCL_JVM_ERR_IO         (-9) /**< 输出重定向/写证词文件失败 */
#define SXCL_JVM_ERR_MISSING   (-10) /**< 自检没拿到 java.version / java.home(输出不对) */

#define SXCL_JVM_PATH_MAX  1024
#define SXCL_JVM_LDPATH_MAX 3072
#define SXCL_JVM_ARG_MAX     40
#define SXCL_JVM_ARG_LEN   2048
#define SXCL_JVM_ERROR_MAX   512
#define SXCL_JVM_VALUE_MAX   512

/** 返回码的稳定名字("ok"/"arg"/"no_home"/"no_jli"/"dlopen"/"dlsym"/"launch"/"unsupported"/
 *  "nomem"/"io"/"missing")。 */
const char *sxcl_jvm_code_name(int code);

/** 一次启动的参数(字符串生命周期:调用期间不得释放)。 */
typedef struct sxcl_jvm_opts {
    const char *java_home;              /**< 必需,<home>/bin/java 得在 */
    /** 追加到 LD_LIBRARY_PATH 的目录(安卓上就是 APK 的 nativeLibraryDir),NULL 结尾,可空。 */
    const char *const *extra_lib_dirs;
    const char *tmp_dir;                /**< 空 = <home>/tmp(会被建出来) */
    const char *user_home;              /**< 空 = <home>/home(会被建出来) */
    /** 安卓版本号(如 "16"):非空时加 -Dos.version=Android-<它>;空则不加这一条。 */
    const char *android_version;
    /** java.library.path;空 = <home>/lib:<home>/lib/server:<extra_lib_dirs…>。 */
    const char *java_library_path;
    const char *class_path;             /**< 可空 */
    const char *main_class;             /**< 可空(只跑 -version 自检时不给) */
    const char *const *extra_args;      /**< 追加的 JVM 参数,NULL 结尾,可空 */
    const char *const *app_args;        /**< 主类参数,NULL 结尾,可空 */
    /** 1(默认)= 真的 setenv 上面那四个环境变量;0 = 只算不设(测试用)。 */
    int apply_env;
    /** 1(默认)= dlopen 之前把 <home>/lib 与 <home>/lib/server 里的 .so 用 RTLD_GLOBAL
     *  预加载一遍(bionic 的 LD_LIBRARY_PATH 是启动时读的,setenv 不一定来得及)。 */
    int preload_libs;
    /** 非空 = 把 stdout/stderr 重定向到该文件(自检留证);返回前恢复。 */
    const char *capture_path;
} sxcl_jvm_opts;

/** 算出来的环境变量(纯数据,方便单测钉住)。 */
typedef struct sxcl_jvm_env {
    char java_home[SXCL_JVM_PATH_MAX];
    char ld_library_path[SXCL_JVM_LDPATH_MAX];
    char tmp_dir[SXCL_JVM_PATH_MAX];
    char home[SXCL_JVM_PATH_MAX];
    char java_library_path[SXCL_JVM_LDPATH_MAX];
} sxcl_jvm_env;

/** 纯函数:按 opts 算环境变量(不碰进程环境)。失败返回负错误码并写 err。 */
int sxcl_jvm_build_env(const sxcl_jvm_opts *opts, sxcl_jvm_env *out, char *err, size_t err_len);

/** 把 env 真的设进当前进程(setenv/_putenv_s)。返回 SXCL_JVM_OK / 负错误码。 */
int sxcl_jvm_apply_env(const sxcl_jvm_env *env, char *err, size_t err_len);

/** 算出来的参数向量(argv[0] = <home>/bin/java)。 */
typedef struct sxcl_jvm_args {
    size_t count;
    const char *argv[SXCL_JVM_ARG_MAX];             /**< argv[count] = NULL(方便直接传) */
    char storage[SXCL_JVM_ARG_MAX][SXCL_JVM_ARG_LEN];
    int dropped_start_on_first_thread;              /**< 1 = 丢掉了调用方误传的 -XstartOnFirstThread */
} sxcl_jvm_args;

/** 纯函数:拼 JLI_Launch 的 argv。for_version=1 时加 -XshowSettings:properties -version。
 *  绝不把 -XstartOnFirstThread 放进去(安卓上加了会起不来),误传的会被丢掉并记一笔。 */
int sxcl_jvm_build_args(const sxcl_jvm_opts *opts, const sxcl_jvm_env *env, int for_version,
                        sxcl_jvm_args *out, char *err, size_t err_len);

/** 一次启动的结果。 */
typedef struct sxcl_jvm_result {
    int code;
    int jli_rc;                          /**< JLI_Launch 的返回码(JLI 没跑到就是 0) */
    char java_home[SXCL_JVM_PATH_MAX];
    char jli_path[SXCL_JVM_PATH_MAX];    /**< 实际 dlopen 的那个文件 */
    char ld_library_path[SXCL_JVM_LDPATH_MAX];
    char tmp_dir[SXCL_JVM_PATH_MAX];
    char user_home[SXCL_JVM_PATH_MAX];
    char os_version[SXCL_JVM_VALUE_MAX]; /**< 传进去的 Android-<版本>(可能空) */
    char output_path[SXCL_JVM_PATH_MAX]; /**< 原始输出落在哪(没重定向就是空) */
    int64_t output_bytes;
    char java_version[SXCL_JVM_VALUE_MAX]; /**< 从原始输出里抠出来的 java.version */
    char reported_home[SXCL_JVM_VALUE_MAX];/**< 从原始输出里抠出来的 java.home */
    int home_matches;                    /**< 1 = 输出里的 java.home 与我们要的一致 */
    char error[SXCL_JVM_ERROR_MAX];
} sxcl_jvm_result;

/** 只做 dlopen + dlsym(绝不起 JVM):探"这份 JRE 能不能被进程内加载"。
 *  副作用仅限:算出环境变量并按 opts->apply_env 设定;预加载若干 .so。 */
int sxcl_jvm_probe(const sxcl_jvm_opts *opts, sxcl_jvm_result *out);

/** 真的调 JLI_Launch。自检请用 sxcl_jvm_selfcheck()。 */
int sxcl_jvm_launch(const sxcl_jvm_opts *opts, int for_version, sxcl_jvm_result *out);

/** 自举自检:起一次 JVM 只打印版本与属性(-XshowSettings:properties -version),
 *  把**原始输出**写到 capture_path(必须非空,这是"留证"),再从输出里抠出
 *  java.version 与 java.home 填进结果。返回 SXCL_JVM_OK / 负错误码。 */
int sxcl_jvm_selfcheck(const sxcl_jvm_opts *opts, const char *capture_path, sxcl_jvm_result *out);

/** 在一段文本里找 "key = value"(属性输出形如 "    java.version = 17.0.9"),
 *  把 value 写进 out。找到返回 0,没找到返回 -1。 */
int sxcl_jvm_scan_property(const char *text, const char *key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_JVM_H */
