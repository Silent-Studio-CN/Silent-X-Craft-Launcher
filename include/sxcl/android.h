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
/** 算出来的库目录与清单里写的 shim_dir 不一致(硬失败:宁可报错也不把 .so 放进一个 JVM 不看的目录)。 */
#define SXCL_ANDROID_JRE_ERR_SHIM_MISMATCH (-5)

/** 结果的稳定名字("ok"/"arg"/"no_lib_dir"/"no_source"/"copy"/"shim_mismatch")。 */
const char *sxcl_android_jre_patch_code_name(int code);

/** 算"JRE 侧库该放进哪个目录"(**相对 java_home** 的路径,如 "lib/aarch64" / "jre/lib"),
 *  规则按 FCL 的 RuntimeUtils.getJavaLibDir() 对齐(2026-09 核对):
 *
 *    1) 先从 <java_home>/release 读 OS_ARCH(如 "aarch64");
 *    2) **优先 <java_home>/lib/<OS_ARCH>** —— Termux 那份 JRE 镜像布局就是 bin/ + lib/aarch64/
 *       (jre8 也一样:lib/aarch64/jli/libjli.so),它的 sun.boot.library.path 正是这个目录;
 *    3) 只有在 <java_home>/jre **与** <java_home>/bin/javac **同时存在**(FCL 的 isJDK8())
 *       时才考虑 <java_home>/jre/lib[/<OS_ARCH>] —— 那种才是"真 JDK8"的老布局;
 *    4) 都没有就退回 <java_home>/lib;再没有就 <java_home>/jre/lib;
 *    5) 一个都不是目录 -> SXCL_ANDROID_JRE_ERR_NO_LIB_DIR。
 *
 *  为什么不能只看 "<jre>/lib 在不在":jre8 那份**没有 jre/ 子目录**,库在 lib/aarch64;
 *  按老写法会把它放进 <jre>/lib,而 JVM 从 <jre>/lib/aarch64 找 —— **拷了但不生效**,
 *  设备上表现为 AWT/声音相关的一堆 NoClassDefFound/dlopen 失败,极难查。
 *
 *  纯读盘,不写任何东西。out 装不下就写空串并返回 ERR_ARG。 */
int sxcl_android_jre_shim_dir(const char *java_home, char *out, size_t out_len, char *err,
                              size_t err_len);

/** 同 sxcl_android_jre_patch_libs,但多一个"清单里说的 shim_dir"。
 *
 *  @param expected_shim_dir 可空/空串 = 不校验;非空时与算出来的相对路径**逐字比较**,
 *         不一致直接 SXCL_ANDROID_JRE_ERR_SHIM_MISMATCH(并把人话写进 err)。
 *         来源:自托管清单 sxcl.jre.index/1 里每个组件的 "shim_dir" 字段。
 *         为什么硬失败:两处不一致说明"清单说的"和"盘上实际"对不上,这时候悄悄选一个
 *         等于赌 —— 赌错就是设备上一片 dlopen 失败。
 *
 *  其余语义与 sxcl_android_jre_patch_libs 完全一致。 */
int sxcl_android_jre_patch_libs_ex(const char *java_home, const char *native_lib_dir,
                                   const char *expected_shim_dir, char *lib_dir_out,
                                   size_t lib_dir_len, char *err, size_t err_len);

/** 把 nativeLibraryDir 里的 libawt_xawt.so / libjsound.so 补进**已装好的** JRE。
 *
 *  为什么非做不可:安卓 arm64 的 JRE 资产里没有 libawt_xawt.so,jre17/21/25 连
 *  libjsound.so 也没有;JVM 启动时按名字从 <jre>/lib dlopen 它们,缺一个就是硬启动失败,
 *  而且运行时也补不回来(组件清单里没有这两个文件)。FCL 在装完 JRE 后做同一件事:
 *  RuntimeUtils.patchJava()(FCL/src/main/java/com/tungsten/fcl/util/RuntimeUtils.java:239-249)。
 *
 *  @param java_home      已装的 JRE 根(bin/java 的上一级)
 *  @param native_lib_dir 打包层给的 ApplicationInfo.nativeLibraryDir(真实文件,不是指向 APK 的符号链接)
 *  @param lib_dir_out    可空;写实际用的库目录(见 sxcl_android_jre_shim_dir 的规则)。
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
