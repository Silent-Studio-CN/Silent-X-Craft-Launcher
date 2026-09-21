/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_JRE_HOSTED_H
#define SXCL_JRE_HOSTED_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/engine.h"   /* sxcl_engine_opts(下载引擎) */
#include "sxcl/net.h"      /* sxcl_transport(传输后端) */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 自托管 JRE:远端目录结构 + index.json + 我们自己的 jre.json ──
 *
 * 为什么另起一套(而不是接着 mojang 的 all.json 走):官方清单里的 Java 运行时是
 * **桌面**构建,安卓 arm64 那一档在清单里根本不存在(docs/18 §4.2/§6)。用户拍板的路线是
 * "云端由用户自己托管、启动器去下载",所以远端就是我们自己定的一棵静态目录树,
 * 唯一入口是一份 index.json(见 docs/19 与 docs/fixtures/java-index.example.json)。
 *
 * 远端目录结构(用户照着上传即可;根由用户自定,例如 https://example.com/sxcl/java/):
 *
 *     index.json                      ← 唯一入口
 *     jre8/{universal.tar.xz, bin-arm64.tar.xz, version}
 *     jre17/{...}  jre21/{...}  jre25/{...}
 *     (可选 bin-armeabi-v7a.tar.xz / bin-x86_64.tar.xz 等其它架构)
 *
 * index.json(schema 1):
 *   { "schema": 1, "generatedAt": "<ISO8601>",
 *     "components": [ { "id": "jre17", "javaMajor": 17, "version": "<构建版本串>",
 *                       "files": [ {"file":"jre17/universal.tar.xz","size":N,
 *                                   "sha256":"<hex>","sha1":"<hex>"}, … ] } ] }
 *
 * 校验信息的**三种形态都吃**(最终口径,见 docs/19):
 *   1) sha256 = 64 位十六进制            -> 直接用(这是推荐形态,**信任锚**);
 *   2) sha256 = http(s) 的 .sha256 URL   -> GET 它,去首尾空白后取**第一个连续的 64 位
 *      十六进制串**(兼容 sha256sum 那种 "<hex>  <文件名>" 的输出);主 URL 取不到时
 *      **拿 mirrors 的同名 .sha256 再试一次**;
 *   3) sha256 缺失                       -> 只校验 size,并在结果/进度里**明确写"没有校验信息"**,
 *      绝不假装校验过。
 *   sha1 字段同样支持这三种形态(Mojang 系资源是 sha1)。
 *
 * 硬规矩(引擎本来就强制校验,这里在**解析**阶段就先拦一道,免得跑到一半才炸):
 *   * 每个文件**必须**有 size(字节数;size_mb 只是给人看的,不能当 size 用);
 *   * version 是"要不要重装"的唯一判据(照 FCL 的 version 文件思路:先比 version 字符串,
 *     相等直接跳过,不重算哈希、不联网)。
 *
 * 装完落盘的是**我们自己的** <目标目录>/jre.json:
 *   { "schema": 1, "component": "jre17", "javaMajor": 17, "version": "...",
 *     "installedAt": "<ISO8601>", "abi": "arm64", "indexUrl": "...",
 *     "files": [ {"file": ..., "size": ..., "sha256": ..., "sha1": ...} ] }
 * 它记的是**下载下来并校验通过的那几个包**的 sha256 —— 与 index.json 一一对应,
 * 所以"这份运行时是从哪儿、哪个版本来的"永远可追溯。
 *
 * 一句话:**index.json 说"有什么",jre.json 说"我装了什么"。**
 */

/* ── 常量 ── */

/** 支持的 index.json schema 版本(不认识的版本直接报错,不猜)。 */
#define SXCL_JRE_INDEX_SCHEMA 1

