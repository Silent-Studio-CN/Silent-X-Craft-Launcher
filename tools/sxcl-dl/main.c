/* sxcl-dl —— 纯 C 命令行下载器(引擎的端到端验收工具,不含任何 Python)。
 *
 * 用法:
 *   sxcl-dl get <url> <目标路径> [--sha1 HEX] [--size N] [--rate 5MB] [--workers N] [--mirror URL]
 *   sxcl-dl manifest <目标路径> [--rate 5MB]
 *   sxcl-dl version <版本号|latest> <游戏目录> [--rate 5MB] [--workers N] [--verbose]
 *       └ 拉取该版本的客户端 jar + 全部依赖库 + 资源索引,每个文件强校验 SHA-1
 *   sxcl-dl list [--limit N]      列出官方版本清单(取 latest 与前 N 个)
 *
 * 限速值支持 "5MB" / "512kb" / "0"(不限速),解析规则与 Python 版 parse_rate 一致。
 * 退出码:0 全部成功;1 有任务失败;2 参数错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/json.h"
#include "sxcl/limiter.h"
#include "sxcl/manifest.h"
#include "sxcl/net.h"

static const char *kManifestUrl = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

typedef struct cli_state {
    int verbose;
    int done;
    int failed;
} cli_state;

static void on_progress(void *userdata, const sxcl_task *task)
{
    cli_state *st = (cli_state *)userdata;
    const char *name = task->label ? task->label : task->dest;
    if (task->state == SXCL_TASK_DONE) {
        ++st->done;
        if (st->verbose) {
            printf("  [完成] %s  %.2f MB\n", name, (double)task->bytes_done / (1024.0 * 1024.0));
        } else {
            printf("\r  已完成 %d 个文件", st->done);
        }
    } else if (task->state == SXCL_TASK_FAILED) {
        ++st->failed;
        if (!st->verbose) {
            printf("\n");
        }
        printf("  [失败] %s  %s\n", name, task->error);
    } else if (st->verbose) {
        printf("  [%5.1f%%] %s  %.2f MB  %.2f MB/s\n",
               task->total_bytes > 0 ? (double)task->bytes_done * 100.0 / (double)task->total_bytes : 0.0,
               name, (double)task->bytes_done / (1024.0 * 1024.0),
               task->speed_bps / (1024.0 * 1024.0));
    }
    fflush(stdout);
}

#if defined(SXCL_HAVE_QT_TRANSPORT)
static sxcl_transport *make_qt_transport(void *userdata)
{
    (void)userdata;
    return sxcl_transport_qt_create(); /* 每个工作线程各一个:Qt 有线程亲和性 */
}
#endif

typedef struct cli_opts {
    double rate;
    int workers;
    int verbose;
    int limit;
    const char *mirror;
} cli_opts;

static int usage(void)
{
    printf("sxcl-dl —— SXCL-C 下载引擎命令行前端\n"
           "  sxcl-dl get <url> <dest> [--sha1 HEX] [--size N] [--rate 5MB] [--workers N] [--mirror URL]\n"
           "  sxcl-dl manifest <dest> [--rate 5MB]\n"
           "  sxcl-dl version <版本号|latest> <游戏目录> [--rate 5MB] [--workers N] [--verbose]\n"
           "  sxcl-dl list [--limit N]\n");
    return 2;
}

static int make_engine(const cli_opts *o, cli_state *st, sxcl_engine **out)
{
    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = o->workers;
    opts.rate_bps = o->rate;
    opts.retry_per_source = 2;
    opts.on_progress = on_progress;
    opts.userdata = st;
#if defined(SXCL_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    opts.transport_factory = make_qt_transport;
#else
    fprintf(stderr, "本产物没有编译进任何传输后端(需要 Qt6::Network)\n");
    return -1;
#endif
    *out = sxcl_engine_create(&opts);
    return *out ? 0 : -1;
}

/** 提交一个任务并跑到结束;返回任务状态。 */
static int run_one(sxcl_engine *engine, sxcl_task *task)
{
    if (sxcl_engine_submit(engine, task) != 0) {
        return SXCL_TASK_FAILED;
    }
    (void)sxcl_engine_run(engine);
    return (int)task->state;
}

/* ── 子命令 ── */

