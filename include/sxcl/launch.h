/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LAUNCH_H
#define SXCL_LAUNCH_H

#include <stddef.h>

#include "sxcl/crash.h" /* 崩溃取证:退出后读 crash-report/latest.log(sxcl_launch_result 里有它) */
#include "sxcl/engine.h"   /* sxcl_engine_opts:启动前"补全文件"用现成的下载引擎 */
#include "sxcl/json.h"
#include "sxcl/manifest.h" /* sxcl_version_plan:启动前"补全文件"要拿它算"这版本要哪些文件" */

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
    SXCL_JAVA_RUN_APP_DATA_EXEC_DENIED, /**< 安卓 W^X:文件在应用私有目录里且可读可执行,
                                         *   但 SELinux 拒 `execute_no_trans`(execve),
                                         *   起不了进程 —— 只能进程内 dlopen 起 JVM */
    SXCL_JAVA_RUN_COUNT           /**< 枚举计数(不是结论) */
} sxcl_java_run_verdict;

/** 结论的中文短名("可用"/"不在"/"沙箱拒绝"/"共享存储不能执行"/"没有执行位"/"跑不起来"/"不是 Java"/"架构不符"/"私有目录不能执行")。 */
const char *sxcl_java_run_verdict_name(sxcl_java_run_verdict verdict);
/** 结论的稳定英文键("ok"/"missing"/"denied"/"noexec"/"not_executable"/"exec_failed"/"not_a_jre"/"arch_mismatch")。 */
const char *sxcl_java_run_verdict_key(sxcl_java_run_verdict verdict);
/** 一条人话建议(为什么 + 下一步)。永远返回非空串。 */
const char *sxcl_java_run_verdict_hint(sxcl_java_run_verdict verdict);

/** "进程起不来 + 一行输出都没有"(退出码 127)时的**失败文案** —— 纯函数,便于单测钉住分支。
 *
 *  @param exit_code        子进程退出码(实测是 127)
 *  @param is_android       平台是不是安卓(1 = 是)
 *  @param looks_executable 只读体检结论:文件存在、可读、有执行位、且不在 noexec 挂载上(1 = 是)
 *  @param out/out_len      输出缓冲
 *  @return 写入的字节数(不含结尾 0);入参不合法返回 -1
 *
 *  is_android && looks_executable 时给**安卓专有**那条:文件本身没问题,是 SELinux 的
 *  W^X 规则拒绝 execve 应用私有目录里的文件(`execute_no_trans`,`permissive=0`)。
 *  设备实测的审计原文会长这样(进程树里 app= 是我们的包名):
 *    avc: denied { execute_no_trans } for path="/data/data/com.silentstudio.sxcl/.../bin/java"
 *         scontext=u:r:untrusted_app:s0:... tcontext=u:object_r:app_data_file:s0:...
 *         tclass=file permissive=0 app=com.silentstudio.sxcl
 *  结论:JVM 只能在**进程内** dlopen(libjli.so) + JLI_Launch 起来,不能 fork+exec。 */
int sxcl_java_exec_failure_text(int exit_code, int is_android, int looks_executable, char *out,
                                size_t out_len);

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

/* ── 原因键:比"类别"更具体的一层,每个键都对应一条**可执行**的建议 ──
 *
 * 为什么要有它(用户反馈与实测):只说"图形栈出问题"用户没法照着做 ——
 * 是驱动太旧?是选了 Vulkan 但设备不支持?是 Intel 核显的 OpenGL 版本不够?
 * 所以这里把结论细到"原因键"(稳定英文键,给 UI/统计/工单用),
 * 文案(短名 + 建议)放在同目录的 reason_lang.inc,中英各一份,与 lang_table.inc 同一套写法。
 *
 * 覆盖面参考 PCL(48 项)与 HMCL(约 60 条正则)的经验,文案全部自己写。 */
