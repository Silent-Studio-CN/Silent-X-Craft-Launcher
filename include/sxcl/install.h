/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_INSTALL_H
#define SXCL_INSTALL_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/engine.h"
#include "sxcl/json.h"
#include "sxcl/loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 阶段(固定:数值 = 顺序,字符串名稳定,UI/日志都靠它) ── */

typedef enum sxcl_install_stage {
    SXCL_INSTALL_STAGE_MANIFEST = 0,          /**< 获取版本清单 */
    SXCL_INSTALL_STAGE_VERSION_JSON = 1,      /**< 下载并解析版本 JSON */
    SXCL_INSTALL_STAGE_CLIENT_JAR = 2,        /**< 下载客户端 jar */
    SXCL_INSTALL_STAGE_LIBRARIES = 3,         /**< 下载依赖库 */
    SXCL_INSTALL_STAGE_ASSET_INDEX = 4,       /**< 下载资源索引 */
    SXCL_INSTALL_STAGE_ASSET_OBJECTS = 5,     /**< 下载资源对象 */
    SXCL_INSTALL_STAGE_LOADER_INSTALLER = 6,  /**< 下载加载器安装器 */
    SXCL_INSTALL_STAGE_LOADER_RUN = 7,        /**< 执行加载器安装 */
    SXCL_INSTALL_STAGE_NATIVES = 8,           /**< 解压 natives */
    SXCL_INSTALL_STAGE_FINISH = 9,            /**< 整理文件 */
    SXCL_INSTALL_STAGE_END = 10               /**< 不是阶段:阶段个数(也用作"成功/未失败"的哨兵值) */
} sxcl_install_stage;

/** 阶段总数(枚举项个数)。表长与它不一致时 install.c 直接编不过(_Static_assert)。 */
#define SXCL_INSTALL_STAGE_COUNT ((int)SXCL_INSTALL_STAGE_END)

/** 稳定字符串名("manifest"/"version_json"/…/"finish"),日志可解析。
 *  SXCL_INSTALL_STAGE_END(哨兵 = "没失败")返回 "none";其它越界值返回 "unknown"。 */
const char *sxcl_install_stage_id(sxcl_install_stage stage);
/** 中文显示名("获取版本清单"/…),UI 可直接显示。END 返回 "未失败",越界返回 "未知阶段"。 */
const char *sxcl_install_stage_name(sxcl_install_stage stage);
/** 是不是"只有含加载器的计划才跑"的阶段(6/7)。 */
int sxcl_install_stage_is_loader(sxcl_install_stage stage);

/* ── 计划:要装什么 ── */

/** 资源完整性级别。**零初始化 = SXCL_INSTALL_ASSETS_DEFAULT = 全量**(与 loader.h 的
 *  "反向开关"一个口径:结构体 memset 0 之后就是正常语义,不用先想默认值)。 */
typedef enum sxcl_install_assets {
    SXCL_INSTALL_ASSETS_DEFAULT = 0, /**< 全量:索引 + 所有资源对象(等价于 FULL) */
    SXCL_INSTALL_ASSETS_NONE = 1,    /**< 一个资源都不碰(索引也不下) */
    SXCL_INSTALL_ASSETS_INDEX = 2,   /**< 只下资源索引(不展开对象);给"快速进游戏"用 */
    SXCL_INSTALL_ASSETS_FULL = 3     /**< 全量(显式写法,语义同 DEFAULT) */
} sxcl_install_assets;

/** 官方版本清单地址(与 Python 的 MOJANG_VERSION_MANIFEST_URL 一致;plan.manifest_url 可覆盖)。 */
#define SXCL_INSTALL_MANIFEST_URL "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

