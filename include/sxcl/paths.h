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
#define SXCL_PATHS_MAX_CANDIDATES 8     /* 一次探测最多列出几个候选 */
#define SXCL_PATHS_PATH_MAX       640   /* 单个路径的字节数上限(UTF-8,中文一个 3 字节) */
#define SXCL_PATHS_LABEL_MAX      64    /* "官方启动器(APPDATA)" 这类来源标签 */
#define SXCL_PATHS_DESC_MAX       256   /* 人话描述 */
#define SXCL_PATHS_ERROR_MAX      256   /* 人话错误 */

/** 一个候选游戏目录。全部字段由模块填好,调用方只读。 */
typedef struct sxcl_game_folder {
    char path[SXCL_PATHS_PATH_MAX];       /**< 绝对/原样路径(UTF-8) */
    char label[SXCL_PATHS_LABEL_MAX];     /**< 这是谁留下的目录:官方启动器(APPDATA)/用户目录/… */
    char describe[SXCL_PATHS_DESC_MAX];   /**< 人话:"<路径>(<label>,N 个版本)" */
    int versions;                         /**< versions/ 下数出来的版本数(0 = 没有 versions/ 或空) */
    int has_assets;                       /**< 有 assets/ 说明真的玩过 */
    int has_launcher_profiles;            /**< 有 launcher_profiles.json */
    int exists;                           /**< 1 = 这个路径确实是目录 */
    int score;                            /**< 排序用:有版本 > 有资源 > 有 profiles;不存在为 -1 */
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
 *  (界面要能把"当前配置的目录不存在"显示出来,所以这不是错误)。 */
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

#ifdef __cplusplus
}
#endif
#endif /* SXCL_PATHS_H */