typedef enum sxcl_log_reason {
    SXCL_REASON_UNKNOWN = 0,           /**< "unknown":还没有可判断的线索 */
    SXCL_REASON_EXIT_OK,               /**< "exit_ok":正常退出 */
    SXCL_REASON_KILLED_BY_USER,        /**< "killed_by_user":用户/启动器主动结束 */
    SXCL_REASON_TIMED_OUT,             /**< "timed_out":超时被终止 */
    /* ── 内存与 JVM ── */
    SXCL_REASON_OUT_OF_MEMORY,         /**< "out_of_memory":Java 堆耗尽 */
    SXCL_REASON_OUT_OF_NATIVE_MEMORY,  /**< "out_of_native_memory":本地内存/线程不够 */
    SXCL_REASON_JVM_32BIT,             /**< "jvm_32bit":32 位 Java 上不了大内存 */
    SXCL_REASON_HEAP_TOO_LARGE,        /**< "heap_too_large":-Xmx 给大了/写错了 */
    SXCL_REASON_JVM_FATAL,             /**< "jvm_fatal":hs_err 级别的 JVM 崩溃 */
    SXCL_REASON_JVM_INTERNAL,          /**< "jvm_internal_error":JVM 内部断言/错误 */
    SXCL_REASON_STACK_OVERFLOW,        /**< "stack_overflow":递归/栈溢出 */
    /* ── Java 运行时本身 ── */
    SXCL_REASON_JAVA_NOT_FOUND,        /**< "java_not_found":没找到可用的 Java */
    SXCL_REASON_JAVA_BROKEN,           /**< "java_broken":Java 起不来/不完整 */
    SXCL_REASON_JAVA_VERSION_MISMATCH, /**< "java_version_mismatch":版本不符 */
    /* ── 图形栈 ── */
    SXCL_REASON_GRAPHICS_DRIVER,        /**< "graphics_driver":图形驱动(厂商未知) */
    SXCL_REASON_GRAPHICS_DRIVER_INTEL,  /**< "graphics_driver_intel":Intel 核显 */
    SXCL_REASON_GRAPHICS_DRIVER_NVIDIA, /**< "graphics_driver_nvidia":NVIDIA */
    SXCL_REASON_GRAPHICS_DRIVER_AMD,    /**< "graphics_driver_amd":AMD */
    SXCL_REASON_GRAPHICS_DRIVER_MESA,   /**< "graphics_driver_mesa":Mesa/llvmpipe */
    SXCL_REASON_GRAPHICS_DRIVER_ADRENO, /**< "graphics_driver_adreno":高通 Adreno */
    SXCL_REASON_GRAPHICS_DRIVER_MALI,   /**< "graphics_driver_mali":ARM Mali */
    SXCL_REASON_GRAPHICS_DRIVER_POWERVR,/**< "graphics_driver_powervr":PowerVR */
    SXCL_REASON_GRAPHICS_DRIVER_SOFTWARE,/**< "graphics_driver_software":软件渲染 */
    SXCL_REASON_GRAPHICS_DRIVER_OUTDATED,/**< "graphics_driver_outdated":驱动太旧 */
    SXCL_REASON_OPENGL_TOO_LOW,         /**< "opengl_too_low":OpenGL 版本不够 */
    SXCL_REASON_GLFW_INIT_FAILED,       /**< "glfw_init_failed":GLFW 初始化失败 */
    SXCL_REASON_PIXEL_FORMAT_FAILED,    /**< "pixel_format_failed":像素格式/加速不可用 */
    SXCL_REASON_NO_GL_CONTEXT,          /**< "no_gl_context":拿不到 GL 上下文 */
    SXCL_REASON_REMOTE_DESKTOP,         /**< "remote_desktop":远程桌面/无 GPU */
    SXCL_REASON_GPU_DRIVER_CRASH,       /**< "gpu_driver_crash":崩在显卡驱动里 */
    SXCL_REASON_VULKAN_UNAVAILABLE,     /**< "vulkan_unavailable":设备不支持 Vulkan */
    SXCL_REASON_VULKAN_FALLBACK,        /**< "vulkan_fallback":回退到了 OpenGL */
    /* ── 模组与加载器 ── */
    SXCL_REASON_MOD_DUPLICATE,          /**< "mod_duplicate":同一个模组装了两份 */
    SXCL_REASON_MOD_RESOLUTION_CONFLICT,/**< "mod_resolution_conflict":依赖解析冲突 */
    SXCL_REASON_MOD_MISSING_DEPENDENCY, /**< "mod_missing_dependency":缺前置模组 */
    SXCL_REASON_MOD_VERSION_MISMATCH,   /**< "mod_version_mismatch":模组与游戏版本不符 */
    SXCL_REASON_MOD_FILE_CORRUPTED,     /**< "mod_file_corrupted":模组文件坏了 */
    SXCL_REASON_MOD_LOADER_MISSING,     /**< "mod_loader_missing":没装对应的加载器 */
    SXCL_REASON_MOD_LOADER_VERSION,     /**< "mod_loader_version":加载器版本不匹配 */
    SXCL_REASON_MIXIN_FAILURE,          /**< "mixin_failure":Mixin 注入失败 */
    SXCL_REASON_OPTIFINE_CONFLICT,      /**< "optifine_conflict":OptiFine 与其它模组冲突 */
    SXCL_REASON_SHADER_FAILURE,         /**< "shader_failure":光影包出错 */
    SXCL_REASON_MOD_CRASH,              /**< "mod_crash":崩在某个模组的代码里 */
    /* ── 文件与库 ── */
    SXCL_REASON_MISSING_JAVA_LIBRARY,   /**< "missing_java_library":缺 Java 侧的库/原生库 */
    SXCL_REASON_MISSING_CLASS,          /**< "missing_class":缺类 */
    SXCL_REASON_MISSING_ASSET,          /**< "missing_asset":缺资源文件 */
    SXCL_REASON_NATIVES_EXTRACT_FAILED, /**< "natives_extract_failed":原生库没解出来 */
    SXCL_REASON_CORRUPT_JAR,            /**< "corrupt_jar":jar/zip 坏了 */
    SXCL_REASON_CLASSPATH_BROKEN,       /**< "classpath_broken":类路径不对/主类找不到 */
    SXCL_REASON_FILE_PERMISSION,        /**< "file_permission":权限/占用 */
    SXCL_REASON_DISK_FULL,              /**< "disk_full":磁盘满 */
    SXCL_REASON_PATH_NOT_FOUND,         /**< "path_not_found":路径不存在/非 ASCII 路径 */
    SXCL_REASON_ARCH_MISMATCH,          /**< "arch_mismatch":架构不符 */
    /* ── 账户与网络 ── */
    SXCL_REASON_ACCOUNT_INVALID_SESSION,/**< "account_invalid_session":登录态失效 */
    SXCL_REASON_ACCOUNT_AUTH_FAILED,    /**< "account_auth_failed":验证失败/没有这个游戏 */
    SXCL_REASON_NETWORK_UNREACHABLE,    /**< "network_unreachable":连不上 */
    SXCL_REASON_DNS_FAILURE,            /**< "dns_failure":域名解析失败 */
    SXCL_REASON_SSL_FAILURE,            /**< "ssl_failure":TLS/证书问题 */
    SXCL_REASON_NET_TIMEOUT,            /**< "net_timeout":网络超时 */
    SXCL_REASON_SERVER_OFFLINE,         /**< "server_offline":官方服务不可用 */
    /* ── 平台特有 ── */
    SXCL_REASON_ANDROID_NOEXEC,         /**< "android_noexec":共享存储不能执行 */
    SXCL_REASON_ANDROID_SELINUX,        /**< "android_selinux_exec":SELinux 拒绝 exec */
    /* ── 兜底 ── */
    SXCL_REASON_CRASH_UNKNOWN,          /**< "crash_unknown":崩了但线索不足 */
    SXCL_REASON_COUNT                   /**< 枚举计数(不是原因) */
} sxcl_log_reason;