typedef struct sxcl_install_plan {
    /* ── 必填 ── */
    const char *game_dir;        /**< 游戏目录(UTF-8,形如 "D:/mc/.minecraft") */
    const char *version_id;      /**< 要装的 Minecraft 版本号,如 "1.20.1"(去清单里找它) */

    /* ── 可空 ── */
    const char *instance_name;   /**< 本地实例名(版本目录名);空 = 用 version_id。
                                  *   客户端 jar / 版本 JSON / natives 都落在 versions/<实例名>/ 下。 */
    sxcl_loader_kind loader;     /**< 加载器类型;零初始化 = SXCL_LOADER_VANILLA(只装原版) */
    const char *loader_version;  /**< 加载器版本(给安装器用),可空 */
    const char *installer_url;   /**< 加载器安装器下载地址(含加载器时:**installer_jar 为空则必填**)。
                                  *   由加载器版本列表层给(规格 §7 缺口 2);本层不会自己拼 URL。 */
    const char *installer_jar;   /**< 已下好的安装器 jar(给了就不下,只核对存在);可空 */
    const char *java_path;       /**< java 可执行路径(含加载器时必填:安装器要它) */
    const char *manifest_url;    /**< 版本清单地址;空 = SXCL_INSTALL_MANIFEST_URL */
    const char *manifest_text;   /**< 已经拿在手里的清单文本(UI 刚取过就别再取一次);空 = 自己去取 */
    const char *asset_base_url;  /**< 资源对象 CDN 根;空 = SXCL_ASSET_OBJECTS_BASE(manifest.h) */
    const char *mirror_base;     /**< 资源对象镜像根(第二候选路);可空 */
    const char *loader_mirror_maven; /**< 给加载器安装器的 maven 镜像(--mirror);可空 */
    sxcl_install_assets assets;  /**< 资源完整性级别;零初始化 = 全量 */
    int keep_installer;          /**< 1 = 留着本次下到的安装器 jar(排查用);0 = 收尾时删掉(与 Python 一致) */
    int loader_timeout_ms;       /**< 加载器安装超时;<=0 = loader.h 的默认(30 分钟 / OptiFine 15 分钟) */
    const sxcl_engine_opts *engine_opts; /**< 交给引擎的配置(workers/限速/哈希缓存/**传输后端工厂**)。
                                          *   默认表实现**必须**有 transport_factory,否则下载全失败。 */
} sxcl_install_plan;

/** 把版本清单 JSON 落到**启动层要的位置**:<game_dir>/versions/<version_id>/<version_id>.json。
 *
 * 为什么单独给一个函数:启动层(sxcl_launch_run)读的就是这个路径,而"下载一个版本"
 * (sxcl-dl version)和"安装一个版本"(sxcl_install_run)是两条不同的路 ——
 * 两边都必须把 JSON 写到同一个地方,否则会出现**文件都下完了却启动不了**这种产品级缺陷
 * (实测踩过:sxcl-dl version 只把 JSON 留在缓存目录,启动层报找不到版本 JSON)。
 *
 * 做法:
 *   - 先写 <目标>.tmp 再原子改名 -> **重复执行不会损坏已有的好文件**(改名是原子的);
 *   - version_id 只允许 [0-9A-Za-z._-] -> 挡住 "../" 之类逃出 versions/ 的路径;
 *   - 内容原样拷贝 json_path(调用方通常给缓存里那份已做过 SHA-1 校验的)。
 * 成功返回 0;失败返回负错误码并写人话 err。 */
int sxcl_install_write_version_json(const char *game_dir, const char *version_id,
                                   const char *json_path, char *err, size_t err_len);

/** 计划里实际会跑的阶段数(UI 照它画行)。plan 为空返回 0。 */
size_t sxcl_install_plan_stage_count(const sxcl_install_plan *plan);
/** 第 index 个阶段的枚举;越界返回 SXCL_INSTALL_STAGE_END。 */
sxcl_install_stage sxcl_install_plan_stage_at(const sxcl_install_plan *plan, size_t index);
/** 这个阶段在这次计划里跑不跑。 */
int sxcl_install_plan_stage_included(const sxcl_install_plan *plan, sxcl_install_stage stage);
/** 实际实例名(instance_name 为空就是 version_id);plan 为空返回 ""。 */
const char *sxcl_install_plan_instance(const sxcl_install_plan *plan);
/** 有没有加载器(loader != VANILLA 且 kind 认得出)。 */
int sxcl_install_plan_has_loader(const sxcl_install_plan *plan);

/* ── 进度回调 ── */

#define SXCL_INSTALL_STATUS_MAX 192  /**< 人话状态的缓冲长度 */
#define SXCL_INSTALL_CURRENT_MAX 160 /**< "当前文件"的缓冲长度 */
#define SXCL_INSTALL_ERROR_MAX 256   /**< 人话原因的缓冲长度 */
#define SXCL_INSTALL_PATH_MAX 1024   /**< 路径缓冲长度(与 natives.h 的建议值一致) */

