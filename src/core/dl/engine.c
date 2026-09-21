/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/engine.h"

#include "sxcl/fs.h"
#include "sxcl/limiter.h"
#include "sxcl/log.h"
#include "sxcl/verify.h"

#include "../internal/platform_lock.h"
#include "../internal/platform_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <unistd.h>
#endif

#define SXCL_DEFAULT_UA "SilentXCraftLauncher/1.0 (+https://github.com/Silent-Studio-CN)"
#define SXCL_READ_BLOCK ((size_t)256 * 1024)
#define SXCL_PROGRESS_INTERVAL 0.5 /* 秒 */

typedef struct sxcl_worker {
    struct sxcl_engine *engine;
    int index;
    sxcl_transport *transport;
} sxcl_worker;

/* ── 源健康表(R3)──
 *
 * 背景:某个下载源连续失败时,以前只影响**当前这个文件** —— 下一个文件还会从头再撞一次那个坏源,
 * 批量安装(几千个资源对象)时既慢又刷屏。PCL 有 SourceFail(),我们没有(全仓 blacklist/banned
 * 搜不到一个命中)。
 *
 * 这里的口径**照 PCL**:
 *   * 线程上限 ThreadLimit = Clamp(workers, 5, 30)(PCL NetTaskThreadLimit 同一套钳位);
 *   * 连续失败数 > ThreadLimit + 2 才判死(PCL 的 FailCount > ThreadLimit + 2);
 *   * 判死的源**不从候选列表里删掉**,只是在后续任务的候选顺序里排到最后 —— 保留兜底,
 *     万一别的源也挂了,它还有机会被试到;
 *   * 只要失败过(哪怕没判死),排序时也排在"零失败的源"后面。
 *
 * **只在内存里**:引擎销毁即清空,绝不落盘 —— 上次网络不好不能让用户永久失去一个下载源。
 * 粒度:挂在引擎对象上。install.c 是"每批下载建一次引擎、用完销毁"(两处),所以表的生命期 =
 * 一批任务(几百~几千个文件) —— 足够覆盖"批量安装里反复撞坏源"这件事;跨批次不保留,
 * 这是有意的(下一批重新看网络状况,不给用户留下"上次坏源"的永久记忆)。
 */
#define SXCL_SOURCE_TABLE_MAX 16   /* 候选源本来就只有几个(官方+镜像+maven 各家),16 条绰绰有余 */
#define SXCL_SOURCE_KEY_MAX 128

typedef struct sxcl_source_health {
    char key[SXCL_SOURCE_KEY_MAX]; /* URL 前缀:scheme://host[:port] */
    int fails;                     /* 连续失败数(成功一次清零) */
    int dead;                      /* 1 = 已判死(失败数超过阈值);排序时永远最后 */
} sxcl_source_health;

struct sxcl_engine {
    sxcl_engine_opts opts;
    sxcl_source_health sources[SXCL_SOURCE_TABLE_MAX];
    int source_count;
    int source_thread_limit;      /* = Clamp(workers, 5, 30);判死阈值 = 它 + 2 */
    sxcl_limiter *limiter;
    sxcl_hash_cache *cache;   /**< 可空;见 opts.cache_path */
    sxcl_lock_t lock;
    sxcl_task **tasks;
    int task_count;
    int task_capacity;
    int64_t bytes_done;
    int cancelled;
    char ua_header[192];
    const char *headers[2];
    sxcl_worker *workers;
    sxcl_thread_t *threads;
    int thread_count;
};