/** 原因键缓冲长度(含 NUL)。 */
#define SXCL_LOG_REASON_KEY_MAX 48
/** 原因短名缓冲长度(含 NUL)。 */
#define SXCL_LOG_REASON_NAME_MAX 96
/** 可执行建议缓冲长度(含 NUL;中英都要放得下)。 */
#define SXCL_LOG_REASON_ADVICE_MAX 512

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
    sxcl_log_reason reason_kind; /**< 这一行指向的原因键(认不出 = SXCL_REASON_UNKNOWN);
                                  *   注意与下面那个 reason 文本字段区分:一个是"分类",
                                  *   一个是"原文摘录"(历史字段名,没改是为了不动旧调用点) */
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
    char advice[256];                  /**< 一条人话结论(UI 直接显示这一条) */
    /* ── 原因键(比 conclusion 更具体;见 sxcl_log_reason)── */
    sxcl_log_reason reason;            /**< 最有把握的原因键 */
    int reason_score;                  /**< 判定分数(越大越具体;内部用,便于调试) */
    char reason_key[SXCL_LOG_REASON_KEY_MAX];     /**< 稳定英文键 */
    char reason_name[SXCL_LOG_REASON_NAME_MAX];   /**< 短名(按当前语言)*/
    char reason_advice[SXCL_LOG_REASON_ADVICE_MAX]; /**< 可执行建议(按当前语言)*/
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

/* ── 原因键的查表与判定(第 3b 节)── */