typedef enum sxcl_install_event {
    SXCL_INSTALL_EVENT_STAGE_BEGIN = 1,    /**< 某个阶段开始 */
    SXCL_INSTALL_EVENT_STAGE_PROGRESS = 2, /**< 阶段内进度(下载每有进展就发一次) */
    SXCL_INSTALL_EVENT_STAGE_END = 3,      /**< 某个阶段正常结束 */
    SXCL_INSTALL_EVENT_CLEANUP = 4,        /**< 正在清理未完成产物(取消/失败/收尾都会发) */
    SXCL_INSTALL_EVENT_DONE = 5            /**< 整个安装结束(成功/失败/取消都发最后这一次) */
} sxcl_install_event;

/** 一次进度事件。回调里**只读**,别持有指针(结构体在栈上,回调返回即失效)。 */
typedef struct sxcl_install_progress {
    sxcl_install_event event;
    sxcl_install_stage stage;   /**< 当前阶段(END 时是刚结束的那个;DONE 时是最后一个) */
    size_t stage_index;         /**< 在本计划阶段表里的下标(从 0 起) */
    size_t stage_total;         /**< 本计划的阶段总数(原版 8 / 含加载器 10;资源级别会影响) */
    int stage_percent;          /**< 本阶段 0..100(单调不减) */
    int percent;                /**< 整体 0..100(单调不减;结束时 = 100) */
    int64_t bytes_done;         /**< 本阶段已完成字节(含续传前已有的部分) */
    int64_t bytes_total;        /**< 本阶段总字节(期望大小之和) */
    size_t files_done;          /**< 本阶段已完成文件数(含"已存在且校验通过"跳过的) */
    size_t files_total;         /**< 本阶段总文件数 */
    size_t files_skipped;       /**< 其中:已存在且校验通过、一个字节都没下的 */
    size_t files_failed;        /**< 其中:失败的(非致命阶段也照常报) */
    char status[SXCL_INSTALL_STATUS_MAX]; /**< 一条人话状态,**永远非空** */
    char current[SXCL_INSTALL_CURRENT_MAX]; /**< 当前文件(可空串;单文件阶段是阶段名) */
} sxcl_install_progress;

/** 进度回调:同步调用,别做重活(只投递事件)。
 *  **线程**:阶段开始/结束/清理/结束事件在调用 sxcl_install_run 的那个线程里发;
 *  但**下载阶段的进度事件来自引擎的工作线程**(engine.h 的约定就是这样:回调在工作线程里触发,
 *  500ms 节流)。也就是说同一个回调可能被不同线程调用,Qt 侧请用队列连接投递到 UI 线程;
 *  编排层内部的"单调进度"也因此在那些线程上更新(只影响显示的百分比,不影响正确性)。 */
typedef void (*sxcl_install_progress_fn)(void *userdata, const sxcl_install_progress *progress);

/* ── 依赖注入:编排层要用的全部外部动作 ── */

typedef struct sxcl_install_download_stats {
    size_t files_total;    /**< 本批任务数 */
    size_t files_done;     /**< 成功(含跳过) */
    size_t files_skipped;  /**< 已存在且校验通过、没重复下载的 */
    size_t files_failed;   /**< 失败的 */
    int64_t bytes_total;   /**< 期望字节总和 */
    int64_t bytes_done;    /**< 已完成字节(含续传前已有的部分) */
} sxcl_install_download_stats;

/** 一批待下载的文件(引擎语义的任务,地址稳定,所有权归版本计划)。 */
typedef struct sxcl_install_download {
    sxcl_install_stage stage;        /**< 属于哪个阶段 */
    const char *label;               /**< 人话标签("依赖库"/"客户端 jar"…) */
    sxcl_task *const *tasks;         /**< 任务指针数组(地址稳定;钩子**不许**留着指针) */
    size_t count;
    int fatal;                       /**< 1 = 任一失败就算这个阶段失败(客户端 jar/安装器) */
    const sxcl_engine_opts *engine_opts; /**< 可空:引擎配置(真实实现要用) */
    /** 编排层的回报钩子:每有进展(含每个任务落定)调一次。
     *  task 是引擎写好的任务(可空 = 只有字节在动);index 是它在 tasks 里的下标;
     *  done = 已落定文件数(含刚落定的这个),total = count。
     *  **返回非 0 = 调用方要求停下**(取消):钩子要尽快中止并返回,让编排层去清理。 */
    int (*report)(void *userdata, const sxcl_task *task, size_t index, size_t done, size_t total);
    void *report_userdata;
    int (*is_cancelled)(void *userdata); /**< 可空:非 0 = 用户已取消 */
    void *cancel_userdata;
} sxcl_install_download;

