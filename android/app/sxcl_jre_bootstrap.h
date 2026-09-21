/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_jre_bootstrap.h - 安卓侧"进程内起 JVM"的**薄适配层**(Android only)。
 *
 * 分工(2026-09-21 收敛后的唯一一份实现):
 *   * 核心库 include/sxcl/jvm.h + src/services/launch/jvm.c(**跨平台,单一份实现**):
 *       算环境变量(JAVA_HOME / LD_LIBRARY_PATH / TMPDIR / HOME / java.library.path)、
 *       拼 JLI_Launch 的 argv(-Djava.home / -cp / -Djava.library.path / -Dos.version …)、
 *       预加载 <home>/lib 与 <home>/lib/server、dlopen(libjli.so) + JLI_Launch、输出留证。
 *     **环境变量与 JVM 参数的拼法只许有这一份** —— 以前漂移过(tls 部署、设置路径都吃过亏)。
 *   * 本文件只保留**安卓/游戏进程特有**的三件事:
 *       1) 崩溃兜底(pipe + 看门线程;信号处理器只写管道,看门线程回调 fatal 钩子后 _exit(128+sig))
 *          —— 进程内起 JVM 和宿主同生共死,崩了必须留下"信号号"再退;
 *       2) 日志出口钩子(游戏进程把它接到 文件 + 通道 + logcat 三条路上);
 *       3) 有序收尾里的 JVM 部分(sxcl_jre_request_shutdown;没有 JVM 时是空操作)。
 *
 * 调用方:android/app/sxcl_jre_probe.c(诊断探针)与 android/app/sxcl_game.c(:game 游戏进程)。
 * 纪律:只在**显式请求**时被调用(boot 文件里有 jreprobe=,或游戏会话给了 JRE 路径)。
 */

#ifndef SXCL_JRE_BOOTSTRAP_H
#define SXCL_JRE_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* 日志出口。默认 __android_log_print(ANDROID_LOG_INFO, "sxcl") + stderr;
 * 游戏进程把它改成"同时写自己的日志文件 + 通道",主线一行都不用改。 */
typedef void (*sxcl_jre_log_fn)(void *ud, const char *line);

/* 致命信号钩子。**在看门线程里执行**(不是信号处理器里),记完日志就 _exit(128+sig)。
 * 游戏进程用它在死之前把"信号号 + 退出码"写进通道与退出记录文件。 */
typedef void (*sxcl_jre_fatal_fn)(void *ud, int sig);

typedef struct sxcl_jre_launch_opts {
    const char *java_home;        /* <jre> 根目录(必填) */
    const char *native_lib_dir;   /* APK 的 nativeLibraryDir(可空) */
    const char *class_path;       /* 游戏 classpath(可空)-> -cp */
    const char *main_class;       /* 主类;NULL/空 = 不跑主类(只做自检) */
    const char *const *args;      /* JVM 参数(主类之前) */
    int arg_count;
    const char *const *app_args;  /* 主类之后的参数(游戏的 args) */
    int app_arg_count;
    const char *android_version;  /* 系统版本串(如 "16");非空 -> -Dos.version=Android-<它> */
    const char *report_path;      /* 落盘报告(可空) */
    const char *log_prefix;       /* 日志前缀,NULL -> "[jre-probe]" */
    const int *fatal_signals;     /* 要跟踪的致命信号;NULL -> SIGABRT/SIGBUS/SIGILL/SIGFPE */
    int fatal_signal_count;
} sxcl_jre_launch_opts;

/* 钩子必须在 sxcl_jre_launch() 之前装好(进程级,一个进程只起一个 JVM)。 */
void sxcl_jre_set_log_hook(sxcl_jre_log_fn fn, void *ud);
void sxcl_jre_set_fatal_hook(sxcl_jre_fatal_fn fn, void *ud);
void sxcl_jre_set_log_prefix(const char *prefix);

/* 打一行日志(前缀 + 钩子/默认出口)。游戏进程自己的状态行也走这里,口径统一。 */
void sxcl_jre_logf(const char *fmt, ...);

/* 装崩溃兜底(pipe + 看门线程)。
 *   signals/count: 要跟踪的致命信号;NULL/0 -> SIGABRT/SIGBUS/SIGILL/SIGFPE
 *   clear_all:     1 = 先把 SIGHUP..NSIG 全部清成 SIG_DFL(SIGSEGV 用 SIG_IGN)。
 *                  核心库起 JVM 前不能有别人占着信号处理器(HotSpot 会自己装),
 *                  所以真要起 JVM 时传 1;游戏进程在没有 JRE 时传 0(ART 原样不动)。
 * 可以重复调用:管道与看门线程只建一次;clear_all=1 时每次都会重新清一遍并重装。 */
void sxcl_jre_install_guard(const int *signals, int count, int clear_all);

/* 进程内起 JVM(环境变量 + argv + dlopen + JLI_Launch 全在核心库 sxcl/jvm.c)。
 * 返回:>=0 = JLI_Launch 的返回码;负 = 核心库的 SXCL_JVM_ERR_*(见 sxcl/jvm.h)。 */
int sxcl_jre_launch(const sxcl_jre_launch_opts *opts);

/* 这个进程里到底起过 JVM 没有(0 = 没起过,收尾时只 flush,不做 DestroyJavaVM)。 */
int sxcl_jre_was_started(void);

/* JVM 是否已经返回(JLI_Launch 回来了)。 */
int sxcl_jre_has_returned(void);

/* 有序收尾里的 JVM 部分:
 *   返回 0 = 这个进程没有活着的 JVM(调用方直接走 flush 路径);
 *   返回 1 = 已请求 JVM 结束(辅助线程 DestroyJavaVM;等 JLI_Launch 返回即可)。
 * 注意:真机上"有 JRE 时"这条路要等接了游戏主类才走得到,本轮的收尾是 flush 路径。 */
int sxcl_jre_request_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SXCL_JRE_BOOTSTRAP_H */
