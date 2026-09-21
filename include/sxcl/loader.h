/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LOADER_H
#define SXCL_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 返回码 ── */
#define SXCL_LOADER_OK           0
#define SXCL_LOADER_ERR_ARG     (-1)
#define SXCL_LOADER_ERR_IO      (-2)
#define SXCL_LOADER_ERR_FORMAT  (-3)
#define SXCL_LOADER_ERR_NOMEM   (-4)
#define SXCL_LOADER_ERR_SPACE   (-5)

/* sxcl_loader_install 自己的返回码:0 = 成功;1 = 安装失败(阶段与人话原因在结果里);
 * 负数 = 参数不合法(压根没开始装,复用上面的 SXCL_LOADER_ERR_*)。 */
#define SXCL_LOADER_INSTALL_OK      0
#define SXCL_LOADER_INSTALL_FAILED  1

/* ── 尺寸上限(都是"够用就行",超了报 SXCL_LOADER_ERR_SPACE,不静默截断) ── */
#define SXCL_LOADER_CMD_ARG_MAX   640   /* 单个参数/路径的最长字节数(UTF-8,中文一个占 3 字节) */
#define SXCL_LOADER_CMD_MAX_ARGS  12    /* 一组命令行的参数个数上限 */
#define SXCL_LOADER_MAX_ISSUES    8     /* 一次兼容判定最多给几条结论(超出记在 dropped 里) */
#define SXCL_LOADER_MESSAGE_MAX   192   /* 给人看的一句话 */
#define SXCL_LOADER_FIX_MAX       160   /* 给人看的修法 */
#define SXCL_LOADER_ERROR_MAX     256   /* 人话错误信息 */
#define SXCL_LOADER_TEXT_MAX      160   /* 进度说明 */
#define SXCL_LOADER_MAX_LIBRARIES 256   /* 一次能报出的依赖库条数上限 */

/** 安装器总超时(Python: TIMEOUT = 1800,大版本要下几百 MB 库)。 */
#define SXCL_LOADER_DEFAULT_TIMEOUT_MS (30 * 60 * 1000)
/** OptiFine 安装器的超时(Python 版给它 900 秒)。 */
#define SXCL_LOADER_OPTIFINE_TIMEOUT_MS (15 * 60 * 1000)

/* ── 加载器类型 ── */

typedef enum sxcl_loader_kind {
    SXCL_LOADER_VANILLA = 0,   /**< 只装原版(没有加载器) */
    SXCL_LOADER_FORGE,
    SXCL_LOADER_NEOFORGE,
    SXCL_LOADER_FABRIC,
    SXCL_LOADER_QUILT,
    SXCL_LOADER_OPTIFINE
} sxcl_loader_kind;

/** 类型的字符串 id("forge"/"neoforge"/"fabric"/"quilt"/"optifine"/"vanilla"),越界返回 "vanilla"。 */
const char *sxcl_loader_kind_id(sxcl_loader_kind kind);
/** 给人看的名字("Forge"/"NeoForge"/"Fabric"/"Quilt"/"OptiFine"/"原版")。 */
const char *sxcl_loader_kind_name(sxcl_loader_kind kind);
/** 从字符串 id 反查(大小写不敏感;认不出来 = SXCL_LOADER_VANILLA,传 NULL 也一样)。 */
sxcl_loader_kind sxcl_loader_kind_from_id(const char *id);
/** 我们已经有静默安装实现的加载器(Python: IMPLEMENTED_LOADERS = Forge/NeoForge/Fabric/OptiFine)。 */
int sxcl_loader_kind_implemented(sxcl_loader_kind kind);

/* ── 版本串解析(纯逻辑,不联网、不碰进程) ── */

typedef struct sxcl_loader_version_info {
    char mc[32];       /**< 它要求的 Minecraft 版本,如 "1.20.1";认不出来是空串 */
    char loader[48];   /**< 加载器自己的版本,如 "47.2.0";只有原版版本号时是空串 */
    int ok;            /**< 1 = 至少解析出了 MC 版本 */
} sxcl_loader_version_info;

