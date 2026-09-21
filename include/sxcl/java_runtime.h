/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_JAVA_RUNTIME_H
#define SXCL_JAVA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/engine.h"   /* sxcl_engine_opts(引擎配置) */
#include "sxcl/json.h"
#include "sxcl/launch.h"   /* sxcl_java_info / sxcl_java_os(探测与平台判定) */
#include "sxcl/net.h"      /* sxcl_transport(传输后端) */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 常量(全部对齐参考实现,不许另起一套) ── */

/** 官方 JRE 组件总清单。Python mojang_runtime.py:59-62 的同一条 URL。 */
#define SXCL_JAVA_RUNTIME_MANIFEST_URL \
    "https://launchermeta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"

/** BMCLAPI 镜像主机(实测 launchermeta/piston-meta/piston-data 同路径透传,见 mirror.py:27-31)。 */
#define SXCL_JAVA_RUNTIME_MIRROR_HOST "bmclapi2.bangbang93.com"

/** 标记文件名(Python `.sxcl_runtime.json`):装完写一份,清单来源与版本可追溯。 */
#define SXCL_JAVA_RUNTIME_MARKER ".sxcl_runtime.json"

#define SXCL_JAVA_RUNTIME_COMPONENT_MAX 32
#define SXCL_JAVA_RUNTIME_VERSION_MAX   64
#define SXCL_JAVA_RUNTIME_URL_MAX       512
#define SXCL_JAVA_RUNTIME_SHA1_MAX      48
#define SXCL_JAVA_RUNTIME_PLATFORM_MAX  24
#define SXCL_JAVA_RUNTIME_PATH_MAX      1024
#define SXCL_JAVA_RUNTIME_ERROR_MAX     256

/* 返回码(负数) */
#define SXCL_JAVA_RUNTIME_OK               0
#define SXCL_JAVA_RUNTIME_ERR_ARG        (-1)  /**< 参数不合法(必填为空/路径太长) */
#define SXCL_JAVA_RUNTIME_ERR_NET        (-2)  /**< 清单取不到(网络/状态码/太大) */
#define SXCL_JAVA_RUNTIME_ERR_MANIFEST   (-3)  /**< 组件不存在 / 清单解析失败 / 清单 SHA-1 不符 */
#define SXCL_JAVA_RUNTIME_ERR_DOWNLOAD   (-4)  /**< 有文件下载或校验失败 */
#define SXCL_JAVA_RUNTIME_ERR_FINISH     (-5)  /**< 装完了却找不到 bin/java(清单不完整/平台不对) */
#define SXCL_JAVA_RUNTIME_ERR_CANCELLED  (-6)  /**< 用户取消 */
#define SXCL_JAVA_RUNTIME_ERR_NOMEM      (-7)  /**< 内存不足 */
#define SXCL_JAVA_RUNTIME_ERR_IO         (-8)  /**< 文件操作失败(建目录/写标记/设可执行位) */
#define SXCL_JAVA_RUNTIME_ERR_DISK       (-9)  /**< 磁盘空间不够(装之前就算出来,**没下过一个字节**) */

/** 预置(一次装齐)最多几个组件;超出部分按"多余"丢掉并如实报。 */
#define SXCL_JAVA_RUNTIME_PRESET_MAX 8

/** 返回码的稳定名字("ok"/"net"/"manifest"/"download"/"finish"/"cancelled"/"nomem"/"io"/"arg")。 */
const char *sxcl_java_runtime_code_name(int code);

/* ── 平台键与组件挑选 ── */

/** all.json 第一层的平台键。Windows: windows-x64 / windows-arm64 / windows-x86;
 *  macOS: mac-os / mac-os-arm64;Linux+Android: linux(32 位 x86 是 linux-i386)。
 *  依据:Python platform_key()(mojang_runtime.py:75-85)与 HMCL JavaManager.getMojangJavaPlatform()。
 *  Android 归 linux(与 launch.h 的 rules 口径一致)。 */
