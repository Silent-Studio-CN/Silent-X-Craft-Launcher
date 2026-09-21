/* SXCL-C 游戏目录探测(路径层)—— 别让用户自己填路径。
 *
 * 逐条对齐 Python 版 src/services/minecraft/folders.py(那份是实测过的):
 *   1) 启动器所在目录(便携版习惯:SXCL.exe 旁边的 .minecraft,也认 minecraft / MC)
 *   2) APPDATA/.minecraft(官方启动器;HMCL 也爱往这儿塞)
 *   3) 用户主目录下的 .minecraft
 *   4) 桌面上的 .minecraft(真的有用户这么放,含中文"桌面")
 *   5) 当前配置里的目录(哪怕是空的也列出来,让用户知道现在用的是哪个)
 * 每个候选都**实际数一下 versions/ 里有多少版本**(含只有 JSON 没有 jar 的加载器版本),
 * 这样"装了 12 个版本的那个目录"会排在"刚建的空目录"前面。
 *
 * 术语:候选(候选目录)= 一个可能放游戏数据的路径;择优 = 按 Python 的 score 取第一个。
 *
 * 不抛异常:所有函数都返回错误码,人话原因写进调用方的 err 缓冲(可空)。
 * 路径一律 UTF-8(Windows 侧内部转宽字符,中文/日文路径可用)。
 */
#ifndef SXCL_PATHS_H
#define SXCL_PATHS_H

#include <stddef.h>

#include "sxcl/android.h" /* Android 侧的"能不能读/能不能执行"分类 */

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码(负数为错,与 fs/net/loader 的约定一致) */
#define SXCL_PATHS_OK               0
#define SXCL_PATHS_ERR_ARG        (-1)  /* 参数不合法(空指针/空串) */
#define SXCL_PATHS_ERR_IO         (-2)  /* 路径不可访问 / 目录读不出来 */
#define SXCL_PATHS_ERR_SPACE      (-3)  /* 输出缓冲不够(内容被截断) */
#define SXCL_PATHS_ERR_UNSUPPORTED (-4) /* 该平台没有默认值(Android:请调用方指定) */
#define SXCL_PATHS_ERR_EMPTY      (-5)  /* 一个候选都没有(连默认路径都拼不出来) */

/* ── 尺寸上限 ── */
#define SXCL_PATHS_MAX_CANDIDATES 32    /* 一次探测最多列出几个候选(安装器家族一种就能出好几个实例目录) */
#define SXCL_PATHS_MAX_ROOTS      96    /* 一次最多产出多少个候选**根**(纯字符串表的上限) */
#define SXCL_PATHS_SOURCE_MAX     24    /* 候选来源的稳定英文键("HMCL"/"Prism"/"CurseForge"…) */
#define SXCL_PATHS_PATH_MAX       640   /* 单个路径的字节数上限(UTF-8,中文一个 3 字节) */
#define SXCL_PATHS_LABEL_MAX      64    /* "官方启动器(APPDATA)" 这类来源标签 */
#define SXCL_PATHS_OWNER_MAX      64    /* 这份目录是谁的:"本应用"/"FCL"/"HMCL"/"共享存储" */
#define SXCL_PATHS_MAX_PROBES     20    /* 安卓诊断一次最多列几个"我们找过的地方" */
#define SXCL_PATHS_DESC_MAX       256   /* 人话描述 */
#define SXCL_PATHS_ERROR_MAX      256   /* 人话错误 */

/** 一个候选游戏目录。全部字段由模块填好,调用方只读。 */
typedef struct sxcl_game_folder {
    char path[SXCL_PATHS_PATH_MAX];       /**< 绝对/原样路径(UTF-8) */
    char label[SXCL_PATHS_LABEL_MAX];     /**< 这是谁留下的目录:官方启动器(APPDATA)/用户目录/… */
    char owner[SXCL_PATHS_OWNER_MAX];     /**< 谁拥有的目录:"本应用"/"FCL"/"HMCL"/"共享存储";桌面版留空 */
    char describe[SXCL_PATHS_DESC_MAX];   /**< 人话:"<路径>(<label>,N 个版本)" */
    int versions;                         /**< versions/ 下数出来的版本数(0 = 没有 versions/ 或空) */
    int has_assets;                       /**< 有 assets/ 说明真的玩过 */
    int has_launcher_profiles;            /**< 有 launcher_profiles.json */
    int exists;                           /**< 1 = 这个路径确实是目录 */
    int score;                            /**< 排序用:有版本 > 有资源 > 有 profiles;不存在为 -1 */
    int priority;                         /**< 候选来源的优先级(见 SXCL_PATHS_PRIORITY_*);
                                               排序时**先看它**,同优先级才比 score。
                                               已配置目录最高,保证"用户指定的那个"永远排在第一位。 */
    char source[SXCL_PATHS_SOURCE_MAX];   /**< 来源的稳定英文键,给日志/测试断言用 */
} sxcl_game_folder;