/** 解析 Forge 风格 / NeoForge 风格的版本串。规则(与 Python 版对齐,见 .c 里的逐条注释):
 *    "1.20.1-47.2.0"        -> mc="1.20.1" loader="47.2.0"
 *    "1.12.2-14.23.5.2860"  -> mc="1.12.2" loader="14.23.5.2860"
 *    "1.20.1"               -> mc="1.20.1" loader=""
 *    "forge-1.20.1-47.2.0"  -> mc="1.20.1" loader="47.2.0"(前缀被跳过)
 *    "1.20.1-OptiFine_HD_U_I6" -> mc="1.20.1" loader=""
 *    "21.1.72"(kind=NeoForge)-> mc="1.21.1" loader="21.1.72"
 *    认不出来(""、"abc")   -> ok=0,mc/loader 都是空串
 *  kind 只在"整串里找不到 MC 段"时参与判定:只有 NeoForge 走 21.1.72 -> 1.21.1 那条换算。
 *  返回 SXCL_LOADER_OK;out 为 NULL 返回 ERR_ARG。 */
int sxcl_loader_parse_version(const char *text, sxcl_loader_kind kind, sxcl_loader_version_info *out);

/** 从 OptiFine 列表的 forge 字段里取出 Forge 版本号("Forge 61.0.6" -> "61.0.6")。
 *  Python: parse_forge_requirement。取不到返回空串(仍然返回 SXCL_LOADER_OK)。 */
int sxcl_loader_parse_forge_requirement(const char *text, char *out, size_t out_len);

/* ── 兼容性判定 ── */

typedef enum sxcl_loader_issue_level {
    SXCL_LOADER_ISSUE_WARN = 0,   /**< 可以继续,但要告诉用户 */
    SXCL_LOADER_ISSUE_ERROR = 1   /**< 禁止继续 */
} sxcl_loader_issue_level;

typedef struct sxcl_loader_issue {
    sxcl_loader_issue_level level;
    char message[SXCL_LOADER_MESSAGE_MAX];
    char fix[SXCL_LOADER_FIX_MAX];
} sxcl_loader_issue;

typedef struct sxcl_loader_issues {
    sxcl_loader_issue items[SXCL_LOADER_MAX_ISSUES];
    size_t count;      /**< 有效条数 */
    size_t dropped;    /**< 因为超过上限被丢掉的条数 */
} sxcl_loader_issues;

const char *sxcl_loader_issue_level_name(sxcl_loader_issue_level level);
/** 有没有 error 级结论(= 界面应当禁止继续)。 */
int sxcl_loader_issues_has_error(const sxcl_loader_issues *issues);

/** 已经装好的一个版本(调用方从 versions/ 扫出来填进来;字符串只需活到调用结束)。 */
typedef struct sxcl_loader_installed {
    const char *instance_id;      /**< 版本目录名 */
    const char *base_version;     /**< 它对应的原版版本号 */
    const char *loader_id;        /**< "forge"/"fabric"…;NULL 或空串 = 原版 */
    const char *loader_version;   /**< 该加载器的版本,可空 */
} sxcl_loader_installed;

/** 当前选择 + 判断所需的上下文。 */
typedef struct sxcl_loader_selection {
    const char *base_version;               /**< 选中的原版版本,如 "1.20.1" */
    sxcl_loader_kind kind;                  /**< 选中的加载器;VANILLA = 只装原版 */
    const char *loader_version;             /**< 选中的加载器版本,可空 */
    const char *instance_name;              /**< 目标实例名(版本目录名),可空 */
    const char *const *available_versions;  /**< 该加载器在该原版下可用的版本串,可空 */
    size_t available_count;
    int list_failed;                        /**< 该加载器的版本列表没取到(网络/接口失败) */
    const sxcl_loader_installed *installed; /**< 已经装好的版本,可空 */
    size_t installed_count;
    const char *optifine_forge_hint;        /**< OptiFine 条目的 forge 字段,可空 */
} sxcl_loader_selection;