const char *sxcl_task_state_name(sxcl_task_state state)
{
    switch (state) {
    case SXCL_TASK_PENDING:
        return "pending";
    case SXCL_TASK_RUNNING:
        return "running";
    case SXCL_TASK_DONE:
        return "done";
    case SXCL_TASK_FAILED:
        return "failed";
    case SXCL_TASK_CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

static int default_workers(void)
{
    unsigned n = 4;
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    n = info.dwNumberOfProcessors;
#else
    long p = sysconf(_SC_NPROCESSORS_ONLN);
    if (p > 0) {
        n = (unsigned)p;
    }
#endif
    if (n == 0) {
        n = 2;
    }
    n *= 2;
    if (n > 8) {
        n = 8;
    }
    return (int)n;
}

sxcl_engine *sxcl_engine_create(const sxcl_engine_opts *opts)
{
    sxcl_engine *e = (sxcl_engine *)calloc(1, sizeof(sxcl_engine));
    if (!e) {
        return NULL;
    }
    if (opts) {
        e->opts = *opts;
    }
    if (e->opts.workers <= 0) {
        e->opts.workers = default_workers();
    }
    if (e->opts.retry_per_source <= 0) {
        e->opts.retry_per_source = 1;
    }
    snprintf(e->ua_header, sizeof(e->ua_header), "User-Agent: %s",
             e->opts.user_agent ? e->opts.user_agent : SXCL_DEFAULT_UA);
    e->headers[0] = e->ua_header;
    e->headers[1] = NULL;
    e->limiter = sxcl_limiter_create(e->opts.rate_bps);
    if (!e->limiter) {
        free(e);
        return NULL;
    }
    e->cache = sxcl_hash_cache_open(e->opts.cache_path); /* 打不开就当没有缓存,不影响下载 */
    /* 源健康表:阈值 = Clamp(workers, 5, 30) + 2(PCL 口径,见 sxcl_source_health 的注释)。
     * 表本身 calloc 出来就是空的 —— 每次 create 都是全新的一张表。 */
    e->source_thread_limit = e->opts.workers;
    if (e->source_thread_limit < 5) {
        e->source_thread_limit = 5;
    }
    if (e->source_thread_limit > 30) {
        e->source_thread_limit = 30;
    }
    sxcl_lock_init(&e->lock);
    return e;
}

void sxcl_engine_destroy(sxcl_engine *e)
{
    if (!e) {
        return;
    }
    sxcl_engine_cancel(e);
    if (e->cache) {
        sxcl_hash_cache_close(e->cache); /* 内部会原子落盘 */
        e->cache = NULL;
    }
    sxcl_limiter_destroy(e->limiter);
    sxcl_lock_destroy(&e->lock);
    free(e->tasks);
    free(e->workers);
    free(e->threads);
    free(e);
}

int sxcl_engine_submit(sxcl_engine *e, sxcl_task *task)
{
    if (!e || !task || !task->dest) {
        return -1;
    }
    sxcl_lock_acquire(&e->lock);
    if (e->task_count == e->task_capacity) {
        const int next = e->task_capacity ? e->task_capacity * 2 : 64;
        sxcl_task **grown = (sxcl_task **)realloc(e->tasks, (size_t)next * sizeof(sxcl_task *));
        if (!grown) {
            sxcl_lock_release(&e->lock);
            return -1;
        }
        e->tasks = grown;
        e->task_capacity = next;
    }
    task->state = SXCL_TASK_PENDING;
    task->bytes_done = 0;
    task->total_bytes = task->size;
    task->source_index = 0;
    task->speed_bps = 0.0;
    task->error[0] = '\0';
    if (task->algo != SXCL_HASH_SHA256) {
        task->algo = SXCL_HASH_SHA1; /* 默认与 Mojang 元数据一致 */
    }
    e->tasks[e->task_count++] = task;
    sxcl_lock_release(&e->lock);
    return 0;
}

void sxcl_engine_cancel(sxcl_engine *e)
{
    if (!e) {
        return;
    }
    e->cancelled = 1;
}

void sxcl_engine_set_rate(sxcl_engine *e, double rate_bps)
{
    if (e) {
        sxcl_limiter_set_rate(e->limiter, rate_bps);
    }
}

int64_t sxcl_engine_bytes_done(const sxcl_engine *e)
{
    return e ? e->bytes_done : 0;
}

int sxcl_engine_task_count(const sxcl_engine *e)
{
    return e ? e->task_count : 0;
}

/** 取下一个待办任务:优先级小的先做,同优先级按入队顺序(FIFO,避免大任务饿死小任务)。 */
static sxcl_task *take_next(sxcl_engine *e)
{
    sxcl_task *best = NULL;
    sxcl_lock_acquire(&e->lock);
    for (int i = 0; i < e->task_count; ++i) {
        sxcl_task *t = e->tasks[i];
        if (t->state != SXCL_TASK_PENDING) {
            continue;
        }
        if (!best || t->priority < best->priority) {
            best = t;
        }
    }
    if (best) {
        best->state = SXCL_TASK_RUNNING;
    }
    sxcl_lock_release(&e->lock);
    /* 加固:必填字段不全的任务直接判失败,绝不把 NULL/垃圾 URL 交给传输层
     * (Qt 侧 QString::fromUtf8(NULL) 会崩在 strlen) */
    if (best && (!best->dest || !best->urls[0])) {
        sxcl_lock_acquire(&e->lock);
        best->state = SXCL_TASK_FAILED;
        snprintf(best->error, sizeof(best->error), "任务字段不全(缺 dest 或 urls[0])");
        sxcl_lock_release(&e->lock);
        if (e->opts.on_progress) {
            e->opts.on_progress(e->opts.userdata, best);
        }
        return take_next(e);
    }
    return best;
}

static char *part_path_of(const char *dest)
{
    const size_t n = strlen(dest);
    char *p = (char *)malloc(n + 6);
    if (!p) {
        return NULL;
    }
    memcpy(p, dest, n);
    memcpy(p + n, ".part", 6);
    return p;
}

static int more_sources(const sxcl_task *t, int from)
{
    return (from + 1 < 4 && t->urls[from + 1] != NULL) ? 1 : 0;
}

/* 进度回调:按 0.5 秒节流,速度用两次采样之间的增量算 */
typedef struct progress_clock {
    double last_time;
    int64_t last_bytes;
} progress_clock;

static void report_progress(sxcl_engine *e, sxcl_task *t, progress_clock *pc, int force)
{
    if (!e->opts.on_progress) {
        return;
    }
    const double now = sxcl_limiter_now();
    if (!force && pc->last_time > 0.0 && (now - pc->last_time) < SXCL_PROGRESS_INTERVAL) {
        return;
    }
    const double dt = pc->last_time > 0.0 ? (now - pc->last_time) : 0.0;
    const int64_t db = t->bytes_done - pc->last_bytes;
    if (dt > 0.0) {
        t->speed_bps = (double)db / dt;
    }
    pc->last_time = now;
    pc->last_bytes = t->bytes_done;
    e->opts.on_progress(e->opts.userdata, t);
}

/* 下载的候选路日志。**级别是 debug**:一条候选路失败是一个文件的明细,装机时可能成百上千条,
 * 默认级别(INFO)只留"控制面"的网络行(见 src/core/instance/http.c 的 http_log_line)。
 * 排查"镜像挂了/某条路一直 404"时把 SXCL_LOG_LEVEL=debug 打开即可看到候选路下标与打码 URL。 */
static void engine_log_source(int level, const char *what, int src, const char *url, const char *detail)
{
    char masked[SXCL_LOG_URL_MAX + 48];
    (void)sxcl_log_mask_url((url != NULL && url[0] != '\0') ? url : "(空)", masked, sizeof(masked));
    sxcl_log_write(level, "net", "候选 #%d %s url=%s%s%s", src + 1, what, masked,
                   (detail != NULL && detail[0] != '\0') ? " | " : "",
                   (detail != NULL) ? detail : "");
}

/* ── 源健康表:读写与候选排序(R3)── */

/** URL -> 源键(scheme://host[:port])。取不到就返回空串(不记这张表)。 */
static void source_key_of(const char *url, char *out, size_t cap)
{
    out[0] = '\0';
    if (!url || !*url || cap < 8) {
        return;
    }
    const char *sep = strstr(url, "://");
    const char *host = sep ? sep + 3 : url;
    const char *end = host;
    while (*end && *end != '/' && *end != '?' && *end != '#') {
        ++end;
    }
    const size_t n = (size_t)(end - host);
    const size_t scheme = sep ? (size_t)(sep - url) + 3 : 0;
    if (n == 0 || scheme + n + 1 > cap) {
        return;
    }
    memcpy(out, url, scheme + n);
    out[scheme + n] = '\0';
}

/** 这个源的主机是不是 BMCLAPI(403/429 不算失败的判定要用)。 */
static int is_bmclapi_url(const char *url)
{
    return url && strstr(url, "bmclapi") != NULL;
}

/** 找/建一条源记录。**调用方必须已持有 e->lock**。 */
static sxcl_source_health *source_slot(sxcl_engine *e, const char *url)
{
    char key[SXCL_SOURCE_KEY_MAX];
    source_key_of(url, key, sizeof(key));
    if (key[0] == '\0') {
        return NULL;
    }
    for (int i = 0; i < e->source_count; ++i) {
        if (strcmp(e->sources[i].key, key) == 0) {
            return &e->sources[i];
        }
    }
    if (e->source_count >= SXCL_SOURCE_TABLE_MAX) {
        return NULL; /* 表满:不记了(候选源本来就那么几个,正常到不了这里) */
    }
    sxcl_source_health *slot = &e->sources[e->source_count++];
    snprintf(slot->key, sizeof(slot->key), "%s", key);
    slot->fails = 0;
    slot->dead = 0;
    return slot;
}

/** 记一次失败(连不上 / HTTP 错误状态 / 传输中断 / 校验不过)。
 *  http_status 只在"HTTP 状态码不对"时非 0 —— **BMCLAPI 的 403/429 一律不记**:
 *  它高频请求时就会这样(PCL 的同款细节),把这种限流当成"源坏了"会把一个好源拉黑。 */
static void source_note_failure(sxcl_engine *e, const char *url, int http_status)
{
    if (http_status == 403 || http_status == 429) {
        if (is_bmclapi_url(url)) {
            sxcl_log_write(SXCL_LOG_DEBUG, "net",
                           "BMCLAPI 返回 %d(高频请求限流,不算源失败:PCL 同款口径) url=%s",
                           http_status, url ? url : "(空)");
            return;
        }
    }
    sxcl_lock_acquire(&e->lock);
    sxcl_source_health *slot = source_slot(e, url);
    if (slot) {
        ++slot->fails;
        if (slot->fails > e->source_thread_limit + 2) {
            slot->dead = 1;
        }
        sxcl_log_write(SXCL_LOG_DEBUG, "net", "源失败计数 %s fails=%d dead=%d(阈值=%d)",
                       slot->key, slot->fails, slot->dead, e->source_thread_limit + 2);
    }
    sxcl_lock_release(&e->lock);
}

/** 一次成功:连续失败数清零、复活(源恢复了就该继续用它)。 */
static void source_note_success(sxcl_engine *e, const char *url)
{
    sxcl_lock_acquire(&e->lock);
    sxcl_source_health *slot = source_slot(e, url);
    if (slot && (slot->fails != 0 || slot->dead)) {
        slot->fails = 0;
        slot->dead = 0;
        sxcl_log_write(SXCL_LOG_DEBUG, "net", "源恢复 %s(失败计数清零)", slot->key);
    }
    sxcl_lock_release(&e->lock);
}

/** 这个源现在的排序权重(越小越先试):先看死没死,再看失败数。取不到记录 = 全新源(0,0)。 */
static void source_rank(sxcl_engine *e, const char *url, int *dead, int *fails)
{
    *dead = 0;
    *fails = 0;
    char key[SXCL_SOURCE_KEY_MAX];
    source_key_of(url, key, sizeof(key));
    if (key[0] == '\0') {
        return;
    }
    sxcl_lock_acquire(&e->lock);
    for (int i = 0; i < e->source_count; ++i) {
        if (strcmp(e->sources[i].key, key) == 0) {
            *dead = e->sources[i].dead;
            *fails = e->sources[i].fails;
            break;
        }
    }
    sxcl_lock_release(&e->lock);
}

/** 按源健康度排出这次任务的候选顺序(稳定:健康度相同的仍按调用方给的先后)。
 *  **判死的源不删,只排到最后** —— 别的源都不行时它还有机会被试到。 */
static int order_candidates(sxcl_engine *e, const sxcl_task *t, int *order, int cap)
{
    int n = 0;
    for (int i = 0; i < 4 && i < cap && t->urls[i] != NULL; ++i) {
        order[n++] = i;
    }
    for (int i = 1; i < n; ++i) { /* 插入排序:n 最多 4 */
        const int key = order[i];
        int dead_key = 0, fails_key = 0;
        source_rank(e, t->urls[key], &dead_key, &fails_key);
        int j = i - 1;
        while (j >= 0) {
            int dead_j = 0, fails_j = 0;
            source_rank(e, t->urls[order[j]], &dead_j, &fails_j);
            if (dead_j < dead_key || (dead_j == dead_key && fails_j <= fails_key)) {
                break;
            }
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }
    return n;
}

/* 单连接路径的 .part.json 读写(定义在分片那一段旁边,这里先声明:try_source 要用)。 */
static int load_part_written(const char *path, int64_t size, int64_t *out_written);
static int save_part_written(const char *path, int64_t size, int64_t written);

/** 试一条候选路。返回 0 = 整个文件完成并通过校验;1 = 换下一条路;-1 = 取消/致命。
 *  part_json 可空 = 不用 .part.json 记进度;续传起点只认它记下的 written=<n>(见 load_part_written)。 */
static int try_source(sxcl_worker *w, sxcl_task *t, const char *part, int src, int64_t *offset_io,
                      const char *part_json)
{
    sxcl_engine *e = w->engine;
    sxcl_transport *tr = w->transport;
    if (!tr) {
        snprintf(t->error, sizeof(t->error), "没有可用的传输后端(Qt 后端必须每线程一个)");
        return -1;
    }
    int64_t offset = *offset_io;
    progress_clock pc;
    pc.last_time = 0.0;
    pc.last_bytes = offset;

    unsigned char *buf = (unsigned char *)malloc(SXCL_READ_BLOCK);
    if (!buf) {
        snprintf(t->error, sizeof(t->error), "内存不足");
        return -1;
    }

    for (int attempt = 0; attempt <= e->opts.retry_per_source; ++attempt) {
        if (e->cancelled) {
            free(buf);
            return -1;
        }
        sxcl_http_request req;
        memset(&req, 0, sizeof(req));
        req.url = t->urls[src];
        req.method = "GET";
        req.range_start = offset > 0 ? offset : -1;
        req.range_end = -1;
        req.extra_headers = e->headers;
        req.timeout_ms = 30000;

        sxcl_http_response resp;
        sxcl_http_body *body = NULL;
        const int rc = tr->request(tr->ctx, &req, &resp, &body);
        if (rc != SXCL_NET_OK) {
            if (rc == SXCL_NET_ERR_CANCELLED) {
                free(buf);
                return -1;
            }
            if (body) {
                tr->close_body(tr->ctx, body);
            }
            source_note_failure(e, t->urls[src], 0); /* R3:这一路不行,记一笔(下一批任务排它靠后) */
            snprintf(t->error, sizeof(t->error), "连接失败(候选 #%d): %s", src + 1,
                     rc == SXCL_NET_ERR_CONNECT ? "拿不到响应" : "IO 错误");
            engine_log_source(SXCL_LOG_DEBUG, "连接失败,同一路重试", src, t->urls[src],
                              rc == SXCL_NET_ERR_CONNECT ? "拿不到响应(DNS/连接/TLS/超时)"
                                                         : "传输 IO 错误");
            continue; /* 同一路再试一次 */
        }
        if (resp.status == 416 && offset > 0) {
            /* 记录的续传起点比真实文件还长(服务端文件换了/记录与现实不符):Range 不可满足,
             * 丢掉重下。正常路径**不该**走到这里 —— 起点来自 .part.json 的 written=,
             * 不是 .part 的文件大小(那个永远等于最终大小,必 416)。 */
            tr->close_body(tr->ctx, body);
            sxcl_fs_remove(part);
            if (part_json != NULL) {
                sxcl_fs_remove(part_json);
            }
            offset = 0;
            *offset_io = 0;
            t->bytes_done = 0;
            snprintf(t->error, sizeof(t->error), "残留分片过大(416),已丢弃重下");
            continue;
        }
        if (resp.status != 200 && resp.status != 206) {
            tr->close_body(tr->ctx, body);
            /* R3:状态码不对也算这一路的失败 —— **除了 BMCLAPI 的 403/429**
             * (它高频请求就会这样,算成失败会把好源拉黑;判定在 source_note_failure 里)。 */
            source_note_failure(e, t->urls[src], resp.status);
            snprintf(t->error, sizeof(t->error), "HTTP %d(候选 #%d)", resp.status, src + 1);
            engine_log_source(SXCL_LOG_DEBUG, "状态码不对,换路", src, t->urls[src], t->error);
            return 1; /* 状态码不对:换路,重试没意义 */
        }
        t->source_index = src;
        if (resp.total_length > 0) {
            t->total_bytes = offset + (resp.total_length - offset);
            if (t->size <= 0) {
                t->total_bytes = resp.total_length;
            }
        }
        if (offset > 0) {
            const int honored = (resp.status == 206) && (resp.range_start < 0 || resp.range_start == offset);
            if (!honored) {
                /* 服务端没兑现 Range(实测 CDN 在带 gzip 时会这样):从头重下 */
                tr->close_body(tr->ctx, body);
                sxcl_fs_remove(part);
                if (part_json != NULL) {
                    sxcl_fs_remove(part_json); /* 记录作废:从哪里重新开始由下一次写盘说了算 */
                }
                offset = 0;
                *offset_io = 0;
                t->bytes_done = 0;
                snprintf(t->error, sizeof(t->error), "服务端忽略 Range,改为全量重下");
                continue;
            }
        }

        sxcl_file *file = sxcl_file_open_write(part, t->size > 0 ? t->size : -1);
        if (!file) {
            tr->close_body(tr->ctx, body);
            snprintf(t->error, sizeof(t->error), "无法写入 %s", part);
            free(buf);
            return -1;
        }

        const int64_t start_offset = offset;
        const double t_begin = sxcl_limiter_now();
        int finished = 0;
        int switch_source = 0;
        for (;;) {
            if (e->cancelled) {
                break;
            }
            const int64_t n = tr->read(tr->ctx, body, buf, SXCL_READ_BLOCK);
            if (n < 0) {
                if (n == SXCL_NET_ERR_CANCELLED) {
                    sxcl_file_close(file);
                    tr->close_body(tr->ctx, body);
                    free(buf);
                    return -1;
                }
                source_note_failure(e, t->urls[src], 0); /* R3:这一路断了,记一笔 */
                snprintf(t->error, sizeof(t->error), "传输中断(候选 #%d,%lld 字节处)", src + 1,
                         (long long)offset);
                break;
            }
            if (n == 0) {
                finished = 1;
                break;
            }
            sxcl_limiter_consume(e->limiter, (uint64_t)n);
            const int64_t wrote = sxcl_file_write_at(file, buf, (size_t)n, offset);
            if (wrote != n) {
                snprintf(t->error, sizeof(t->error), "写盘失败(偏移 %lld)", (long long)offset);
                break;
            }
            offset += n;
            *offset_io = offset;
            t->bytes_done = offset;
            sxcl_lock_acquire(&e->lock);
            e->bytes_done += n;
            sxcl_lock_release(&e->lock);
            report_progress(e, t, &pc, 0);

            const double elapsed = sxcl_limiter_now() - t_begin;
            if (elapsed > SXCL_SLOW_SOURCE_GRACE && more_sources(t, src)) {
                const double avg = (double)(offset - start_offset) / elapsed;
                if (avg < (double)SXCL_MIN_SOURCE_SPEED) {
                    switch_source = 1;
                    break;
                }
            }
        }

        sxcl_file_flush(file);
        sxcl_file_close(file);
        tr->close_body(tr->ctx, body);
        /* 把"写到哪了"落盘:这是单连接路径**唯一**的续传起点来源(.part 被预分配到最终大小,
         * 文件大小永远等于 t->size,拿它当起点会拼出 Range: bytes=<final>- -> 416 -> 删片重下)。 */
        if (part_json != NULL && offset > 0) {
            (void)save_part_written(part_json, t->size, offset);
        }
        report_progress(e, t, &pc, 1);

        if (e->cancelled) {
            free(buf);
            return -1;
        }
        if (switch_source) {
            snprintf(t->error, sizeof(t->error), "候选 #%d 太慢(<%dKB/s),换路继续", src + 1,
                     SXCL_MIN_SOURCE_SPEED / 1024);
            engine_log_source(SXCL_LOG_DEBUG, "太慢,换路续传", src, t->urls[src], t->error);
            free(buf);
            return 1; /* 保留已下部分,换路续传 */
        }
        if (!finished) {
            continue; /* 传输中断:同一路重试,从头接着 offset 续 */
        }

        /* 读完 → 强校验(大小 + 摘要)。续传过的文件无法边下边算,这里统一读回校验 */
        sxcl_verify_result vr;
        const sxcl_verify_status st = sxcl_verify_file_cached(part, t->size, t->sha1, t->algo,
                                                              e->cache, &vr);
        if (st == SXCL_VERIFY_OK) {
            source_note_success(e, t->urls[src]); /* R3:这一路给的内容是对的,失败计数清零 */
            if (sxcl_fs_rename_replace(part, t->dest) == 0) {
                /* 目标就位 -> 进度记录随之作废(留着会让下次从"已经写满"的旧记录续传) */
                if (part_json != NULL) {
                    sxcl_fs_remove(part_json);
                }
                /* 把 .part 的校验结果按"改名后的路径 + 对应大小/修改时间"登记进缓存:
                 * 改名不改内容与时间戳,下次运行就能直接命中,不必重算这几百 MB */
                if (e->cache && vr.actual_hex[0] != '\0') {
                    int64_t fsize = 0;
                    int64_t fmtime = 0;
                    if (sxcl_fs_stat(t->dest, &fsize, &fmtime) == 0) {
                        sxcl_hash_cache_put(e->cache, t->dest, fsize, fmtime, vr.actual_hex);
                    }
                }
                t->bytes_done = offset;
                if (t->total_bytes <= 0) {
                    t->total_bytes = offset;
                }
                t->speed_bps = 0.0;
                snprintf(t->error, sizeof(t->error), "%s", "ok");
                free(buf);
                return 0;
            }
            snprintf(t->error, sizeof(t->error), "改名到目标失败: %s", t->dest);
            free(buf);
            return -1;
        }
        source_note_failure(e, t->urls[src], 0); /* R3:这一路给的内容是坏的,记一笔 */
        snprintf(t->error, sizeof(t->error), "校验失败(%s,候选 #%d),重下",
                 sxcl_verify_status_name(st), src + 1);
        engine_log_source(SXCL_LOG_DEBUG, "校验失败,重下", src, t->urls[src], t->error);
        sxcl_fs_remove(part);
        if (part_json != NULL) {
            sxcl_fs_remove(part_json);
        }
        offset = 0;
        *offset_io = 0;
        t->bytes_done = 0;
        free(buf);
        return 1;
    }

    free(buf);
    return 1;
}

/* ── 多连接分片:大文件按 Range 切片并发,每片一个线程,写入同一 .part 的不同偏移 ── */

typedef struct sxcl_segment {
    struct sxcl_engine *engine;
    sxcl_task *task;
    const char *part; /* 所有分片写同一个 .part,靠绝对偏移互不干扰 */
    const char *url;
    int64_t start;
    int64_t end;    /* 闭区间 */
    int64_t cursor; /* 该片下一个要写的绝对偏移 */
    int index;
    int rc; /* 0 = 成功,-1 = 失败 */
    char error[128];
} sxcl_segment;

static char *part_json_path(const char *part)
{
    const size_t n = strlen(part);
    char *p = (char *)malloc(n + 6);
    if (!p) {
        return NULL;
    }
    memcpy(p, part, n);
    memcpy(p + n, ".json", 6);
    return p;
}

/* .part.json:极简行式,记录每个分片已完成的游标,便于续传 */
static int load_part_json(const char *path, int64_t size, sxcl_segment *segs, int count)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return 0; /* 没有文件 = 全新下载 */
    }
    char line[256];
    int64_t file_size = -1;
    int matched = 0;
    while (fgets(line, (int)sizeof(line), fh)) {
        if (strncmp(line, "size=", 5) == 0) {
            file_size = (int64_t)strtoll(line + 5, NULL, 10);
            continue;
        }
        int idx = 0;
        long long st = 0, en = 0, cur = 0;
        if (sscanf(line, "seg %d %lld %lld %lld", &idx, &st, &en, &cur) == 4) {
            if (idx >= 0 && idx < count && segs[idx].start == st && segs[idx].end == en &&
                cur >= st && cur <= en + 1) {
                segs[idx].cursor = cur;
                ++matched;
            }
        }
    }
    fclose(fh);
    if (file_size != size || matched == 0) {
        return 0; /* 文件变了或布局对不上:当作全新下载 */
    }
    return 1;
}

static int save_part_json(const char *path, int64_t size, const sxcl_segment *segs, int count)
{
    const size_t len = strlen(path);
    char *tmp = (char *)malloc(len + 5);
    if (!tmp) {
        return -1;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, ".tmp", 5);
    int rc = -1;
    if (sxcl_fs_mkdirs_for_file(path) == 0) {
        FILE *fh = fopen(tmp, "wb");
        if (fh) {
            rc = 0;
            fprintf(fh, "sxcl-part 1\nsize=%lld\n", (long long)size);
            for (int i = 0; i < count; ++i) {
                fprintf(fh, "seg %d %lld %lld %lld\n", i, (long long)segs[i].start,
                        (long long)segs[i].end, (long long)segs[i].cursor);
            }
            if (fclose(fh) != 0) {
                rc = -1;
            }
            if (rc == 0 && sxcl_fs_rename_replace(tmp, path) != 0) {
                rc = -1;
            }
        }
    }
    if (rc != 0) {
        sxcl_fs_remove(tmp);
    }
    free(tmp);
    return rc;
}

/* ── 单连接路径的续传起点:也记进同一个 <dest>.part.json ──
 *
 *  **为什么不能用文件大小当起点**:.part 会被 sxcl_file_open_write(part, t->size) 预分配到
 *  最终大小(platform_win32.c:299-307 / platform_posix.c:191-194),所以"文件有多大"永远是
 *  最终大小 —— 照它拼出来的是 Range: bytes=<final>- ,服务端必然回 416,残片被整块删掉重下。
 *  起点只认这里显式记下的 written=<n>。 */
static int load_part_written(const char *path, int64_t size, int64_t *out_written)
{
    if (out_written) {
        *out_written = 0;
    }
    if (!path || size <= 0) {
        return 0; /* 不知道目标多大:没法判断记录还作不作数,当全新下载 */
    }
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return 0;
    }
    char line[256];
    int64_t file_size = -1;
    int64_t written = -1;
    while (fgets(line, (int)sizeof(line), fh)) {
        if (strncmp(line, "size=", 5) == 0) {
            file_size = (int64_t)strtoll(line + 5, NULL, 10);
        } else if (strncmp(line, "written=", 8) == 0) {
            written = (int64_t)strtoll(line + 8, NULL, 10);
        }
    }
    fclose(fh);
    if (file_size != size || written <= 0 || written >= size) {
        return 0; /* 文件变了 / 没有记录 / 已经写满:一律当全新下载(绝不凭猜续传) */
    }
    if (out_written) {
        *out_written = written;
    }
    return 1;
}