const char *sxcl_java_runtime_platform_key(void);
/** 指定平台/架构的版本(arch 取 "x64"/"x86"/"arm64",认不出按 x64);测试与非本机分析用。 */
const char *sxcl_java_runtime_platform_key_for(sxcl_java_os os, const char *arch);

/** Minecraft 版本 -> 需要的 Java 主版本(Python required_java_major,mojang_runtime.py:88-109)。
 *  mc_version 空/认不出 -> 21(与 Python 一致)。 */
int sxcl_java_runtime_required_major(const char *mc_version);


/** UI 下拉用的组件候选(Python COMPONENT_PREVIEW,mojang_runtime.py:65-69)。 */
typedef struct sxcl_java_runtime_preset {
    const char *component;
    const char *label;
    int major;
} sxcl_java_runtime_preset;

size_t sxcl_java_runtime_preset_count(void);
const sxcl_java_runtime_preset *sxcl_java_runtime_preset_at(size_t index);

/** all.json 里的一个组件(某平台下的一条候选)。 */
typedef struct sxcl_java_runtime_component {
    char component[SXCL_JAVA_RUNTIME_COMPONENT_MAX];
    char version[SXCL_JAVA_RUNTIME_VERSION_MAX];   /**< version.name,如 "17.0.8" */
    char released[24];                             /**< version.released(可空) */
    char manifest_url[SXCL_JAVA_RUNTIME_URL_MAX];
    char manifest_sha1[SXCL_JAVA_RUNTIME_SHA1_MAX];
    int64_t manifest_size;
    int major;                                     /**< 从 version.name 解析的主版本;0 = 认不出 */
} sxcl_java_runtime_component;

/** 枚举某平台的全部组件(排除 minecraft-java-exe 与 java-runtime-gamma-snapshot,
 *  与 Python _EXCLUDED_COMPONENTS 一致)。返回写入条数;all_json 为空/结构不对返回 0。
 *  只读、不分配。 */
size_t sxcl_java_runtime_components(const sxcl_json *all_json, const char *platform,
                                    sxcl_java_runtime_component *out, size_t cap);

/** 挑一个"够用且优先"的组件(Python choose_component,mojang_runtime.py:133-152):
 *  主版本 >= required 里取最小,同主版本按官方推荐顺序(delta > gamma > epsilon > legacy > …)。
 *  成功返回 0 并写 out;找不到返回 SXCL_JAVA_RUNTIME_ERR_MANIFEST。 */
int sxcl_java_runtime_choose(const sxcl_json *all_json, const char *platform, int required_major,
                             sxcl_java_runtime_component *out);

/* ── 清单抓取 ── */

/** 抓清单/组件的请求。 */
typedef struct sxcl_java_runtime_query {
    const char *all_json_text;   /**< 非空 = 直接用这份 all.json(离线/夹具),不联网 */
    const char *all_json_url;    /**< 空 = SXCL_JAVA_RUNTIME_MANIFEST_URL */
    const char *manifest_text;   /**< 非空 = 直接用这份组件清单,不联网 */
    const char *platform;        /**< 空 = 本机 */
    int manifest_relaxed_mirror; /**< 1 = 镜像那份组件清单不校验清单级 sha1(逐文件 sha1 照旧强校验) */
    int *manifest_source_out;    /**< 非空时写:0 = 官方,1 = 镜像(取组件清单实际用了哪一条) */
    sxcl_transport *(*transport_factory)(void *ud); /**< 传输后端工厂(必需,除非文本都已给) */
    void *ud;                    /**< 回传给 transport_factory */
} sxcl_java_runtime_query;

/** 取 + 解析 all.json。成功返回文档(调用方 sxcl_json_free),失败返回 NULL 并写人话 err。 */
sxcl_json *sxcl_java_runtime_fetch_all(const sxcl_java_runtime_query *query, char *err, size_t err_len);