/** 原因总数(不含 SXCL_REASON_UNKNOWN?包含 —— 遍历文案完备性用)。 */
size_t sxcl_log_reason_count(void);
/** 第 index 个原因(按枚举顺序);越界返回 SXCL_REASON_UNKNOWN。 */
sxcl_log_reason sxcl_log_reason_at(size_t index);
/** 稳定英文键("out_of_memory" / "mod_duplicate" …);越界/UNKNOWN 返回 "unknown"。 */
const char *sxcl_log_reason_key(sxcl_log_reason reason);
/** 短名(人话;"内存不足(Java 堆耗尽)")。lang 传 "en-us" 得英文,其余得中文。
 *  用户语言包里有 crash.reason.<键> 时优先用它(见 lang_table 的覆盖规则)。 */
const char *sxcl_log_reason_name(sxcl_log_reason reason, const char *lang);
/** 可执行建议(为什么 + 下一步)。lang 同上。**永远返回非空串**。 */
const char *sxcl_log_reason_advice(sxcl_log_reason reason, const char *lang);
/** "短名:建议" 合成一句(给 UI/CLI 的行内显示用)。写不下会截断(保证 NUL 结尾)。
 *  返回写入字节数;参数不合法返回 -1。 */
int sxcl_log_reason_text(sxcl_log_reason reason, const char *lang, char *out, size_t out_cap);
/** 单行 -> 原因键(认不出返回 SXCL_REASON_UNKNOWN)。规则表与汇总用的是**同一张**
 *  (规则见 logscan.c 的 kReasonRules),所以单测可以逐行钉死。 */
sxcl_log_reason sxcl_log_classify_reason(const char *line);
/** 原因键属于哪一类结论(给旧代码/UI 分组用)。 */
sxcl_log_conclusion sxcl_log_reason_conclusion(sxcl_log_reason reason);

/* ══════════════════════ 3c. 崩溃取证(读游戏自己写的文件) ══════════════════════
 *
 * stdout/stderr 只能看到游戏**还活着**时吐出来的那部分;进程一崩,真正的死因在
 * <game>/crash-reports 目录下的 txt 报告与 <game>/logs/latest.log 里。这两个文件与 stdout
 * (注:这里**不能**写出 "斜杠 + 星号" 那两个字,NDK 的 clang 在 -Werror 下会把块注释里的
 *  "斜杠星号" 当 -Wcomment 直接判编译失败 —— 我们交叉编译 jre_hosted.c 时真踩过。)
 * 走**同一套**分析(sxcl_log_summary_add),所以原因键不会因为来源不同而分叉。 */

/** 扫游戏目录里的取证文件,逐行喂进 summary(summary 可空 = 只要事实)。
 *  facts 可空;返回喂出的行数;-1 = 参数不合法。
 *  flags 用 SXCL_CRASH_SCAN_*(默认 SXCL_CRASH_SCAN_ALL);max_bytes = 0 用默认。
 *  "用户点查看详情"与"进程退出后发现是崩溃"都走这里 —— 纯读文件,不起进程。 */
long long sxcl_launch_scan_artifacts(const char *game_dir, sxcl_log_summary *summary,
                                     sxcl_crash_evidence *facts, unsigned flags, size_t max_bytes,
                                     char *err, size_t err_len);

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

/* ── 启动前"补全文件"(用户点名:PCL 启动前有这一步,我们以前只报告不补) ──
 *
 * 参照(PCL 的 ModLaunch.vb:113-124 第 3 步 "补全文件" = DlClientFix):
 *   把"这个版本运行需要哪些文件"翻译成一张带 (大小, SHA1) 的清单交给下载器 ——
 *   缺失的下来、**已存在且校验通过的一个字节都不下**;它跑在"拼参数/解压 natives"之前。
 * 我们的做法与 PCL 有一处不同、也是更强的地方:检查与下载都在**同一个下载引擎**里
 * (engine.h:多候选路重试 / 分片 / 限速 / 断点续传 / 哈希缓存),所以"分析"这一步很便宜。 */