/** 一次探测的结果。**零分配**:固定大小数组,调用方可以放栈上。 */
typedef struct sxcl_game_folders {
    sxcl_game_folder items[SXCL_PATHS_MAX_CANDIDATES];
    size_t count;      /**< 有效候选数(已按 score 降序) */
    size_t dropped;    /**< 因为超过上限被丢掉的候选数(不静默,调用方可以提示) */
} sxcl_game_folders;

/** 平台默认游戏目录(不是"探测",只是拼默认路径):
 *    Windows  %APPDATA%\.minecraft(APPDATA 没有时用 %USERPROFILE%\.minecraft)
 *    macOS    ~/Library/Application Support/minecraft
 *    Linux    ~/.minecraft
 *    Android  **没有默认**,返回 SXCL_PATHS_ERR_UNSUPPORTED 并写人话 err,
 *             请调用方(安卓 UI)把自己的私有目录传进来。
 *  成功返回 SXCL_PATHS_OK 并把路径写进 out(截断返回 SXCL_PATHS_ERR_SPACE)。 */
int sxcl_paths_default_game_dir(char *out, size_t out_len, char *err, size_t err_len);

/** 启动器自己所在的目录(便携版的判定依据:它旁边有没有 .minecraft)。
 *  拿不到(Android/iOS 沙箱等)返回 SXCL_PATHS_ERR_UNSUPPORTED。 */
int sxcl_paths_program_dir(char *out, size_t out_len, char *err, size_t err_len);

/** 探测单个候选目录(Python 的 _inspect)。只读不写,失败不报错:
 *  目录不存在时 exists=0、versions=0、score=-1,仍然返回 SXCL_PATHS_OK
 *  (界面要能把"当前配置的目录不存在"显示出来,所以这不是错误)。
 *  owner 是"这份目录是谁的"(可空;桌面版不区分,安卓上用来显示 FCL/HMCL)。 */
int sxcl_paths_inspect_full(const char *path, const char *label, const char *owner,
                            sxcl_game_folder *out);

/** 等价于 sxcl_paths_inspect_full(path, label, NULL, out)(老调用点不用改)。 */
int sxcl_paths_inspect(const char *path, const char *label, sxcl_game_folder *out);

/** 把一个候选追加到列表(满了丢最差的:分数更低者,记 dropped)。 */
int sxcl_paths_push(sxcl_game_folders *folders, const sxcl_game_folder *folder);

/** 按 score 降序排序(稳定;同分保持原有先后,便于测试断言)。 */
void sxcl_paths_sort(sxcl_game_folders *folders);

/** 探测所有候选位置(Python 的 detect_game_folders):
 *  平台候选 + configured_dir(可空;非空且不是平台候选之一时追加,标签"当前配置")。
 *  去重按路径(Windows 大小写不敏感)、**只保留真实存在的目录**、按 score 降序。
 *  一个都没找到时:count=0,返回 SXCL_PATHS_OK(err 里给人话说明:没找到游戏目录)。
 *  err 可空;err_len 为 0 时什么都不写。 */
int sxcl_paths_detect(sxcl_game_folders *folders, const char *configured_dir, char *err, size_t err_len);

/** 挑一个最像"用户平时在玩的"目录(Python 的 best_game_folder):
 *  第一个 versions>0 的;都不为 0 时取第一个;列表为空返回 NULL。
 *  列表必须已经排过序(sxcl_paths_detect / sxcl_paths_sort 之后)。 */
const sxcl_game_folder *sxcl_paths_best(const sxcl_game_folders *folders);

/** 人话描述(Python 的 describe):"<路径>(<label>,N 个版本)"。
 *  目录不存在时在末尾补"(目录不存在)"。写不进返回 SXCL_PATHS_ERR_SPACE。 */
int sxcl_paths_describe(const sxcl_game_folder *folder, char *out, size_t out_len);