/** 记下"单连接已经写到哪了"。与分片版同一个文件格式,只多一行 written=。 */
static int save_part_written(const char *path, int64_t size, int64_t written)
{
    if (!path || size <= 0 || written <= 0) {
        return -1;
    }
    const size_t len = strlen(path);
    char *tmp = (char *)malloc(len + 5);
    if (!tmp) {
        return -1;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, ".tmp", 5);
    int rc = -1;
    if (sxcl_fs_mkdirs_for_file(path) == 0) {
        FILE *fh = fopen(tmp, "wb");
        if (fh) {
            rc = 0;
            fprintf(fh, "sxcl-part 1\nsize=%lld\nwritten=%lld\n", (long long)size,
                    (long long)written);
            if (fclose(fh) != 0) {
                rc = -1;
            }
            if (rc == 0 && sxcl_fs_rename_replace(tmp, path) != 0) {
                rc = -1;
            }
        }
    }
    if (rc != 0) {
        sxcl_fs_remove(tmp);
    }
    free(tmp);
    return rc;
}

SXCL_THREAD_FN(segment_main)
{
    sxcl_segment *seg = (sxcl_segment *)arg;
    sxcl_engine *e = seg->engine;
    sxcl_transport *tr = e->opts.transport_factory ? e->opts.transport_factory(e->opts.userdata) : NULL;
    if (!tr) {
        snprintf(seg->error, sizeof(seg->error), "没有可用的传输后端");
        seg->rc = -1;
        SXCL_THREAD_RETURN(-1);
    }
    unsigned char *buf = (unsigned char *)malloc(SXCL_READ_BLOCK);
    sxcl_file *file = sxcl_file_open_write(seg->part, seg->task->size);
    if (!buf || !file) {
        snprintf(seg->error, sizeof(seg->error), "内存/文件不可用");
        seg->rc = -1;
        free(buf);
        tr->destroy(tr->ctx);
        SXCL_THREAD_RETURN(-1);
    }

    sxcl_http_request req;
    memset(&req, 0, sizeof(req));
    req.url = seg->url;
    req.method = "GET";
    req.range_start = seg->cursor;
    req.range_end = seg->end;
    req.extra_headers = e->headers;
    req.timeout_ms = 30000;

    sxcl_http_response resp;
    sxcl_http_body *body = NULL;
    const int rc = tr->request(tr->ctx, &req, &resp, &body);
    if (rc != SXCL_NET_OK) {
        snprintf(seg->error, sizeof(seg->error), "候选 #1 连接失败");
        seg->rc = -1;
    } else if (resp.status != 206 && !(seg->cursor == seg->start && resp.status == 200)) {
        snprintf(seg->error, sizeof(seg->error), "HTTP %d(分片需要 206)", resp.status);
        seg->rc = -1;
    } else {
        seg->rc = 0;
        for (;;) {
            if (e->cancelled) {
                seg->rc = -1;
                snprintf(seg->error, sizeof(seg->error), "已取消");
                break;
            }
            const int64_t n = tr->read(tr->ctx, body, buf, SXCL_READ_BLOCK);
            if (n < 0) {
                seg->rc = -1;
                snprintf(seg->error, sizeof(seg->error), "传输中断(片 #%d,%lld 处)", seg->index + 1,
                         (long long)seg->cursor);
                break;
            }
            if (n == 0) {
                break; /* 本片读完 */
            }
            sxcl_limiter_consume(e->limiter, (uint64_t)n);
            const int64_t wrote = sxcl_file_write_at(file, buf, (size_t)n, seg->cursor);
            if (wrote != n) {
                seg->rc = -1;
                snprintf(seg->error, sizeof(seg->error), "写盘失败(片 #%d)", seg->index + 1);
                break;
            }
            seg->cursor += n;
            sxcl_lock_acquire(&e->lock);
            seg->task->bytes_done += n;
            e->bytes_done += n;
            sxcl_lock_release(&e->lock);
        }
        if (seg->rc == 0 && seg->cursor != seg->end + 1) {
            seg->rc = -1;
            snprintf(seg->error, sizeof(seg->error), "片 #%d 字节数不足", seg->index + 1);
        }
    }
    if (body) {
        tr->close_body(tr->ctx, body);
    }
    sxcl_file_close(file);
    free(buf);
    tr->destroy(tr->ctx);
    SXCL_THREAD_RETURN(seg->rc);
}
/** 多连接分片下载。成功返回 0(已校验并落位);失败返回 -1(残片与 .part.json 留给下次续传)。 */
static int segmented_download(sxcl_engine *e, sxcl_task *t, const char *part, const char *url)
{
    if (!url || t->size < SXCL_SEGMENT_MIN_SIZE) {
        return -1;
    }
    int n = (int)(t->size / SXCL_SEGMENT_MIN_PART);
    if (n > e->opts.max_conn_per_file) {
        n = e->opts.max_conn_per_file;
    }
    if (n < 2) {
        return -1;
    }
    sxcl_segment *segs = (sxcl_segment *)calloc((size_t)n, sizeof(sxcl_segment));
    if (!segs) {
        return -1;
    }
    const int64_t part_size = t->size / n;
    for (int i = 0; i < n; ++i) {
        segs[i].engine = e;
        segs[i].task = t;
        segs[i].part = part;
        segs[i].url = url;
        segs[i].index = i;
        segs[i].start = (int64_t)i * part_size;
        segs[i].end = (i == n - 1) ? (t->size - 1) : ((int64_t)(i + 1) * part_size - 1);
        segs[i].cursor = segs[i].start;
    }
    char *pj = part_json_path(part);
    if (pj) {
        (void)load_part_json(pj, t->size, segs, n);
    }
    sxcl_file *probe = sxcl_file_open_write(part, t->size);
    if (!probe) {
        free(pj);
        free(segs);
        return -1;
    }
    sxcl_file_close(probe);

    int64_t done = 0;
    for (int i = 0; i < n; ++i) {
        done += segs[i].cursor - segs[i].start;
    }
    t->bytes_done = done;

    sxcl_thread_t *threads = (sxcl_thread_t *)calloc((size_t)n, sizeof(sxcl_thread_t));
    int started = 0;
    if (threads) {
        for (int i = 0; i < n; ++i) {
            if (sxcl_thread_start(&threads[i], segment_main, &segs[i]) != 0) {
                break;
            }
            ++started;
        }
        for (int i = 0; i < started; ++i) {
            sxcl_thread_join(threads[i]);
        }
    }
    int ok = (threads != NULL && started == n);
    done = 0;
    for (int i = 0; i < n; ++i) {
        if (segs[i].rc != 0) {
            ok = 0;
        }
        done += segs[i].cursor - segs[i].start;
    }
    free(threads);
    t->bytes_done = done;

    if (!ok) {
        if (pj) {
            (void)save_part_json(pj, t->size, segs, n);
        }
        const char *why = "分片下载失败";
        for (int i = 0; i < n; ++i) {
            if (segs[i].rc != 0 && segs[i].error[0]) {
                why = segs[i].error;
                break;
            }
        }
        snprintf(t->error, sizeof(t->error), "%s", why);
        free(pj);
        free(segs);
        return -1;
    }

    sxcl_verify_result vr;
    const sxcl_verify_status st = sxcl_verify_file_cached(part, t->size, t->sha1, t->algo, e->cache, &vr);
    if (st != SXCL_VERIFY_OK) {
        snprintf(t->error, sizeof(t->error), "校验失败(%s)", sxcl_verify_status_name(st));
        sxcl_fs_remove(part);
        if (pj) {
            sxcl_fs_remove(pj);
        }
        free(pj);
        free(segs);
        return -1;
    }
    if (sxcl_fs_rename_replace(part, t->dest) != 0) {
        snprintf(t->error, sizeof(t->error), "改名到目标失败: %s", t->dest);
        free(pj);
        free(segs);
        return -1;
    }
    if (e->cache && vr.actual_hex[0] != '\0') {
        int64_t fsize = 0, fmtime = 0;
        if (sxcl_fs_stat(t->dest, &fsize, &fmtime) == 0) {
            sxcl_hash_cache_put(e->cache, t->dest, fsize, fmtime, vr.actual_hex);
        }
    }
    if (pj) {
        sxcl_fs_remove(pj);
    }
    t->bytes_done = t->size;
    t->total_bytes = t->size;
    t->source_index = 0;
    snprintf(t->error, sizeof(t->error), "%s", "ok");
    free(pj);
    free(segs);
    return 0;
}