/** 组件枚举(高层):取 all.json -> 枚举本平台组件。返回条数,<0 = 失败(err 里人话)。 */
int sxcl_java_runtime_list(const sxcl_java_runtime_query *query, sxcl_java_runtime_component *out,
                           size_t cap, char *err, size_t err_len);

/** 取 + 解析组件清单(带 manifest.sha1 强校验)。entries = 已挑好的组件(见上面两个函数)。
 *  成功返回文档,失败 NULL 并写 err。 */
sxcl_json *sxcl_java_runtime_fetch_manifest(const sxcl_java_runtime_query *query,
                                            const sxcl_java_runtime_component *entry,
                                            char *err, size_t err_len);

/* ── 安装 ── */

/** 安装阶段(数值 = 顺序)。 */
typedef enum sxcl_java_runtime_stage {
    SXCL_JAVA_RUNTIME_STAGE_QUERY = 0,     /**< 取/解析 all.json 并挑组件 */
    SXCL_JAVA_RUNTIME_STAGE_MANIFEST = 1,  /**< 取/解析并校验组件清单 */
    SXCL_JAVA_RUNTIME_STAGE_DOWNLOAD = 2,  /**< 下载 + 逐文件 SHA-1(引擎) */
    SXCL_JAVA_RUNTIME_STAGE_FINISH = 3,    /**< 可执行位 / 标记文件 / 核对 bin/java */
    SXCL_JAVA_RUNTIME_STAGE_END = 4        /**< 不是阶段:个数(也用作"没失败"的哨兵) */
} sxcl_java_runtime_stage;

#define SXCL_JAVA_RUNTIME_STAGE_COUNT ((int)SXCL_JAVA_RUNTIME_STAGE_END)

/** 阶段稳定字符串名("query"/"manifest"/"download"/"finish";END -> "none")。 */
const char *sxcl_java_runtime_stage_id(sxcl_java_runtime_stage stage);
/** 阶段中文名("获取组件清单"/"下载组件清单"/"下载并校验文件"/"收尾";END -> "未失败")。 */
const char *sxcl_java_runtime_stage_name(sxcl_java_runtime_stage stage);

/** 一次进度回调。**线程**:下载阶段的回调来自引擎的工作线程(与 engine.h/install.h 一个口径),
 *  UI 侧请用队列连接投递到界面线程;回调里别做重活。回调期间指针都只是"看一眼"的,别存。 */
typedef struct sxcl_java_runtime_progress {
    sxcl_java_runtime_stage stage;
    const char *stage_id;
    const char *stage_name;
    int percent;                 /**< 整体 0..100(单调不减,结束 = 100) */
    int64_t bytes_done;
    int64_t bytes_total;
    size_t files_done;           /**< 已落定(含"已存在且校验通过"跳过的) */
    size_t files_total;
    size_t files_skipped;        /**< 其中:一个字节都没下的 */
    size_t files_failed;
    const char *component;
    const char *version;
    const char *current;         /**< 当前文件(相对路径;可空串) */
    char message[192];           /**< 人话状态(**永远非空**) */

    /* ── 预置(一次装齐)才填的字段;单组件安装时 component_total = 1、index = 0 ── */
    int component_index;         /**< 正在装第几个组件(0 起) */
    int component_total;         /**< 这次一共几个组件 */
    int component_skipped;       /**< 1 = 这个组件已装好,整个跳过(没联网、没下载) */
    int overall_percent;         /**< 全部组件合计 0..100 */
    int64_t overall_bytes_done;  /**< 全部组件合计已确认字节 */
    int64_t overall_bytes_total; /**< 全部组件合计字节(装之前量出来的) */
} sxcl_java_runtime_progress;

typedef void (*sxcl_java_runtime_progress_fn)(void *ud, const sxcl_java_runtime_progress *progress);