/** **我们自己的安卓 JRE 来源**用的清单 schema 串(sxcl.jre.index/1)。
 *  它就是 D:\SilentStudio\_termux_jre\out\index.json 那份(见 docs/19 §2.1):
 *
 *    { "schema": "sxcl.jre.index/1",
 *      "abi": "arm64-v8a",
 *      "generated": "<ISO8601>",
 *      "components": {                       <- **对象**,按组件名索引
 *        "jre17": {
 *          "component": "jre17", "major": 17, "version": "17.0.20",
 *          "java_version": "17.0.20", "abi": "arm64-v8a", "implementor": "Termux",
 *          "id": "jre17/17.0.20/arm64-v8a/9f87f8de24c52431",
 *          "file_count": 339, "installed_bytes": 158165753,
 *          "packages": [ { "name":"universal.tar.xz", "size":31132644,
 *                          "sha256":"<hex>", "sha1":"<hex>", "url":"jre17/universal.tar.xz" }, … ],
 *          "version_file": { … }, "notice": [ … 290 条 … ],
 *          "manifest": { "files": { "bin/java": { … } } }, "source": { … } } } }
 *
 *  我们**只下载 packages[]**(那才是运行时本体);version_file / notice / manifest / source
 *  是溯源与许可材料,不进安装流程(它们的 url 形状与 packages 一样,将来要给"许可"页用,
 *  照同一条链路就能抓)。 */
#define SXCL_JRE_INDEX_SCHEMA_STRING "sxcl.jre.index/1"

/** index.json 地址的环境变量(优先级见 sxcl_jre_resolve_index_url)。 */
#define SXCL_JRE_INDEX_URL_ENV "SXCL_JAVA_JRE_INDEX_URL"

/* ── 托管方式:**GitHub 单点**(用户决定;SilentCloud 那条不再依赖) ──
 *
 *   index.json 与包**放同一个 GitHub 仓库**(根 = 现在 out/ 的内容):
 *   https://raw.githubusercontent.com/<owner>/<repo>/<branch>/<相对路径>
 *   备用镜像:https://gh-proxy.com/ + 上面那条(url 直接做前缀拼接,实测可达)。
 *   **不依赖任何云盘特性**(列目录 API / .sha256 侧车 / 网页预览);校验只按
 *   **清单里的字面哈希 + size**(见 docs/19 §3)。
 *
 * **落位已定**(用户选):仓库 Silent-Studio-CN/index 的 SXCL/jre/ 子目录。
 * 三个宏都可以用编译开关覆盖(换仓库/换分支不用改代码):
 *     -DSXCL_JRE_GH_REPO="owner/repo" -DSXCL_JRE_GH_BRANCH=main -DSXCL_JRE_GH_SUBDIR="SXCL/jre"
 * 若有人把默认值改成带 <REPO> 的占位,resolve 会把它当成"没有默认值"并明确报错,
 * 而不是拿一个假地址去请求(sxcl_jre_default_index_url_is_placeholder 就是这个判据)。 */
#ifndef SXCL_JRE_GH_REPO
#  define SXCL_JRE_GH_REPO "Silent-Studio-CN/index"
#endif
#ifndef SXCL_JRE_GH_BRANCH
#  define SXCL_JRE_GH_BRANCH "main"
#endif
#ifndef SXCL_JRE_GH_SUBDIR
#  define SXCL_JRE_GH_SUBDIR "SXCL/jre"
#endif
/** 镜像前缀(gh-proxy 实测可达;把**完整** raw URL 接在它后面)。 */
#define SXCL_JRE_MIRROR_PREFIX "https://gh-proxy.com/"

/** 编译期默认的 index.json 地址(GitHub raw;落位已定)。 */
#define SXCL_JRE_INDEX_URL_DEFAULT \
    "https://raw.githubusercontent.com/" SXCL_JRE_GH_REPO "/" SXCL_JRE_GH_BRANCH \
    "/" SXCL_JRE_GH_SUBDIR "/index.json"

/** 默认的**相对根**(包与清单都挂在它下面;拼相对 url 时用它)。
 *  = https://raw.githubusercontent.com/<repo>/<branch>/<subdir>/ */
#define SXCL_JRE_RAW_BASE_DEFAULT \
    "https://raw.githubusercontent.com/" SXCL_JRE_GH_REPO "/" SXCL_JRE_GH_BRANCH \
    "/" SXCL_JRE_GH_SUBDIR "/"