/** 给出这次选择的全部结论。空列表(count=0)= 没问题,可以继续。
 *  判定项(逐条对应 Python 版 check_selection + 任务要求的冲突检测,详见 .c):
 *    1) 选中的加载器版本列表没取到 -> error(状态未知就不该让人装)
 *    2) 该加载器我们还没有安装实现(Quilt)-> error
 *    3) 这个原版版本下该加载器一个版本都没有 -> error
 *    4) 选了版本但不在可用列表里 -> error
 *    5) 加载器版本串里写的 MC 版本与选中的原版不一致 -> error
 *    6) 同名实例已经存在 -> 同加载器=warn(会覆盖),别的加载器/原版=error(别覆盖)
 *    7) 同一个原版下已经装了**别的**加载器 -> warn(不能共用一个实例,会另建一个)
 *    8) 同一个原版 + 同一个加载器版本已经装过 -> warn
 *    9) OptiFine 条目的配套 Forge 提示 -> warn(实测 OptiFine 独立也能装上,不拦)
 *  未选加载器(kind=VANILLA)直接返回空列表:列表取没取到都不影响装原版。 */
int sxcl_loader_check_selection(const sxcl_loader_selection *sel, sxcl_loader_issues *out);

/* ── 依赖库坐标(Maven) ── */

/** Maven 坐标 -> libraries/ 下的相对路径(Python: LoaderAnalyzer._coord_to_path)。
 *  要处理两个 Forge 特有的写法:
 *    * 分类器 net.minecraftforge:forge:1.12.2-14.23.5.2863:universal
 *      -> net/minecraftforge/forge/1.12.2-14.23.5.2863/forge-1.12.2-14.23.5.2863-universal.jar
 *    * 扩展名 de.oceanlabs.mcp:mcp_config:1.12.2-20200226.224830@zip
 *      -> de/oceanlabs/mcp/mcp_config/1.12.2-20200226.224830/mcp_config-1.12.2-20200226.224830.zip
 *    不认 @zip 会拼出 "...@zip.jar" 这种根本不存在的文件名,下载必然 404。 */
int sxcl_loader_maven_path(const char *coord, char *out, size_t out_len);

typedef struct sxcl_loader_library {
    char name[160];   /**< 原始 Maven 坐标 */
    char path[320];   /**< 相对 libraries/ 的路径(正斜杠) */
    char url[256];    /**< 下载根地址(不带 path;以 '/' 结尾) */
} sxcl_loader_library;

/** 从版本 JSON(以及它的 processors[].classpath[])收集需要下载的依赖库。
 *  url 缺失的坐标用 default_maven(Forge: https://maven.minecraftforge.net/;
 *  NeoForge: https://maven.neoforged.net/releases/)。
 *  返回**实际条数**(可能大于 out_cap,此时只填了前 out_cap 条);out 可为 NULL(out_cap=0)
 *  用来先问"一共几条" —— 此时没有存名字的地方,所以**不去重**,拿到的是原始条目数;
 *  给了 out 才会按坐标去重。 */
size_t sxcl_loader_collect_libraries(const sxcl_json *version_json, const char *default_maven,
                                     sxcl_loader_library *out, size_t out_cap);

/* ── 命令行构造(纯函数:只拼字符串,不碰进程,可单测) ── */

/** 一组待执行的安装器命令行(固定大小数组:零分配、零所有权问题)。 */
typedef struct sxcl_loader_cmd {
    char program[SXCL_LOADER_CMD_ARG_MAX];                     /**< java 可执行 */
    char args[SXCL_LOADER_CMD_MAX_ARGS][SXCL_LOADER_CMD_ARG_MAX];
    size_t argc;
    char work_dir[SXCL_LOADER_CMD_ARG_MAX];                    /**< 工作目录(空 = 继承) */
    char appdata[SXCL_LOADER_CMD_ARG_MAX];                     /**< 非空 = 这组参数必须在它下面跑 */
    char desc[96];                                             /**< 这组参数的说明(日志/测试用) */
} sxcl_loader_cmd;

/** 构造命令行需要的上下文。game_dir 是**安装器实际写入的目标目录**
 *  (OptiFine 沙箱时是沙箱里的假游戏目录,不是真实实例目录)。 */
typedef struct sxcl_loader_cmd_env {
    const char *java_path;        /**< 必填 */
    const char *installer_jar;    /**< 必填 */
    const char *game_dir;         /**< 必填 */
    const char *fake_appdata;     /**< 可空;非空 = OptiFine 沙箱:命令行要在这个 APPDATA 下跑 */
    const char *base_version;     /**< Fabric 的 --mcversion */
    const char *loader_version;   /**< Fabric 的 --loader */
    const char *instance_name;    /**< Fabric 的 --name */
    const char *mirror_maven;     /**< 可空:BMCLAPI 之类的 maven 镜像,装在参数最后 */
} sxcl_loader_cmd_env;

