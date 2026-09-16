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
#include "sxcl/options.h"

static const char *kManifestUrl = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

typedef struct cli_state {
    int verbose;
    int done;
    int failed;
} cli_state;

static void on_progress(void *userdata, const sxcl_task *task)
{
    cli_state *st = (cli_state *)userdata;
    /* 两个都可能为空(任务结构被写坏时):UCRT 下 printf("%s", NULL) 会直接崩在 strnlen */
    const char *name = (task->label && task->label[0]) ? task->label
                                                       : (task->dest ? task->dest : "(未知任务)");
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
        /* 诊断用:失败时把 label/dest/err 与指针全打出来,空字段也能一眼看出是"指针坏了"还是"本来就是空" */
        printf("  [失败] label='%s' dest='%s' err='%s'  (task=%p label=%p dest=%p)\n",
               task->label ? task->label : "(null)", task->dest ? task->dest : "(null)",
               task->error, (const void *)task, (const void *)task->label,
               (const void *)task->dest);
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
    int skip_assets;
    const char *mirror;
    const char *asset_mirror;
    const char *cache;
} cli_opts;

#if defined(_WIN32)
/* ── 崩溃处理器:打印异常码、出错地址、以及每一帧的"模块+偏移(+符号)" ──
 * 这台机器上 WER 不记录我们的崩溃(其他程序都有记录),没有它就只能靠猜。 */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#  include <stdio.h>

static void crash_frame_name(HANDLE proc, DWORD64 addr, char *out, size_t out_len)
{
    out[0] = '\0';
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &mod) &&
        mod) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char *base = strrchr(path, '\\');
            base = base ? base + 1 : path;
            snprintf(out, out_len, "%s+0x%llx", base, (unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
        }
    }
    char sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sym_buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym)) {
        const size_t used = strlen(out);
        snprintf(out + used, out_len > used ? out_len - used : 0, "  %s+0x%llx", sym->Name,
                 (unsigned long long)disp);
    }
}

static LONG WINAPI sxcl_crash_handler(EXCEPTION_POINTERS *info);

/* 向量异常处理器(第一现场):SetUnhandledExceptionFilter 是在栈展开之后才跑的,
 * 那时原始调用帧已经没了 —— 实测只抓到处理器自己的栈,看不到真正的调用者。 */
static LONG CALLBACK sxcl_veh_handler(EXCEPTION_POINTERS *info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH; /* 其它异常(C++ 异常等)不插手 */
    }
    sxcl_crash_handler(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI sxcl_crash_handler(EXCEPTION_POINTERS *info)
{
    fprintf(stderr, "\n=========== 崩溃 ===========\n");
    fprintf(stderr, "异常码: 0x%08lX   地址: %p\n", info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress);
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);

    void *frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, NULL);
    for (USHORT i = 0; i < n; ++i) {
        char desc[512];
        crash_frame_name(proc, (DWORD64)(uintptr_t)frames[i], desc, sizeof(desc));
        fprintf(stderr, "  #%02u  %p  %s\n", i, frames[i], desc);
    }
    fflush(stderr);
    SymCleanup(proc);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static int usage(void)
{
    printf("sxcl-dl —— SXCL-C 下载引擎命令行前端\n"
           "  sxcl-dl get <url> <dest> [--sha1 HEX] [--size N] [--rate 5MB] [--workers N] [--mirror URL]\n"
           "  sxcl-dl manifest <dest> [--rate 5MB]\n"
           "  sxcl-dl version <版本号|latest> <游戏目录> [--rate 5MB] [--workers N] [--verbose]\n"
           "  sxcl-dl list [--limit N]\n"
           "  sxcl-dl options <options.txt> [--get KEY] [--set KEY=VALUE] [--remove KEY] [--dump]\n");
    return 2;
}