/** 这个任务有没有"已存在且完好"的判定凭据(见 engine.h 的 sxcl_task_verify_state)。 */
static sxcl_task_verify_state task_verify_state(const sxcl_task *t)
{
    if (t->sha1 != NULL && t->sha1[0] != '\0') {
        return SXCL_TASK_VERIFY_HASH;
    }
    if (t->size > 0) {
        return SXCL_TASK_VERIFY_SIZE;
    }
    return SXCL_TASK_VERIFY_NONE; /* 没大小也没摘要:没有任何凭据 */
}

static int run_task(sxcl_worker *w, sxcl_task *t)
{
    sxcl_engine *e = w->engine;
    t->verify_state = task_verify_state(t);
    t->skipped_existing = 0;
    t->resume_from = 0;
    /* 快路径:目标文件已存在且校验通过(有缓存时走查表,不重读文件)。
     * **无校验信息**的任务一律不走这里:否则磁盘上任何同名残留(加载器依赖库就是 size=0/无哈希)
     * 都会被算成"已存在且校验通过",一个字节都不下(实测事故)。 */
    if (t->verify_state != SXCL_TASK_VERIFY_NONE &&
        sxcl_verify_file_cached(t->dest, t->size, t->sha1, t->algo, e->cache, NULL) == SXCL_VERIFY_OK) {
        t->bytes_done = t->size > 0 ? t->size : 0;
        t->total_bytes = t->bytes_done;
        t->skipped_existing = 1;
        snprintf(t->error, sizeof(t->error), "%s", "已存在且校验通过");
        return 0;
    }
    char *part = part_path_of(t->dest);
    if (!part) {
        snprintf(t->error, sizeof(t->error), "内存不足");
        return -1;
    }
    if (sxcl_fs_mkdirs_for_file(t->dest) != 0) {
        snprintf(t->error, sizeof(t->error), "无法创建目录: %s", t->dest);
        free(part);
        return -1;
    }
    char *pj = part_json_path(part);
    /* 续传起点 = .part.json 里显式记下的 written=<n>,**不是** .part 的文件大小
     * (预分配到最终大小 -> 拿它当起点必 416 -> 删片重下;见 load_part_written)。 */
    int64_t offset = 0;
    if (pj != NULL && !load_part_written(pj, t->size, &offset)) {
        offset = 0;
    }
    t->bytes_done = offset;
    t->resume_from = offset;

    /* R3:候选顺序按源健康度重排(坏源/判死的源排最后,但**不删掉** —— 保留兜底)。
     * 同一批安装里几千个文件,坏源只在第一个文件上撞一次,后面的文件直接先走好源。 */
    int order[4];
    const int order_count = order_candidates(e, t, order, 4);

    /* 大文件先试多连接分片:成功就直接结束;不满足前提或失败就清掉残片退回单连接 */
    if (t->size >= SXCL_SEGMENT_MIN_SIZE && e->opts.max_conn_per_file > 1) {
        const char *seg_url = order_count > 0 ? t->urls[order[0]] : t->urls[0];
        if (segmented_download(e, t, part, seg_url) == 0) {
            free(pj);
            free(part);
            return 0;
        }
        /* 分片没成:残片与分片记录都清掉,退回单连接从头下(与以前一致) */
        if (pj != NULL) {
            sxcl_fs_remove(pj);
        }
        sxcl_fs_remove(part);
        offset = 0;
        t->bytes_done = 0;
        t->resume_from = 0;
    }

    int rc = 1;
    for (int k = 0; k < order_count; ++k) {
        rc = try_source(w, t, part, order[k], &offset, pj);
        if (rc == 0 || rc == -1) {
            break;
        }
    }
    free(pj);
    free(part);
    return rc == 0 ? 0 : -1;
}