/** 按优先级构造"方式 A"的命令行组合,返回实际条数(0 = 这种加载器没有静默 CLI,直接走方式 B)。
 *  Forge/NeoForge(首选项就是任务里点名的那个写法):
 *      1) -jar <jar> --installClient <游戏目录> [--mirror <镜像>]
 *      2) -jar <jar> --installClient [--mirror <镜像>](装到当前目录 = 工作目录)
 *      3) -jar <jar> --installDir=<游戏目录> [--mirror <镜像>](老安装器)
 *      4) -jar <jar> --installClient --target <游戏目录>(兜底)
 *  Fabric/Quilt(Python: _build_command_variants 的 Fabric 分支):
 *      1) client --mcversion <mc> --loader <lv> --dir <游戏目录> --name <实例名>
 *      2) client --mcversion <mc> --loader <lv> --dir <游戏目录>
 *  OptiFine(只有一组,且必须带沙箱 APPDATA):
 *      1) -jar <jar> --installClient <沙箱游戏目录>,appdata = <沙箱 APPDATA>
 *  绝不构造"不带参数"的命令行:那会弹出图形安装器,用户关掉窗口同样返回 0,
 *  会被误判成安装成功(这个坑 Python 版踩过)。 */
size_t sxcl_loader_build_commands(sxcl_loader_kind kind, const sxcl_loader_cmd_env *env,
                                  sxcl_loader_cmd *out, size_t out_cap);

/* ── 进度标记解析(纯函数) ── */

typedef enum sxcl_loader_progress_stage {
    SXCL_LOADER_PROGRESS_NONE = 0,        /**< 这一行没有有用信息 */
    SXCL_LOADER_PROGRESS_EXTRACT_JSON,    /**< "Extracting json" */
    SXCL_LOADER_PROGRESS_DOWNLOAD_LIBS,   /**< "Downloading libraries" */
    SXCL_LOADER_PROGRESS_BUILD_PROCESSORS,/**< "Building Processors" */
    SXCL_LOADER_PROGRESS_TASK,            /**< "Task: xxx" */
    SXCL_LOADER_PROGRESS_FINISHED         /**< 安装器自己说装完了 */
} sxcl_loader_progress_stage;

typedef struct sxcl_loader_progress {
    int matched;      /**< 1 = 这一行有信息(进度或完成标记) */
    int percent;      /**< 0..100;这一行没有百分比信息时是 -1 */
    int finished;     /**< 1 = 这行表明安装已完成(可以主动终止进程,别死等) */
    sxcl_loader_progress_stage stage;
    char text[SXCL_LOADER_TEXT_MAX];   /**< 人话说明(可直接显示) */
} sxcl_loader_progress;

/** 解析安装器的一行输出。标记字符串与 Python 版 PROGRESS_MARKERS 完全一致,
 *  它们是安装器自己打印的(PCL 解析的也是同一批):
 *      Extracting json        -> 35%  解压版本信息
 *      Downloading libraries  -> 45%  下载支持库
 *      Building Processors    -> 60%  执行安装处理器
 *      Task: <名字>           -> 70%  处理器任务 <名字>
 *  完成标记(conservative,宁可多等一会也不要误杀):
 *      * 整行(去空白、忽略大小写)就是 "true" —— Python 版判的就是它;
 *      * 出现 "installation was successful" / "successfully installed" —— Forge 安装器自己的成功行。
 *  没有信息的行:matched=0,percent=-1。返回 SXCL_LOADER_OK(永不为失败,方便逐行喂进来)。 */
int sxcl_loader_parse_progress(const char *line, sxcl_loader_progress *out);

/* ── launcher_profiles.json(安装器的硬性前置) ── */

/** 顶层键覆盖(序列化时用):同名成员的值被换成给定字符串,不存在则追加到末尾。 */
typedef struct sxcl_loader_json_override {
    const char *key;
    const char *value;
} sxcl_loader_json_override;

