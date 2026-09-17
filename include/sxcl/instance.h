/* SXCL-C 实例扫描与识别 —— 扫 <游戏目录>/versions/* 产出"这个实例是什么"。
 *
 * 这一份是 Python 版 src/services/minecraft/loaders.py + pcl_compat.py 的逐条搬运,
 * 规则**不重新发明**,方括号里就是 Python 版注释的原意。三条硬性事实决定了怎么写:
 *
 *  1) Forge 1.13+ / Fabric / Quilt 装的版本**没有自己的 jar**(靠 inheritsFrom 继承原版)。
 *     以前按"jar + json 都在"判断,这些版本在界面里根本不显示;现在只要求 JSON。
 *  2) PCL 装出来的版本是"拍平"的单层 JSON:没有 inheritsFrom、没有 jar,id == 文件夹名,
 *     另外多一个 clientVersion 字段(= Mojang 原始版本号)—— 这是它的身份标记,
 *     所以取原版版本号要**优先看 clientVersion**。
 *  3) 识别加载器宁可多认几条线索:JSON 里的 libraries 坐标 > mainClass > 整段 JSON 文本
 *     (PCL 的判据) > PCL 的 Setup.ini 缓存 > 目录名兜底。同一加载器只保留一条
 *     (越靠前的线索版本号越准)。另外 PCL 会把状态缓存在 versions/<版本>/PCL/Setup.ini,
 *     我们只读不写,用来补全信息与显示它设过的自定义图标。
 *
 * 搬过来的规则清单(逐条,括号里是 Python 的位置;测试里一条一个用例):
 *   A. libraries 坐标(loaders.py LIBRARY_RULES):net.neoforged:neoforge|net.neoforged:forge|
 *      net.neoforge -> NeoForge(必须排在 Forge 前面,NeoForge 的坐标里也含 minecraftforge);
 *      net.minecraftforge:forge|fmlloader|minecraftforge -> Forge;
 *      net.fabricmc:fabric-loader|intermediary -> Fabric;org.quiltmc:quilt-loader|quilted-fabric-loader -> Quilt;
 *      optifine:OptiFine|launchwrapper-of -> OptiFine;com.mumfrey:liteloader -> LiteLoader。
 *      坐标版本号取 "group:artifact:version" 的第 3 段;坐标缺失时退回 downloads.artifact.path。
 *   B. mainClass(MAIN_CLASS_RULES):net.neoforged / net.minecraftforge.bootstrap / cpw.mods.modlauncher /
 *      net.minecraftforge -> Forge;net.fabricmc.loader -> Fabric;org.quiltmc.loader -> Quilt;
 *      net.optifine -> OptiFine;com.mumfrey.liteloader -> LiteLoader(都只给种类,不给版本号)。
 *   C. 整段 JSON 文本(TEXT_RULES,PCL 的判据):net.neoforge / minecraftforge / net.fabricmc:fabric-loader /
 *      org.quiltmc:quilt-loader / optifine / liteloader;其中"文本里有 net.neoforge"时**不再**认 Forge。
 *      OptiFine 的版本号从 "HD_U_" 后面取;Fabric/Quilt 从 "fabric-loader:"/"quilt-loader:" 后面取数字;
 *      NeoForge 从 neoForgeVersion/forgeVersion 键取。取到的版本号里的 "+build" 会被去掉。
 *   D. PCL 的 versions/<版本>/PCL/Setup.ini(Key:Value,无 section):VersionFabric/VersionForge/
 *      VersionNeoForge/VersionOptiFine/VersionLiteLoader;值为空或 "Unknown" 时不算。
 *   E. 目录名(NAME_RULES,PCL 的命名习惯):neoforge<N> / forge<N>(前面是 "neo" 的不算,即 NeoForge 不会被
 *      当成 Forge)/ fabric[_-]?loader?<N> / quilt...<N> / OptiFine<VER> / LiteLoader<VER>。
 *      这一条只在该种类前面几条线索都没认出来时才用(所以 from_json=0)。
 *   F. 原版版本号推断顺序(PCL 的顺序,loaders.py _base_from_json):
 *        1. clientVersion(PCL 拍平标记,可靠)
 *        2. HMCL:patches 里 id=="game" 的 version —— 但顶层有 "time" 字段时这条不生效
 *        3. inheritsFrom(标准继承,可靠)
 *        4. JSON 文本里的 --fml.mcVersion(新版 Forge 写在 arguments 里,可靠)
 *        5. "jar" 字段(LiteLoader 常靠它,可靠)
 *        6. 目录名里像 1.x / a1.x / b1.x 的那一段(不可靠,base_reliable=0)
 *        7. 目录名里任意 x.y.z(不可靠)
 *   G. 加载器版本号去掉原版前缀:版本号以 "<base>-" 开头时把这段删掉("1.20.1-47.2.0" -> "47.2.0")。
 *   H. 前置版本缺失(inheritsFrom 指向的 <父>/<父>.json 不在 versions/ 下)-> missing_parent,
 *      文案"需要安装 X 作为前置版本"(PCL 的说法),不可启动。
 *   I. HMCL 补丁(patches 是数组且顶层没有 "time")-> launcher="hmcl";
 *      否则有 PCL/Setup.ini 或 PCL/ 目录 -> launcher="pcl"(识别出来只是想让用户少困惑)。
 *   J. 自定义图标:Setup.ini 的 LogoCustom 为 True/1 且 PCL/Logo.png 存在 -> has_custom_logo,
 *      路径给界面直接用。
 *   K. 版本目录的取舍:空文件夹跳过(PCL 也跳过);JSON 和 jar 都没有就跳过;
 *      没有 JSON 时目录名叫 cache/BLClient/PCL 的跳过;同名 JSON 优先,没有就找任意一个
 *      含 mainClass+type+id 的 *.json 顶上(PCL 的兜底)。
 *   L. PCL 的 versions/<版本>/PCL/config.json 里的 InstanceForcedJava = 启动页"选 Java 第一优先级"
 *      (Python: read_instance_java;配了但文件不在了不算数)。
 *
 * 两处**故意与 Python 不同**(都在测试里写明了,父代理可以裁决):
 *   1) Python 的 LOADER_ORDER 漏了 Quilt(于是 Quilt 实例被当成原版显示),C 版把 Quilt 补进返回顺序。
 *   2) Python 的 ready 只看 JSON(不管 jar),C 版按任务书把"原版实例缺自己的 jar"也算成不可启动,
 *      并给出独立问题码 SXCL_INSTANCE_PROBLEM_MISSING_JAR;另外"目录名与 JSON id 不一致"会给
 *      SXCL_INSTANCE_PROBLEM_ID_MISMATCH —— 因为 C 的启动驱动只认 <版本 id>/<版本 id>.json
 *      (src/services/launch/driver.c:213-215),这种实例现在必然启动失败,提前告诉用户。
 *      json_path 字段始终给出**实际读到的**那个 JSON 的路径,父代理若让驱动改用它,忽略问题码即可。
 *
 * 线程安全:纯函数,无全局可变状态;scan 只读磁盘。异常一律不抛:失败给人话 err。
 */