SXCL_THREAD_FN(worker_main)
{
    sxcl_worker *w = (sxcl_worker *)arg;
    sxcl_engine *e = w->engine;
    /* 传输后端必须在它自己的线程里创建:
     * QNetworkAccessManager 有线程亲和性,主线程建的实例拿到工作线程用会直接报
     * "Cannot create children for a parent that is in a different thread"。 */
    w->transport = e->opts.transport_factory ? e->opts.transport_factory(e->opts.userdata) : NULL;
    for (;;) {
        if (e->cancelled) {
            break;
        }
        sxcl_task *t = take_next(e);
        if (!t) {
            break;
        }
        const int rc = run_task(w, t);
        sxcl_lock_acquire(&e->lock);
        if (e->cancelled && rc != 0) {
            t->state = SXCL_TASK_CANCELLED;
        } else {
            t->state = rc == 0 ? SXCL_TASK_DONE : SXCL_TASK_FAILED;
        }
        sxcl_lock_release(&e->lock);
        if (e->opts.on_progress) {
            e->opts.on_progress(e->opts.userdata, t);
        }
    }
    if (w->transport) {
        w->transport->destroy(w->transport->ctx);
        w->transport = NULL;
    }
    SXCL_THREAD_RETURN(0);
}

int sxcl_engine_run(sxcl_engine *e)
{
    if (!e) {
        return -1;
    }
    if (e->thread_count > 0) {
        return -1; /* 不重入 */
    }
    const int workers = e->opts.workers > 0 ? e->opts.workers : 1;
    e->workers = (sxcl_worker *)calloc((size_t)workers, sizeof(sxcl_worker));
    e->threads = (sxcl_thread_t *)calloc((size_t)workers, sizeof(sxcl_thread_t));
    if (!e->workers || !e->threads) {
        free(e->workers);
        free(e->threads);
        e->workers = NULL;
        e->threads = NULL;
        return -1;
    }
    for (int i = 0; i < workers; ++i) {
        e->workers[i].engine = e;
        e->workers[i].index = i;
        e->workers[i].transport = NULL; /* 由各线程自己创建,见 worker_main */
    }
    e->thread_count = workers;
    int started = 0;
    for (int i = 0; i < workers; ++i) {
        if (sxcl_thread_start(&e->threads[i], worker_main, &e->workers[i]) != 0) {
            break;
        }
        ++started;
    }
    for (int i = 0; i < started; ++i) {
        sxcl_thread_join(e->threads[i]);
    }
    free(e->workers);
    free(e->threads);
    e->workers = NULL;
    e->threads = NULL;
    e->thread_count = 0;

    int failed = 0;
    for (int i = 0; i < e->task_count; ++i) {
        if (e->tasks[i]->state != SXCL_TASK_DONE) {
            ++failed;
        }
    }
    return failed;
}
