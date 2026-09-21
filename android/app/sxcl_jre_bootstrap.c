/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_jre_bootstrap.c - 安卓侧"进程内起 JVM"的薄适配层(实现,见头文件的分工说明)。
 *
 * 这一层**故意很薄**:环境变量怎么算、argv 怎么拼、怎么 dlopen、怎么给 -Djava.home,
 * 全部在核心库(include/sxcl/jvm.h + src/services/launch/jvm.c)里,那才是唯一一份实现。
 * 这里只补三件核心库不该管的事:
 *   1) 崩溃兜底(进程内起 JVM 与宿主同生共死;信号 -> 看门线程 -> fatal 钩子 + _exit(128+sig));
 *   2) 日志出口钩子(游戏进程接到 文件 + 通道 + logcat);
 *   3) 有序收尾里的 JVM 部分(DestroyJavaVM 辅助线程;没有 JVM 时是空操作)。
 *
 * 历史:这一层原来自己写过一遍 env/argv/dlopen(docs/18 §4 的探针就是它跑出来的)。
 * 2026-09-21 核心库补齐 sxcl/jvm.h 之后**收敛成现在这样** —— 两份实现必然漂移。
 */

#include "sxcl_jre_bootstrap.h"

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "sxcl/jvm.h"

#define SXCL_JRE_LOG_TAG "sxcl"

/* ── 进程级状态(一个进程只起一个 JVM) ── */

static sxcl_jre_log_fn g_log_fn = NULL;
static void *g_log_ud = NULL;
static sxcl_jre_fatal_fn g_fatal_fn = NULL;
static void *g_fatal_ud = NULL;
static char g_prefix[48] = "[jre-probe]";
static volatile sig_atomic_t g_jli_started = 0;
static volatile sig_atomic_t g_jli_returned = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;

void sxcl_jre_set_log_hook(sxcl_jre_log_fn fn, void *ud)
{
    g_log_fn = fn;
    g_log_ud = ud;
}

void sxcl_jre_set_fatal_hook(sxcl_jre_fatal_fn fn, void *ud)
{
    g_fatal_fn = fn;
    g_fatal_ud = ud;
}

void sxcl_jre_set_log_prefix(const char *prefix)
{
    if (prefix == NULL || prefix[0] == '\0')
        return;
    (void)snprintf(g_prefix, sizeof(g_prefix), "%s", prefix);
}

void sxcl_jre_logf(const char *fmt, ...)
{
    char msg[1024];
    char line[1200];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (void)snprintf(line, sizeof(line), "%s %s", g_prefix, msg);

    if (g_log_fn != NULL) {
        g_log_fn(g_log_ud, line);
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, SXCL_JRE_LOG_TAG, "%s", line);
    (void)fprintf(stderr, "%s\n", line);
    (void)fflush(stderr);
}

/* ── 崩溃兜底:信号 -> 看门线程回调 fatal 钩子 + 打日志 + _exit(128+sig) ── */

static int g_sig_pipe[2] = {-1, -1};

static void sxcl_jre_signal_handler(int sig)
{
    const int saved = errno;
    if (g_sig_pipe[1] >= 0) {
        const int rc = (int)write(g_sig_pipe[1], &sig, sizeof(sig));
        (void)rc;
    }
    /* 不再往下走:JVM 的崩溃现场已经不可信,交给看门线程收尸 */
    for (;;) {
        (void)pause();
    }
    errno = saved;
}

static void *sxcl_jre_signal_waiter(void *arg)
{
    int sig = 0;
    (void)arg;
    const ssize_t n = read(g_sig_pipe[0], &sig, sizeof(sig));
    if (n == (ssize_t)sizeof(sig)) {
        /* 先让宿主把"信号号 + 退出码"记到可靠的地方(游戏进程:通道 + 退出记录文件)。
         * 这一步比日志重要:日志可能还没 flush,而对端要靠这条记录才能报出真信号。 */
        if (g_fatal_fn != NULL)
            g_fatal_fn(g_fatal_ud, sig);
        sxcl_jre_logf("FATAL: 进程内 JVM 收到信号 %d -> 记日志后退出(退出码 %d)", sig, 128 + sig);
    } else {
        sxcl_jre_logf("FATAL: 看门线程被异常唤醒(n=%d)", (int)n);
    }
    _exit(128 + sig);
    return NULL;
}

