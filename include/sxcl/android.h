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

/** 本机是不是安卓(编译期常量,给 UI 决定要不要显示"别的启动器的 Java 用不了"这类提示)。 */
int sxcl_android_is_android(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_ANDROID_H */
