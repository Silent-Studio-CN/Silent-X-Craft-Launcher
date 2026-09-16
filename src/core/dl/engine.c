#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/engine.h"

#include "sxcl/fs.h"
#include "sxcl/limiter.h"
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

struct sxcl_engine {
    sxcl_engine_opts opts;
    sxcl_limiter *limiter;
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
    sxcl_lock_init(&e->lock);
    return e;
}

void sxcl_engine_destroy(sxcl_engine *e)
{
    if (!e) {
        return;
    }
    sxcl_engine_cancel(e);
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

/** 试一条候选路。返回 0 = 整个文件完成并通过校验;1 = 换下一条路;-1 = 取消/致命。 */
static int try_source(sxcl_worker *w, sxcl_task *t, const char *part, int src, int64_t *offset_io)
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
            snprintf(t->error, sizeof(t->error), "连接失败(候选 #%d): %s", src + 1,
                     rc == SXCL_NET_ERR_CONNECT ? "拿不到响应" : "IO 错误");
            continue; /* 同一路再试一次 */
        }
        if (resp.status != 200 && resp.status != 206) {
            tr->close_body(tr->ctx, body);
            snprintf(t->error, sizeof(t->error), "HTTP %d(候选 #%d)", resp.status, src + 1);
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
        report_progress(e, t, &pc, 1);

        if (e->cancelled) {
            free(buf);
            return -1;
        }
        if (switch_source) {
            snprintf(t->error, sizeof(t->error), "候选 #%d 太慢(<%dKB/s),换路继续", src + 1,
                     SXCL_MIN_SOURCE_SPEED / 1024);
            free(buf);
            return 1; /* 保留已下部分,换路续传 */
        }
        if (!finished) {
            continue; /* 传输中断:同一路重试,从头接着 offset 续 */
        }

        /* 读完 → 强校验(大小 + 摘要)。续传过的文件无法边下边算,这里统一读回校验 */
        sxcl_verify_result vr;
        const sxcl_verify_status st = sxcl_verify_file(part, t->size, t->sha1, t->algo, &vr);
        if (st == SXCL_VERIFY_OK) {
            if (sxcl_fs_rename_replace(part, t->dest) == 0) {
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
        snprintf(t->error, sizeof(t->error), "校验失败(%s,候选 #%d),重下",
                 sxcl_verify_status_name(st), src + 1);
        sxcl_fs_remove(part);
        offset = 0;
        *offset_io = 0;
        t->bytes_done = 0;
        free(buf);
        return 1;
    }

    free(buf);
    return 1;
}

static int run_task(sxcl_worker *w, sxcl_task *t)
{
    if (sxcl_verify_file(t->dest, t->size, t->sha1, t->algo, NULL) == SXCL_VERIFY_OK) {
        t->bytes_done = t->size > 0 ? t->size : 0;
        t->total_bytes = t->bytes_done;
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
    int64_t offset = 0;
    if (sxcl_fs_stat(part, &offset, NULL) != 0) {
        offset = 0; /* 没有半成品 → 从头下 */
    }
    t->bytes_done = offset;

    int rc = 1;
    for (int src = 0; src < 4 && t->urls[src]; ++src) {
        rc = try_source(w, t, part, src, &offset);
        if (rc == 0 || rc == -1) {
            break;
        }
    }
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