static const char *sxcl_jre_signal_name(int sig)
{
    switch (sig) {
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGSEGV: return "SIGSEGV";
    case SIGTERM: return "SIGTERM";
    case SIGINT:  return "SIGINT";
    case SIGHUP:  return "SIGHUP";
    case SIGQUIT: return "SIGQUIT";
    case SIGPIPE: return "SIGPIPE";
    case SIGSYS:  return "SIGSYS";
    case SIGTRAP: return "SIGTRAP";
    default:      return "SIG?";
    }
}

void sxcl_jre_install_guard(const int *signals, int count, int clear_all)
{
    struct sigaction sa;
    static const int kDefault[] = {SIGABRT, SIGBUS, SIGILL, SIGFPE};
    size_t i = 0;

    if (signals == NULL || count <= 0) {
        signals = kDefault;
        count = (int)(sizeof(kDefault) / sizeof(kDefault[0]));
    }
    if (g_sig_pipe[0] < 0) {
        if (pipe(g_sig_pipe) != 0) {
            sxcl_jre_logf("崩溃兜底装不上(pipe 失败 errno=%d),继续但无保护", errno);
            return;
        }
        {
            pthread_t tid;
            if (pthread_create(&tid, NULL, sxcl_jre_signal_waiter, NULL) == 0) {
                (void)pthread_detach(tid);
            } else {
                sxcl_jre_logf("崩溃兜底:看门线程起不来");
            }
        }
    }
    /* clear_all:起 JVM 前必须把信号处理器清干净(HotSpot 要自己装;
     * SIGSEGV 用 SIG_IGN,安卓对 SIG_DFL 有意见)。只在真的要起 JVM 时做。 */
    if (clear_all) {
        (void)memset(&sa, 0, sizeof(sa));
        for (int sig = SIGHUP; sig < NSIG; ++sig) {
            sa.sa_handler = (sig == SIGSEGV) ? SIG_IGN : SIG_DFL;
            (void)sigaction(sig, &sa, NULL);
        }
    }
    for (i = 0; i < (size_t)count; ++i) {
        (void)memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sxcl_jre_signal_handler;
        (void)sigaction(signals[i], &sa, NULL);
    }
    {
        char names[160];
        size_t used = 0;
        names[0] = '\0';
        for (i = 0; i < (size_t)count && used + 12u < sizeof(names); ++i) {
            used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%s", i ? "/" : "",
                                     sxcl_jre_signal_name(signals[i]));
        }
        sxcl_jre_logf("崩溃兜底就绪(跟踪 %s;SIGSEGV 留给 JVM)", names);
    }
}

/* ── JVM 收尾(没有 JVM 时是空操作) ── */

int sxcl_jre_was_started(void) { return (int)g_jli_started; }
int sxcl_jre_has_returned(void) { return (int)g_jli_returned; }

int sxcl_jre_wait_returned(int timeout_ms)
{
    int waited = 0;
    while (!g_jli_returned && waited < timeout_ms) {
        (void)usleep(50 * 1000);
        waited += 50;
    }
    return (int)g_jli_returned;
}

static void *sxcl_jre_shutdown_thread(void *arg)
{
    typedef jint (*get_vms_t)(JavaVM **, jsize, jsize *);
    get_vms_t get_vms = NULL;
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    jsize count = 0;
    (void)arg;

    get_vms = (get_vms_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (get_vms == NULL) {
        sxcl_jre_logf("JVM 收尾:拿不到 JNI_GetCreatedJavaVMs(dlerror=%s),改由调用方 flush 收尾",
                      dlerror() != NULL ? dlerror() : "(null)");
        return NULL;
    }
    if (get_vms(&vm, 1, &count) != JNI_OK || count < 1 || vm == NULL) {
        sxcl_jre_logf("JVM 收尾:JNI_GetCreatedJavaVMs 没给出 JVM,改由调用方 flush 收尾");
        return NULL;
    }
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
        sxcl_jre_logf("JVM 收尾:AttachCurrentThread 失败,改由调用方 flush 收尾");
        return NULL;
    }
    sxcl_jre_logf("JVM 收尾:DestroyJavaVM() 开始(等所有非守护线程结束)");
    (void)(*vm)->DestroyJavaVM(vm);
    sxcl_jre_logf("JVM 收尾:DestroyJavaVM() 已返回");
    return NULL;
}