/** 一次补全的统计。回调写,driver 只读并回填到 sxcl_launch_result。 */
typedef struct sxcl_launch_complete {
    int files_total;       /**< 版本 JSON 里这个版本要用的文件总数(依赖库 + 客户端 jar) */
    int files_downloaded;  /**< 真的下下来的文件数 */
    int files_failed;      /**< 下失败的 */
    int files_skipped;     /**< 命中"已存在且校验通过"、一个字节都没下的 */
    int64_t bytes_done;    /**< 这次真的写下去的字节数(命中的不算) */
    char error[192];       /**< 非空 = 有文件没补上(人话;driver **不把它当致命**) */
} sxcl_launch_complete;

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
    int dry_run;                /**< 非 0 = 只准备(选 Java / 写 options.txt / 拼 argv),不起进程。
                                 *   **注意:dry-run 也会补全文件**(与 PCL 一致 —— 补全在"拼参数"之前,
                                 *   不补的话拼出来的命令行指着一堆不存在的文件,自检就没意义了)。 */
    /* ── 启动前"补全文件"(见 manifest.h 的 sxcl_version_plan_fetch) ──
     * 两样都要给才算数:complete_files 非 0 且 engine_opts 里有 transport_factory。
     * 不给就保持旧行为:**缺什么只报告** —— 而且那份"缺什么"是从**游戏自己的日志**里
     * 读出来的(游戏已经崩了才知道)。 */
    int complete_files;
    /** 下载引擎配置(workers / 限速 / 分片 / 哈希缓存 / **传输后端工厂**);可空 = 不补全。
     *  与 install.h 的 engine_opts 同一个口径:核心库只认这张表,不自己造后端。 */
    const sxcl_engine_opts *engine_opts;
    /** 非 0 = 镜像优先(与安装同一个口径:download.source=bmclapi/auto 时置位)。 */
    int prefer_mirror;
    /** 镜像根;空 = 核心默认(BMCLAPI)。只有 prefer_mirror 非 0 时才用得上。 */
    const char *mirror_base;
    /** 资源文件(assets objects)补到哪一档(docs/24 的 P0b):
     *    0 = 不补(只要清单里那些:版本 JSON / 客户端 jar / 依赖库 / 资源**索引**);
     *    1 = **只比大小**(默认;PCL 启动前就是这个口径 —— 5000+ 个文件不逐个算哈希);
     *    2 = 强校验(每个对象算 SHA-1;有哈希缓存时第二次很快,首次慢)。
     *  资源对象要先有索引才能展开,所以它是**第二遍**(第一遍把索引下下来之后再跑一次)。 */
    int complete_assets;
    /** 每落定一个文件问一次;非 0 = 取消(可空)。**从工作线程调用**,实现里别做重活。 */
    int (*complete_is_cancelled)(void *ud);
    void *complete_cancel_ud;
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
    char missing[160];          /**< 从日志里原样带出来的"缺什么"(游戏已经崩了才知道的那一种) */
    /* ── 启动前补全文件的结果(见 sxcl_launch_complete) ── */
    int complete_ran;           /**< 1 = 这一步真的跑过(注入了执行器且没被 no_complete_files 关掉) */
    sxcl_launch_complete complete; /**< 补全统计:共几个 / 下了几个 / 跳过几个 / 失败几个 */
    sxcl_log_conclusion conclusion; /**< 汇总结论 */
    char conclusion_text[256];  /**< 一条人话结论(UI 直接显示这一条) */
    sxcl_log_summary log;       /**< 完整日志汇总:想深入到"缺哪个类/多少行"就用它 */
    /* ── 原因键与崩溃取证(加了这一层之后:UI 不再只能说"图形栈出问题")── */
    sxcl_log_reason reason;     /**< 最有把握的原因键(见 sxcl_log_reason) */
    char reason_key[SXCL_LOG_REASON_KEY_MAX];       /**< "out_of_memory" / "mixin_failure" … */
    char reason_name[SXCL_LOG_REASON_NAME_MAX];     /**< 短名(按语言)*/
    char reason_advice[SXCL_LOG_REASON_ADVICE_MAX]; /**< 可执行建议(按语言)*/
    sxcl_crash_evidence artifacts; /**< 读了哪份报告/latest.log:路径、编码、行数(全是事实)*/
    int artifacts_scanned;      /**< 1 = 退出后真的扫过游戏目录(崩了才扫) */
    char crash_report_path[SXCL_CRASH_PATH_MAX]; /**< 便捷副本 = artifacts.report_path */
    long long crash_report_lines;   /**< 便捷副本 = artifacts.report_lines */
    long long latest_log_lines;     /**< 便捷副本 = artifacts.latest_log_lines */
} sxcl_launch_result;

/** 跑一次启动:读版本 JSON -> 选 Java -> 写 options.txt(渲染后端) -> 建 natives -> 拼 argv
 *  -> 起进程 -> 逐行归类(游戏输出同时落盘,见 log.h 第 5 节)-> 出一条人话结论。
 *
 *  进程退出后如果**是崩溃**(退出码非 0 / 归到崩溃类),会再读一次游戏自己写的
 *  crash-reports 与 logs/latest.log,喂进同一套分析,把原因键与可执行建议填进结果
 *  (artifacts 里是"读了哪个文件、什么编码、多少行"这些事实);正常退出不读,
 *  免得白扫一遍大日志。
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