/** 数一个候选目录里 versions/ 下有几个版本(Python 的 _count_versions):
 *  <name>/<name>.json 或 <name>/<name>.jar 存在算一个;
 *  否则目录里只要有任意 *.json 也算一个(加载器版本常常只有 JSON,jar 靠继承)。
 *  不是目录 / 没有 versions/ 返回 0。 */
int sxcl_paths_count_versions(const char *path);

/** "这算不算一个游戏目录":有 versions/ 且里面至少有 1 个版本。返回 0/1。 */
int sxcl_paths_is_game_dir(const char *path);

/** 给所有页面用的取值来源:configured_dir 非空就先用它(哪怕目录不存在,也要如实返回让界面提示);
 *  否则用探测出来的最佳候选;再没有就用平台默认目录。
 *  成功返回 SXCL_PATHS_OK;连平台默认都拼不出来返回 SXCL_PATHS_ERR_EMPTY。 */
int sxcl_paths_resolve_game_dir(const char *configured_dir, char *out, size_t out_len,
                                char *err, size_t err_len);

/* ══════════════════════ 候选根表(纯字符串,三个平台 + 安卓) ══════════════════════
 *
 * 为什么要把"候选表"从 sxcl_paths_detect 里拆出来:
 *   1) 桌面路原来只有 Python folders.py 的 5 条(便携/APPDATA/HOME/桌面/当前配置),
 *      而机器上真正在玩的数据常常在**别的启动器**留下的目录里 —— HMCL、MultiMC/Prism
 *      这一家族(instances/<名字>/minecraft)、CurseForge、ATLauncher、FCL、PojavLauncher…
 *      实测(本机 Windows)就有一份在 %APPDATA%\.minecraft 之外,用户反馈"明明有游戏却扫不到"。
 *   2) 候选表要能**注入环境变量**,否则单测只能测本机那一个平台,Linux/macOS/安卓三套
 *      规则永远没人验证(与 sxcl_java_candidate_paths / sxcl_java_scan_roots 同一个理由)。
 *   3) 候选根**不代表存在**:存在性、能不能读、像不像 MC 目录,由探测层实测后如实标注。
 *
 * 排序口径(与"已配置目录最高优先"一致):priority 降序 -> score 降序 -> 保持候选表原有先后。
 */

/** 目标平台。paths.h 不依赖 launch.h(那边拖着 json/engine 一大串),所以自带一份枚举。 */
typedef enum sxcl_paths_os {
    SXCL_PATHS_OS_WINDOWS = 0,
    SXCL_PATHS_OS_LINUX,
    SXCL_PATHS_OS_MACOS,
    SXCL_PATHS_OS_ANDROID
} sxcl_paths_os;

/** 候选来源优先级(数值越大越优先;排序先看它)。 */
#define SXCL_PATHS_PRIORITY_CONFIGURED  100 /**< 已配置目录:用户明确指定的,永远第一 */
#define SXCL_PATHS_PRIORITY_PORTABLE     80 /**< 启动器自己旁边的 .minecraft(便携版) */
#define SXCL_PATHS_PRIORITY_OFFICIAL     60 /**< 官方启动器(%APPDATA% / Application Support) */
#define SXCL_PATHS_PRIORITY_ANDROID      50 /**< 安卓:本应用私有目录 + 共享存储已知位置 */
#define SXCL_PATHS_PRIORITY_THIRD_PARTY  40 /**< HMCL/MultiMC/Prism/CurseForge/ATLauncher… */
#define SXCL_PATHS_PRIORITY_USER_HOME    30 /**< 用户主目录 / Flatpak 沙箱 */
#define SXCL_PATHS_PRIORITY_DESKTOP      20 /**< 桌面(真的有用户这么放) */

/** 探测用的环境变量集合(值都是 UTF-8,可空)。 */
typedef struct sxcl_paths_env {
    const char *app_data;       /**< %APPDATA%(Windows/macOS 的习惯位置靠它) */
    const char *local_app_data; /**< %LOCALAPPDATA% */
    const char *user_home;      /**< $HOME / %USERPROFILE% */
    const char *program_dir;    /**< 启动器自己所在目录(便携版判据);可空 */
    const char *xdg_data_home;  /**< $XDG_DATA_HOME;空 = <home>/.local/share */
    const char *android_files;  /**< Android:本应用私有 files 目录 */
    const char *android_shared; /**< Android:共享存储根;空 = /storage/emulated/0 */
} sxcl_paths_env;