/** 把已解析的 DOM 序列化成 UTF-8 JSON 文本(4 空格缩进,末尾不写换行)。
 *  **调用方不认识的字段一律原样写回** —— launcher_profiles.json 的"只合并不覆盖"靠的就是它。
 *  overrides 非空时按上面的规则改顶层键(版本 JSON 的 "id" 就靠它改)。
 *  成功返回 SXCL_LOADER_OK 并让 *out_text 指向 malloc 出来的文本(调用方 free)。 */
int sxcl_loader_json_dump(const sxcl_json_value *root, const sxcl_loader_json_override *overrides,
                          size_t override_count, char **out_text, char *err, size_t err_len);

/** 合并 launcher_profiles.json 的**纯文本**入口(不碰磁盘,便于单测):
 *  在 existing_json(可空 = 全新文件)的基础上,把我们的档案并进 profiles[<key>],
 *  并补齐 selectedProfile / clientToken。已经存在同名档案时**保持原样**
 *  (Python: "已经有了就不动它" —— 用户可能在官方启动器/PCL 里改过它),
 *  但这次合并仍然算"没变化"(*changed = 0,不重写文件)。
 *  原有档案、不认识的字段全部保留。last_used 为空则用当前 UTC 时间。
 *  last_version_id 是这个档案指向的版本 id:装完实例后用实例名调一次,档案就能直接启动它;
 *  为空则用 Python 版那条固定值 "latest-release"。
 *  成功返回 SXCL_LOADER_OK 并让 *out_text 指向 malloc 出来的文本(调用方 free)。
 *  注意与 Python 版的一处**故意不同**:existing_json 解析失败时这里报 SXCL_LOADER_ERR_FORMAT
 *  并且不动原文件(Python 会把整份数据当成空字典然后覆盖写 —— 那会毁掉用户的档案与登录信息)。 */
int sxcl_loader_merge_profiles_text(const char *existing_json, const char *key, const char *name,
                                   const char *last_version_id, const char *last_used, char **out_text,
                                   int *changed, char *err, size_t err_len);

/** 保证 <game_dir>/launcher_profiles.json 存在(不存在就按 Python 的默认内容建一份),
 *  存在就按上面的规则合并一次,需要时才原子写回(写 <路径>.tmp 再改名)。
 *  key/name/last_version_id 可为空(默认 "SXCL" / "Silent X Craft Launcher" / "latest-release");
 *  装完某个实例后登记它,就传 key=name=last_version_id=<实例名>。
 *  返回 SXCL_LOADER_OK = 文件已存在(安装器可以开工;err 里可能有非致命的合并告警);
 *  SXCL_LOADER_ERR_IO = 文件不在且写不出来(安装器必然失败,必须挡住)。 */
int sxcl_loader_ensure_launcher_profiles(const char *game_dir, const char *key, const char *name,
                                         const char *last_version_id, const char *last_used, int *changed,
                                         char *err, size_t err_len);

/* ── 安装驱动 ── */

typedef enum sxcl_loader_fail_stage {
    SXCL_LOADER_FAIL_NONE = 0,
    SXCL_LOADER_FAIL_PREPARE,        /**< 备目录 / launcher_profiles.json / 原版文件 */
    SXCL_LOADER_FAIL_RUN_INSTALLER,  /**< 方式 A 全试完了都不行 */
    SXCL_LOADER_FAIL_EXTRACT,        /**< 方式 B(解包)失败 */
    SXCL_LOADER_FAIL_VERIFY,         /**< 产物不完整:versions/<实例名>/ 里没有版本 JSON */
    SXCL_LOADER_FAIL_CANCELLED,      /**< 用户取消 */
    SXCL_LOADER_FAIL_TIMEOUT         /**< 总超时 */
} sxcl_loader_fail_stage;

const char *sxcl_loader_fail_stage_name(sxcl_loader_fail_stage stage);

typedef struct sxcl_loader_install_result {
    int ok;                        /**< 1 = 成功 */
    sxcl_loader_fail_stage fail_stage;
    int used_fallback;             /**< 1 = 方式 A 失败后走了方式 B */
    int used_sandbox;              /**< 1 = 走了临时 APPDATA 沙箱(OptiFine) */
    int percent;                   /**< 最后的进度 0..100 */
    int exit_code;                 /**< 最后一次安装器进程的退出码;-1 = 没跑起来 */
    char error[SXCL_LOADER_ERROR_MAX];  /**< 人话错误(失败时给界面看的) */
    char version_dir[SXCL_LOADER_CMD_ARG_MAX]; /**< 产物目录(versions/<实例名>) */
} sxcl_loader_install_result;

