/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LAUNCH_H
#define SXCL_LAUNCH_H

#include <stddef.h>

#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ 1. Java 运行时探测 ══════════════════════════ */

/** 路径缓冲上限。Windows 长路径(260)与 Android 私有目录都够用。 */
#define SXCL_JAVA_PATH_MAX 512

/** 一次 Java 安装的画像。解析失败时 error 里有原因,major 为 0。 */
typedef struct sxcl_java_info {
    char path[SXCL_JAVA_PATH_MAX]; /**< java 可执行文件全路径(纯文本解析时为空串) */
    char home[SXCL_JAVA_PATH_MAX]; /**< JAVA_HOME,即 bin 的上一级(未知为空串) */
    int major;                     /**< 主版本:8 / 17 / 21 / 25;0 = 未知 */
    int is_64bit;                  /**< 1 是 64 位,0 是 32 位,-1 未知 */
    int is_jre;                    /**< 1 = 只有运行时(无 javac),0 = JDK,-1 未知 */
    char version[64];              /**< 完整版本串,如 "21.0.3" / "1.8.0_402" */
    char vendor[64];               /**< 厂商,如 "Eclipse Adoptium";未知为 "Unknown" */
    char arch[16];                 /**< 归一化架构:"x64" / "x86" / "arm64" / "arm32" */
    char source[24];               /**< 来源:"JAVA_HOME"/"PATH"/"ProgramFiles"/"Runtime" 等 */
    char error[128];               /**< 失败原因(成功时为空串) */
} sxcl_java_info;

/** 目标平台。探测规则与路径布局按它分叉,单测才能覆盖非本机平台。 */
typedef enum sxcl_java_os {
    SXCL_JAVA_OS_WINDOWS = 0,
    SXCL_JAVA_OS_LINUX,
    SXCL_JAVA_OS_MACOS,
    SXCL_JAVA_OS_ANDROID
} sxcl_java_os;

/** 探测用的环境变量集合(值都是 UTF-8,可空)。 */
typedef struct sxcl_java_env {
    const char *java_home;         /**< JAVA_HOME */
    const char *program_files;     /**< %ProgramFiles% */
    const char *program_files_x86; /**< %ProgramFiles(x86)% */
    const char *local_app_data;    /**< %LOCALAPPDATA% */
    const char *app_data;          /**< %APPDATA%(官方运行时与 .minecraft 在这下面) */
    const char *user_home;         /**< $HOME / %USERPROFILE% */
    const char *path;              /**< PATH(';' 与 ':' 两种分隔都认) */
    const char *android_files;     /**< Android:本应用私有 files 目录(环境变量 SXCL_ANDROID_FILES);
                                        **唯一**能放可执行 Java 的地方,见 android.h 的说明 */
    const char *android_shared;    /**< Android:共享存储根;空 = /storage/emulated/0(只在列表/诊断里用) */
} sxcl_java_env;

/** 环境变量 + 背后的存储。字段被 env 引用,别把它当成可随意拷贝的值。 */
typedef struct sxcl_java_env_store {
    sxcl_java_env env;
    char java_home[SXCL_JAVA_PATH_MAX];
    char program_files[SXCL_JAVA_PATH_MAX];
    char program_files_x86[SXCL_JAVA_PATH_MAX];
    char local_app_data[SXCL_JAVA_PATH_MAX];
    char app_data[SXCL_JAVA_PATH_MAX];
    char user_home[SXCL_JAVA_PATH_MAX];
    char path[4096];
    char android_files[SXCL_JAVA_PATH_MAX];
    char android_shared[SXCL_JAVA_PATH_MAX];
} sxcl_java_env_store;

