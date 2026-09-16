/* sxcl-dl —— 纯 C 命令行下载器(引擎的端到端验收工具,不含任何 Python)。
 *
 * 用法:
 *   sxcl-dl get <url> <目标路径> [--sha1 HEX] [--size N] [--rate 5MB] [--workers N] [--mirror URL]
 *   sxcl-dl manifest <目标路径> [--rate 5MB]
 *
 * 限速值支持 "5MB" / "512kb" / "0"(不限速),解析规则与 Python 版 parse_rate 一致。
 * 退出码:0 全部成功;1 有任务失败;2 参数错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/limiter.h"
#include "sxcl/net.h"

static const char *kManifestUrl = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

static void on_progress(void *userdata, const sxcl_task *task)
{
    (void)userdata;
    const char *name = task->label ? task->label : task->dest;
    const double mb = (double)task->bytes_done / (1024.0 * 1024.0);
    const double total = (double)task->total_bytes / (1024.0 * 1024.0);
    const double speed = task->speed_bps / (1024.0 * 1024.0);
    if (task->state == SXCL_TASK_DONE) {
        printf("  [完成] %s  %.2f MB  来源#%d\n", name, mb, task->source_index + 1);
    } else if (task->state == SXCL_TASK_FAILED) {
        printf("  [失败] %s  %s\n", name, task->error);
    } else if (total > 0.0) {
        printf("  [%5.1f%%] %s  %.2f/%.2f MB  %.2f MB/s\n", mb * 100.0 / total, name, mb, total,
               speed);
    } else {
        printf("  [下载] %s  %.2f MB  %.2f MB/s\n", name, mb, speed);
    }
    fflush(stdout);
}

#if defined(SXCL_HAVE_QT_TRANSPORT)
static sxcl_transport *make_qt_transport(void *userdata)
{
    (void)userdata;
    return sxcl_transport_qt_create(); /* 每个工作线程各一个(Qt 有线程亲和性) */
}
#endif

static int usage(void)
{
    printf("sxcl-dl —— SXCL-C 下载引擎命令行前端\n"
           "  sxcl-dl get <url> <dest> [--sha1 HEX] [--size N] [--rate 5MB] [--workers N] [--mirror URL]\n"
           "  sxcl-dl manifest <dest> [--rate 5MB]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    const int is_manifest = strcmp(argv[1], "manifest") == 0;
    const int is_get = strcmp(argv[1], "get") == 0;
    if (!is_manifest && !is_get) {
        return usage();
    }
    if (is_get && argc < 4) {
        return usage();
    }
    if (is_manifest && argc < 3) {
        return usage();
    }

    const char *url = is_manifest ? kManifestUrl : argv[2];
    const char *dest = is_manifest ? argv[2] : argv[3];
    const char *sha1 = NULL;
    const char *mirror = NULL;
    int64_t size = 0;
    double rate = 0.0;
    int workers = 0;

    for (int i = is_manifest ? 3 : 4; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--sha1") == 0 && v) {
            sha1 = v;
            ++i;
        } else if (strcmp(a, "--size") == 0 && v) {
            size = (int64_t)strtoll(v, NULL, 10);
            ++i;
        } else if (strcmp(a, "--rate") == 0 && v) {
            rate = sxcl_limiter_parse_rate(v);
            ++i;
        } else if (strcmp(a, "--workers") == 0 && v) {
            workers = atoi(v);
            ++i;
        } else if (strcmp(a, "--mirror") == 0 && v) {
            mirror = v;
            ++i;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }

    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = dest;
    task.urls[0] = url;
    task.urls[1] = mirror;
    task.urls[2] = NULL;
    task.sha1 = sha1;
    task.algo = SXCL_HASH_SHA1;
    task.size = size;
    task.priority = 0;
    task.label = dest;

    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = workers;
    opts.rate_bps = rate;
    opts.retry_per_source = 2;
    opts.on_progress = on_progress;
    opts.transport_factory = NULL;

#if defined(SXCL_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap(); /* 必须主线程先建 QCoreApplication */
    opts.transport_factory = make_qt_transport;
#else
    fprintf(stderr, "本产物没有编译进任何传输后端(需要 Qt6::Network)\n");
    return 1;
#endif

    sxcl_engine *engine = sxcl_engine_create(&opts);
    if (!engine) {
        fprintf(stderr, "创建引擎失败\n");
        return 1;
    }
    if (sxcl_engine_submit(engine, &task) != 0) {
        fprintf(stderr, "任务入队失败\n");
        sxcl_engine_destroy(engine);
        return 1;
    }
    if (rate > 0.0) {
        printf("限速: %.2f MB/s   线程数: %d\n", rate / (1024.0 * 1024.0),
               workers > 0 ? workers : 4);
    }
    const double t0 = sxcl_limiter_now();
    const int failed = sxcl_engine_run(engine);
    const double elapsed = sxcl_limiter_now() - t0;
    const int64_t bytes = sxcl_engine_bytes_done(engine);
    printf("结果: %s   耗时 %.2fs   平均 %.2f MB/s   合计 %.2f MB\n",
           failed == 0 ? "成功" : "有失败", elapsed,
           elapsed > 0.0 ? ((double)bytes / elapsed) / (1024.0 * 1024.0) : 0.0,
           (double)bytes / (1024.0 * 1024.0));
    sxcl_engine_destroy(engine);
    return failed == 0 ? 0 : 1;
}