int sxcl_jre_request_shutdown(void)
{
    pthread_t tid;
    if (!g_jli_started)
        return 0; /* 这个进程里根本没起过 JVM:收尾只剩 flush */
    if (g_jli_returned)
        return 1;
    if (g_shutdown_requested)
        return 1;
    g_shutdown_requested = 1;
    if (pthread_create(&tid, NULL, sxcl_jre_shutdown_thread, NULL) == 0) {
        (void)pthread_detach(tid);
        return 1;
    }
    sxcl_jre_logf("JVM 收尾:辅助线程起不来,改由调用方 flush 收尾");
    return 0;
}

/* ── 主体:把参数交给核心库,自己只留日志与兜底 ── */

int sxcl_jre_launch(const sxcl_jre_launch_opts *opts)
{
    sxcl_jvm_opts jo;
    sxcl_jvm_env env;
    sxcl_jvm_result res;
    char err[SXCL_JVM_ERROR_MAX];
    const char *extra[2];
    const char *jvm_args[SXCL_JVM_ARG_MAX];
    const char *app_args[SXCL_JVM_ARG_MAX];
    char android_release[PROP_VALUE_MAX];
    const char *android_version = NULL;
    int i = 0;
    int rc = 0;
    int for_version = 0;
    FILE *report = NULL;

    if (opts == NULL || opts->java_home == NULL || opts->java_home[0] == '\0') {
        sxcl_jre_logf("自举没拿到 JRE 路径,放弃");
        return SXCL_JVM_ERR_NO_HOME;
    }
    if (opts->arg_count < 0 || opts->app_arg_count < 0 ||
        opts->arg_count >= SXCL_JVM_ARG_MAX || opts->app_arg_count >= SXCL_JVM_ARG_MAX) {
        sxcl_jre_logf("自举参数太多(arg=%d app=%d,上限 %d),放弃", opts->arg_count,
                      opts->app_arg_count, SXCL_JVM_ARG_MAX - 1);
        return SXCL_JVM_ERR_ARG;
    }

    /* 计数数组 -> 核心库要的 NULL 结尾数组 */
    for (i = 0; i < opts->arg_count; ++i)
        jvm_args[i] = opts->args[i];
    jvm_args[i] = NULL;
    for (i = 0; i < opts->app_arg_count; ++i)
        app_args[i] = opts->app_args[i];
    app_args[i] = NULL;
    extra[0] = (opts->native_lib_dir != NULL && opts->native_lib_dir[0] != '\0')
                   ? opts->native_lib_dir : NULL;
    extra[1] = NULL;

    for_version = opts->game_argv == NULL &&
                  (opts->main_class == NULL || opts->main_class[0] == '\0') &&
                  opts->arg_count == 0 && opts->app_arg_count == 0;

    /* 系统版本串(ro.build.version.release,与 Java 的 Build.VERSION.RELEASE 同一个属性):
     * 交给核心库拼 -Dos.version=Android-<它>。调用方没给就自己读,免得两个调用点各写一遍。 */
    android_release[0] = '\0';
    {
        const char *av = opts->android_version;
        if (av == NULL || av[0] == '\0') {
            if (__system_property_get("ro.build.version.release", android_release) > 0)
                av = android_release;
        }
        if (av == NULL)
            av = "";
        android_version = av;
    }

    sxcl_jre_logf("=== 进程内 JVM 自举开始(环境变量与 argv 由核心库 sxcl/jvm.c 算) ===");
    sxcl_jre_logf("java_home=%s nativeLibraryDir=%s classpath=%s main=%s jvm-args=%d app-args=%d "
                  "android=%s 自检模式=%d",
                  opts->java_home, extra[0] != NULL ? extra[0] : "(空)",
                  (opts->class_path != NULL && opts->class_path[0] != '\0') ? "(给了)" : "(空)",
                  (opts->main_class != NULL && opts->main_class[0] != '\0') ? opts->main_class
                                                                             : "(无)",
                  opts->arg_count, opts->app_arg_count,
                  android_version[0] != '\0' ? android_version : "(空)", for_version);
    if (opts->game_argv != NULL) {
        int n = 0;
        while (opts->game_argv[n] != NULL)
            ++n;
        sxcl_jre_logf("游戏命令行:核心库拼好的 %d 个参数原样交给 jvm 层(classpath/主类/游戏参数都在里面)",
                      n);
    }

    /* 崩溃兜底必须在起 JVM **之前**装好:进程内 JVM 崩了要留下信号号再退 */
    sxcl_jre_install_guard(opts->fatal_signals, opts->fatal_signal_count, 1);

    (void)memset(&jo, 0, sizeof(jo));
    (void)memset(&env, 0, sizeof(env));
    err[0] = '\0';
    jo.java_home = opts->java_home;
    jo.extra_lib_dirs = extra;
    jo.android_version = android_version[0] != '\0' ? android_version : NULL;
    /* 有整条游戏命令行时(classpath/主类/游戏参数都在里面):原样传给 jvm 层当 extra_args,
     * 不再单独设 class_path/main_class/app_args —— 同一件事不许拼两遍。 */
    jo.class_path = opts->game_argv != NULL ? NULL : opts->class_path;
    jo.main_class = opts->game_argv != NULL ? NULL : opts->main_class;
    jo.extra_args = opts->game_argv != NULL ? opts->game_argv : (const char *const *)jvm_args;
    jo.app_args = opts->game_argv != NULL ? NULL : (const char *const *)app_args;
    jo.java_library_path = opts->java_library_path;
    jo.apply_env = 1;
    jo.preload_libs = 1;
    jo.capture_path = NULL; /* 日志统一走本层的三个出口(游戏进程:文件 + 通道 + logcat) */

    /* 先把环境变量的结论算出来打一行(算法在核心库,这里只是取证) */
    rc = sxcl_jvm_build_env(&jo, &env, err, sizeof(err));
    if (rc != SXCL_JVM_OK) {
        sxcl_jre_logf("核心库算环境变量失败:%s(%s)", sxcl_jvm_code_name(rc), err);
    } else {
        sxcl_jre_logf("JAVA_HOME=%s", env.java_home);
        sxcl_jre_logf("LD_LIBRARY_PATH=%s", env.ld_library_path);
        sxcl_jre_logf("TMPDIR=%s HOME=%s", env.tmp_dir, env.home);
        sxcl_jre_logf("java.library.path=%s", env.java_library_path);
    }

    (void)memset(&res, 0, sizeof(res));
    g_jli_started = 1;
    rc = sxcl_jvm_launch(&jo, for_version, &res);
    g_jli_returned = 1;
    if (rc == SXCL_JVM_OK) {
        sxcl_jre_logf("JLI_Launch 正常返回 rc=%d(libjli=%s)", res.jli_rc, res.jli_path);
    } else {
        sxcl_jre_logf("自举失败:%s(rc=%d jli_rc=%d)%s%s", sxcl_jvm_code_name(rc), rc, res.jli_rc,
                      res.error[0] != '\0' ? " / " : "", res.error);
    }
    sxcl_jre_logf("=== 进程内 JVM 自举结束(宿主进程继续活着)===");

    if (opts->report_path != NULL && opts->report_path[0] != '\0') {
        report = fopen(opts->report_path, "wb");
        if (report != NULL) {
            (void)fprintf(report,
                          "stage=done\nresult=%s\nrc=%d\njli_rc=%d\njava_home=%s\n"
                          "libjli=%s\nld_paths=%s\ntmp=%s\nhome=%s\njava_library_path=%s\n"
                          "java_version=%s\nreported_home=%s\nselfcheck=%d\nerror=%s\n",
                          rc == SXCL_JVM_OK ? "ok" : "fail", rc, res.jli_rc, opts->java_home,
                          res.jli_path, env.ld_library_path, env.tmp_dir, env.home,
                          env.java_library_path, res.java_version, res.reported_home, for_version,
                          res.error);
            (void)fclose(report);
        }
    }
    return rc == SXCL_JVM_OK ? res.jli_rc : rc;
}
