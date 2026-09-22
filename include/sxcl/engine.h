/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_ENGINE_H
#define SXCL_ENGINE_H

#include <stdint.h>

#include "sxcl/hash.h"
#include "sxcl/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单文件分片的门槛:小于这个大小不分片(分片的握手开销比省下的时间还多)。 */
#define SXCL_SEGMENT_MIN_SIZE ((int64_t)4 * 1024 * 1024)
/** 每片的最小字节数:片太小会让连接数虚高、尾片拖尾。 */
#define SXCL_SEGMENT_MIN_PART ((int64_t)1 * 1024 * 1024)

/** 慢源判定阈值(字节/秒)与宽限(秒),与 Python 版保持一致。 */
#define SXCL_MIN_SOURCE_SPEED (512 * 1024)
#define SXCL_SLOW_SOURCE_GRACE 8.0
/** 候选来源最多走**几轮**。一轮 = 把候选按健康度排一遍、逐个试。
 *  为什么要多轮:慢源判定会为"更快的备选"掉头,而备选可能根本是死的(实测:Quilt 官方
 *  maven 只有 32KB/s,备选镜像没有 Quilt -> 只走一轮就等于"把唯一能出数据的源赶走,然后全失败")。
 *  现在一轮结束**只要有进展**就再来一轮(每轮按最新死活记录重排),慢源也能一轮一轮磨完;
 *  一轮下来一个字节都没进展才认输。 */
#define SXCL_SOURCE_ROUNDS 8

typedef enum sxcl_task_state {
    SXCL_TASK_PENDING = 0,
    SXCL_TASK_RUNNING = 1,
    SXCL_TASK_DONE = 2,      /**< 完成且校验通过(或目标文件本就完好) */
    SXCL_TASK_FAILED = 3,
    SXCL_TASK_CANCELLED = 4
} sxcl_task_state;

/** 这个任务**有没有**可以用来判断"目标文件已存在且完好"的凭据(引擎写,调用方只读)。
 *
 *  为什么要有这个显式状态:verify.c 的退化分支是"期望尺寸 <= 0 且没有摘要 -> 文件存在就算过",
 *  而加载器依赖库正是 size=0/无哈希建的 —— 于是磁盘上任何同名残留都会被算成
 *  "已存在且校验通过",一个字节都不下(实测事故)。引擎据此**不做**那条快路径。 */
typedef enum sxcl_task_verify_state {
    SXCL_TASK_VERIFY_UNKNOWN = 0, /**< 还没判过(任务还没被引擎处理) */
    SXCL_TASK_VERIFY_NONE = 1,    /**< 既没期望大小也没摘要 = **无校验信息**,不许当"已存在" */
    SXCL_TASK_VERIFY_SIZE = 2,    /**< 只有期望大小(弱校验) */
    SXCL_TASK_VERIFY_HASH = 3     /**< 有摘要(强校验) */
} sxcl_task_verify_state;

/** 一个待下载文件。输入字段由调用方填,输出字段由引擎写。 */
typedef struct sxcl_task {
    /* ── 输入 ── */
    const char *dest;      /**< 目标路径(UTF-8)。引擎会在同目录用 <dest>.part 作临时文件 */
    const char *urls[4];   /**< 候选来源,官方在前镜像在后,NULL 结尾 */
    const char *sha1;      /**< 期望摘要十六进制(可空:空则只校验大小) */
    sxcl_hash_algo algo;   /**< 摘要算法(SHA-1 对应 Mojang 元数据) */
    int64_t size;          /**< 期望大小(0 = 未知) */
    int priority;          /**< 越小越先做:元数据 0 / 依赖库 10 / 资源 20 */
    const char *label;     /**< 进度显示名(可空,空则用 dest 的 basename) */

    /* ── 输出(引擎写,调用方只读) ── */
    sxcl_task_state state;
    sxcl_task_verify_state verify_state; /**< 校验信息状态(见上);NONE = 不走"已存在"快路径 */
    int skipped_existing;  /**< 1 = 一个字节都没下,命中了"已存在且校验通过"快路径。
                            *   **统计用这个结构化字段**,不要再去比 error 里的中文文案
                            *  (改一次文案就会静默把统计打回 0)。 */
    int64_t resume_from;   /**< 本次续传的起点字节数(显式记录,不是文件大小);全新下载 = 0 */
    int64_t bytes_done;    /**< 已写入字节数(含续传前已有的部分) */
    int64_t total_bytes;   /**< 总字节数(期望大小或 Content-Range 得到) */
    int source_index;      /**< 实际成功的候选索引 */
    double speed_bps;      /**< 最近一次采样的速度 */
    char error[160];       /**< 失败原因(人话,可直接显示) */
} sxcl_task;

