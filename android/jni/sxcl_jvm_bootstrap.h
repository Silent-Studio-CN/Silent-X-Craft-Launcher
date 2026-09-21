/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_JNI_JVM_BOOTSTRAP_H
#define SXCL_JNI_JVM_BOOTSTRAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 安卓侧的进程内 JVM 自举(对 include/sxcl/jvm.h 的一层薄封装) ──
 *
 * **标注:诊断用,不进 APK 构建目标、不随包发布**(对账结论见 docs/19 §8.2)。
 * 出货的自举层是 android/app/sxcl_jre_bootstrap.h → android/app/sxcl_jre_bootstrap.c
 * (被 sxclui 与 sxclgame 两个 .so 编进去);本文件只供 clang 单编的取证工具使用。
 * 两者不重复:env/argv/dlopen 的规则只有核心库 src/services/launch/jvm.c 那一份。
 *
 * 分工:
 *   * 核心库(include/sxcl/jvm.h + src/services/launch/jvm.c)**跨平台**:算环境变量、
 *     拼 argv、dlopen(libjli) -> JLI_Launch、重定向输出留证。桌面单测能覆盖它。
 *   * 本文件只做**安卓特有**的两件事:
 *       1) 在 dlopen 之前把 LD_LIBRARY_PATH 交给 linker 的私有入口
 *          (android_update_LD_LIBRARY_PATH / __loader_android_update_LD_LIBRARY_PATH)
 *          —— bionic 在进程启动时就把 LD_LIBRARY_PATH 读进去了,运行期只 setenv 不一定生效;
 *          拿不到这个符号时退回"RTLD_GLOBAL 预加载"(核心库里的 preload_libs 已经在做);
 *       2) 把关键结论打到 logcat(__android_log_print,与 docs/18 的诊断通路同一条)。
 *
 * 思路参考 Boardwalk / PojavLauncher 一系(FCL 也是 dlopen + JLI_Launch),
 * **代码为本项目自写**:这里没有任何一行是从 FCL 的源码里抄来的。
 *
 * 返回:0 = 成功;负数 = include/sxcl/jvm.h 的 SXCL_JVM_ERR_*。
 * for_version != 0 时只跑 -version + -XshowSettings:properties(自检),不跑游戏。
 */
int sxcl_android_jvm_bootstrap(const char *java_home, const char *native_lib_dir,
                               const char *tmp_dir, const char *user_home,
                               const char *android_version, const char *capture_path,
                               int for_version);

/** 只探"能不能进程内加载"(dlopen + dlsym,绝不起 JVM)。返回 0 / 负错误码。 */
int sxcl_android_jvm_probe(const char *java_home, const char *native_lib_dir,
                           const char *android_version);

/** 自举自检:起一次 JVM 打印版本与属性,原始输出写到 capture_path。
 *  ok_out / version_out / home_out / jli_out 都可空(能拿到就写)。
 *  返回 0 = 成功(-XshowSettings 的输出里确实有 java.version)。 */
int sxcl_android_jvm_selfcheck(const char *java_home, const char *native_lib_dir,
                               const char *android_version, const char *capture_path, int *ok_out,
                               char *version_out, size_t version_len, char *home_out,
                               size_t home_len, char *jli_out, size_t jli_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_JNI_JVM_BOOTSTRAP_H */