/** 环境变量 + 背后的存储(字段被 env 引用,别当成可随意拷贝的值)。 */
typedef struct sxcl_paths_env_store {
    sxcl_paths_env env;
    char app_data[SXCL_PATHS_PATH_MAX];
    char local_app_data[SXCL_PATHS_PATH_MAX];
    char user_home[SXCL_PATHS_PATH_MAX];
    char program_dir[SXCL_PATHS_PATH_MAX];
    char xdg_data_home[SXCL_PATHS_PATH_MAX];
    char android_files[SXCL_PATHS_PATH_MAX];
    char android_shared[SXCL_PATHS_PATH_MAX];
} sxcl_paths_env_store;

/** 一个候选**根**:路径 + 谁留下的 + 优先级。
 *  expand=1 表示"这是个容器目录(installer 家族的数据目录),要往下展开一层 instances/"。 */
typedef struct sxcl_paths_root {
    char path[SXCL_PATHS_PATH_MAX];
    char label[SXCL_PATHS_LABEL_MAX];   /**< 直接给界面用:"Prism Launcher 实例" */
    char owner[SXCL_PATHS_OWNER_MAX];   /**< "本应用"/"HMCL"/"Prism"/"共享存储"…;桌面通用目录留空 */
    char source[SXCL_PATHS_SOURCE_MAX]; /**< 稳定英文键:"APPDATA"/"HMCL"/"Prism"/… */
    int priority;
    int expand;                         /**< 1 = 还要展开实例(见 sxcl_paths_expand_roots) */
} sxcl_paths_root;

/** 本机平台。 */
sxcl_paths_os sxcl_paths_current_os(void);
/** 平台名("windows"/"linux"/"macos"/"android"),日志用。 */
const char *sxcl_paths_os_name(sxcl_paths_os os);

/** 从真实环境变量抓一份(UTF-8)。 */
void sxcl_paths_env_capture(sxcl_paths_env_store *store);

/** 平台候选根表(**纯字符串,不碰文件系统**;与 Python folders.py 的候选顺序同源)。
 *  覆盖:便携目录 -> 官方启动器 -> HMCL -> MultiMC/Prism 家族 -> CurseForge -> ATLauncher
 *        -> 用户主目录 / Flatpak -> 桌面 -> 安卓本应用私有目录 + 共享存储已知位置。
 *  已配置目录不在这里(由 sxcl_paths_detect_ex 以最高优先级插到最前面)。
 *  返回写入 out 的条数(≤ cap)。 */
size_t sxcl_paths_roots(const sxcl_paths_env *env, sxcl_paths_os os, sxcl_paths_root *out,
                        size_t cap);

/** 把 expand=1 的容器根展开一层(installer 家族的 instances/<名字>/{.minecraft,minecraft,<名字>})。
 *  只读文件系统;不是容器/不存在就原样保留。返回写入 out 的条数。 */
size_t sxcl_paths_expand_roots(const sxcl_paths_root *roots, size_t count, sxcl_paths_root *out,
                               size_t cap);

/** 探测全部候选(可注入 env/os 的版本,夹具测试走这个):
 *  候选根(平台表 + 展开实例 + 最高优先的 configured_dir)逐个实测 -> 只留**真实存在**的目录
 *  -> 去重(Windows 大小写不敏感)-> 按 priority/score 降序。语义与 sxcl_paths_detect 一致。 */
int sxcl_paths_detect_ex(const sxcl_paths_env *env, sxcl_paths_os os, sxcl_game_folders *folders,
                         const char *configured_dir, char *err, size_t err_len);

/** 一个候选根"像不像 MC 目录"的判据(借用 PCL/官方启动器的常识,全部实测):
 *  versions/ 里数出来的版本数、libraries/、assets/、launcher_profiles.json、logs/ 之类。
 *  返回判据位掩码(见 SXCL_PATHS_MARK_*)。 */
#define SXCL_PATHS_MARK_VERSIONS   1 /**< 有 versions/ 且至少一个版本(最硬的证据) */
#define SXCL_PATHS_MARK_LIBRARIES  2 /**< 有 libraries/ */
#define SXCL_PATHS_MARK_ASSETS     4 /**< 有 assets/ */
#define SXCL_PATHS_MARK_PROFILES   8 /**< 有 launcher_profiles.json(官方启动器留下的) */
#define SXCL_PATHS_MARK_LOGS      16 /**< 有 logs/(跑过一次就有) */
int sxcl_paths_marks(const char *path);