/** 1 = 编译期默认地址里还带着 <REPO> 占位(仓库名没定)。 */
int sxcl_jre_default_index_url_is_placeholder(void);
/** index.json 地址的设置键(设置页 / settings.conf)。 */
#define SXCL_JRE_INDEX_URL_SETTING "java.jre_index_url"

/** 我们自己的标记文件名(**不是**官方的 .sxcl_runtime.json)。 */
#define SXCL_JRE_MARKER "jre.json"

#define SXCL_JRE_MAX_FILES        8
#define SXCL_JRE_ID_MAX          32
#define SXCL_JRE_VERSION_MAX     64
#define SXCL_JRE_FILE_MAX       768
#define SXCL_JRE_URL_MAX        768
#define SXCL_JRE_PATH_MAX      1024
#define SXCL_JRE_SHA256_MAX      72
#define SXCL_JRE_SHA1_MAX        48
#define SXCL_JRE_ERROR_MAX      256
#define SXCL_JRE_MESSAGE_MAX    192

/* ── 返回码(负数) ── */
#define SXCL_JRE_OK                 0
#define SXCL_JRE_ERR_ARG          (-1) /**< 参数不合法(必填为空 / 路径太长) */
#define SXCL_JRE_ERR_INDEX        (-2) /**< index.json 取不到 / 解析失败 / schema 不符 / 字段缺失 */
#define SXCL_JRE_ERR_COMPONENT    (-3) /**< index 里没有这个组件或没有适用于本机 ABI 的文件 */
#define SXCL_JRE_ERR_NET          (-4) /**< 传输层失败(连响应都没拿到) */
#define SXCL_JRE_ERR_DOWNLOAD     (-5) /**< 有文件下载或 SHA-256 校验失败 */
#define SXCL_JRE_ERR_EXTRACT      (-6) /**< 解包失败(.tar.xz 坏了 / 路径不安全 / 写不进去) */
#define SXCL_JRE_ERR_FINISH       (-7) /**< 收尾失败(找不到 bin/java、写不了 jre.json) */
#define SXCL_JRE_ERR_CANCELLED    (-8) /**< 用户取消 */
#define SXCL_JRE_ERR_NOMEM        (-9) /**< 内存不足 */
#define SXCL_JRE_ERR_IO          (-10) /**< 文件操作失败 */
#define SXCL_JRE_ERR_DISK        (-11) /**< 磁盘空间不够(下第一个字节之前就算出来) */
#define SXCL_JRE_ERR_NO_ABI      (-12) /**< index 里没有任何适用于本机 ABI 的文件 */

/** 返回码的稳定名字("ok"/"arg"/"index"/"component"/"net"/"download"/"extract"/"finish"/
 *  "cancelled"/"nomem"/"io"/"disk"/"abi")。 */
const char *sxcl_jre_code_name(int code);

/* ── index.json 的数据模型 ── */

typedef struct sxcl_jre_file {
    char file[SXCL_JRE_FILE_MAX];  /**< 相对 index 的路径,或**绝对 URL**(两种都吃) */
    char mirror[SXCL_JRE_URL_MAX]; /**< 可空的第二条候选(绝对 URL):清单在 GitHub、包在 SC 时用 */
    int64_t size;                  /**< **必填**:字节数(引擎按它强校验 + 分片) */
    char sha256[SXCL_JRE_SHA256_MAX]; /**< **必填**:64 位十六进制 */
    char sha1[SXCL_JRE_SHA1_MAX];  /**< 可空(空串 = index 没给) */
    /** 老 schema(与 Oracle 的下载页)把 sha256 写成**一个 .sha256 侧车文件的 URL**。
     *  非空时:先取它,再按下面三条解析规则拿哈希。
     *  优先级:**清单里能直接给的字面哈希 > 去侧车 URL 取**(见 docs/19 的信任口径)。 */
    char sha256_url[SXCL_JRE_URL_MAX];
    /** sha1 的侧车 URL(同样三种形态都吃:Mojang 系资源是 sha1)。 */
    char sha1_url[SXCL_JRE_URL_MAX];
    /** 1 = 清单里**没有任何校验信息**(既没有字面哈希也没有侧车 URL)。
     *  这时只按 size 校验,并且**必须**在结果与进度里写明"没有校验信息"——
     *  不许假装校验过(verify.c 上刚修过同类问题)。 */
    int no_hash;
    /** 1 = 这条来自 versions/platforms/<os>/<abi> 里被明确选中的那一档,不必再按文件名判 ABI。 */
    int platform_selected;
} sxcl_jre_file;