/** 下载一批文件。返回 0 = 全部成功(含跳过);>0 = 失败文件数(部分失败);<0 = 整批没跑起来。
 *  约定:
 *   - **绝不重复下载**:目标已存在且校验通过的任务要走引擎快路径(或等价判断),不要重新下;
 *     跳过数填 stats.files_skipped,让界面能说"跳过 N 个已完好的文件";
 *   - 只有钩子**调用期间**能碰 tasks:真实实现内部建的引擎必须在返回前 destroy(引擎会一直持有
 *     任务指针,见 engine.h 的生命周期契约);
 *   - 取消时尽快返回(编排层会再看 is_cancelled 决定是不是"已取消")。 */
typedef int (*sxcl_install_download_fn)(void *userdata, const sxcl_install_download *request,
                                        sxcl_install_download_stats *stats, char *err, size_t err_len);

/** 取一段文本(版本清单)。成功返回 0 并让 *out_text 指向 malloc 的 UTF-8 文本(调用方 free)。
 *  失败返回负,err 里是人话原因。真实实现见 sxcl_install_http_get_text。 */
typedef int (*sxcl_install_fetch_fn)(void *userdata, const char *url, char **out_text, char *err,
                                     size_t err_len);

/** 装加载器(真实实现直接转发给 sxcl_loader_install)。返回 0 成功,非 0 失败。 */
typedef int (*sxcl_install_loader_fn)(void *userdata, const sxcl_loader_install_request *request,
                                      sxcl_loader_install_result *out);

/** 解压 natives(真实实现直接转发给 sxcl_natives_prepare_json)。
 *  成功时 *out_count = natives 目录里可用的原生库文件数(不知道就填 -1)。返回 0 成功。 */
typedef int (*sxcl_install_natives_fn)(void *userdata, const sxcl_json *version_json,
                                       const char *game_dir, const char *natives_dir, int *out_count,
                                       char *err, size_t err_len);

typedef struct sxcl_install_io {
    sxcl_install_fetch_fn fetch_text;         /**< 必填 */
    sxcl_install_download_fn download;        /**< 必填 */
    sxcl_install_loader_fn loader_install;    /**< 含加载器时必填 */
    sxcl_install_natives_fn natives_prepare;  /**< 必填 */
    void *userdata;                           /**< 原样回传给上面每个函数 */
} sxcl_install_io;

/** 真实现(engine.h + loader.h + natives.h + WinHTTP/Qt 传输)。可以改 userdata 后直接用,
 *  例如把 engine_opts 里没有的传输后端工厂塞进 userdata。返回的表是静态常量,不用释放。 */
const sxcl_install_io *sxcl_install_default_io(void);

/** 取文本的默认实现:用 engine_opts.transport_factory 造一个传输后端,GET 到内存。
 *  (规格 §7 缺口 4 的同一个东西;net.h 是流式的,这里给"一次性拿一段文本"的便捷入口。
 *   URL 非 http/https、连不上、状态码不是 2xx、超过 max_bytes 都会失败并写人话 err。)
 *  userdata 必须是 const sxcl_engine_opts *。 */
int sxcl_install_http_get_text(void *userdata, const char *url, char **out_text, char *err,
                               size_t err_len);

/* ── 结果 ── */

/* 返回码(负数;0 = 成功)。**失败阶段**在 result.fail_stage / fail_stage_id 里,
 * 所以这里的码只分"哪一类",给界面决定文案与是否给"重试"按钮。 */