static int cmd_get(int argc, char **argv, const cli_opts *o)
{
    if (argc < 4) {
        return usage();
    }
    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.urls[0] = argv[2]; /* get <url> <dest> */
    task.dest = argv[3];
    task.urls[1] = o->mirror;
    task.urls[2] = NULL;
    task.algo = SXCL_HASH_SHA1;
    task.priority = 0;
    task.label = argv[3];

    for (int i = 4; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--sha1") == 0 && v) {
            task.sha1 = v;
            ++i;
        } else if (strcmp(a, "--size") == 0 && v) {
            task.size = (int64_t)strtoll(v, NULL, 10);
            ++i;
        } else if (strcmp(a, "--mirror") == 0 && v) {
            task.urls[1] = v;
            ++i;
        } else if (strcmp(a, "--rate") == 0 || strcmp(a, "--workers") == 0) {
            ++i;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }

    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        return 1;
    }
    const int state = run_one(engine, &task);
    const int64_t bytes = sxcl_engine_bytes_done(engine);
    printf("\n结果: %s   合计 %.2f MB\n", state == SXCL_TASK_DONE ? "成功" : task.error,
           (double)bytes / (1024.0 * 1024.0));
    sxcl_engine_destroy(engine);
    return state == SXCL_TASK_DONE ? 0 : 1;
}

/** 下载并解析版本清单,返回文档与路径(调用方负责 free)。 */
static sxcl_json *fetch_manifest(sxcl_engine *engine, const char *path, size_t path_len)
{
    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = path;
    task.urls[0] = kManifestUrl;
    task.urls[1] = NULL;
    task.algo = SXCL_HASH_SHA1;
    task.priority = 0;
    task.label = "version_manifest_v2.json";
    if (run_one(engine, &task) != SXCL_TASK_DONE) {
        fprintf(stderr, "下载版本清单失败: %s\n", task.error);
        return NULL;
    }
    (void)path_len;
    char err[256];
    sxcl_json *doc = sxcl_json_parse_file(path, err, sizeof(err));
    if (!doc) {
        fprintf(stderr, "解析版本清单失败: %s\n", err);
    }
    return doc;
}

static int cmd_manifest(const cli_opts *o, const char *dest)
{
    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        return 1;
    }
    sxcl_json *doc = fetch_manifest(engine, dest, strlen(dest));
    sxcl_engine_destroy(engine);
    if (!doc) {
        return 1;
    }
    sxcl_json_free(doc);
    return 0;
}

static int cmd_list(const cli_opts *o)
{
    char path[512];
    snprintf(path, sizeof(path), "build/dl/manifest.json");
    sxcl_fs_mkdirs_for_file(path);
    cli_state st;
    memset(&st, 0, sizeof(st));
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        return 1;
    }
    sxcl_json *doc = fetch_manifest(engine, path, sizeof(path));
    sxcl_engine_destroy(engine);
    if (!doc) {
        return 1;
    }
    sxcl_version_list *list = sxcl_version_list_build(doc);
    if (!list) {
        fprintf(stderr, "版本清单结构不对\n");
        sxcl_json_free(doc);
        return 1;
    }
    printf("最新正式版: %s   最新快照: %s   共 %zu 个版本\n",
           sxcl_version_list_latest_release(list), sxcl_version_list_latest_snapshot(list),
           sxcl_version_list_count(list));
    const int limit = o->limit > 0 ? o->limit : 8;
    for (int i = 0; i < limit; ++i) {
        const sxcl_version_entry *e = sxcl_version_list_at(list, (size_t)i);
        if (!e) {
            break;
        }
        printf("  %-16s %-10s %10lld B  sha1=%s\n", e->id, e->type, (long long)e->size, e->sha1);
    }
    sxcl_version_list_free(list);
    sxcl_json_free(doc);
    return 0;
}