static int make_engine(const cli_opts *o, cli_state *st, sxcl_engine **out)
{
    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = o->workers;
    opts.rate_bps = o->rate;
    opts.retry_per_source = 2;
    opts.cache_path = o->cache;
    opts.on_progress = on_progress;
    opts.userdata = st;
#if defined(SXCL_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    opts.transport_factory = make_qt_transport;
    *out = sxcl_engine_create(&opts);
    return *out ? 0 : -1;
#else
    /* 没有 Qt 就没有传输后端。注意:这里 return 之后不能再有代码,
     * 否则 MSVC 的 C4702(unreachable code)会在 /WX 下把构建打挂 —— CI 上抓到过。 */
    fprintf(stderr, "本产物没有编译进任何传输后端(需要 Qt6::Network),无法下载\n");
    *out = NULL;
    return -1;
#endif
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
    /* 必须是静态存储:引擎会一直持有这个任务指针(后面还要跑资源那一批),
     * 用栈上局部变量的话函数一返回地址就被复用了 —— 引擎第二次 run() 扫描任务表时会读到
     * 被覆盖的"状态"和垃圾 URL 指针,然后崩在 Qt 的 strlen 上(实测就是这个,查了很久)。 */
    static sxcl_task task;
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

/* options.txt 读写:启动器要在每次启动前把渲染后端写进去,并在启动后核对游戏有没有把它改回去 */
static int cmd_options(int argc, char **argv)
{
    if (argc < 3) {
        return usage();
    }
    const char *path = argv[2];
    sxcl_options *opts = sxcl_options_load(path);
    if (!opts) {
        fprintf(stderr, "打开失败: %s\n", path);
        return 1;
    }
    int changed = 0, failed = 0;
    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--get") == 0 && i + 1 < argc) {
            const char *v = sxcl_options_get(opts, argv[++i]);
            printf("%s\n", v ? v : "(不存在)");
        } else if (strcmp(a, "--set") == 0 && i + 1 < argc) {
            const char *kv = argv[++i];
            const char *eq = strchr(kv, '=');
            if (!eq) {
                fprintf(stderr, "--set 需要 KEY=VALUE\n");
                ++failed;
                continue;
            }
            char key[160];
            const size_t n = (size_t)(eq - kv);
            if (n == 0 || n >= sizeof(key)) {
                fprintf(stderr, "键名非法\n");
                ++failed;
                continue;
            }
            memcpy(key, kv, n);
            key[n] = '\0';
            if (sxcl_options_set(opts, key, eq + 1) != 0) {
                ++failed;
            } else {
                ++changed;
                printf("%s=%s\n", key, eq + 1);
            }
        } else if (strcmp(a, "--remove") == 0 && i + 1 < argc) {
            sxcl_options_remove(opts, argv[++i]);
            ++changed;
            printf("已删除 %s\n", argv[i]);
        } else if (strcmp(a, "--dump") == 0) {
            for (size_t k = 0; k < sxcl_options_count(opts); ++k) {
                printf("%s=%s\n", sxcl_options_key_at(opts, k), sxcl_options_value_at(opts, k));
            }
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            ++failed;
        }
    }
    if (changed > 0 && sxcl_options_save(opts, path) != 0) {
        fprintf(stderr, "保存失败: %s\n", path);
        ++failed;
    }
    sxcl_options_free(opts);
    return failed == 0 ? 0 : 1;
}

