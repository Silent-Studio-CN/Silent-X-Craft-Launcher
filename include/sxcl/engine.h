/* SXCL-C 下载引擎 —— 自研部分在此处:任务调度、续传、限速、校验、换源。
 *
 * 与传输层(net.h)的分工:传输层只负责把字节拿回来,本引擎负责
 *   - 工作线程池 + 优先级队列(清单/版本 JSON 先于库,库先于资源)
 *   - .part 断点续传(按已写字节数续起,服务端不兑现 Range 时自动改全量重下)
 *   - 全局限速(所有连接共用一个令牌桶,限速值就是整条管道上限)
 *   - 强校验:大小 + SHA-1/SHA-256,失败即换下一条候选路重下
 *   - 慢源判定:某条路持续低于阈值就换路(阈值与 Python 版一致:512KB/s / 8 秒宽限)
 *   - 进度与速度采样(500ms 一次,回调在工作线程里触发)
 *
 * 取消语义:sxcl_engine_cancel 是异步的,已入队的任务状态变 CANCELLED,随后 run() 返回。
 */
#ifndef SXCL_ENGINE_H
#define SXCL_ENGINE_H

#include <stdint.h>

#include "sxcl/hash.h"
#include "sxcl/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 慢源判定阈值(字节/秒)与宽限(秒),与 Python 版保持一致。 */
#define SXCL_MIN_SOURCE_SPEED (512 * 1024)
#define SXCL_SLOW_SOURCE_GRACE 8.0

typedef enum sxcl_task_state {
    SXCL_TASK_PENDING = 0,
    SXCL_TASK_RUNNING = 1,
    SXCL_TASK_DONE = 2,      /**< 完成且校验通过(或目标文件本就完好) */
    SXCL_TASK_FAILED = 3,
    SXCL_TASK_CANCELLED = 4
} sxcl_task_state;

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

/** 入队(不阻塞)。任务结构体的生命周期由调用方负责,必须活到 run() 返回之后。 */
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