/** 一次安装请求。字符串生命周期:函数返回前不得释放。 */
typedef struct sxcl_java_runtime_request {
    /* ── 选什么(优先级 component > required_major > mc_version) ── */
    const char *component;      /**< 组件名;空 = 自动挑 */
    int required_major;         /**< <=0 = 由 mc_version 推(也没有 = 21) */
    const char *mc_version;     /**< 如 "1.20.1";可空 */
    const char *platform;       /**< 空 = 本机(sxcl_java_runtime_platform_key) */

    /* ── 装到哪(target_dir 优先;都没有则报参数错) ── */
    const char *target_root;    /**< 装到 <root>/<组件>-<平台>;空 = sxcl_java_runtime_default_root() */
    const char *target_dir;     /**< 直接指定最终目录(覆盖 target_root;测试/自定义布局用) */

    /* ── 清单来源(离线/夹具:给了就不联网) ── */
    const char *all_json_text;
    const char *all_json_url;   /**< 空 = 官方 URL */
    const char *manifest_text;
    int use_mirror;             /**< 1 = 官方 URL 之外再加一条 BMCLAPI 候选(默认开) */

    /** 1 = <target>/bin/java 与标记文件都在 -> 立刻成功返回(不联网、不算哈希)。
     *  这是"别每次重下"的**组件级**快路径;0 时仍会逐文件校验并跳过完好的文件。 */
    int skip_if_installed;
    /** 1 = 官方取不到组件清单时,接受 BMCLAPI 的那一份(**逐文件 raw.sha1 仍然强校验**)。
     *  为什么需要:BMCLAPI 会把清单里的下载地址改写成它自己的,字节与官方必然不同,
     *  于是 manifest.sha1 永远对不上 —— 严格校验等于"镜像这条路对组件清单从来没生效过"。
     *  0 = 老行为(镜像清单也必须过 manifest.sha1)。 */
    int manifest_relaxed_mirror;

    /* ── 传输与引擎 ── */
    sxcl_transport *(*transport_factory)(void *ud); /**< 引擎的后端工厂(必填,除非清单文本都给了) */
    const sxcl_engine_opts *engine_opts;  /**< 可空:引擎配置(workers/限速/分片/哈希缓存);
                                           *   非空时它的 transport_factory 会被本请求的覆盖 */
    void *ud;

    /* ── 回调 ── */
    sxcl_java_runtime_progress_fn on_progress; /**< 可空 */
    int (*is_cancelled)(void *ud);             /**< 可空:非 0 = 取消(与引擎/install 同语义) */
} sxcl_java_runtime_request;

/** 一次安装的结果。 */
typedef struct sxcl_java_runtime_result {
    int code;                    /**< 见 SXCL_JAVA_RUNTIME_ERR_* */
    int cancelled;
    sxcl_java_runtime_stage fail_stage;
    const char *fail_stage_id;
    char component[SXCL_JAVA_RUNTIME_COMPONENT_MAX];
    char platform[SXCL_JAVA_RUNTIME_PLATFORM_MAX];
    char version[SXCL_JAVA_RUNTIME_VERSION_MAX];
    char java_home[SXCL_JAVA_RUNTIME_PATH_MAX];  /**< 装出来的 JAVA_HOME(成功时非空) */
    char java_path[SXCL_JAVA_RUNTIME_PATH_MAX];  /**< <home>/bin/java[.exe] */
    char marker_path[SXCL_JAVA_RUNTIME_PATH_MAX];
    size_t files_total;
    size_t files_done;
    size_t files_skipped;        /**< 已存在且校验通过、一个字节都没下的(绝不重复下载的证据) */
    size_t files_failed;
    int64_t bytes_done;
    int64_t bytes_total;
    int skipped;                 /**< 1 = 整个组件已装好,直接跳过(没联网、没下载) */
    char manifest_source[16];    /**< 组件清单来源:"official" / "mirror" / "provided" / "" */
    char error[SXCL_JAVA_RUNTIME_ERROR_MAX];     /**< 人话原因(成功时空串) */
} sxcl_java_runtime_result;