#define SXCL_INSTALL_OK               0
#define SXCL_INSTALL_ERR_ARG        (-1)  /**< 参数不合法(必填项为空/路径太长),压根没开始装 */
#define SXCL_INSTALL_ERR_MANIFEST   (-2)  /**< 版本清单取不到 / 没这个版本 */
#define SXCL_INSTALL_ERR_VERSION    (-3)  /**< 版本 JSON 下载或解析失败 */
#define SXCL_INSTALL_ERR_CLIENT_JAR (-4)  /**< 客户端 jar 失败(必须中断,不继续后面的阶段) */
#define SXCL_INSTALL_ERR_LOADER     (-5)  /**< 加载器安装器下载或执行失败 */
#define SXCL_INSTALL_ERR_NATIVES    (-6)  /**< natives 抽取失败 */
#define SXCL_INSTALL_ERR_IO         (-7)  /**< 其余下载/文件操作失败(依赖库/资源/整理) */
#define SXCL_INSTALL_ERR_NOMEM      (-8)  /**< 内存不足 */
#define SXCL_INSTALL_ERR_CANCELLED  (-9)  /**< 用户取消(与普通失败可区分) */

/** 返回码的稳定名字(日志用)。 */
const char *sxcl_install_code_name(int code);   /**< "ok"/"arg"/"manifest"/…/"cancelled"/"unknown" */
/** 这个码对应的失败是不是"重试可能就好了"(网络类 = 1;参数/内存/natives/取消 = 0)。 */
int sxcl_install_code_retryable(int code);

typedef struct sxcl_install_result {
    int code;                        /**< 见上面的 SXCL_INSTALL_ERR_* */
    int cancelled;                   /**< 1 = 用户取消(code 此时是 ERR_CANCELLED) */
    int retryable;                   /**< 1 = 建议直接重试(网络抖动一类) */
    sxcl_install_stage fail_stage;   /**< 失败的阶段;成功时是 SXCL_INSTALL_STAGE_END */
    const char *fail_stage_id;       /**< 失败阶段的稳定字符串名(fail_stage 的投影) */
    size_t fail_stage_index;         /**< 失败阶段在本计划阶段表里的下标;没失败是 stage_total */
    size_t stages_done;              /**< 完整跑完的阶段数 */
    int percent;                     /**< 最后的整体进度(成功 = 100) */
    size_t files_skipped;            /**< 全程"已存在且校验通过"的文件数(绝不重复下载的证据) */
    size_t files_failed;             /**< 非致命失败累计(库/资源里失败的文件数) */
    int64_t bytes_done;              /**< 已下载/已确认的字节总数 */
    int natives_files;               /**< natives 就绪文件数;-1 = 没跑到/不知道 */
    char error[SXCL_INSTALL_ERROR_MAX]; /**< 人话原因(成功时空串) */
    char version_json_path[SXCL_INSTALL_PATH_MAX]; /**< 版本 JSON 落盘路径(没跑到为空串) */
    char natives_dir[SXCL_INSTALL_PATH_MAX];       /**< natives 目录(没跑到为空串) */
} sxcl_install_result;

/* ── 入口 ── */

typedef struct sxcl_install_request {
    const sxcl_install_plan *plan;      /**< 必填 */
    const sxcl_install_io *io;          /**< 可空 = sxcl_install_default_io() */
    sxcl_install_progress_fn on_progress; /**< 可空 */
    void *userdata;                     /**< 原样回传给 on_progress */
    int (*is_cancelled)(void *userdata); /**< 可空:非 0 = 用户已取消(和 engine/loader 一个语义) */
    void *cancel_userdata;               /**< 回传给 is_cancelled */
} sxcl_install_request;

/** 跑一次安装。返回 0 = 成功;否则 = out->code(负数,见 SXCL_INSTALL_ERR_*)。
 *  out 必须非空(**失败信息也写在它里面**);req/plan 里的字符串只需活到函数返回。
 *
 *  阶段推进(每个阶段都会发这三次 EVENT):
 *    STAGE_BEGIN → 若干 STAGE_PROGRESS(下载阶段按文件/字节) → STAGE_END;
 *  失败/取消时:发 CLEANUP(清理 .part 等),然后发 DONE 收尾。
 *  进度保证:stage_percent 与 percent 都**单调不减**,成功结束时 percent = 100,status 永远非空。 */
int sxcl_install_run(const sxcl_install_request *request, sxcl_install_result *out);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_INSTALL_H */