static int cmd_version(int argc, char **argv, const cli_opts *opts_in)
{
    if (argc < 4) {
        return usage();
    }
    /* 参数可能要在里面补默认值(缓存路径),所以用一份可写副本 */
    cli_opts opts_local = *opts_in;
    cli_opts *o = &opts_local;
    const char *want = argv[2];
    const char *game_dir = argv[3];

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/sxcl-cache", game_dir);
    /* 哈希缓存默认落在游戏目录下:第二次运行核对 5000+ 个资源文件时几乎零成本 */
    char hash_cache_path[600];
    if (!o->cache) {
        snprintf(hash_cache_path, sizeof(hash_cache_path), "%s/hashes.txt", cache_dir);
        o->cache = hash_cache_path;
    } else if (o->cache[0] == '\0') {
        o->cache = NULL; /* --no-cache */
    }
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

    /* 4) 第一批:客户端 jar + 资源索引 + 依赖库(优先级已保证索引先下完) */
    size_t total = sxcl_version_plan_count(plan);
    printf("第一批计划: %zu 个文件, 共 %.2f MB\n", total,
           (double)sxcl_version_plan_total_bytes(plan) / (1024.0 * 1024.0));
    for (size_t i = 0; i < total; ++i) {
        sxcl_task *t = sxcl_version_plan_task(plan, i);
        if (sxcl_engine_submit(engine, t) != 0) {
            fprintf(stderr, "任务入队失败: %s\n", t->label);
        }
    }
    const double t0 = sxcl_limiter_now();
    int failed = sxcl_engine_run(engine);
    if (!o->verbose && st.done > 0) {
        printf("\n");
    }
    printf("第一批完成 %d 个, 失败 %d 个\n", st.done, st.failed);

    /* 5) 展开资源对象(索引在上一步已经下好并做过 SHA-1 强校验) */
    size_t assets_added = 0;
    if (!o->skip_assets) {
        const sxcl_json_value *ai = sxcl_json_get(sxcl_json_root(vdoc), "assetIndex");
        const char *index_id = sxcl_json_get_string(ai, "id", "assets");
        char index_path[700];
        snprintf(index_path, sizeof(index_path), "%s/assets/indexes/%s.json", game_dir, index_id);
        char aerr[256];
        sxcl_json *idoc = sxcl_json_parse_file(index_path, aerr, sizeof(aerr));
        if (!idoc) {
            fprintf(stderr, "读资源索引失败(%s): %s\n", index_path, aerr);
            ++failed;
        } else {
            const int before = st.done + st.failed;
            const int added = sxcl_version_plan_add_asset_objects(plan, idoc, game_dir, NULL,
                                                                  o->asset_mirror, aerr, sizeof(aerr));
            if (added < 0) {
                fprintf(stderr, "展开资源对象失败: %s\n", aerr);
                ++failed;
            } else {
                assets_added = (size_t)added;
                int64_t asset_bytes = 0;
                const size_t now = sxcl_version_plan_count(plan);
                for (size_t i = total; i < now; ++i) {
                    sxcl_task *t = sxcl_version_plan_task(plan, i);
                    asset_bytes += t->size;
                    if (sxcl_engine_submit(engine, t) != 0) {
                        fprintf(stderr, "任务入队失败: %s\n", t->label);
                    }
                }
                printf("资源对象: %zu 个, 共 %.2f MB\n", assets_added,
                       (double)asset_bytes / (1024.0 * 1024.0));
                failed += sxcl_engine_run(engine);
                (void)before;
                if (!o->verbose && st.done > 0) {
                    printf("\n");
                }
            }
            sxcl_json_free(idoc);
            total = sxcl_version_plan_count(plan);
        }
    }
    const double elapsed = sxcl_limiter_now() - t0;
    printf("全部完成 %d 个, 失败 %d 个, 耗时 %.1fs, 平均 %.2f MB/s, 合计 %.2f MB\n", st.done,
           st.failed, elapsed,
           elapsed > 0.0 ? ((double)sxcl_engine_bytes_done(engine) / elapsed) / (1024.0 * 1024.0)
                         : 0.0,
           (double)sxcl_engine_bytes_done(engine) / (1024.0 * 1024.0));
    (void)assets_added;

    sxcl_version_plan_free(plan);
    sxcl_json_free(vdoc);
    sxcl_version_list_free(list);
    sxcl_json_free(doc);
    sxcl_engine_destroy(engine);
    return failed == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    /* 无缓冲输出:崩溃时不会把最后一段输出留在缓冲区里丢掉(排查跨平台崩溃吃过这个亏) */
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, sxcl_veh_handler); /* 第一现场,栈还没展开 */
    SetUnhandledExceptionFilter(sxcl_crash_handler);
#endif
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
        } else if (strcmp(a, "--skip-assets") == 0) {
            o.skip_assets = 1;
        } else if (strcmp(a, "--asset-mirror") == 0 && v) {
            o.asset_mirror = v;
            ++i;
        } else if (strcmp(a, "--cache") == 0 && v) {
            o.cache = v;
            ++i;
        } else if (strcmp(a, "--no-cache") == 0) {
            o.cache = ""; /* 空串 = 明确不要缓存(与"未指定,取默认路径"区分) */
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
    if (strcmp(argv[1], "options") == 0) {
        return cmd_options(argc, argv);
    }
    return usage();
}