/** 下载并安装官方 JRE。返回 0 = 成功(=out->code);否则 = out->code(负数)。
 *
 *  行为(逐条对齐 mojang_runtime.py:213-304):
 *    1) 取 all.json -> 按平台挑组件(给了 component 就用它,不存在直接报错,不"顺手换一个");
 *    2) 取组件清单,按 manifest.sha1 强校验;
 *    3) 目标目录 = target_dir,否则 <target_root>/<组件>-<平台>;清单里的 directory 条目先建出来;
 *    4) 文件按 downloads.raw(URL/sha1/size)入队给引擎 —— 引擎自带"已存在且校验通过就跳过"
 *       的快路径,所以**重复安装不会重复下载**;
 *    5) 非 Windows 给 executable 条目补可执行位;核对 <home>/bin/java 存在;
 *    6) 写 .sxcl_runtime.json 标记(Python 版认这个文件)。
 *
 *  out 必须非空(**失败信息也写在它里面**)。 */
int sxcl_java_runtime_install(const sxcl_java_runtime_request *request, sxcl_java_runtime_result *out);

/* ── 预置策略:首启一次装齐(用户要求,见 docs/07「一次装齐」) ──
 *
 * 为什么要"策略"而不是写死四个名字:清单是**活的** —— jre-legacy/gamma/delta 会升版本,
 * epsilon(Java 25)是 2025-12 才出现的。写死名字的那天就是"清单里没有它"的那天。
 * 所以策略只谈**主版本**:8/17/21 是必须覆盖的基线,清单里有更新的主版本也一并纳入。 */

/** 这次要装哪些 Java 主版本(升序,去重)。规则:
 *  1) 基线永远是 8 / 17 / 21 —— 覆盖 1.16.5 及更早、1.18~1.20.4、1.20.5+;
 *  2) mc_versions 里每个版本按 sxcl_java_runtime_required_major 折算出的主版本也算进去
 *     (例如只玩 1.20.1 也仍会带上 8/17/21 —— 用户要的是"一次装好,以后不用再遇到");
 *  3) include_newest 且 newest_major > 0 时,把清单里**最新的那个主版本**也加进来
 *     (当前真实清单 = 25),已经不超过已有最大值就不重复加。
 *  返回写入 out 的条数(<= cap);out 与 cap 由调用方给(容量 >= 8 就够)。 */
size_t sxcl_java_runtime_needed_majors(const char *const *mc_versions, size_t mc_version_count,
                                       int newest_major, int include_newest, int *out, size_t cap);

/** 计划里的一项(装之前就能看到的"要装什么 / 多大 / 装到哪 / 装过没")。 */
typedef struct sxcl_java_runtime_plan_item {
    char component[SXCL_JAVA_RUNTIME_COMPONENT_MAX];
    char version[SXCL_JAVA_RUNTIME_VERSION_MAX];
    int major;
    int installed;               /**< 1 = 目标目录里已经有可用的 bin/java */
    int64_t bytes;               /**< 组件清单里 raw.size 的合计;0 = 没量(measure=0 或量不出来) */
    size_t files;                /**< 要下的文件数(type=file 且有 downloads.raw) */
    char target_dir[SXCL_JAVA_RUNTIME_PATH_MAX];
    char java_path[SXCL_JAVA_RUNTIME_PATH_MAX];
    char reason[96];             /**< 人话:"1.20.1 需要 Java 17" / "官方清单里最新的 Java 25" */
} sxcl_java_runtime_plan_item;

/** 一次"要装什么"的查询。 */
typedef struct sxcl_java_runtime_plan_request {
    const char *const *mc_versions;  /**< 想玩的 MC 版本(可空 = 只有基线+最新) */
    size_t mc_version_count;
    const char *platform;            /**< 空 = 本机 */
    const char *target_root;         /**< 空 = sxcl_java_runtime_default_root() */
    int include_newest;              /**< 1 = 清单里比基线新的最大主版本也纳入(建议 1) */
    int measure;                     /**< 1 = 取每个组件的清单,把所有 raw.size 加起来(要网络) */
    int skip_installed;              /**< 1 = 已装好的不出现在计划里 */
    /* 清单来源(离线/夹具:给了就不联网) */
    const char *all_json_text;
    const char *all_json_url;
    sxcl_transport *(*transport_factory)(void *ud);
    void *ud;
} sxcl_java_runtime_plan_request;