typedef struct sxcl_jre_component {
    char id[SXCL_JRE_ID_MAX];      /**< 组件名,如 "jre17" */
    int java_major;                /**< 如 17 */
    char version[SXCL_JRE_VERSION_MAX];
    size_t file_count;
    sxcl_jre_file files[SXCL_JRE_MAX_FILES];
    /** 下列字段由 sxcl.jre.index/1 的清单填(别的形式留空/0),见 docs/19 §2.1。 */
    char abi[24];                  /**< 组件自己的 ABI,如 "arm64-v8a";空 = 清单没分 ABI */
    char index_id[160];            /**< 清单里的 id,如 "jre17/17.0.20/arm64-v8a/9f87f8de24c52431" */
    char dir_key[256];             /**< 落盘相对路径(<component>/<version>/<abi>);空 = 用 id */
    int64_t installed_bytes;       /**< 清单里报的解开后字节数(只用于展示/核对,不做判据) */
} sxcl_jre_component;

/** 解析 index.json(纯函数,不联网)。返回组件条数;失败返回负错误码并写人话 err。 */
int sxcl_jre_index_parse(const char *text, size_t len, sxcl_jre_component *out, size_t cap,
                         char *err, size_t err_len);

/** 从解析好的列表里挑一个组件:component_id 非空则**必须**按它精确匹配(找不到就报错,
 *  不顺手换一个);否则按 java_major 挑主版本相等里 id 字典序最小的那个。
 *  找到返回 0。 */
int sxcl_jre_index_pick(const sxcl_jre_component *list, size_t count, const char *component_id,
                        int java_major, sxcl_jre_component *out, char *err, size_t err_len);

/** 本机 ABI 名(与 index 里 bin-<abi>.tar.xz 的后缀对应):
 *  安卓 arm64-v8a -> "arm64"、armeabi-v7a -> "armeabi-v7a"、x86_64 -> "x86_64";
 *  桌面 x64 -> "x64"、arm64 -> "arm64"。认不出按 "x64"。 */
const char *sxcl_jre_host_abi(void);

/** 这个文件条目要不要装到本机:名字里含 "universal" 一律要;形如 bin-<abi> 且 abi 等于本机
 *  ABI 的要;其余不要。1 = 要。 */
int sxcl_jre_file_applies(const sxcl_jre_file *file, const char *abi);

/** 来源解析(**不许把来源写死**),优先级从高到低:
 *    1) explicit_url(调用方直接给,含测试/自带托管)
 *    2) env_value(打包层/运维通过 SXCL_JAVA_JRE_INDEX_URL 给)
 *    3) setting_value(设置键 java.jre_index_url)
 *    4) SXCL_JRE_INDEX_URL_DEFAULT(GitHub raw;但**仓库名待确认**时它带 <REPO> 占位,
 *       这时视为"没有默认值")
 *    5) 都没有 -> **返回 ERR_ARG 并把人话说清楚**:默认仓库名还没确认,请填设置项/环境变量。
 *  纯函数(环境变量与设置值由调用方读好传进来),为的是能用单测钉住优先级。 */
int sxcl_jre_resolve_index_url(const char *explicit_url, const char *setting_value,
                               const char *env_value, char *out, size_t out_len);

/** 把 index.json 里的一条 file 变成可下载的绝对 URL。**两条口径都吃**:
 *    * file 是**相对路径**("jre17/universal.tar.xz")-> 相对 index.json 的位置解析:
 *      "https://h/sxcl/java/index.json" + "jre17/universal.tar.xz"
 *          -> "https://h/sxcl/java/jre17/universal.tar.xz"
 *    * file 是**绝对 URL**("https://cloud.example.cn/api/v1/o/p/JRE/jre17/universal.tar.xz")
 *      -> 原样返回(包放用户自己的云、清单放 GitHub 时就是这条)。
 *  相对路径里不许有 ".."、不许以 '/' 开头。 */