#ifndef SXCL_INSTANCE_H
#define SXCL_INSTANCE_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/json.h"
#include "sxcl/loader.h"   /* sxcl_loader_kind:下面这份枚举与它**逐值对应** */

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码(负数为错) */
#define SXCL_INSTANCE_OK           0
#define SXCL_INSTANCE_ERR_ARG    (-1)  /* 参数不合法 */
#define SXCL_INSTANCE_ERR_IO     (-2)  /* 目录/文件读不出来 */
#define SXCL_INSTANCE_ERR_SPACE  (-3)  /* 输出缓冲不够(内容被截断) */
#define SXCL_INSTANCE_ERR_NOMEM  (-4)  /* 内存不足 */
#define SXCL_INSTANCE_ERR_PARSE  (-5)  /* JSON 解析失败 */

/* ── 尺寸上限 ── */
#define SXCL_INSTANCE_ID_MAX      128  /* 实例 id(= 版本目录名) */
#define SXCL_INSTANCE_VERSION_MAX 64   /* 原版版本号 / 加载器版本号 */
#define SXCL_INSTANCE_TYPE_MAX    32   /* release / snapshot / old_alpha… 与 PCL 的 State */
#define SXCL_INSTANCE_PATH_MAX    512  /* 文件路径 */
#define SXCL_INSTANCE_PROBLEM_MAX 192  /* 不能启动的人话原因 */
#define SXCL_INSTANCE_SUMMARY_MAX 128  /* "Forge 47.2.0 + OptiFine I6" */
#define SXCL_INSTANCE_DESC_MAX    256  /* 整条实例的人话描述 */
#define SXCL_INSTANCE_ERROR_MAX   256  /* 人话错误 */
#define SXCL_INSTANCE_MAX_LOADERS 6    /* 一个实例最多识别出几个加载器(Quilt 也算了) */
#define SXCL_INSTANCE_DEFAULT_CAP 256  /* 一次扫描默认最多返回几条(超过记 dropped) */
#define SXCL_INSTANCE_SKIP_NAMES  3    /* cache / BLClient / PCL */

