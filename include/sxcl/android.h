/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_ANDROID_H
#define SXCL_ANDROID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一个候选(Java 可执行文件 / 游戏目录 / 运行时目录)的可访问性结论。数值稳定,可直接落日志。 */
typedef enum sxcl_android_access {
    SXCL_ANDROID_OK = 0,         /**< 存在、可读;(要求可执行时)也真的能执行 */
    SXCL_ANDROID_MISSING,        /**< 路径不存在(ENOENT/ENOTDIR) */
    SXCL_ANDROID_DENIED,         /**< 存在但被沙箱拒绝(EACCES/EPERM)—— 别人的私有目录 */
    SXCL_ANDROID_NOEXEC,         /**< 在 noexec 文件系统上(共享存储):有 +x 也起不了进程 */
    SXCL_ANDROID_NOT_EXECUTABLE, /**< 可读,但没有执行位 */
    SXCL_ANDROID_NOT_READABLE,   /**< 存在但读不了(其它 IO 错误) */
    SXCL_ANDROID_ACCESS_COUNT    /**< 枚举计数(不是结论) */
} sxcl_android_access;

/** 结论的中文短名:"可用" / "不存在" / "沙箱拒绝" / "共享存储不能执行" / "没有执行位" / "读不了"。 */
const char *sxcl_android_access_name(sxcl_android_access access);

/** 结论的稳定英文键("ok"/"missing"/"denied"/"noexec"/"not_executable"/"unreadable"),
 *  给日志与测试断言用(界面显示用 sxcl_android_access_name)。 */
const char *sxcl_android_access_key(sxcl_android_access access);

/** 结论对应的一条人话建议(为什么 + 怎么办)。永远返回非空串。 */
const char *sxcl_android_access_hint(sxcl_android_access access);

/** 纯文本解析:在 /proc/self/mounts 的文本里,取**最长**前缀匹配的挂载点
 *  (嵌套挂载:/storage/emulated 要盖住 /),看它的选项里有没有 noexec。
 *  返回 1 = noexec,0 = 该挂载点可执行,-1 = 文本里找不到匹配的挂载点(或入参为空)。
 *  **不做**挂载点里的 \\040 这类八进制转义还原(安卓的挂载点里没有空格)。 */
int sxcl_android_noexec_in_mounts(const char *mounts_text, const char *path);

/** 读 /proc/self/mounts 再调 sxcl_android_noexec_in_mounts。非 Linux/Android 恒返回 -1。 */
int sxcl_android_mount_noexec(const char *path);

/** 体检一个路径(真的去 stat / access,只读)。
 *  want_exec != 0:除了"存在且可读",还要求它落在可执行的文件系统上**且**有执行位。
 *  reason(可空)写人话原因;reason_len 为 0 时不写。
 *  返回结论。挂载表当场读(等价于把 mounts_text 传 NULL)。 */
sxcl_android_access sxcl_android_probe_path(const char *path, int want_exec,
                                            char *reason, size_t reason_len);

/** 同上,但挂载表文本由调用方给(单测注入;传 NULL = 真的去读 /proc/self/mounts)。 */
sxcl_android_access sxcl_android_probe_path_with_mounts(const char *mounts_text, const char *path,
                                                        int want_exec,
                                                        char *reason, size_t reason_len);

/* ── JRE 侧共享库补齐(参考实现 FCL RuntimeUtils.patchJava 的语义) ── */

/** 打包层必须放进 APK 的 lib/<abi>/(= nativeLibraryDir)的两个 JRE 侧库,固定两条、顺序固定。 */
#define SXCL_ANDROID_JRE_LIB_COUNT 2
/* 名字写死是有意的:JVM 是按**文件名**从 <jre>/lib dlopen 它们的,少一个字符都加载不到。 */
/* 0 = "libawt_xawt.so"(AWT/X11 桩;安卓 arm64 的 JRE 资产里根本没有这个文件),
 * 1 = "libjsound.so"(OpenJDK javax.sound 核心 + OpenAL 后端;jre17/21/25 资产里也没有)。 */
const char *sxcl_android_jre_lib_name(int index);

#define SXCL_ANDROID_JRE_PATH_MAX 1024

/* 补齐结果(负数 = 失败,数值稳定可落日志)。 */
#define SXCL_ANDROID_JRE_OK             0
#define SXCL_ANDROID_JRE_ERR_ARG        (-1) /**< 参数不合法(空路径 / 太长) */
#define SXCL_ANDROID_JRE_ERR_NO_LIB_DIR (-2) /**< JRE 里既没有 <java_home>/jre/lib 也没有 <java_home>/lib */
#define SXCL_ANDROID_JRE_ERR_NO_SOURCE  (-3) /**< nativeLibraryDir 里缺源文件 = APK 打包坏了(硬失败) */
#define SXCL_ANDROID_JRE_ERR_COPY       (-4) /**< 拷贝/校验失败(磁盘满、权限) */

/** 结果的稳定名字("ok"/"arg"/"no_lib_dir"/"no_source"/"copy")。 */
const char *sxcl_android_jre_patch_code_name(int code);

/** 把 nativeLibraryDir 里的 libawt_xawt.so / libjsound.so 补进**已装好的** JRE。
 *
 *  为什么非做不可:安卓 arm64 的 JRE 资产里没有 libawt_xawt.so,jre17/21/25 连
 *  libjsound.so 也没有;JVM 启动时按名字从 <jre>/lib dlopen 它们,缺一个就是硬启动失败,
 *  而且运行时也补不回来(组件清单里没有这两个文件)。FCL 在装完 JRE 后做同一件事:
 *  RuntimeUtils.patchJava()(FCL/src/main/java/com/tungsten/fcl/util/RuntimeUtils.java:239-249)。
 *
 *  @param java_home      已装的 JRE 根(bin/java 的上一级)
 *  @param native_lib_dir 打包层给的 ApplicationInfo.nativeLibraryDir(真实文件,不是指向 APK 的符号链接)
 *  @param lib_dir_out    可空;写实际用的库目录(jre8 是 <java_home>/jre/lib,其余是 <java_home>/lib)。
 *                        装不下时只写空串(**不**截断出半条路径),返回码不受影响
 *  @param lib_dir_len    lib_dir_out 的容量
 *  @param err            可空;失败时写人话原因
 *  @return SXCL_ANDROID_JRE_OK 或 SXCL_ANDROID_JRE_ERR_*
 *
 *  **两个源文件都必须存在**:我们的 APK 构建在缺任何一个时直接失败(android/scripts/build_apk.ps1),
 *  所以这里"源不存在"只可能是打包坏了 —— 必须硬失败,不许悄悄跳过(静默跳过 = 上设备才炸)。
 *  不校验目标是否已存在:重复调用是覆盖,幂等。 */
int sxcl_android_jre_patch_libs(const char *java_home, const char *native_lib_dir, char *lib_dir_out,
                                size_t lib_dir_len, char *err, size_t err_len);

/** 本机是不是安卓(编译期常量,给 UI 决定要不要显示"别的启动器的 Java 用不了"这类提示)。 */
int sxcl_android_is_android(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_ANDROID_H */
