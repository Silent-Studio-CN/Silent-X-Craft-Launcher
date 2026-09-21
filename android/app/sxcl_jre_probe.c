/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_jre_probe.c - 应用域内的"进程内起 JVM"自举探针(Android only,诊断用)。
 *
 * 为什么需要它:真机实测(2026-09-21,docs/18 §4.2)证明应用自己的 untrusted_app 域**不能**
 * exec 私有目录里的文件(SELinux avc denied { execute_no_trans },permissive=0),
 * 所以 `fork+exec <jre>/bin/java` 这条路是死的。剩下的唯一路线是**进程内**把 JVM 起来:
 *
 *     dlopen("<jre>/lib/libjli.so") -> dlsym("JLI_Launch") -> 调用
 *
 * 这正是 FCL/PojavLauncher 的做法(dlopen + JLI_Launch,全仓没有一处 exec 过 java)。
 *
 * 本轮(安卓游戏独立进程)把真正干活的那套抽成了 android/app/sxcl_jre_bootstrap.c:
 * 探针与 :game 游戏进程**共用同一份**(LD_LIBRARY_PATH 的补法、JLI_Launch 的调用约定、
 * 崩溃兜底,只要有一份走偏就会以"JVM 起不来"的形式表现出来且极难查)。这里只剩入口。
 *
 * 纪律:本探针只在**显式请求**时跑(boot 文件里有 `jreprobe=`),正常启动一个字节都不动。
 */

#include "sxcl_jre_bootstrap.h"

#include <string.h>

int sxcl_android_jre_bootstrap_probe(const char *java_home, const char *native_lib_dir,
                                     const char *report_path)
{
    sxcl_jre_launch_opts opts;

    (void)memset(&opts, 0, sizeof(opts));
    opts.java_home = java_home;
    opts.native_lib_dir = native_lib_dir;
    opts.report_path = report_path;
    opts.log_prefix = "[jre-probe]";
    opts.class_path = NULL;
    opts.args = NULL;
    opts.arg_count = 0;      /* 0 = 探针参数(-XshowSettings:properties -version) */
    opts.app_args = NULL;
    opts.app_arg_count = 0;
    opts.android_version = NULL; /* 适配层自己读 ro.build.version.release */
    opts.main_class = NULL;  /* 探针不跑主类 */
    opts.fatal_signals = NULL; /* 默认跟踪 SIGABRT/SIGBUS/SIGILL/SIGFPE */
    opts.fatal_signal_count = 0;
    return sxcl_jre_launch(&opts);
}