/* ── 加载器种类 ──
 * 前 6 个与 sxcl/loader.h 的 sxcl_loader_kind **值完全相同**(0..5),可以直接强转;
 * LITELOADER 是 loader.h 目前没有的种类(Python 有),这里补上,父代理若要统一,
 * 建议在 loader.h 里加 SXCL_LOADER_LITELOADER = 6。 */
typedef enum sxcl_instance_loader_kind {
    SXCL_INSTANCE_VANILLA    = 0,
    SXCL_INSTANCE_FORGE      = 1,
    SXCL_INSTANCE_NEOFORGE   = 2,
    SXCL_INSTANCE_FABRIC     = 3,
    SXCL_INSTANCE_QUILT      = 4,
    SXCL_INSTANCE_OPTIFINE   = 5,
    SXCL_INSTANCE_LITELOADER = 6
} sxcl_instance_loader_kind;

/** 字符串 id("vanilla"/"forge"/"neoforge"/"fabric"/"quilt"/"optifine"/"liteloader")。 */
const char *sxcl_instance_kind_id(sxcl_instance_loader_kind kind);
/** 给人看的名字("原版"/"Forge"/…/"LiteLoader")。 */
const char *sxcl_instance_kind_name(sxcl_instance_loader_kind kind);
/** 从字符串 id 反查(大小写不敏感;认不出来 = VANILLA)。 */
sxcl_instance_loader_kind sxcl_instance_kind_from_id(const char *id);
/** 转成 loader.h 的种类(LiteLoader 在 loader.h 里没有 -> 返回 SXCL_LOADER_VANILLA)。 */
sxcl_loader_kind sxcl_instance_kind_to_loader(sxcl_instance_loader_kind kind);

/* ── 不能启动的原因(问题码;界面按码做颜色/图标,文案给用户看) ── */
typedef enum sxcl_instance_problem {
    SXCL_INSTANCE_PROBLEM_NONE = 0,
    SXCL_INSTANCE_PROBLEM_BAD_JSON,        /* 版本 JSON 损坏或缺少 mainClass */
    SXCL_INSTANCE_PROBLEM_MISSING_PARENT,  /* 缺前置版本(PCL:需要安装 X 作为前置版本) */
    SXCL_INSTANCE_PROBLEM_NO_JSON,         /* 缺版本 JSON */
    SXCL_INSTANCE_PROBLEM_MISSING_JAR,     /* 原版实例缺自己的 jar(没有继承也没有加载器) */
    SXCL_INSTANCE_PROBLEM_ID_MISMATCH      /* 目录名与 JSON id 不一致(C 的启动驱动只认 <id>.json) */
} sxcl_instance_problem;