/** 出一份安装计划(不下载任何组件文件;measure=1 时只会取组件清单)。
 *  成功返回条数(可能是 0 = 全装好了);失败返回负的错误码并写 err。
 *  out/cap:调用方给的数组(容量 >= SXCL_JAVA_RUNTIME_PRESET_MAX 就够)。 */
int sxcl_java_runtime_plan(const sxcl_java_runtime_plan_request *request,
                           sxcl_java_runtime_plan_item *out, size_t cap, char *err, size_t err_len);

/* ── 一次装齐(预置) ── */

/** 单个组件的安装结果(预置里一项)。 */
typedef struct sxcl_java_runtime_item_result {
    char component[SXCL_JAVA_RUNTIME_COMPONENT_MAX];
    char version[SXCL_JAVA_RUNTIME_VERSION_MAX];
    int major;
    int code;                    /**< 0 = 成功;负数 = 见 SXCL_JAVA_RUNTIME_ERR_* */
    int skipped;                 /**< 1 = 已装好,整个跳过 */
    int installed;               /**< 1 = 结束时可用的运行时确实在盘上 */
    char java_home[SXCL_JAVA_RUNTIME_PATH_MAX];
    char java_path[SXCL_JAVA_RUNTIME_PATH_MAX];
    size_t files_total;
    size_t files_done;
    size_t files_skipped;
    size_t files_failed;
    int64_t bytes_total;
    int64_t bytes_done;
    int64_t bytes_on_disk;       /**< 结束时该组件在盘上实际占的字节(递归统计) */
    double seconds;              /**< 这个组件花了多少秒 */
    char error[SXCL_JAVA_RUNTIME_ERROR_MAX];
} sxcl_java_runtime_item_result;

/** 一次预置的请求。 */
typedef struct sxcl_java_runtime_preset_request {
    const char *const *mc_versions;  /**< 想玩的 MC 版本(可空 = 基线 8/17/21 + 最新) */
    size_t mc_version_count;
    const char *platform;            /**< 空 = 本机 */
    const char *target_root;         /**< 空 = 默认 runtime 根;测试/取证用临时目录 */
    int include_newest;              /**< 1 = 最新主版本也装(建议 1) */
    int force;                       /**< 1 = 已装好的也重装(默认 0 = 跳过) */
    /** 预留余量:要求"可用空间 >= 预计下载量 + margin"才开下。<=0 时用 64MiB 兜底。
     *  拿不到可用空间时**不拦**(如实报未知),不编一个数字出来。 */
    int64_t min_free_margin_bytes;
    /* 清单来源 */
    const char *all_json_text;
    const char *all_json_url;
    int use_mirror;
    /* 引擎 */
    sxcl_transport *(*transport_factory)(void *ud);
    const sxcl_engine_opts *engine_opts;
    void *ud;
    /* 回调 */
    sxcl_java_runtime_progress_fn on_progress;  /**< 可空;每个组件都报(带 index/total) */
    int (*is_cancelled)(void *ud);              /**< 可空:非 0 = 取消 */
} sxcl_java_runtime_preset_request;

