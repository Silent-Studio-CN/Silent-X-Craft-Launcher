/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_android_game_args.h - 安卓上"起游戏主类"的参数装配(Android only,但**不依赖任何安卓头文件**)。
 *
 * 为什么单独一层、又是纯 C:
 *   * 参数怎么拼、classpath/natives/mainClass/游戏参数从哪来 —— 这些规则**全在核心库**
 *     (include/sxcl/launch.h 的 sxcl_launch_build_args + instance.h + natives.h);
 *     这层只做安卓特有的三件事,并且**能在桌面上离线跑**(离线夹具断言就靠这一点):
 *       1) natives 目录按**安卓布局**给:<files_dir>/natives/<instance>(应用私有目录 ——
 *          /storage 是 noexec 挂载,原生库放那儿一定加载不了);
 *       2) ctx.os = SXCL_LAUNCH_OS_ANDROID(rules 按 linux 匹配、classpath 用 ':');
 *       3) 把核心库拼出来的**整条游戏命令行**原样交给 JVM 层当 extra_args
 *          (java 可执行文件、-Djava.home/-Dos.name=Linux/-Dos.version/-Djava.library.path
 *           由 include/sxcl/jvm.h 那层加,两边不重复拼)。
 *
 * 主类、游戏参数、JVM 参数的口径:全部来自 sxcl_launch_build_args()(读版本 JSON 的
 * arguments.jvm / arguments.game / mainClass / libraries),这层一个字都不自己拼。
 *
 * 已知缺口(如实写在 note 里,不假装能做):
 *   * **inheritsFrom 合并**:核心库目前不合并(instance.c 只读单个 JSON,args.c 也只吃这一份),
 *     所以 Fabric/Forge 这种"子 JSON + 父版本"的实例,拼出来的 classpath 会缺父版本的 libraries/jar。
 *     这不是安卓特有的缺口 —— 桌面那条路(driver.c)读的也是同一份单 JSON。要补就补在核心库一处。
 */

#ifndef SXCL_ANDROID_GAME_ARGS_H
#define SXCL_ANDROID_GAME_ARGS_H

#include <stddef.h>

#include "sxcl/json.h"
#include "sxcl/launch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXCL_ANDROID_GAME_PATH_MAX 1024
#define SXCL_ANDROID_GAME_CP_MAX   8192
#define SXCL_ANDROID_GAME_NOTE_MAX 512

/** 装配输入。字符串一律 UTF-8,生命周期由调用方保证(装配期间不得释放)。 */
typedef struct sxcl_android_game_spec {
    const char *files_dir;      /* 应用私有目录(getFilesDir());natives 落它下面 */
    const char *game_dir;       /* .minecraft(游戏目录) */
    const char *instance_id;    /* 版本/实例 id(versions/<id>) */
    const char *jre_home;       /* JRE 根目录(拼 -Djava.home 用) */
    /* 身份(与桌面同口径,空则用核心库默认值) */
    const char *player_name;
    const char *uuid;
    const char *access_token;
    const char *user_type;
    const char *xuid;
    const char *client_id;
    /* 运行参数 */
    int memory_mb;              /* <=0 = 核心库默认档位 */
    int java_major;             /* 0 = 未知 */
    int is_64bit;               /* -1 = 未知 */
    const char *android_version;/* ro.build.version.release,如 "16";非空 -> -Dos.version=Android-16 */
    const char *renderer;       /* 渲染器:**只进日志**(options.txt 由启动器写,核心库不管它) */
    const char *const *extra_jvm_args;  /* 我们这层补的安卓 -D(NULL 结尾,可空) */
    const char *const *extra_game_args; /* 追加的游戏参数(NULL 结尾,可空) */
} sxcl_android_game_spec;

/** 装配结果。 */
typedef struct sxcl_android_game_plan {
    /* 交给 sxcl_jvm_opts 的字段 */
    char natives_dir[SXCL_ANDROID_GAME_PATH_MAX];
    char java_library_path[SXCL_ANDROID_GAME_PATH_MAX]; /* = natives_dir(两处取值必须一致) */
    /* 取证:从核心库拼出来的命令行里抽出来的三样(只读,给日志/界面看) */
    char classpath[SXCL_ANDROID_GAME_CP_MAX];
    char main_class[SXCL_ANDROID_GAME_PATH_MAX];
    int arg_count;
    /* 核心库的完整游戏命令行(不含 java 可执行文件)。作为 jvm 层的 extra_args 原样传下去。 */
    sxcl_launch_args *args;
    const char *const *argv;
    /* 版本 JSON 与路径(调用方用 plan_free 释放) */
    sxcl_json *doc;
    char json_path[SXCL_ANDROID_GAME_PATH_MAX];
    /* 取证与缺口 */
    char inherits_from[128];    /* 非空 = 这个 JSON 有 inheritsFrom */
    int natives_count;          /* 核心库解出来的原生库文件数 */
    char note[SXCL_ANDROID_GAME_NOTE_MAX];
} sxcl_android_game_plan;

/** 日志出口(可空)。安卓层接到 logcat,离线夹具接到 stdout。 */
typedef void (*sxcl_android_game_log_fn)(void *ud, const char *line);
void sxcl_android_game_set_log(sxcl_android_game_log_fn fn, void *ud);

/** 按 spec 装配一次。成功返回 0;失败返回负并写 err。
 *  **失败一定是"参数/文件"层面的**:拼装规则本身在核心库,这里只做上面注释里的三件事。 */
int sxcl_android_game_plan_build(const sxcl_android_game_spec *spec, sxcl_android_game_plan *out,
                                 char *err, size_t err_len);

/** 释放 plan 持有的东西(doc / args)。plan 自身是调用方的栈变量。 */
void sxcl_android_game_plan_free(sxcl_android_game_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* SXCL_ANDROID_GAME_ARGS_H */
