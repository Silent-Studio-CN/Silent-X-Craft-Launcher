/* SXCL-C 启动层(纯逻辑)—— Java 运行时探测 / 启动参数拼装 / 日志归类。
 *
 * 这一层**不启动任何进程**:它只做"能单独喂输入就能断言输出"的纯逻辑,
 * 把真正起 java.exe 的活留给上层(process.h + 下一步的 launch/session)。
 * 这样单测不需要这台机器装了 JDK、也不需要 GPU。
 *
 * 三块内容:
 *   1) Java 运行时探测(§1,java.c)
 *      - 按平台给出候选路径(纯字符串)+ 真实扫目录找 bin/java;
 *      - 解析 <java_home>/release 键值清单(比跑 java -version 快,且不依赖执行权限);
 *      - 也支持从**已有的 java -version 文本**解析,供调用方把捕获到的输出喂进来;
 *      - 按版本 JSON 的 javaVersion.majorVersion 排序候选:精确匹配 > 更高 > 更低。
 *   2) 启动参数拼装(§2,args.c):版本 JSON + 上下文 -> argv 数组(可直接给 sxcl_process_opts)。
 *   3) 日志归类(§3,logscan.c):一行文本 -> 类别 + 抽取到的关键信息;一批行 -> 一条人话结论。
 *
 * 命名/内存约定与其它模块一致:返回的指针归句柄所有,句柄释放即失效;不抛出、不 abort。
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
} sxcl_java_env_store;

/** 一个候选安装点(java 可执行文件全路径 + 归属)。 */
typedef struct sxcl_java_candidate {
    char path[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    char source[24];
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

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LAUNCH_H */