/** 进度回调:安装线程里同步调用,别做重活(只投递事件)。percent 单调不减。 */
typedef void (*sxcl_loader_progress_fn)(void *userdata, int percent, const char *status);

typedef struct sxcl_loader_install_request {
    const char *game_dir;        /**< 必填:游戏目录(实例所在的 .minecraft) */
    const char *instance_name;   /**< 必填:目标实例名(版本目录名) */
    const char *base_version;    /**< 必填:原版版本号 */
    sxcl_loader_kind kind;       /**< 加载器类型 */
    const char *loader_version;  /**< 加载器版本(Fabric 的 --loader;Forge 可空) */
    const char *installer_jar;   /**< 必填:已经下好的安装器 jar */
    const char *java_path;       /**< 必填:java 可执行 */
    const char *mirror_maven;    /**< 可空:maven 镜像(只影响命令行里的 --mirror) */
    int timeout_ms;              /**< <=0 = 用默认 30 分钟(OptiFine 15 分钟) */
    int no_fallback;             /**< 非 0 = 方式 A 失败就直接失败;0(默认)= 回退方式 B。
                                  *  注意是"反向开关":零初始化的请求 = 走 Python 版的行为(失败就回退)。 */
    int keep_sandbox;            /**< 1 = 保留临时沙箱目录(排查用;默认删掉) */
    sxcl_loader_progress_fn on_progress;   /**< 可空 */
    void *userdata;
    /** 可空:返回非 0 = 用户已取消。**每读到一行安装器输出时检查一次**
     *  (process.h 的 on_line 是唯一能在跑的过程中插入我们代码的地方)。 */
    int (*is_cancelled)(void *userdata);
    void *cancel_userdata;
    /** 可空:方式 B 收尾时把"要下载的依赖库"交给调用方。
     *  典型接法就是启动器自己的下载引擎(sxcl/engine.h):给每个库建一个 sxcl_task
     *  (dest = <game_dir>/libraries/<path>,urls[0] = <url><path>),再 sxcl_engine_run。
     *  返回非 0 = 失败(安装按方式 B 失败收场)。 */
    int (*on_libraries)(void *userdata, const sxcl_loader_library *libs, size_t count);
    /** 可空:安装器每输出一行原始输出就回调一次(与 process.h 的 on_line 同签名同语义)。
     *  用途:CLI 的 --verbose、启动器记日志/进度界面;返回非 0 = 请求终止安装。
     *  注意用的是上面那个 userdata(不是 cancel_userdata)。 */
    int (*on_line)(void *userdata, int is_stderr, const char *line);
} sxcl_loader_install_request;

/** 装一个模组加载器。返回 0 = 成功;1 = 失败(看 out->fail_stage / out->error);
 *  负数 = 参数不合法(压根没开始装)。out 必须非空。
 *
 *  流程(Python: ModLoaderInstaller.install):
 *    1) 保证 launcher_profiles.json 在(OptiFine 时在沙箱里保证);
 *    2) OptiFine 走"临时 APPDATA 沙箱 + 装完搬产物";其它加载器遍历方式 A 的命令行组合;
 *    3) 每跑完一组:看到完成标记**或**退出码为 0,并且 versions/<实例名>/ 里有版本 JSON 才算成功;
 *       安装器把版本写到别的名字时,按候选名(_handle_generated_files)找回来;
 *    4) 方式 A 全失败且 allow_fallback 且是 Forge/NeoForge -> 方式 B 解包安装;
 *    5) 收尾:把版本 JSON 的 id/文件名统一成实例名(启动器按目录名找版本)。
 *
 *  未实现的边界(明说,不含糊):方式 B 只做到 Python 版 _extract_install 那一步 ——
 *  解出安装器自带的 maven/ 与通用 jar、拼出版本 JSON、把依赖库清单交给 on_libraries;
 *  重放 processors(需要解 data/*.lzma、拼 classpath 起 Java)不在这里,版本 JSON 里的
 *  processors 字段原样保留,交给启动/补全流程。 */
int sxcl_loader_install(const sxcl_loader_install_request *req, sxcl_loader_install_result *out);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LOADER_H */