/** 一个候选安装点(java 可执行文件全路径 + 归属)。 */
typedef struct sxcl_java_candidate {
    char path[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    char source[24];
    char owner[64]; /**< 这份 Java 是谁的:"本应用"/"FCL"/"HMCL"/"PojavLauncher"/"共享存储";桌面版留空 */
} sxcl_java_candidate;

/** 本机平台。 */
sxcl_java_os sxcl_java_current_os(void);
/** 平台名与 rules 的 os.name 一致;"android" 也归 "linux"(Mojang rules 里没有 android 这个名字)。 */
const char *sxcl_java_os_name(sxcl_java_os os);
/** Windows 下 java 可执行文件名,其它平台是 "java"。 */
const char *sxcl_java_exe_name(sxcl_java_os os);

/** 从真实环境变量抓一份(UTF-8)。Windows 侧走 _wgetenv,避免中文用户名被代码页毁掉。 */
void sxcl_java_env_capture(sxcl_java_env_store *store);

/** 从版本串取主版本:"21.0.3"->21,"1.8.0_402"->8,"17"->17,认不出返回 0。
 *  单独暴露这个口子:调用方从别处拿到版本串也能直接算主版本。 */
int sxcl_java_major_of(const char *version_text);

/** 由环境变量得到的候选 java 可执行文件(纯字符串,不碰文件系统):
 *  JAVA_HOME/bin/java、PATH 里的每一项、macOS 的 Homebrew 前缀等固定位置。 */
size_t sxcl_java_candidate_paths(const sxcl_java_env *env, sxcl_java_os os,
                                 sxcl_java_candidate *out, size_t cap);

/** 需要**真实扫目录**的根(纯字符串,不碰文件系统):
 *  Windows 的 Program Files 各家 JDK 目录与官方运行时目录;Linux 的 /usr/lib/jvm;macOS 的 JVM 目录。 */
size_t sxcl_java_scan_roots(const sxcl_java_env *env, sxcl_java_os os,
                            char (*out)[SXCL_JAVA_PATH_MAX], size_t cap);

/** 解析 JDK 自带的 release 文件(键值清单,值可带引号)。纯文本,不碰文件系统。
 *  返回 0 成功(major > 0),-1 失败(原因写到 out->error)。 */
int sxcl_java_parse_release(const char *text, size_t len, sxcl_java_info *out);

/** 解析 java -version 的输出(Java 8 与 9+ 两种版式都认)。纯文本。
 *  返回 0 成功(path/home 留空由调用方补),-1 失败。 */
int sxcl_java_parse_version_output(const char *text, size_t len, sxcl_java_info *out);

/** 只读文件系统的探测:java_exe_or_home 可以是可执行文件全路径,也可以是 JAVA_HOME 目录。
 *  读 <home>/release 得出画像(绝**不**执行 java)。返回 0 成功,-1 失败(见 out->error)。 */
int sxcl_java_inspect(const char *java_exe_or_home, sxcl_java_info *out);

/** 真实扫描(仍然不执行 java):候选路径 + 扫描根逐个定位并解析 release,去重后写入 out。
 *  返回写入条数。env 传 NULL 表示现场抓一份真实环境。 */
size_t sxcl_java_discover(const sxcl_java_env *env, sxcl_java_os os,
                          sxcl_java_info *out, size_t cap);


/* ══════════════════════ 1b. Android:为什么检不出 Java(可读的结论) ══════════════════════
 *
 * 用户反馈原文(小米平板 192.168.220.33,Android 16):
 *   "我平板有 HMCL 不可能没有 JAVA 和游戏目录…… 成功检出游戏,但是没检出 JAVA"。
 * 原因不是"没扫",是**安卓不让用**:别的启动器的 Java 在它自己的私有目录里,
 * 我们既 stat 不到(沙箱),共享存储上的又起不了进程(noexec)。
 * 所以这里不只返回"能用的",还把**每个候选为什么用不了**分类带出来,界面照实显示。
 * 分类规则见 sxcl/android.h;sxcl_java_probe_* 全程只读文件系统,**不执行** java
 * (与 sxcl_java_discover 的约定一致)。
 */

/** 一个候选 Java 的体检结论。 */
typedef enum sxcl_java_verdict {
    SXCL_JAVA_VERDICT_USABLE = 0,   /**< 可执行、读得出 release —— 能用 */
    SXCL_JAVA_VERDICT_MISSING,      /**< 这个位置没有东西 */
    SXCL_JAVA_VERDICT_DENIED,       /**< 沙箱拒绝:通常是**别的应用**的私有目录 */
    SXCL_JAVA_VERDICT_NOEXEC,       /**< 在 noexec 文件系统上(共享存储),起不了进程 */
    SXCL_JAVA_VERDICT_NOT_EXECUTABLE, /**< 有文件,但没有执行位 */
    SXCL_JAVA_VERDICT_NOT_A_JRE,    /**< 能执行,但读不出 JRE 画像(缺 release / 不是 Java) */
    SXCL_JAVA_VERDICT_UNREADABLE,   /**< 存在但读不了(其它 IO 错误) */
    SXCL_JAVA_VERDICT_COUNT
} sxcl_java_verdict;

#define SXCL_JAVA_MAX_PROBES 20

/** 一条体检记录。path 是候选路径;usable=1 时它还带着完整画像(major/version)。 */
typedef struct sxcl_java_probe {
    char path[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    char source[24];                       /**< 来源键:"AndroidPrivate"/"AndroidForeign"/… */
    char owner[64];                        /**< "本应用"/"FCL"/"HMCL"/"PojavLauncher"/"共享存储" */
    sxcl_java_verdict verdict;
    int major;                             /**< USABLE 时的 Java 主版本,否则 0 */
    char version[64];                      /**< USABLE 时的版本串 */
    char reason[192];                      /**< 原始原因(带路径与 errno 原话) */
} sxcl_java_probe;

typedef struct sxcl_java_report {
    sxcl_java_probe items[SXCL_JAVA_MAX_PROBES];
    size_t count;
    size_t usable;                         /**< 其中 verdict == USABLE 的条数 */
} sxcl_java_report;

/** 结论的中文短名("可用"/"不在"/"沙箱拒绝"/"共享存储不能执行"/"没有执行位"/"不是 JRE")。 */
const char *sxcl_java_verdict_name(sxcl_java_verdict verdict);
/** 结论的稳定英文键("usable"/"missing"/"denied"/"noexec"/"not_executable"/"not_a_jre")。 */
const char *sxcl_java_verdict_key(sxcl_java_verdict verdict);
/** 一条人话建议(为什么 + 下一步)。永远返回非空串。 */
const char *sxcl_java_verdict_hint(sxcl_java_verdict verdict);

/** 体检一串候选(os 决定 java 可执行文件名;mounts_text 传 NULL = 真的读 /proc/self/mounts)。
 *  第 i 条候选对应 filter[i](0 = 只要不是 MISSING 就收,1 = 只收 USABLE)。
 *  返回写进 out->count 的条数。out 为 NULL 返回 0。 */
size_t sxcl_java_probe_candidates(const sxcl_java_candidate *cands, size_t cand_count,
                                  sxcl_java_os os, const char *mounts_text, sxcl_java_report *out);

/** Android 专用:把"已知会放 Java 的地方"全列出来逐个体检 ——
 *   1) 本应用私有目录(<files>/runtime、<files>/jre、<files>/java、<data>/app_runtime/java)
 *      —— 真的往下扫 bin/java,扫到就是 USABLE(这是**唯一**能用的一类);
 *   2) 别的启动器(HMCL/FCL/PojavLauncher)的私有目录 —— 如实报 DENIED;
 *   3) 共享存储上的运行时目录 —— 如实报 NOEXEC。
 *  files_dir 可空;shared_root 可空(默认 /storage/emulated/0)。返回 out->count。 */
size_t sxcl_java_probe_android(const char *files_dir, const char *shared_root,
                               sxcl_java_report *out);

/* ═══════════════ 1c. 真实执行 `java -version`(不能只看目录名) ═══════════════
 *
 * 为什么必须真的起进程:
 *   * `release` 文件只是**文本**,可以是从别处拷来的、可以是 32 位被换成 64 位的,
 *     更常见的是"目录名写着 jdk-21,里面其实是另一个版本";
 *   * 只有 java 自己打印的那一行,才是"这个二进制在这台机器上真的能跑、跑起来是这个版本"。
 * 所以本节的函数会**真的执行** <path> -version,再把输出解析成画像(major/vendor/arch)。
 *
 * 起不来的情形必须**分类**告诉用户,不能静默丢弃:
 *   * 沙箱拒绝(别的应用私有目录)/ 共享存储 noexec(安卓实测)/ 没有执行位;
 *   * 能读但读不出 Java 版本(不是 JRE);
 *   * 架构不符(arm64 设备上放了个 x86 的 java)—— 靠读文件头判,不靠猜。
 */

/** 一次"实测"的结论(比只读体检的 sxcl_java_verdict 多一层:执行失败/架构不符)。 */
typedef enum sxcl_java_run_verdict {
    SXCL_JAVA_RUN_OK = 0,         /**< 真的执行成功,解析出 Java 版本 —— 能用 */
    SXCL_JAVA_RUN_MISSING,        /**< 这个位置没有东西 */
    SXCL_JAVA_RUN_DENIED,         /**< 沙箱拒绝(通常是别的应用的私有目录) */
    SXCL_JAVA_RUN_NOEXEC,         /**< 在 noexec 文件系统上(共享存储),起不了进程 */
    SXCL_JAVA_RUN_NOT_EXECUTABLE, /**< 有文件,但没有执行位 */
    SXCL_JAVA_RUN_EXEC_FAILED,    /**< 进程起不来(退出码 127 且无输出)/ 超时 */
    SXCL_JAVA_RUN_NOT_A_JRE,      /**< 能执行,但输出里认不出 Java 版本 */
    SXCL_JAVA_RUN_ARCH_MISMATCH,  /**< 可执行文件的机器码与本机不符 */
    SXCL_JAVA_RUN_COUNT           /**< 枚举计数(不是结论) */
} sxcl_java_run_verdict;

/** 结论的中文短名("可用"/"不在"/"沙箱拒绝"/"共享存储不能执行"/"没有执行位"/"跑不起来"/"不是 Java"/"架构不符")。 */
const char *sxcl_java_run_verdict_name(sxcl_java_run_verdict verdict);
/** 结论的稳定英文键("ok"/"missing"/"denied"/"noexec"/"not_executable"/"exec_failed"/"not_a_jre"/"arch_mismatch")。 */
const char *sxcl_java_run_verdict_key(sxcl_java_run_verdict verdict);
/** 一条人话建议(为什么 + 下一步)。永远返回非空串。 */
const char *sxcl_java_run_verdict_hint(sxcl_java_run_verdict verdict);

/** 一个候选 Java 的实测结果。 */
typedef struct sxcl_java_installation {
    sxcl_java_info info;              /**< 画像;实测成功时才有 major/version/vendor/arch */
    sxcl_java_run_verdict verdict;
    int executed;                     /**< 1 = 真的执行过 java -version(不是只读了 release) */
    char source[24];                  /**< 来源键:"JAVA_HOME"/"PATH"/"ProgramFiles"/"AndroidPrivate"… */
    char owner[64];                   /**< "本应用"/"FCL"/"HMCL"…;桌面通用位置留空 */
    int priority;                     /**< 排序用:越大越优先(本应用私有目录 > JAVA_HOME > PATH > 扫描) */
    char reason[192];                 /**< 原始原因(带退出码/errno 原话/路径) */
} sxcl_java_installation;

#define SXCL_JAVA_MAX_INSTALLS 32

typedef struct sxcl_java_installations {
    sxcl_java_installation items[SXCL_JAVA_MAX_INSTALLS];
    size_t count;    /**< 全部条目(含不可用的 —— 用户有权知道为什么没检出来) */
    size_t usable;   /**< verdict == OK 的条数 */
    size_t broken;   /**< verdict == ARCH_MISMATCH / NOT_A_JRE / EXEC_FAILED 的条数 */
} sxcl_java_installations;

/** 真实执行 `<java_exe> -version` 并解析。java_exe 可以是可执行文件,也可以是 JAVA_HOME。
 *  timeout_ms <= 0 用默认(8 秒)。返回 0 成功(major > 0),-1 失败(out->error 有人话原因)。 */
int sxcl_java_exec_version(const char *java_exe, int timeout_ms, sxcl_java_info *out);

/** 本机 CPU 架构的归一化名("x64"/"x86"/"arm64"/"arm32")。 */
const char *sxcl_java_host_arch(void);

/** 读可执行文件的机器码(ELF / PE / Mach-O 文件头),归一化成 "x64"/"x86"/"arm64"/"arm32"。
 *  **只读文件头,不执行**。认不出(不是可执行文件/读不了)返回 -1 并把 out 置空。 */
int sxcl_java_binary_arch(const char *path, char *out, size_t out_len);

/** 全链路检测(②的主入口):候选(JAVA_HOME/PATH/各家安装目录/官方 runtime/安卓私有目录,
 *  与 sxcl_java_discover 同一张表)-> 逐个**真的执行** java -version -> 分类。
 *  env 传 NULL = 现场抓一份真实环境;timeout_ms <= 0 = 每个候选 8 秒。
 *  不可用的候选**照样进列表**(带 verdict 与 reason),usable 只数能用的。
 *  按 priority 降序 -> 主版本降序 -> 路径升序排列。返回 out->count。 */
size_t sxcl_java_detect(const sxcl_java_env *env, sxcl_java_os os, int timeout_ms,
                        sxcl_java_installations *out);

/** 版本 JSON 要求的 Java 主版本(javaVersion.majorVersion)。
 *  字段缺失返回 8 —— 官方启动器的行为:1.13 之前一律 Java 8。 */
int sxcl_java_required_major(const sxcl_json *version_json);

/** 按需求排序候选,把下标写进 order(最多 cap 个),返回排序后的条数。
 *  顺序:精确匹配优先 -> 更高版本(升序,最接近的在前)-> 更低版本(降序,最接近的在前)-> 版本未知的垫底。
 *  同一主版本内:64 位优先,再按路径升序(结果稳定可复现)。
 *  required_major <= 0 表示"无要求":按主版本从新到旧排。 */
size_t sxcl_java_rank(const sxcl_java_info *list, size_t count, int required_major,
                      size_t *order, size_t cap);

/* ══════════════════════════ 2. 启动参数拼装 ══════════════════════════ */

/** 拼装参数时的目标平台。AUTO = 本机。 */
typedef enum sxcl_launch_os {
    SXCL_LAUNCH_OS_AUTO = 0,
    SXCL_LAUNCH_OS_WINDOWS,
    SXCL_LAUNCH_OS_LINUX,
    SXCL_LAUNCH_OS_MACOS,
    SXCL_LAUNCH_OS_ANDROID
} sxcl_launch_os;

/** 默认内存档位(调用方可用 memory_mb 覆盖,这里只是"没人管的时候别把机器打死")。
 *  64 位 -> 4096MB;32 位 -> 1024MB;位数未知 -> 2048MB(32 位 JVM 的 -Xmx 上不去,保守)。 */
int sxcl_launch_default_memory_mb(int is_64bit);

/** 启动上下文。字符串一律 UTF-8,生命周期由调用方保证(拼装期间不得释放)。 */
typedef struct sxcl_launch_ctx {
    /* 身份(占位符 ${auth_*}/${user_type});空则用注释里的默认值 */
    const char *player_name;    /**< 默认 "Player" */
    const char *uuid;           /**< 默认全零 UUID */
    const char *access_token;   /**< 默认 "0"(离线) */
    const char *user_type;      /**< 默认 "msa" */
    const char *xuid;           /**< ${auth_xuid}(1.20.2+ 的 --xuid);空 = "0" */
    const char *client_id;      /**< ${clientid}(1.20.2+ 的 --clientId);空 = "" */
    /* 版本与目录 */
    const char *version_name;   /**< ${version_name},也用于推断客户端 jar 路径 */
    const char *version_type;   /**< ${version_type},默认 "release" */
    const char *game_directory; /**< ${game_directory},也是游戏的 cwd */
    const char *assets_root;    /**< ${assets_root}(通常 <game>/assets) */
    const char *assets_index_name; /**< ${assets_index_name} */
    const char *natives_directory; /**< ${natives_directory} 与 java.library.path */
    const char *library_directory; /**< ${library_directory};空则 <game>/libraries */
    const char *client_jar;     /**< 客户端 jar;空则 <game>/versions/<version>/<version>.jar */
    /* 启动器标识(${launcher_name}/${launcher_version} 与 -Dminecraft.launcher.*) */
    const char *launcher_name;    /**< 默认 "SilentXCraftLauncher" */
    const char *launcher_version; /**< 默认 SXCL_VERSION,见 version.h */
    /* 内存与 GC:memory_mb <= 0 用默认;java_major/is_64bit 决定 GC 与默认内存档位 */
    int memory_mb;
    int java_major;   /**< 0 = 未知(按最保守处理) */
    int is_64bit;     /**< 1/0;-1 = 未知(按最保守处理) */
    /* 覆盖口子 */
    const char *const *jvm_args;       /**< 非空 = **完全替代**默认 JVM 参数块(内存/GC/编码都不再加) */
    const char *const *extra_jvm_args; /**< 追加到 JVM 参数尾部(NULL 结尾的数组,可空) */
    const char *const *extra_game_args;/**< 追加到游戏参数尾部(NULL 结尾的数组,可空) */
    /* 平台 */
    sxcl_launch_os os;              /**< AUTO = 本机 */
    const char *os_name;            /**< 覆盖 rules 求值用的 os.name;空 = 按 os */
    const char *arch_name;          /**< 覆盖 rules 求值用的 os.arch;空 = 本机 */
    const char *classpath_separator;/**< 覆盖 classpath 分隔符;空 = Windows ';' 其它 ':' */
    int keep_path_separator;        /**< 非 0 = 不把路径统一成目标平台的分隔符 */
} sxcl_launch_ctx;

typedef struct sxcl_launch_args sxcl_launch_args;

/** 拼装 argv。失败返回 NULL 并写 err。
 *  注意:数组里**不含** java 可执行文件本身 —— program 由调用方单独给(process.h 的 program 字段),
 *  这样换一个 java 就不必重拼参数。 */
sxcl_launch_args *sxcl_launch_build_args(const sxcl_json *version_json,
                                         const sxcl_launch_ctx *ctx,
                                         char *err, size_t err_len);

void sxcl_launch_args_free(sxcl_launch_args *args);

/** 参数个数(不含结尾的 NULL)。 */
size_t sxcl_launch_arg_count(const sxcl_launch_args *args);
/** 第 index 个参数;越界返回 NULL。 */
const char *sxcl_launch_arg_at(const sxcl_launch_args *args, size_t index);
/** NULL 结尾的数组,可直接塞进 sxcl_process_opts.args(类型就是它要的 const char *const *)。
 *  句柄释放前有效。 */
const char *const *sxcl_launch_argv(const sxcl_launch_args *args);

/** 展开单个字符串里的占位符(调试/UI 预览用;未知占位符原样保留)。
 *  写入 out(含结尾 0),返回写入长度(不含 0);out 不够则截断但保证 0 结尾。 */
size_t sxcl_launch_expand(const char *text, const sxcl_launch_ctx *ctx, char *out, size_t out_len);

/* ══════════════════════════ 3. 日志归类 ══════════════════════════ */

/** 一行的类别。判定按"越具体越优先"的固定顺序,同一行命中多条时取更具体的那条。 */
typedef enum sxcl_log_kind {
    SXCL_LOG_UNKNOWN = 0,
    SXCL_LOG_GRAPHICS,        /**< LWJGL / GLFW / OpenGL / EGL / 显卡驱动 */
    SXCL_LOG_VULKAN_FALLBACK, /**< 明确说 Vulkan 不可用/回退到 OpenGL */
    SXCL_LOG_JAVA_VERSION,    /**< Java 版本不符(UnsupportedClassVersionError 等) */
    SXCL_LOG_MOD_LOADER,      /**< 模组/加载器缺失或不兼容 */
    SXCL_LOG_MISSING,         /**< 缺类/缺库/缺资源 */
    SXCL_LOG_ACCOUNT_NET,     /**< 账户/网络 */
    SXCL_LOG_CRASH,           /**< hs_err / crash-report / 致命错误 */
    SXCL_LOG_EXIT_OK,         /**< 正常退出 */
    SXCL_LOG_KIND_COUNT
} sxcl_log_kind;

/** 一行文本的扫描结果。所有字符串都保证 NUL 结尾,没抽到就是空串。 */
typedef struct sxcl_log_line {
    sxcl_log_kind kind;
    int severity;            /**< 0 信息 / 1 警告 / 2 错误 */
    int vulkan_fallback;     /**< 1 = 这一行在说 Vulkan 回退 */
    int exit_code;           /**< 抽到的退出码;没提到为 -1 */
    char java_version[32];   /**< 抽到的 Java 版本串 */
    char gl_version[96];     /**< 抽到的 GL 版本串 */
    char gl_renderer[128];   /**< 抽到的渲染器/GPU 串 */
    char missing[160];       /**< 缺失的类/库/资源名 */
    char reason[192];        /**< 崩溃原因首行 / 关键信息 */
} sxcl_log_line;

/** 一批日志的结论。为什么要有它:启动失败时界面只说人话(图形栈/版本支持/账户网络),
 *  不能把整篇日志糊到用户脸上。 */
typedef enum sxcl_log_conclusion {
    SXCL_LOG_CONCLUSION_UNKNOWN = 0,
    SXCL_LOG_CONCLUSION_OK,              /**< 看着是正常退出 */
    SXCL_LOG_CONCLUSION_GRAPHICS,        /**< 图形栈问题 */
    SXCL_LOG_CONCLUSION_VULKAN_FALLBACK, /**< 切了 Vulkan 但回退了 */
    SXCL_LOG_CONCLUSION_JAVA,            /**< Java 版本不符 */
    SXCL_LOG_CONCLUSION_MOD,             /**< 模组/加载器问题 */
    SXCL_LOG_CONCLUSION_MISSING,         /**< 文件/类/库缺失 */
    SXCL_LOG_CONCLUSION_ACCOUNT,         /**< 账户/网络问题 */
    SXCL_LOG_CONCLUSION_CRASH            /**< 崩了但原因不明 */
} sxcl_log_conclusion;

typedef struct sxcl_log_summary {
    size_t lines;                      /**< 累计行数 */
    size_t kind_counts[SXCL_LOG_KIND_COUNT];
    int voted_vulkan_fallback;         /**< 见过回退提示 */
    int voted_crash;                   /**< 见过崩溃 */
    int voted_graphics;                /**< 见过图形栈错误 */
    int voted_java;                    /**< 见过 Java 版本问题 */
    int voted_mod;                     /**< 见过模组/加载器问题 */
    int voted_missing;                 /**< 见过缺东西 */
    int voted_account;                 /**< 见过账户/网络问题 */
    int exited_ok;                     /**< 见过正常退出 */
    int exit_code;                     /**< 抽到的退出码;-1 = 没提到 */
    char java_version[32];
    char gl_version[96];
    char gl_renderer[128];
    char missing[160];
    char crash_reason[192];
    sxcl_log_conclusion conclusion;
    char advice[256];                  /**< 一条人话结论 */
} sxcl_log_summary;

/** 扫描一行。out 允许传 NULL(只要类别)。返回该行的类别。 */
sxcl_log_kind sxcl_log_scan_line(const char *line, sxcl_log_line *out);

/** 清空汇总结构(必须先调,再 add)。 */
void sxcl_log_summary_init(sxcl_log_summary *summary);
/** 累加一行,返回该行类别。 */
sxcl_log_kind sxcl_log_summary_add(sxcl_log_summary *summary, const char *line);
/** 算 conclusion 与 advice(可重复调用;汇总过程中一直是最新结论)。 */
void sxcl_log_summary_finish(sxcl_log_summary *summary);
/** 一把梭:一批行 -> 结论(内部 init + 逐行 add + finish)。 */
void sxcl_log_summarize(const char *const *lines, size_t count, sxcl_log_summary *out);

/** 类别名("graphics" / "vulkan_fallback" / ...),给日志与测试用。 */
const char *sxcl_log_kind_name(sxcl_log_kind kind);
/** 结论文本键("graphics" / "ok" / ...),给 UI 做分支用。 */
const char *sxcl_log_conclusion_name(sxcl_log_conclusion conclusion);

/* ══════════════════════════ 4. 启动驱动(真正起进程) ══════════════════════════ */

/** options.txt 里"渲染后端"的键名。
 *  **只在这一处定义**:哪天某个版本改用别的键名,改这里,驱动与 CLI/测试都跟着走。 */
#define SXCL_LAUNCH_GRAPHICS_KEY "graphicsApi"

/** 实例设置里记录"上次实际生效的后端"的键名,完整键是 instance.<实例名>.<这个>。 */
#define SXCL_LAUNCH_LAST_BACKEND_KEY "lastGraphicsApi"

/** 后端取值(写进 options.txt 与设置里的就是这三个字符串)。 */
#define SXCL_LAUNCH_BACKEND_DEFAULT "default"
#define SXCL_LAUNCH_BACKEND_VULKAN  "vulkan"
#define SXCL_LAUNCH_BACKEND_OPENGL  "opengl"

/** 一次启动请求。字符串一律 UTF-8,生命周期由调用方保证。 */
typedef struct sxcl_launch_request {
    const char *game_dir;       /**< 必填:游戏根目录(内含 versions/ libraries/ assets/) */
    const char *version_name;   /**< 必填:版本名,对应 versions/<名字>/<名字>.json */
    const char *java_path;      /**< 可空:指定 java 可执行文件;空 = 自动探测。
                                 *   指定了就用指定的(哪怕读不出 release —— 用户说了算) */
    int memory_mb;              /**< <=0 = 按位数取默认(见 sxcl_launch_default_memory_mb) */
    const char *instance;       /**< 可空:实例名(读写每实例设置);空 = 用 version_name */
    const char *offline_name;   /**< 可空:离线用户名;空 = "Player" */
    /* ── 正版登录身份(2026-02 加)。都给空 = 与从前完全一致(走离线默认值) ──
     * 注意:给了 access_token 就**必须**同时给 uuid 与 player_name,并且 user_type 用 "msa"
     * (游戏会拿 access_token 去验档案,三者对不上会被踢回主菜单)。 */
    const char *player_name;    /**< 可空:正版玩家名(优先于 offline_name) */
    const char *uuid;           /**< 可空:32 位无横线 uuid(带横线的也接受) */
    const char *access_token;   /**< 可空:Minecraft access_token(真实凭据;**只在内存里传,别落日志**) */
    const char *user_type;      /**< 可空:"msa"(正版)/ "legacy"(离线);空 = 有 token 就是 msa,否则 legacy */
    const char *xuid;           /**< 可空:Xbox XUID(字符串);空 = "0" */
    const char *client_id;      /**< 可空:${clientid};空 = "" */
    const char *backend;        /**< 可空:后端覆盖;空 = 读实例设置(默认 "default") */
    const char *settings_path;  /**< 可空:设置文件;空 = 不读也不写设置 */
    const char *launcher_name;  /**< 可空:覆盖 launcher_name 占位符 */
    const char *launcher_version; /**< 可空:覆盖 launcher_version 占位符 */
    int timeout_ms;             /**< <=0 = 不限时;超时会被终止并置 timed_out */
    int dry_run;                /**< 非 0 = 只准备(选 Java / 写 options.txt / 拼 argv),不起进程 */
    /** 每读到一行输出调用一次(stdout 与 stderr 都走这里,**原始行**未加工)。
     *  返回非 0 = 请求终止进程 —— 取消与"看到完成标记就收工"都走这条路(与 process.h 一致)。 */
    int (*on_line)(void *userdata, int is_stderr, const char *line);
    void *userdata;
    /** 进程**真的起来了**时调用一次,参数是游戏进程的 PID(在 sxcl_launch_run 的同一条线程里)。
     *  可空。存在的理由:界面要在游戏还在跑的时候就能显示 PID、并且能**单独结束它** ——
     *  只靠 result.pid 得等到游戏退出才知道,那对"结束游戏"这件事没有用。
     *  返回非 0 = 立刻终止进程(与 on_line 返回非 0 同义,结果里 killed_by_client = 1)。 */
    int (*on_started)(void *userdata, int64_t pid);
} sxcl_launch_request;

/** 一次启动的结果。 */
typedef struct sxcl_launch_result {
    int exit_code;              /**< 进程退出码;-1 = 没起来 */
    int64_t pid;                /**< 游戏进程 PID;**没起来 / dry_run 时为 -1**。
                                 *   进程退出后仍保留(与 sxcl_process_result.pid 同义)。 */
    int started;                /**< 1 = 真的起过进程(dry_run 时为 0) */
    int timed_out;              /**< 1 = 超时被终止 */
    int killed_by_client;       /**< 1 = on_line / on_started 回调请求终止 */
    int64_t elapsed_ms;         /**< 从起进程到结束的墙钟毫秒 */
    int java_major;             /**< 选中的 Java 主版本;0 = 未知 */
    int java_is_64bit;          /**< 1/0/-1 */
    int vulkan_fell_back;       /**< 1 = 设置的是 vulkan,但日志显示回退到了 OpenGL */
    char java_path[SXCL_JAVA_PATH_MAX]; /**< 选中的 java 路径 */
    char java_version[64];      /**< 选中的 Java 版本串(可能为空) */
    char requested_backend[16]; /**< 本次要求写进 options.txt 的后端 */
    char actual_backend[16];    /**< 日志显示实际生效的后端 */
    char options_path[SXCL_JAVA_PATH_MAX]; /**< 写过的 options.txt 路径 */
    char natives_dir[SXCL_JAVA_PATH_MAX];  /**< 原生库目录(-Djava.library.path 指向的那个) */
    int natives_count;          /**< 原生库目录里就绪的文件数;0 = 这个版本没有原生库 */
    char game_dir[SXCL_JAVA_PATH_MAX];
    char error[256];            /**< 人话失败原因;空 = 没失败 */
    char missing[160];          /**< 从日志里原样带出来的"缺什么"(不做补全,只报告) */
    sxcl_log_conclusion conclusion; /**< 汇总结论 */
    char conclusion_text[256];  /**< 一条人话结论(UI 直接显示这一条) */
    sxcl_log_summary log;       /**< 完整日志汇总:想深入到"缺哪个类/多少行"就用它 */
} sxcl_launch_result;

/** 跑一次启动:读版本 JSON -> 选 Java -> 写 options.txt(渲染后端) -> 建 natives -> 拼 argv
 *  -> 起进程 -> 逐行归类 -> 出一条人话结论;发现 Vulkan 回退就把它写回 lastGraphicsApi。
 *
 *  返回 0 = 进程真的跑起来了(dry_run 时表示"准备好了");<0 = 没启动,原因在 out->error 与 err。
 *
 *  **不做**资源补全:库/assets 缺了就是缺了,由日志结论把"缺什么"原样带出来,交给上层决定补不补。 */
int sxcl_launch_run(const sxcl_launch_request *request, sxcl_launch_result *out,
                    char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LAUNCH_H */