/** 问题码的稳定字符串("ok"/"bad-json"/"missing-parent"/"no-json"/"missing-jar"/"id-mismatch")。
 *  界面做本地化时按它查表,别按中文文案匹配。 */
const char *sxcl_instance_problem_id(sxcl_instance_problem problem);
/** 问题码的默认人话文案(空串 = 没问题)。 */
const char *sxcl_instance_problem_default_text(sxcl_instance_problem problem);

/* ── 识别出来的一个加载器 ── */
typedef struct sxcl_instance_loader {
    sxcl_instance_loader_kind kind;
    char version[SXCL_INSTANCE_VERSION_MAX]; /**< 认不出时是空串 */
    int from_json;                           /**< 1=从 JSON 认出来(可信);0=只能靠 Setup.ini/目录名猜 */
} sxcl_instance_loader;

/** 一个已安装的版本(实例)。全部字段由模块填好,调用方只读。 */
typedef struct sxcl_instance {
    char id[SXCL_INSTANCE_ID_MAX];                 /**< 目录名,也是启动时用的版本 id */
    char base_version[SXCL_INSTANCE_VERSION_MAX];  /**< 对应的原版版本号;认不出来是空串 */
    int base_reliable;                             /**< 1=来自 JSON(clientVersion/inheritsFrom…),0=靠目录名猜的 */
    sxcl_instance_loader loaders[SXCL_INSTANCE_MAX_LOADERS];
    size_t loader_count;                           /**< 0 = 纯原版 */
    int has_jar;                                   /**< <id>.jar 在不在 */
    int has_json;                                  /**< 有没有可用(可解析的)版本 JSON */
    char version_type[SXCL_INSTANCE_TYPE_MAX];     /**< release / snapshot / old_alpha / old_beta */
    int broken;                                    /**< JSON 读不出来,或缺少 mainClass(PCL 的硬性门槛) */
    char inherits_from[SXCL_INSTANCE_ID_MAX];      /**< JSON 里的 inheritsFrom(空 = 没有继承,必须自带 jar) */
    char missing_parent[SXCL_INSTANCE_ID_MAX];     /**< 缺哪个前置版本(空 = 不缺) */
    char launcher[8];                              /**< "pcl" / "hmcl" / "" —— 是哪个启动器装的 */
    char json_path[SXCL_INSTANCE_PATH_MAX];        /**< 实际读到的版本 JSON 的完整路径 */
    char json_id[SXCL_INSTANCE_ID_MAX];            /**< JSON 里写的 id(可能和目录名不同) */
    int json_id_mismatch;                          /**< 1 = 目录名与 JSON id 不一致 */
    char pcl_state[SXCL_INSTANCE_TYPE_MAX];        /**< PCL 缓存的 State(仅参考) */
    int pcl_present;                               /**< 1 = 版本目录里有 PCL 的痕迹 */
    char custom_logo[SXCL_INSTANCE_PATH_MAX];      /**< PCL 里设过的自定义图标(PCL/Logo.png) */
    int has_custom_logo;
    char forced_java[SXCL_INSTANCE_PATH_MAX];      /**< PCL 给这个实例钉的 Java(InstanceForcedJava) */
    int forced_java_state;                         /**< SXCL_INSTANCE_JAVA_* */
    int launchable;                                /**< 1 = 现在就能启动 */
    sxcl_instance_problem problem_code;            /**< 不能启动时的原因码 */
    char problem[SXCL_INSTANCE_PROBLEM_MAX];       /**< 不能启动时的原因文案(可启动时空串) */
    char summary[SXCL_INSTANCE_SUMMARY_MAX];       /**< "原版" / "Forge 47.2.0 + OptiFine I6" */
    char describe[SXCL_INSTANCE_DESC_MAX];         /**< "<id>(<原版>,<summary>,可启动/原因)" */
} sxcl_instance;