typedef struct sxcl_engine sxcl_engine;

typedef struct sxcl_engine_opts {
    int workers;            /**< 工作线程数;<=0 自动取 min(8, CPU*2) */
    double rate_bps;        /**< 全局限速(字节/秒);0 = 不限速 */
    const char *user_agent; /**< User-Agent(可空,用默认值) */
    int retry_per_source;   /**< 每条候选路的额外重试次数;<=0 视为 1 */
    /** 单文件最大连接数:<=1 表示不分片(默认 1);>1 时大文件按 Range 切片并发下载。
     *  只有"时长足够长"的大文件才值得分片(小文件分片反而更慢),阈值见 SXCL_SEGMENT_MIN_SIZE。 */
    int max_conn_per_file;
    /** 哈希缓存文件路径(可空 = 不用缓存)。有缓存时重复校验走查表:
     *  启动器第二次运行要核对 5000+ 个资源文件,不缓存就得把几百 MB 重新读一遍。 */
    const char *cache_path;
    /** 为每个工作线程创建一个传输后端(Qt 后端有线程亲和性,必须每线程一个)。
     *  返回 NULL 视为该线程不可用,对应任务会失败并给出原因。 */
    sxcl_transport *(*transport_factory)(void *userdata);
    void *userdata;
    /** 进度回调:从工作线程调用,实现里别做重活(只投递事件)。 */
    void (*on_progress)(void *userdata, const sxcl_task *task);
} sxcl_engine_opts;

/** 创建引擎。opts 可为 NULL(默认配置),但那样没有传输后端,任务会全部失败。 */
sxcl_engine *sxcl_engine_create(const sxcl_engine_opts *opts);
void sxcl_engine_destroy(sxcl_engine *engine);

/** 入队(不阻塞)。
 *
 *  生命周期契约(踩过坑,别用栈变量):任务结构体必须活到 `sxcl_engine_destroy` 之后,
 *  而不是"活到 run() 返回" —— 引擎会一直持有指针,且 **run() 可以多次调用**
 *  (例如先下版本 JSON,再展开资源对象)。用栈上局部变量传进来,函数返回后地址被复用,
 *  第二次 run() 会把它当成待办任务,拿着垃圾 URL 去请求(实测崩在 Qt 的 strlen)。 */
int sxcl_engine_submit(sxcl_engine *engine, sxcl_task *task);

/** 跑完队列(阻塞)。返回 0 表示全部成功,>0 表示失败任务数,<0 参数错误。 */
int sxcl_engine_run(sxcl_engine *engine);

/** 请求取消:所有传输中止,未开始的任务标记 CANCELLED。 */
void sxcl_engine_cancel(sxcl_engine *engine);

/** 运行时改限速(即时生效)。 */
void sxcl_engine_set_rate(sxcl_engine *engine, double rate_bps);

/** 队列里所有任务的累计已下载字节。 */
int64_t sxcl_engine_bytes_done(const sxcl_engine *engine);

/** 入队任务数(含已完成)。 */
int sxcl_engine_task_count(const sxcl_engine *engine);

/** 任务状态名(日志用)。 */
const char *sxcl_task_state_name(sxcl_task_state state);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_ENGINE_H */