static int cmd_version(int argc, char **argv, const cli_opts *o)
{
    if (argc < 4) {
        return usage();
    }
    const char *want = argv[2];
    const char *game_dir = argv[3];

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/sxcl-cache", game_dir);
    char manifest_path[600];
    snprintf(manifest_path, sizeof(manifest_path), "%s/version_manifest_v2.json", cache_dir);

    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        return 1;
    }

    /* 1) 版本清单 */
    sxcl_json *doc = fetch_manifest(engine, manifest_path, sizeof(manifest_path));
    if (!doc) {
        sxcl_engine_destroy(engine);
        return 1;
    }
    sxcl_version_list *list = sxcl_version_list_build(doc);
    if (!list) {
        fprintf(stderr, "版本清单结构不对\n");
        sxcl_json_free(doc);
        sxcl_engine_destroy(engine);
        return 1;
    }
    const char *target = want;
    if (strcmp(want, "latest") == 0) {
        target = sxcl_version_list_latest_release(list);
    }
    const sxcl_version_entry *entry = sxcl_version_list_find(list, target);
    if (!entry) {
        fprintf(stderr, "清单里没有版本 %s\n", target);
        sxcl_version_list_free(list);
        sxcl_json_free(doc);
        sxcl_engine_destroy(engine);
        return 1;
    }
    printf("目标版本: %s (%s)  版本 JSON %.2f KB\n", entry->id, entry->type,
           (double)entry->size / 1024.0);

    /* 2) 版本 JSON(带 sha1 强校验) */
    char vjson_path[700];
    snprintf(vjson_path, sizeof(vjson_path), "%s/%s.json", cache_dir, entry->id);
    sxcl_task vjson;
    memset(&vjson, 0, sizeof(vjson));
    vjson.dest = vjson_path;
    vjson.urls[0] = entry->url;
    vjson.urls[1] = NULL;
    vjson.sha1 = entry->sha1;
    vjson.algo = SXCL_HASH_SHA1;
    vjson.size = entry->size;
    vjson.priority = 0;
    vjson.label = entry->id;
    if (run_one(engine, &vjson) != SXCL_TASK_DONE) {
        fprintf(stderr, "下载版本 JSON 失败: %s\n", vjson.error);
        sxcl_version_list_free(list);
        sxcl_json_free(doc);
        sxcl_engine_destroy(engine);
        return 1;
    }

    /* 3) 生成下载计划 */
    char err[256];
    sxcl_json *vdoc = sxcl_json_parse_file(vjson_path, err, sizeof(err));
    if (!vdoc) {
        fprintf(stderr, "解析版本 JSON 失败: %s\n", err);
        sxcl_version_list_free(list);
        sxcl_json_free(doc);
        sxcl_engine_destroy(engine);
        return 1;
    }
    sxcl_version_plan *plan = sxcl_version_plan_build(vdoc, game_dir, entry->id, err, sizeof(err));
    if (!plan) {
        fprintf(stderr, "生成下载计划失败: %s\n", err);
        sxcl_json_free(vdoc);
        sxcl_version_list_free(list);
        sxcl_json_free(doc);
        sxcl_engine_destroy(engine);
        return 1;
    }

    /* 4) 批量提交并跑完 */
    const size_t total = sxcl_version_plan_count(plan);
    printf("下载计划: %zu 个文件, 共 %.2f MB\n", total,
           (double)sxcl_version_plan_total_bytes(plan) / (1024.0 * 1024.0));
    for (size_t i = 0; i < total; ++i) {
        sxcl_task *t = sxcl_version_plan_task(plan, i);
        if (sxcl_engine_submit(engine, t) != 0) {
            fprintf(stderr, "任务入队失败: %s\n", t->label);
        }
    }
    const double t0 = sxcl_limiter_now();
    const int failed = sxcl_engine_run(engine);
    const double elapsed = sxcl_limiter_now() - t0;
    if (!o->verbose && st.done > 0) {
        printf("\n");
    }
    printf("完成 %d 个, 失败 %d 个, 耗时 %.1fs, 平均 %.2f MB/s\n", st.done, st.failed, elapsed,
           elapsed > 0.0 ? ((double)sxcl_engine_bytes_done(engine) / elapsed) / (1024.0 * 1024.0)
                         : 0.0);

    sxcl_version_plan_free(plan);
    sxcl_json_free(vdoc);
    sxcl_version_list_free(list);
    sxcl_json_free(doc);
    sxcl_engine_destroy(engine);
    return failed == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    cli_opts o;
    memset(&o, 0, sizeof(o));
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--rate") == 0 && v) {
            o.rate = sxcl_limiter_parse_rate(v);
            ++i;
        } else if (strcmp(a, "--workers") == 0 && v) {
            o.workers = atoi(v);
            ++i;
        } else if (strcmp(a, "--mirror") == 0 && v) {
            o.mirror = v;
            ++i;
        } else if (strcmp(a, "--limit") == 0 && v) {
            o.limit = atoi(v);
            ++i;
        } else if (strcmp(a, "--verbose") == 0) {
            o.verbose = 1;
        }
    }

    if (strcmp(argv[1], "get") == 0) {
        return cmd_get(argc, argv, &o);
    }
    if (strcmp(argv[1], "manifest") == 0) {
        if (argc < 3) {
            return usage();
        }
        return cmd_manifest(&o, argv[2]);
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list(&o);
    }
    if (strcmp(argv[1], "version") == 0) {
        return cmd_version(argc, argv, &o);
    }
    return usage();
}