int sxcl_jre_join_url(const char *index_url, const char *rel, char *out, size_t out_len);

/* ── 安装 ── */

typedef enum sxcl_jre_stage {
    SXCL_JRE_STAGE_INDEX = 0,  /**< 取并解析 index.json */
    SXCL_JRE_STAGE_DOWNLOAD = 1,/**< 下载 + 逐文件 SHA-256(引擎) */
    SXCL_JRE_STAGE_EXTRACT = 2, /**< 解包 .tar.xz 到目标目录 */
    SXCL_JRE_STAGE_FINISH = 3,  /**< 核对 bin/java + 写 jre.json */
    SXCL_JRE_STAGE_END = 4      /**< 不是阶段:个数(也是"没失败"的哨兵) */
} sxcl_jre_stage;

#define SXCL_JRE_STAGE_COUNT ((int)SXCL_JRE_STAGE_END)

/** 阶段稳定名("index"/"download"/"extract"/"finish";END -> "none")与中文名。 */
const char *sxcl_jre_stage_id(sxcl_jre_stage stage);
const char *sxcl_jre_stage_name(sxcl_jre_stage stage);

/** 一次进度回调(阶段/百分比/速度/剩余一律齐全 —— 用户要求"进度与错误都要能报给用户")。
 *  **线程**:下载阶段的回调来自引擎的工作线程(与 engine.h 一个口径),UI 侧请投递到界面线程;
 *  回调期间指针只是"看一眼"的,别存。 */
typedef struct sxcl_jre_progress {
    sxcl_jre_stage stage;
    const char *stage_id;
    const char *stage_name;
    int percent;                 /**< 整体 0..100(单调不减,结束 = 100) */
    int64_t bytes_done;
    int64_t bytes_total;         /**< 已知的总字节数(0 = 还没量出来) */
    double speed_bps;            /**< 最近一次采样的速度(0 = 还不知道) */
    int64_t eta_seconds;         /**< 估计剩余秒数;<0 = 算不出来(不编数字) */
    size_t files_done;
    size_t files_total;
    size_t files_skipped;
    size_t files_failed;
    const char *component;
    const char *version;
    const char *current;         /**< 当前文件(相对路径;可空串) */
    char message[SXCL_JRE_MESSAGE_MAX]; /**< 人话状态(**永远非空**) */
} sxcl_jre_progress;

typedef void (*sxcl_jre_progress_fn)(void *ud, const sxcl_jre_progress *progress);

/** 一次安装请求。字符串生命周期:函数返回前不得释放。 */
typedef struct sxcl_jre_request {
    /* ── 装哪一个 ── */
    int java_major;             /**< 8 / 17 / 21 / 25;<=0 时必须有 component_id */
    const char *component_id;   /**< 如 "jre17";空 = 按 java_major 挑 */
    const char *abi;            /**< 空 = sxcl_jre_host_abi() */

    /* ── 从哪儿装 ── */
    const char *index_url;      /**< 空 = 环境变量 -> 设置键(由调用方读好传进来) */
    const char *index_setting;  /**< 设置键 java.jre_index_url 的值(可空) */
    const char *index_text;     /**< 非空 = 直接用这份 index.json(离线/夹具/测试),不联网 */
    /** 可选镜像前缀(如 "https://mirror.example.com/sxcl/java/"):非空时同一路径多一条候选。
     *  mirror_first=1 时镜像排在官方前面(镜像优先)。 */
    const char *mirror_base;
    int mirror_first;

    /* ── 装到哪 ── */
    const char *target_root;    /**< 空 = sxcl_java_runtime_default_root()(<配置目录>/runtime) */
    const char *target_dir;     /**< 直接指定最终目录(覆盖 target_root;测试用) */
    /** 1 = 目标目录里已有同 version 的 jre.json -> 立刻成功返回(不联网、不算哈希) */
    int skip_if_installed;
    /** 1 = 忽略已装的,重新下一遍(默认 0) */
    int force;

    /* ── 引擎 ── */
    sxcl_transport *(*transport_factory)(void *ud); /**< 必需(除非 index_text 给了且只解析) */
    const sxcl_engine_opts *engine_opts;            /**< 可空(workers/限速/分片/哈希缓存) */
    void *ud;

    /* ── 回调 ── */
    sxcl_jre_progress_fn on_progress;   /**< 可空 */
    int (*is_cancelled)(void *ud);      /**< 可空:非 0 = 取消 */
} sxcl_jre_request;