/** 判据位掩码 -> 人话("有 versions/(3 个版本)、有 libraries/" ;空目录给"空目录")。
 *  写不进返回 SXCL_PATHS_ERR_SPACE。 */
int sxcl_paths_marks_text(int marks, int versions, char *out, size_t out_len);

/* ══════════════════════ Android:自动扫描(第 5 条候选之外) ══════════════════════
 *
 * 桌面的五条候选(便携/APPDATA/用户目录/桌面/当前配置)在安卓上几乎全是空的:
 *   - 没有 APPDATA,没有"桌面";HOME 被安卓打包层指到应用私有 files 目录;
 *   - 用户真正在玩的那份数据,在**共享存储**上,而且是别的启动器留下的:
 *     实测(192.168.220.33)FCL 的目录是 /storage/emulated/0/FCL/.minecraft。
 * 所以安卓必须另给一份候选表,否则用户"明明有游戏,自动扫描却说没找到"(用户反馈原文)。
 *
 * 与 Java 那边同一个道理:能不能读要**问文件系统**,不能假设 —— 所以除了探测,
 * 还提供一份"我们找过哪些地方、为什么没用上"的诊断给设置页显示。
 */

/** 一个候选的更多字段:除了路径,还有"这是谁留下的"。 */
typedef struct sxcl_android_root {
    char path[SXCL_PATHS_PATH_MAX];
    char label[SXCL_PATHS_LABEL_MAX]; /**< 来源标签,直接给界面用 */
    char owner[SXCL_PATHS_OWNER_MAX]; /**< "本应用"/"FCL"/"HMCL"/"PojavLauncher"/"共享存储" */
} sxcl_android_root;

/** 安卓上放游戏目录的已知位置(**纯字符串,不碰文件系统**,所以能在桌面上单测)。
 *  files_dir   : 本应用私有 files 目录(SXCL_ANDROID_FILES);可空。
 *  shared_root : 共享存储根;可空 = /storage/emulated/0。
 *  返回写入 out 的条数(≤ cap)。列表本身**不代表存在**,存在与否由 sxcl_paths_probe_android 判定。 */
size_t sxcl_paths_android_roots(const char *files_dir, const char *shared_root,
                                sxcl_android_root *out, size_t cap);

/** Android 版探测:把安卓候选表 + 当前配置一起过一遍,只保留**真实存在**的目录,
 *  按 score 降序。语义与 sxcl_paths_detect 完全一致,只是候选表不同 —— 单独暴露
 *  是为了能在桌面上用夹具目录单测(安卓真机不方便跑单测)。
 *  返回码与 sxcl_paths_detect 一致(永远 SXCL_PATHS_OK,没找到只是 count=0 + 人话 err)。 */
int sxcl_paths_detect_android(const char *files_dir, const char *shared_root,
                              sxcl_game_folders *folders, const char *configured_dir,
                              char *err, size_t err_len);

/** 一个候选的体检结论(给设置页"为什么没找到"用)。 */
typedef struct sxcl_game_probe {
    char path[SXCL_PATHS_PATH_MAX];
    char label[SXCL_PATHS_LABEL_MAX];
    char owner[SXCL_PATHS_OWNER_MAX];
    int exists;                        /**< 1 = 这个目录真的在 */
    sxcl_android_access access;        /**< 为什么没用上(OK/MISSING/DENIED/NOEXEC/...) */
    int versions;                      /**< 数出来的版本数(0 = 没有 versions/ 或空) */
    char reason[SXCL_PATHS_DESC_MAX];  /**< 人话:一行说清这个候选怎么了(原始原因,含路径) */
    char hint[SXCL_PATHS_DESC_MAX];    /**< 人话:怎么办(按**游戏目录**的场景给,不是 Java 那套) */
} sxcl_game_probe;

typedef struct sxcl_game_probes {
    sxcl_game_probe items[SXCL_PATHS_MAX_PROBES];
    size_t count;
    size_t usable;                     /**< 其中真的能当游戏目录用的条数(exists 且 versions>0) */
} sxcl_game_probes;

/** 安卓诊断:列出**全部**安卓候选(存在的不存在的、读得了读不了的),每人人话一句。
 *  与 sxcl_paths_detect_android 共用同一张候选表,顺序也一致(配置在最前)。
 *  返回写进 out->count 的条数。cfg 可空。 */
size_t sxcl_paths_probe_android(const char *files_dir, const char *shared_root,
                                const char *configured_dir, sxcl_game_probes *out);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_PATHS_H */