/** 启动页"选 Java 第一优先级"的三态。 */
#define SXCL_INSTANCE_JAVA_OK       0   /**< PCL 钉了 Java 且那个文件还在 */
#define SXCL_INSTANCE_JAVA_NOT_SET  1   /**< 没配(或没有 config.json) */
#define SXCL_INSTANCE_JAVA_MISSING  2   /**< 配了,但那个文件已经不在了(Python 也当作没配) */

/** 一次扫描的结果:列表 + 计数。用 sxcl_instance_list_free 释放。 */
typedef struct sxcl_instance_list {
    sxcl_instance *items;        /**< malloc 出来的数组;调用方不要自己 free,用 sxcl_instance_list_free */
    size_t count;                /**< 有效条数 */
    size_t dropped;              /**< 超过上限被丢掉的条数 */
    int truncated;               /**< 1 = 被上限截断过(dropped > 0 的布尔形式) */
    size_t launchable_count;     /**< 其中现在就能启动的有几个 */
    size_t problem_count;        /**< 其中不能启动的有几个 */
    size_t vanilla_count;        /**< 其中没有任何加载器的有几个 */
    size_t with_loader_count;    /**< 其中至少有一个加载器的有几个 */
    int game_dir_exists;         /**< 1 = 传进来的游戏目录确实存在(不存在时列表为空,不是错误) */
} sxcl_instance_list;

/** 扫描选项。**零初始化的结构就是全默认**(opts 也可以直接传 NULL)。
 *  开关都写成"跳过"语义,这样 memset 0 出来的就是"全都要"。 */
typedef struct sxcl_instance_scan_opts {
    size_t max_instances;   /**< 0 = SXCL_INSTANCE_DEFAULT_CAP;超出的记 dropped,不静默漏 */
    int skip_pcl;           /**< 非 0 = 不读 PCL/Setup.ini(状态 / 自定义图标 / 加载器线索) */
    int skip_forced_java;   /**< 非 0 = 不读 PCL/config.json 的 InstanceForcedJava */
} sxcl_instance_scan_opts;

/** 扫一次给全部:列表 + 计数 + 每条的加载器/问题/人话,供主页/版本页直接渲染。
 *  game_dir 为空/不是目录时:返回 SXCL_INSTANCE_OK,列表为空、game_dir_exists=0,
 *  并在 err 里给人话说明(界面走空态,不当作错误)。真正出错(内存不足)返回负值。
 *  out 必须先清零或由本函数初始化;重复调用前先 sxcl_instance_list_free。 */
int sxcl_instance_scan(const char *game_dir, const sxcl_instance_scan_opts *opts,
                       sxcl_instance_list *out, char *err, size_t err_len);

/** 释放列表(允许传 NULL;释放后 items=NULL、count=0)。 */
void sxcl_instance_list_free(sxcl_instance_list *list);

/** 只扫一个实例目录(PCL 装完后/改名后单独刷新一条时用,免得整棵重扫)。
 *  实例不存在/不算版本返回 SXCL_INSTANCE_ERR_IO + 人话 err。 */
int sxcl_instance_scan_one(const char *game_dir, const char *instance_id, sxcl_instance *out,
                           char *err, size_t err_len);

/** 按实例 id 找一条(找不到返回 NULL)。 */
const sxcl_instance *sxcl_instance_find(const sxcl_instance_list *list, const char *instance_id);

/** 数某个原版版本下装了几个实例(Python 的 installed_for_base;0 也可用)。
 *  base 为空时按实例 id 精确匹配。 */
size_t sxcl_instance_count_for_base(const sxcl_instance_list *list, const char *base_version);

/** 重新生成 summary / describe / problem / launchable(字段被调用方改过之后调一次)。
 *  返回 SXCL_INSTANCE_OK。 */