/** 一次预置的结果。 */
typedef struct sxcl_java_runtime_preset_result {
    int code;                    /**< 0 = 全部成功(含"已装好跳过");否则第一个失败的码 */
    int cancelled;
    int64_t free_before;         /**< 开下之前的可用空间;<0 = 拿不到(未知) */
    int64_t planned_bytes;       /**< 计划里所有组件的字节合计(不含已跳过的) */
    int64_t downloaded_bytes;    /**< 实际下下来的字节 */
    int64_t on_disk_bytes;       /**< 结束时所有组件在盘上的字节合计 */
    size_t planned;              /**< 计划里几个组件 */
    size_t installed;            /**< 这次真装了几个 */
    size_t skipped;              /**< 已装好跳过几个 */
    size_t failed;               /**< 失败几个 */
    size_t count;                /**< items 里有效条数 */
    sxcl_java_runtime_item_result items[SXCL_JAVA_RUNTIME_PRESET_MAX];
    char error[SXCL_JAVA_RUNTIME_ERROR_MAX];  /**< 人话原因(成功时空串) */
} sxcl_java_runtime_preset_result;

/** 一次装齐:按策略出计划 -> 量大小 -> 查磁盘 -> 逐个组件安装(已装好的跳过)。
 *  返回 0 = 成功(= out->code);否则 = out->code(负数)。
 *
 *  行为:
 *    1) 出计划(sxcl_java_runtime_plan,measure=1)并**先把每个组件的预计大小与合计报出来**;
 *    2) 目标根目录所在卷的可用空间 **减掉** 预计下载量(已装好的不算)后不足 -> 立刻
 *       返回 SXCL_JAVA_RUNTIME_ERR_DISK,**一个字节都不下**(这就是"提前报人话错误");
 *    3) 逐个组件调用 sxcl_java_runtime_install(skip_if_installed=!force):
 *       已装好的走组件级快路径(不联网、不算哈希);没装的照常逐文件校验+断点续传;
 *    4) 每个组件单独报进度(progress->component_index/component_total/overall_percent),
 *       一个组件失败不拖累其余组件,全部跑完后 code = 第一个失败的码(逐个都在 items 里);
 *    5) 统计每个组件在盘上的实际占用(bytes_on_disk)与耗时(seconds)。
 *
 *  out 必须非空(**失败信息也写在它里面**)。cancel 时已经装好的组件**保留**(不删),
 *  下次再来就是"跳过"——这正是断点续装的意义。 */
int sxcl_java_runtime_install_preset(const sxcl_java_runtime_preset_request *request,
                                     sxcl_java_runtime_preset_result *out);

/** 递归统计目录在盘上占的字节数(目录不存在 = 0)。给"装完报实际占用"用。 */
int64_t sxcl_java_runtime_dir_bytes(const char *dir);

/* ── 找已装的运行时 ── */

/** 默认 runtime 根目录(与设置目录同一套平台分支):
 *   Windows  %APPDATA%/SilentXCraftLauncher/runtime
 *   macOS    ~/Library/Application Support/SilentXCraftLauncher/runtime
 *   Linux    $XDG_CONFIG_HOME/silentxcraftlauncher/runtime(或 ~/.config/…)
 *   Android  $SXCL_ANDROID_FILES/SilentXCraftLauncher/runtime(**应用私有目录**)
 *   环境变量 SXCL_RUNTIME_DIR 覆盖一切。返回 0 / 负错误码(见 sxcl_settings_default_dir 的口径)。 */
int sxcl_java_runtime_default_root(char *out, size_t out_len, char *err, size_t err_len);

/** 扫描 root 下已安装的官方运行时(Python find_installed_runtimes,mojang_runtime.py:307-325):
 *  只认 <child>/bin/java[.exe] 真实存在的目录,逐个读 release 得出画像。
 *  返回写入条数;root 为空/不存在返回 0(不是错误)。 */
size_t sxcl_java_runtime_find(const char *target_root, sxcl_java_info *out, size_t cap);

/** 单个目录是不是一份可用的运行时(<dir>/bin/java 存在)。1 = 是。 */
int sxcl_java_runtime_is_installed(const char *dir);

/** 取某个运行时目录里 java 可执行文件的全路径(不检查存在);写进 out。返回 0 / 负错误码。 */
int sxcl_java_runtime_java_path(const char *dir, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_JAVA_RUNTIME_H */