typedef struct sxcl_jre_result {
    int code;                    /**< 见 SXCL_JRE_ERR_* */
    int cancelled;
    sxcl_jre_stage fail_stage;
    const char *fail_stage_id;
    char component[SXCL_JRE_ID_MAX];
    char version[SXCL_JRE_VERSION_MAX];
    char abi[24];
    char index_url[SXCL_JRE_URL_MAX];
    char java_home[SXCL_JRE_PATH_MAX];   /**< 装出来的 JAVA_HOME(= 目标目录) */
    char java_path[SXCL_JRE_PATH_MAX];   /**< <home>/bin/java */
    char marker_path[SXCL_JRE_PATH_MAX]; /**< <home>/jre.json */
    char archive_dir[SXCL_JRE_PATH_MAX]; /**< 下载的 .tar.xz 留在哪(便于复核/清理) */
    size_t files_total;
    size_t files_done;
    size_t files_skipped;        /**< 已存在且 sha256 相符、一个字节都没下的 */
    size_t files_failed;
    /** 有几个包**没有任何校验信息**(只按 size 校验)。非 0 时 error 里也会写一句 ——
     *  这不是失败,但必须让用户看见。 */
    size_t files_without_hash;
    int64_t bytes_done;
    int64_t bytes_total;
    int skipped;                 /**< 1 = 整个组件已装好,直接跳过 */
    int64_t free_bytes;          /**< 开下之前的可用空间;<0 = 拿不到(未知) */
    char error[SXCL_JRE_ERROR_MAX];      /**< 人话原因(成功时空串) */
} sxcl_jre_result;

/** 下载 + 解包 + 落 jre.json。返回 0 = 成功(= out->code);否则 = out->code(负数)。
 *
 *  行为:
 *    1) 取 index.json(给了 index_text 就用它),按 java_major/component_id 挑组件;
 *    2) 目标目录 = target_dir,否则 <target_root>/<id>(与 java_runtime.c 的
 *       <root>/<组件>-<平台> **不是**同一套:自托管包的目录是 index 里的 id);
 *    3) skip_if_installed 且 <目标>/jre.json 里的 version 与 index 的一致 -> 直接成功返回;
 *    4) 逐文件:目标目录里已有同 sha256 的包就不重下(第二道闸),否则交给引擎
 *       (多连接 / 限速 / 断点续传 / **SHA-256 强校验**),下载到 <目标>/.archives/;
 *    5) 磁盘空间不够在**下第一个字节之前**就报 ERR_DISK;
 *    6) 逐个 .tar.xz 解包到目标目录(tar 的路径安全检查在 sxcl/tar.h 里);
 *    7) 核对 <目标>/bin/java 存在,再写 jre.json。
 *
 *  out 必须非空(**失败信息也写在它里面**)。 */
int sxcl_jre_install(const sxcl_jre_request *request, sxcl_jre_result *out);

/** 读 <dir>/jre.json。成功返回 0 并把 version 写进 version_out(可空)、整份 JSON 写进
 *  json_out(可空)。文件不存在/坏了返回 SXCL_JRE_ERR_IO / ERR_INDEX。 */
int sxcl_jre_read_marker(const char *dir, char *version_out, size_t version_len,
                         char *json_out, size_t json_len);

/** 已装判定:1 = <dir>/bin/java 在 **且** <dir>/jre.json 里的 version 与 want_version 相同
 *  (want_version 为空 = 只要求标记文件在)。0 = 不是;负 = 参数错。 */
int sxcl_jre_is_installed(const char *dir, const char *want_version);

/** 目标目录:<target_root>/<组件 id>(root 空 = sxcl_java_runtime_default_root())。 */
int sxcl_jre_target_dir(const char *target_root, const char *component_id, char *out,
                        size_t out_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_JRE_HOSTED_H */