int sxcl_instance_refresh(sxcl_instance *instance);

/** 把实例的加载器列表填成 loader.h 的"已安装版本"数组(喂 sxcl_loader_check_selection
 *  做重名/同加载器判定)。返回**实际条数**(可能大于 out_cap);out 可空(out_cap=0)只问条数。
 *  注意:LiteLoader(loader.h 目前没有的种类)会被跳过;纯原版实例会给一条 loader_id="vanilla" 的。
 *  返回的 loader_id/base_version 等指针指向 instance 内部的字符串,instance 活着它们才有效。 */
size_t sxcl_instance_to_loader_installed(const sxcl_instance *instance, sxcl_loader_installed *out,
                                         size_t out_cap);

/* ── 读某个实例的版本 JSON(主页"版本简要信息"、下载完成后的收尾都要用) ── */

/** 找到并解析一个实例的版本 JSON(同名优先,其次任意含 mainClass+type+id 的 *.json)。
 *  成功返回文档(调用方 sxcl_json_free),json_path 可空。
 *  找不到/解析失败返回 NULL 并写人话 err(SXCL_INSTANCE_ERR_*)。 */
sxcl_json *sxcl_instance_read_json(const char *game_dir, const char *instance_id,
                                   char *json_path_out, size_t json_path_len,
                                   char *err, size_t err_len);

/* ── PCL 兼容(与 Python 的 pcl_compat.py 对齐;我们只读不写) ── */

/** versions/<id>/PCL/Setup.ini 的解析结果(格式是每行 Key:Value,冒号分隔、没有 section、
 *  不是标准 INI —— PCL 自己写的)。键值都对不出来时 exists=0。 */
#define SXCL_INSTANCE_PCL_KEY_MAX 12
#define SXCL_INSTANCE_PCL_KEYLEN 28
#define SXCL_INSTANCE_PCL_VALLEN 96

typedef struct sxcl_instance_pcl_setup {
    int exists;                       /**< 1 = Setup.ini 这个文件在(哪怕一个键都没解析出来) */
    size_t key_count;                 /**< 真正解析出来的键数(读没读全,界面可以用来判断"PCL 缓存过期") */
    size_t stored_count;              /**< 塞进下面这张表的键数(表满了就不存,但 key_count 照样数) */
    char keys[SXCL_INSTANCE_PCL_KEY_MAX][SXCL_INSTANCE_PCL_KEYLEN];
    char values[SXCL_INSTANCE_PCL_KEY_MAX][SXCL_INSTANCE_PCL_VALLEN];
} sxcl_instance_pcl_setup;

/** 读 Setup.ini。文件不存在返回 SXCL_INSTANCE_ERR_IO(不算异常,err 里说明),
 *  解析成功返回 SXCL_INSTANCE_OK。 */
int sxcl_instance_read_pcl_setup(const char *game_dir, const char *instance_id,
                                 sxcl_instance_pcl_setup *out, char *err, size_t err_len);

/** 从解析结果里取任意键(键名大小写敏感,和 PCL 一致);没有返回空串(永不返回 NULL)。 */
const char *sxcl_instance_pcl_get(const sxcl_instance_pcl_setup *setup, const char *key);

/** 这个版本目录里有没有 PCL 的痕迹(有 PCL/Setup.ini 或 PCL/ 目录)。返回 0/1。 */
int sxcl_instance_pcl_present(const char *game_dir, const char *instance_id);

/** 读 PCL 给这个实例钉的 Java(versions/<id>/PCL/config.json 的 InstanceForcedJava)。
 *  返回 SXCL_INSTANCE_JAVA_OK / _NOT_SET / _MISSING(见上面三个宏);负数是参数错误。
 *  out 里给的是**实际存在**的 java 路径;NOT_SET / MISSING 时是空串。 */
int sxcl_instance_read_forced_java(const char *game_dir, const char *instance_id,
                                   char *out, size_t out_len, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_INSTANCE_H */
