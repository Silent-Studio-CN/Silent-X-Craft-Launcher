/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* fopen/fgets 在 MSVC 下默认被标记弃用 */
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sxcl/auth.h"
#include "sxcl/auth_store.h"
#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/java_runtime.h" /* java 子命令:官方 JRE 运行时预置(一次装齐) */
#include "sxcl/json.h"
#include "sxcl/launch.h"     /* 里面有 crash.h:崩溃取证的类型/常量(crash 子命令用) */
#include "sxcl/log.h"       /* 运行日志落盘 + 游戏输出通道(sxcl_log_game_*) */
#include "sxcl/logexport.h" /* logs export 子命令:一键导出日志包(store 打包 + 打码) */
#include "sxcl/install.h" /* 版本 JSON 落盘 sxcl_install_write_version_json;缺它会 C4013 -> C2220 */
#include "sxcl/limiter.h"
#include "sxcl/loader.h"
#include "sxcl/mods.h"   /* mods 子命令:模组资源来源层(Modrinth)的端到端验收入口 */
#include "sxcl/manifest.h"
#include "sxcl/net.h"
#include "sxcl/options.h"

static const char *kManifestUrl = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
/* 第二路(BMCLAPI,实测与官方同路径透传)。清单、版本 JSON、客户端 jar、依赖库、
 * 资源对象都有对应的镜像路径 —— 没接上就等于"配了镜像也不生效"。 */
static const char *kManifestMirrorUrl =
    "https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json";

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
            /* 打出真正成功的那条候选(source_index)与它的 URL:
             * 这是"镜像到底排第几"的直接证据,不靠猜。 */
            const char *used = (task->source_index >= 0 && task->source_index < 4 &&
                                task->urls[task->source_index] != NULL)
                                   ? task->urls[task->source_index]
                                   : "(未知)";
            printf("  [完成] %s  %.2f MB  来源 #%d %s\n", name,
                   (double)task->bytes_done / (1024.0 * 1024.0), task->source_index, used);
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
    int prefer_mirror; /* --source:1 = 镜像(默认 BMCLAPI)排第一、官方作第二候选;0 = 官方优先 */
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
#  include <shellapi.h> /* CommandLineToArgvW:拿 UTF-16 命令行,避免中文实参乱码 */
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
           "      [--source bmclapi|mojang|auto] [--mirror URL] [--skip-assets] [--asset-mirror URL]\n"
           "      └ --source 决定哪条路排第一(默认 bmclapi):bmclapi/auto = 镜像优先、官方作第二候选\n"
           "        (镜像不通自动切官方);mojang = 官方优先。清单/版本 JSON/客户端 jar/依赖库/资源都算\n"
           "  sxcl-dl list [--limit N]\n"
           "  sxcl-dl options <options.txt> [--get KEY] [--set KEY=VALUE] [--remove KEY] [--dump]\n"
           "  sxcl-dl loader <forge|neoforge|fabric|quilt|optifine> <加载器版本> <MC 版本> <游戏目录>\n"
           "      [--java PATH] [--instance NAME] [--installer JAR] [--timeout MS] [--maven-mirror URL]\n"
           "      [--no-fallback] [--extract-install] [--verbose]\n"
           "      └ --extract-install:跳过安装器 CLI,直接解包安装(跑 processors 重放;排查/验收用)\n"
           "  sxcl-dl launch <版本名> <游戏目录> [--java PATH] [--memory MB] [--instance NAME]\n"
           "      [--offline 玩家名 | --account [--token-file PATH]]\n"
           "      [--backend default|vulkan|opengl] [--timeout 秒] [--settings PATH]\n"
           "      └ --account:用 sxcl-dl auth login 存下的正版身份启动(令牌不明文进日志)\n"
           "      [--dry-run] [--verbose]\n"
           "      └ 读版本 JSON -> 选 Java -> 按实例设置写 options.txt(渲染后端)-> 起进程\n"
           "        -> 每行归类 -> 出一条人话结论(发现 Vulkan 回退会写回 lastGraphicsApi)\n"
           "  sxcl-dl mods search <关键词> [--mc 1.20.1] [--loader fabric] [--type mod|shader] [--limit N]\n"
           "  sxcl-dl mods files <工程 id|短名> [--mc 1.20.1] [--loader fabric]\n"
           "      └ 模组资源层(docs/22 的 A1):搜索走服务端 facets;挑文件要求版本+加载器都对得上,\n"
           "        不自动换加载器;取 JSON 走我们自己的引擎(候选/UA/缓存)\n"
           "  sxcl-dl crash <游戏目录> [--lang en-us] [--no-debug]\n"
           "      └ 崩溃取证:读 crash-reports 里最近的非空报告 + logs/latest.log(必要时 debug.log),\n"
           "        自动探测编码(UTF-8/UTF-16/GBK),出**原因键**(out_of_memory / mod_duplicate …)\n"
           "        与一条可执行建议;与启动层用的是同一个入口,结论不会与界面分叉\n"
           "  sxcl-dl logs export <游戏目录> <输出.zip> [--log-dir DIR] [--debug]\n"
           "      [--max-logs N] [--max-reports N] [--max-bytes N]\n"
           "      └ 一键导出:启动器 sxcl-*.log + 游戏 logs/latest.log + crash-reports/*.txt\n"
           "        打成 zip(store 不压缩);**导出前先打码**(token/UUID/玩家名/路径用户名 -> ***)\n"
           "  sxcl-dl auth <login|status|refresh|logout|bedrock> [...]\n"
           "      └ 微软(Xbox Live)正版登录:授权码+PKCE+环回 / 设备码兜底 / 免密续期\n"
           "        令牌加密落盘,输出只打前缀,绝不打印明文;详见 docs/09-正版登录.md\n"
           "  sxcl-dl java <list|plan|preset> [--root DIR] [--mc 1.20.1]... [--no-newest] [--force]\n"
           "      └ 官方 JRE 运行时:list = 列清单里有哪些组件;plan = 只报要装什么/多大;\n"
           "        preset = 一次装齐(Java 8/17/21 + 清单里最新的 25;已装好的跳过,\n"
           "        装前先算空间,不够直接报错不下载);详见 docs/07-随行运行时与打包.md\n");
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
        } else if (strcmp(a, "--rate") == 0 || strcmp(a, "--workers") == 0 ||
                   strcmp(a, "--conn") == 0 || strcmp(a, "--cache") == 0 ||
                   strcmp(a, "--source") == 0) {
            ++i; /* 通用参数已在 main 里解析,这里只需跳过它的值 */
        } else if (strcmp(a, "--no-cache") == 0 || strcmp(a, "--verbose") == 0) {
            /* 无值参数 */
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
static sxcl_json *fetch_manifest(sxcl_engine *engine, const char *path, size_t path_len,
                                 int prefer_mirror)
{
    /* 必须是静态存储:引擎会一直持有这个任务指针(后面还要跑资源那一批),
     * 用栈上局部变量的话函数一返回地址就被复用了 —— 引擎第二次 run() 扫描任务表时会读到
     * 被覆盖的"状态"和垃圾 URL 指针,然后崩在 Qt 的 strlen 上(实测就是这个,查了很久)。 */
    static sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = path;
    /* --source 默认 bmclapi:镜像排第一,官方退第二(镜像不通引擎自动切回官方) */
    task.urls[0] = prefer_mirror ? kManifestMirrorUrl : kManifestUrl;
    task.urls[1] = prefer_mirror ? kManifestUrl : kManifestMirrorUrl;
    task.algo = SXCL_HASH_SHA1;
    task.priority = 0;
    task.label = "version_manifest_v2.json";
    if (run_one(engine, &task) != SXCL_TASK_DONE) {
        fprintf(stderr, "下载版本清单失败(官方与镜像两条路都不通): %s\n", task.error);
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
    sxcl_json *doc = fetch_manifest(engine, dest, strlen(dest), o->prefer_mirror);
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
    sxcl_json *doc = fetch_manifest(engine, path, sizeof(path), o->prefer_mirror);
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
    int failed_version_json = 0; /* 版本 JSON 落盘失败也算这次没成功 */
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        return 1;
    }

    /* 1) 版本清单 */
    sxcl_json *doc = fetch_manifest(engine, manifest_path, sizeof(manifest_path), o->prefer_mirror);
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
    /* 版本 JSON 的第二路:同一个 URL 的镜像(认不出就没有第二路,不影响官方那条) */
    char vjson_mirror[1024];
    vjson_mirror[0] = '\0';
    if (sxcl_manifest_mirror_url(entry->url, o->mirror, vjson_mirror, sizeof(vjson_mirror)) != 0) {
        vjson_mirror[0] = '\0';
    }
    vjson.dest = vjson_path;
    if (o->prefer_mirror && vjson_mirror[0] != '\0') {
        vjson.urls[0] = vjson_mirror; /* 镜像优先:官方退成第二候选,不通自动切 */
        vjson.urls[1] = entry->url;
    } else {
        vjson.urls[0] = entry->url;
        vjson.urls[1] = (vjson_mirror[0] != '\0') ? vjson_mirror : NULL;
    }
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
    /* 版本 JSON 落盘到启动层要的位置(<游戏目录>/versions/<id>/<id>.json)。
     * 以前只留在缓存目录,导致"文件都下完了却启动不了"(实测踩过)。 */
    {
        char werr[256];
        werr[0] = '\0';
        if (sxcl_install_write_version_json(game_dir, entry->id, vjson_path, werr, sizeof(werr)) == 0) {
            printf("版本 JSON 已写入: %s/versions/%s/%s.json\n", game_dir, entry->id, entry->id);
        } else {
            fprintf(stderr, "写版本 JSON 失败: %s\n", werr);
            ++failed_version_json;
        }
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
    /* 每个文件都补一条镜像路(官方不通时引擎会自己换) —— 只配了清单镜像是没用的。 */
    {
        char merr[256];
        merr[0] = '\0';
        const int mirrored = sxcl_version_plan_add_mirror(plan, o->mirror, merr, sizeof(merr));
        if (mirrored < 0) {
            fprintf(stderr, "补镜像路失败: %s\n", merr);
        } else {
            printf("下载镜像: %s(%d 个文件已备好第二路)\n",
                   (o->mirror != NULL && o->mirror[0] != '\0') ? o->mirror
                                                                : SXCL_MIRROR_BMCLAPI_BASE,
                   mirrored);
            /* --source 默认 bmclapi:把镜像挪到第一路。**只调这一次**(见 manifest.h):
             * 资源对象是后面才追加的,它们的顺序在下面按同一个开关单独定。 */
            if (o->prefer_mirror) {
                const int swapped = sxcl_version_plan_prefer_mirror(plan);
                printf("下载源: 镜像优先,%d 个文件镜像排第一(官方作第二候选,不通自动切)\n", swapped);
            } else {
                printf("下载源: 官方优先(mojang),镜像只作第二候选\n");
            }
        }
    }
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
            /* 资源对象的顺序在这里一次定好(--asset-mirror 没给就默认 BMCLAPI 的 /assets);
             * 不能靠再调一次 prefer_mirror —— 那会把前面那批已换好的又换回官方在前。 */
            char asset_mirror_default[1024];
            const char *asset_base = NULL;
            const char *asset_mirror = o->asset_mirror;
            if (o->prefer_mirror) {
                if (o->asset_mirror && o->asset_mirror[0] != '\0') {
                    snprintf(asset_mirror_default, sizeof(asset_mirror_default), "%s", o->asset_mirror);
                } else {
                    snprintf(asset_mirror_default, sizeof(asset_mirror_default), "%s/assets",
                             SXCL_MIRROR_BMCLAPI_BASE);
                }
                asset_base = asset_mirror_default;
                asset_mirror = SXCL_ASSET_OBJECTS_BASE;
            }
            const int added = sxcl_version_plan_add_asset_objects(plan, idoc, game_dir, asset_base,
                                                                  asset_mirror, aerr, sizeof(aerr));
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
    if (failed_version_json != 0) {
        /* 文件都下好了但版本 JSON 没落盘 = 用户"装完了却启动不了",必须算失败 */
        fprintf(stderr, "注意:版本 JSON 没能写到 versions/%s/%s.json,启动层会报找不到版本\n",
                entry->id, entry->id);
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

/* ── loader:模组加载器静默安装(sxcl/loader.h 的端到端验收入口) ── */

static volatile sig_atomic_t g_loader_cancel = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_loader_cancel = 1;   /* Ctrl+C:交给安装驱动的 is_cancelled 回调去终止安装器进程 */
}

static int loader_cancelled(void *userdata)
{
    (void)userdata;
    return g_loader_cancel != 0;
}

static void loader_progress(void *userdata, int percent, const char *status)
{
    (void)userdata;
    printf("  [%3d%%] %s\n", percent, status);
    fflush(stdout);
}

/* 安装流程用到的两个回调共用一个 userdata(见 loader.h 的说明),所以这里把
 * "CLI 选项 + 游戏目录"打成一包。 */
typedef struct loader_cli_ctx {
    const cli_opts *o;
    const char *game_dir;
    const char *instance;
} loader_cli_ctx;

/* --verbose 时把安装器吐出来的原始每一行也打出来(排查"退出码 1"这种只有结论没有原因的情况)。 */
static int loader_raw_line(void *userdata, int is_stderr, const char *line)
{
    const loader_cli_ctx *ctx = (const loader_cli_ctx *)userdata;
    if (ctx && ctx->o && ctx->o->verbose) {
        printf("      [%s] %s\n", is_stderr ? "err" : "out", line);
        fflush(stdout);
    }
    return 0;
}

/* 客户端映射:见 loader.h 的 on_mappings。
 * 1.14~1.20 的安装器里有条 `--task DOWNLOAD_MOJMAPS`,它直接去 piston-data 抓那份 client.txt
 * (国内经常连不上,FCL 为它专门打了补丁)。我们手里有描述它的 URL + SHA-1 + size(版本 JSON 的
 * downloads.client_mappings),顺手用镜像下掉;下不成返回非 0,处理器照跑(自己再试一次)。 */
static int loader_on_mappings(void *userdata, const char *mc_version, const char *output, char *err,
                              size_t err_len)
{
    const loader_cli_ctx *ctx = (const loader_cli_ctx *)userdata;
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!ctx || !ctx->o || !ctx->game_dir || !output || !output[0]) {
        return 1;
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s/versions/%s/%s.json", ctx->game_dir,
             mc_version ? mc_version : "", mc_version ? mc_version : "");
    if (!sxcl_fs_exists(path) && ctx->instance && ctx->instance[0]) {
        snprintf(path, sizeof(path), "%s/versions/%s/%s.json", ctx->game_dir, ctx->instance,
                 ctx->instance);
    }
    if (!sxcl_fs_exists(path)) {
        snprintf(err, err_len, "版本 JSON 不在: %s", path);
        return 1;
    }
    char jerr[192];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse_file(path, jerr, sizeof(jerr));
    if (!doc) {
        snprintf(err, err_len, "版本 JSON 读不出来: %s", jerr[0] ? jerr : path);
        return 1;
    }
    const sxcl_json_value *downloads = sxcl_json_get(sxcl_json_root(doc), "downloads");
    const sxcl_json_value *cm = downloads ? sxcl_json_get(downloads, "client_mappings") : NULL;
    const char *url = cm ? sxcl_json_get_string(cm, "url", "") : "";
    const char *sha1 = cm ? sxcl_json_get_string(cm, "sha1", "") : "";
    const long long size = cm ? (long long)sxcl_json_get_int64(cm, "size", 0) : 0;
    sxcl_json_free(doc);
    if (!url[0] || !sha1[0] || size <= 0) {
        snprintf(err, err_len, "版本 JSON 里没有 downloads.client_mappings");
        return 1;
    }

    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = ctx->o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(ctx->o, &st, &engine) != 0) {
        snprintf(err, err_len, "下载引擎起不来");
        return 1;
    }
    char mirror[1024];
    mirror[0] = '\0';
    const int have_mirror =
        sxcl_manifest_mirror_url(url, SXCL_MIRROR_BMCLAPI_BASE, mirror, sizeof(mirror)) == 0;
    sxcl_task t;
    memset(&t, 0, sizeof(t));
    t.dest = output;
    if (ctx->o->prefer_mirror && have_mirror) {
        t.urls[0] = mirror;
        t.urls[1] = url;
    } else {
        t.urls[0] = url;
        if (have_mirror) {
            t.urls[1] = mirror;
        }
    }
    t.sha1 = sha1;
    t.algo = SXCL_HASH_SHA1;
    t.size = size;
    t.priority = 0;
    t.label = "client_mappings";
    const int state = run_one(engine, &t);
    sxcl_engine_destroy(engine);
    if (state != SXCL_TASK_DONE) {
        snprintf(err, err_len, "%s", t.error[0] ? t.error : "下载失败");
        return 1;
    }
    printf("  客户端映射已由下载引擎取回: %s\n", output);
    fflush(stdout);
    return 0;
}

/* 方式 B(解包安装)要下的依赖库:交给 CLI 自己的下载引擎,与安装器那条路同一个写法。
 * **清单里给了绝对地址(downloads.artifact.url)就用它** —— 这些库不都躺在同一个主机上
 * (实测 Forge 1.20.1 的 46 条:jsr305 在 libraries.minecraft.net,其余在 maven.minecraftforge.net)。
 * 处理器(jarsplitter / installertools / binarypatcher…)也在这份清单里,所以这一步之后
 * 方式 B 才真的能把 processors 跑起来。 */
static int loader_on_libraries(void *userdata, const sxcl_loader_library *libs, size_t count)
{
    const loader_cli_ctx *ctx = (const loader_cli_ctx *)userdata;
    if (!ctx || !ctx->o || count == 0) {
        return 0;
    }
    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = ctx->o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(ctx->o, &st, &engine) != 0) {
        return 1;
    }
    printf("  方式 B 依赖库: %d 件\n", (int)count);
    fflush(stdout);
    int failed = 0;
    for (size_t i = 0; i < count && !failed; ++i) {
        char dest[1400];
        char url[1600];
        snprintf(dest, sizeof(dest), "%s/libraries/%s", ctx->game_dir, libs[i].path);
        if (libs[i].url_full[0]) {
            snprintf(url, sizeof(url), "%s", libs[i].url_full);
        } else {
            const size_t ul = strlen(libs[i].url);
            const char *sep = (ul > 0 && libs[i].url[ul - 1] == '/') ? "" : "/";
            snprintf(url, sizeof(url), "%s%s%s", libs[i].url, sep, libs[i].path);
        }
        sxcl_task t;
        memset(&t, 0, sizeof(t));
        t.dest = dest;
        t.urls[0] = url;
        t.urls[1] = ctx->o->mirror;
        t.algo = SXCL_HASH_SHA1;
        /* 清单给了哈希/大小就用它:半截文件不会再被当成"下完了",重跑还能走"已存在"快路径 */
        t.sha1 = libs[i].sha1[0] ? libs[i].sha1 : NULL;
        t.size = libs[i].size;
        t.priority = 10;
        t.label = libs[i].name;
        if (run_one(engine, &t) != SXCL_TASK_DONE) {
            fprintf(stderr, "依赖库下载失败(%s): %s\n", libs[i].name, t.error);
            failed = 1;
        }
    }
    sxcl_engine_destroy(engine);
    return failed ? 1 : 0;
}

/* 读一行文本(去掉首尾空白)。返回 0 成功。 */
static int read_trimmed_line(const char *path, char *out, size_t cap)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return -1;
    }
    if (!fgets(out, (int)cap, fh)) {
        fclose(fh);
        return -1;
    }
    fclose(fh);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    return 0;
}

/* 本地没有安装器时,用我们自己的下载引擎去官方 maven 取一份(先取 .sha1,再带哈希下 jar)。
 * 只有 Forge / NeoForge 的地址能从"MC 版本 + 加载器版本"拼出来;其它加载器请用 --installer。 */
static int loader_fetch_installer(const cli_opts *o, sxcl_loader_kind kind, const char *mc_version,
                                  const char *loader_version, const char *dest)
{
    char url[1024];
    if (kind == SXCL_LOADER_FORGE) {
        snprintf(url, sizeof(url),
                 "https://maven.minecraftforge.net/net/minecraftforge/forge/%s-%s/forge-%s-%s-installer.jar",
                 mc_version, loader_version, mc_version, loader_version);
    } else if (kind == SXCL_LOADER_NEOFORGE) {
        snprintf(url, sizeof(url),
                 "https://maven.neoforged.net/releases/net/neoforged/neoforge/%s/neoforge-%s-installer.jar",
                 loader_version, loader_version);
    } else {
        fprintf(stderr, "%s 的安装器地址拼不出来，请用 --installer 指定本地 jar\n",
                sxcl_loader_kind_name(kind));
        return -1;
    }

    char sha_url[1100];
    char sha_path[1100];
    snprintf(sha_url, sizeof(sha_url), "%s.sha1", url);
    snprintf(sha_path, sizeof(sha_path), "%s.sha1", dest);

    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        return -1;
    }

    sxcl_task sha_task;
    memset(&sha_task, 0, sizeof(sha_task));
    sha_task.dest = sha_path;
    sha_task.urls[0] = sha_url;
    sha_task.urls[1] = o->mirror;
    sha_task.algo = SXCL_HASH_SHA1;
    sha_task.priority = 0;
    sha_task.label = "installer.sha1";
    if (run_one(engine, &sha_task) != SXCL_TASK_DONE) {
        fprintf(stderr, "取安装器 .sha1 失败: %s\n", sha_task.error);
        sxcl_engine_destroy(engine);
        return -1;
    }

    char want[96];
    want[0] = '\0';
    if (read_trimmed_line(sha_path, want, sizeof(want)) != 0 || strlen(want) < 32) {
        fprintf(stderr, "读 %s 失败,拿不到官方哈希\n", sha_path);
        sxcl_engine_destroy(engine);
        return -1;
    }
    printf("官方 SHA-1: %s\n", want);

    sxcl_task jar_task;
    memset(&jar_task, 0, sizeof(jar_task));
    jar_task.dest = dest;
    jar_task.urls[0] = url;
    jar_task.urls[1] = o->mirror;
    jar_task.sha1 = want;
    jar_task.algo = SXCL_HASH_SHA1;
    jar_task.priority = 0;
    jar_task.label = "installer";
    const int state = run_one(engine, &jar_task);
    const int ok = (state == SXCL_TASK_DONE);
    if (!ok) {
        fprintf(stderr, "下载安装器失败: %s\n", jar_task.error);
    }
    sxcl_engine_destroy(engine);
    return ok ? 0 : -1;
}

/* ── launch:把游戏真的跑起来(启动驱动的最薄一层外壳) ── */

typedef struct launch_cli_opts {
    const char *java_path;
    const char *instance;
    const char *offline;
    int use_account;          /* --account:用已登录的正版账户启动 */
    const char *token_file;   /* --token-file:令牌文件路径(可空=默认) */
    const char *backend;
    const char *settings;
    int memory_mb;
    int timeout_ms;
    int verbose;
    int dry_run;
} launch_cli_opts;

static int launch_echo_line(void *userdata, int is_stderr, const char *line)
{
    (void)userdata;
    printf("  %s %s\n", is_stderr ? "|!" : "|", line);
    return 0; /* 返回非 0 就是请求终止进程;命令行前端不主动终止 */
}

static int cmd_launch(int argc, char **argv, const cli_opts *o)
{
    if (argc < 4) {
        return usage(); /* launch <版本名> <游戏目录> */
    }
    launch_cli_opts lo;
    memset(&lo, 0, sizeof(lo));
    lo.verbose = o->verbose;
    for (int i = 4; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--java") == 0 && v) {
            lo.java_path = v;
            ++i;
        } else if (strcmp(a, "--memory") == 0 && v) {
            lo.memory_mb = atoi(v);
            ++i;
        } else if (strcmp(a, "--instance") == 0 && v) {
            lo.instance = v;
            ++i;
        } else if (strcmp(a, "--offline") == 0 && v) {
            lo.offline = v;
            ++i;
        } else if (strcmp(a, "--account") == 0) {
            lo.use_account = 1; /* 用已登录的正版账户(令牌从加密存储里读) */
        } else if (strcmp(a, "--token-file") == 0 && v) {
            lo.token_file = v;
            ++i;
        } else if (strcmp(a, "--backend") == 0 && v) {
            lo.backend = v;
            ++i;
        } else if (strcmp(a, "--timeout") == 0 && v) {
            lo.timeout_ms = atoi(v) * 1000; /* 命令行给秒,内部用毫秒 */
            ++i;
        } else if (strcmp(a, "--settings") == 0 && v) {
            lo.settings = v;
            ++i;
        } else if (strcmp(a, "--dry-run") == 0) {
            lo.dry_run = 1; /* 只准备(选 Java / 写 options.txt / 解 natives / 拼 argv),不起进程 */
        } else if (strcmp(a, "--verbose") == 0) {
            lo.verbose = 1;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }

    char settings_path[1024];
    if (!lo.settings) {
        /* 每实例设置默认跟着游戏目录走:命令行前端没有"启动器配置目录"这个概念,
         * 把设置文件和游戏放一起最不容易找错地方。 */
        snprintf(settings_path, sizeof(settings_path), "%s/sxcl-launcher.conf", argv[3]);
        lo.settings = settings_path;
    }

    /* --account:把加密存储里的正版身份读出来喂给启动层。
     * 读的是内存里的明文(只在进程内),令牌**不会**出现在日志或命令行里。 */
    sxcl_auth_session session;
    int have_account = 0;
    char acct_err[SXCL_AUTH_ERROR_MAX];
    acct_err[0] = '\0';
    if (lo.use_account) {
        if (lo.offline != NULL) {
            printf("--account 与 --offline 不能同时用:前者是正版身份,后者是离线身份。\n");
            return 2;
        }
        char tf[SXCL_AUTH_STORE_PATH_MAX];
        const char *path = lo.token_file;
        if (path == NULL) {
            if (sxcl_auth_store_default_path(tf, sizeof(tf), acct_err, sizeof(acct_err)) != SXCL_AUTH_OK) {
                printf("拼不出令牌文件路径:%s\n", acct_err);
                return 1;
            }
            path = tf;
        }
        if (sxcl_auth_store_load(path, &session, acct_err, sizeof(acct_err)) != SXCL_AUTH_OK) {
            printf("读取已登录账户失败:%s\n  先跑 sxcl-dl auth login --device-code\n", acct_err);
            return 1;
        }
        if (sxcl_auth_mc_expired(&session, sxcl_auth_now(), 120)) {
            printf("Minecraft 令牌已过期或没有(需要重新走一次登录链):\n");
            printf("  跑 sxcl-dl auth refresh(免密)试试;不行就 sxcl-dl auth login\n");
            return 1;
        }
        if (session.mc.name[0] == '\0' || session.mc.access_token[0] == '\0') {
            printf("这个账户没有可用的 Java 版身份(没买/没查到)。\n");
            return 1;
        }
        char masked[160];
        (void)sxcl_auth_mask_token(session.mc.access_token, masked, sizeof(masked));
        printf("用已登录账户启动:name=%s uuid=%s(Minecraft 令牌 %s)\n", session.mc.name,
               session.mc.uuid, masked);
        have_account = 1;
    }

    sxcl_launch_request req;
    memset(&req, 0, sizeof(req));
    req.game_dir = argv[3];
    req.version_name = argv[2];
    req.java_path = lo.java_path;
    req.memory_mb = lo.memory_mb;
    req.instance = lo.instance;
    req.offline_name = lo.offline;
    if (have_account) {
        req.player_name = session.mc.name;
        req.uuid = session.mc.uuid;
        req.access_token = session.mc.access_token;
        req.user_type = "msa";
        char xuid[32];
        if (session.xbox.xuid != 0) {
            snprintf(xuid, sizeof(xuid), "%llu", (unsigned long long)session.xbox.xuid);
            req.xuid = xuid;
        }
    }
    req.backend = lo.backend;
    req.settings_path = lo.settings;
    req.timeout_ms = lo.timeout_ms;
    req.dry_run = lo.dry_run;
    req.on_line = lo.verbose ? launch_echo_line : NULL;

    /* 启动前"补全文件"(PCL 启动链第 3 步;dry-run 也补 —— 补全在"拼参数/解压 natives"之前,
     * 不补的话自检出来的命令行指着一堆不存在的路径)。引擎参数与其它下载子命令同一个口径:
     * --workers/--rate/哈希缓存 + 现成的传输后端;没有 Qt 后端时自动退化成"只报告"。 */
    sxcl_engine_opts lopts;
    memset(&lopts, 0, sizeof(lopts));
    lopts.workers = o->workers;
    lopts.rate_bps = o->rate;
    lopts.retry_per_source = 2;
    lopts.cache_path = o->cache;
    /* 与 get/version 同一套进度打印(verbose 时逐文件,否则一行计数)。本子命令没有共享的
     * cli_state,就地建一个 —— 它只用 done/failed/verbose 三个计数。 */
    cli_state lst;
    memset(&lst, 0, sizeof(lst));
    lst.verbose = o->verbose;
    lopts.on_progress = on_progress;
    lopts.userdata = &lst;
    int can_complete = 0;
#if defined(SXCL_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    lopts.transport_factory = make_qt_transport;
    can_complete = 1;
#endif
    req.complete_files = can_complete;
    req.engine_opts = can_complete ? &lopts : NULL;
    req.prefer_mirror = o->prefer_mirror; /* --source 的同一口径 */
    /* 资源对象(P0b):命令行默认"只比大小"(PCL 口径)。要强校验就设
     * SXCL_COMPLETE_ASSETS=2,要整块跳过就设 0 —— 验收/排查两档都用得上。 */
    {
        const char *env = getenv("SXCL_COMPLETE_ASSETS");
        const int level = env != NULL ? atoi(env) : 1;
        req.complete_assets = (level >= 0 && level <= 2) ? level : 1;
    }

    sxcl_launch_result res;
    char err[256];
    const int rc = sxcl_launch_run(&req, &res, err, sizeof(err));

    printf("版本: %s   实例: %s\n", req.version_name, lo.instance ? lo.instance : req.version_name);
    if (res.complete_ran) {
        /* 补全的账要看得见:**命中已有**那几个就是一个字节都没下的(这才是"齐了不重下"的证据)*/
        printf("启动前补全: 共 %d 件,命中已有 %d 件,下载 %d 件(失败 %d 件),写了 %lld 字节",
               res.complete.files_total, res.complete.files_skipped,
               res.complete.files_downloaded, res.complete.files_failed,
               (long long)res.complete.bytes_done);
        if (res.complete.error[0]) {
            printf("  [!] %s", res.complete.error);
        }
        printf("\n");
    }
    if (res.java_path[0]) {
        if (res.java_version[0]) {
            printf("Java: %s (Java %s)\n", res.java_path, res.java_version);
        } else if (res.java_major > 0) {
            printf("Java: %s (Java %d)\n", res.java_path, res.java_major);
        } else {
            printf("Java: %s (版本未知)\n", res.java_path);
        }
    }
    printf("后端: 要求 %s -> 实际 %s%s\n", res.requested_backend, res.actual_backend,
           res.vulkan_fell_back ? "(Vulkan 被回退)" : "");
    if (res.options_path[0]) {
        printf("options.txt: %s\n", res.options_path);
    }
    if (res.natives_dir[0]) {
        printf("natives: 解出 %d 个文件 -> %s\n", res.natives_count, res.natives_dir);
    }
    if (res.missing[0]) {
        printf("缺东西: %s\n", res.missing);
    }
    if (lo.dry_run) {
        printf("退出码: (dry-run 没起进程)   用时 %.2f s\n", (double)res.elapsed_ms / 1000.0);
    } else {
        printf("退出码: %d   用时 %.2f s%s\n", res.exit_code, (double)res.elapsed_ms / 1000.0,
               res.timed_out ? "  (超时被终止)" : (res.killed_by_client ? "  (按请求终止)" : ""));
    }
    printf("结论: %s\n", res.conclusion_text);
    printf("原因键: %s(%s)\n", res.reason_key, res.reason_name);
    printf("建议: %s\n", res.reason_advice);
    /* 也写进运行日志:导出的日志包里要能直接看到结论,不必再翻 stdout */
    SXCL_LOG_I("launch", "原因键=%s(%s) 建议=%s", res.reason_key, res.reason_name,
               res.reason_advice);
    if (res.artifacts_scanned) {
        if (res.crash_report_path[0] != '\0') {
            printf("崩溃报告: %s(%s,%lld 行)\n", res.crash_report_path,
                   sxcl_crash_encoding_name(res.artifacts.report_encoding), res.crash_report_lines);
        } else {
            printf("崩溃报告: (crash-reports 里没有非空报告)\n");
        }
        if (res.artifacts.latest_log_path[0] != '\0') {
            printf("latest.log: %s(%s,%lld 行%s)\n", res.artifacts.latest_log_path,
                   sxcl_crash_encoding_name(res.artifacts.latest_log_encoding),
                   res.latest_log_lines,
                   res.artifacts.latest_log_truncated ? ",只读了尾部" : "");
        }
    }
    if (rc != 0) {
        fprintf(stderr, "启动失败: %s\n", err);
        return 1;
    }
    if (lo.dry_run) {
        return 0; /* 只准备不启动:没起进程就没有"退出码"可言,准备成功就是成功 */
    }
    return res.exit_code == 0 ? 0 : 1;
}

static int cmd_loader(int argc, char **argv, const cli_opts *opts_in)
{
    if (argc < 6) {
        return usage();   /* loader <kind> <加载器版本> <MC 版本> <游戏目录> */
    }
    const cli_opts *o = opts_in;
    const char *kind_text = argv[2];
    const char *loader_version = argv[3];
    const char *mc_version = argv[4];
    const char *game_dir = argv[5];

    const sxcl_loader_kind kind = sxcl_loader_kind_from_id(kind_text);
    if (kind == SXCL_LOADER_VANILLA) {
        fprintf(stderr, "认不出的加载器: %s(可用: forge / neoforge / fabric / quilt / optifine)\n",
                kind_text);
        return 2;
    }
    if (!sxcl_loader_kind_implemented(kind)) {
        fprintf(stderr, "%s 还没有静默安装实现(只有 Forge / NeoForge / Fabric / OptiFine 有)\n",
                sxcl_loader_kind_name(kind));
        return 2;
    }

    const char *java_opt = NULL;
    const char *instance_opt = NULL;
    const char *installer_opt = NULL;
    const char *maven_mirror = NULL;
    int timeout_ms = 0;
    int no_fallback = 0;
    int extract_install = 0;
    for (int i = 6; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--java") == 0 && v) {
            java_opt = v;
            ++i;
        } else if (strcmp(a, "--instance") == 0 && v) {
            instance_opt = v;
            ++i;
        } else if (strcmp(a, "--installer") == 0 && v) {
            installer_opt = v;
            ++i;
        } else if (strcmp(a, "--timeout") == 0 && v) {
            timeout_ms = atoi(v);
            ++i;
        } else if (strcmp(a, "--maven-mirror") == 0 && v) {
            maven_mirror = v;
            ++i;
        } else if (strcmp(a, "--no-fallback") == 0) {
            no_fallback = 1;
        } else if (strcmp(a, "--extract-install") == 0) {
            extract_install = 1;   /* 跳过安装器 CLI,直接走方式 B(解包 + 重放 processors) */
        } else if (strcmp(a, "--rate") == 0 || strcmp(a, "--workers") == 0 ||
                   strcmp(a, "--conn") == 0 || strcmp(a, "--cache") == 0 ||
                   strcmp(a, "--mirror") == 0 || strcmp(a, "--source") == 0) {
            ++i; /* 通用参数已在 main 里解析,这里只需跳过它的值 */
        } else if (strcmp(a, "--no-cache") == 0 || strcmp(a, "--verbose") == 0) {
            /* 无值参数 */
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }

    /* Java:--java > $JAVA_HOME/bin/java > PATH 里的 java。装 1.17+ 的 Forge 要用 17+,别默认挑到太旧的。 */
    char java_buf[1024];
    const char *java_path = java_opt;
    if (!java_path) {
        const char *home = getenv("JAVA_HOME");
        if (home && home[0]) {
#if defined(_WIN32)
            snprintf(java_buf, sizeof(java_buf), "%s/bin/java.exe", home);
#else
            snprintf(java_buf, sizeof(java_buf), "%s/bin/java", home);
#endif
            java_path = java_buf;
        } else {
            java_path = "java";
        }
    }

    /* 实例名:不指定就按加载器拼一个稳定的名字(Forge 用 maven 的 <MC>-<版本> 形式)。 */
    char instance_buf[256];
    const char *instance = instance_opt;
    if (!instance || !instance[0]) {
        if (kind == SXCL_LOADER_OPTIFINE) {
            snprintf(instance_buf, sizeof(instance_buf), "%s-OptiFine_%s", mc_version, loader_version);
        } else {
            snprintf(instance_buf, sizeof(instance_buf), "%s-%s", mc_version, loader_version);
        }
        instance = instance_buf;
    }

    /* 安装器 jar:--installer > <游戏目录>/loaders/<加载器>-<MC>-<版本>-installer.jar > 自己下。 */
    char installer_buf[1200];
    const char *installer = installer_opt;
    if (!installer || !installer[0]) {
        snprintf(installer_buf, sizeof(installer_buf), "%s/loaders/%s-%s-%s-installer.jar", game_dir,
                 sxcl_loader_kind_id(kind), mc_version, loader_version);
        installer = installer_buf;
        if (!sxcl_fs_exists(installer)) {
            printf("本地没有安装器,用内置下载引擎取官方安装器(带 .sha1 强校验):\n  %s\n", installer);
            if (loader_fetch_installer(o, kind, mc_version, loader_version, installer) != 0) {
                fprintf(stderr, "取安装器失败(也可以自己下好再用 --installer 指过来)\n");
                return 1;
            }
        }
    }

    printf("加载器   : %s %s\n", sxcl_loader_kind_name(kind), loader_version);
    printf("原版版本 : %s\n", mc_version);
    printf("游戏目录 : %s\n", game_dir);
    printf("实例名   : %s\n", instance);
    printf("Java     : %s\n", java_path);
    printf("安装器   : %s\n", installer);
    fflush(stdout);

    signal(SIGINT, on_sigint);
    g_loader_cancel = 0;

    sxcl_loader_install_request req;
    memset(&req, 0, sizeof(req));
    req.game_dir = game_dir;
    req.instance_name = instance;
    req.base_version = mc_version;
    req.kind = kind;
    req.loader_version = loader_version;
    req.installer_jar = installer;
    req.java_path = java_path;
    req.mirror_maven = maven_mirror;
    req.timeout_ms = timeout_ms;
    req.no_fallback = no_fallback;
    req.force_extract_install = extract_install;
    loader_cli_ctx lctx;
    memset(&lctx, 0, sizeof(lctx));
    lctx.o = o;
    lctx.game_dir = game_dir;
    lctx.instance = instance;
    req.on_progress = loader_progress;
    req.is_cancelled = loader_cancelled;
    req.userdata = &lctx;   /* on_line 看 --verbose,on_libraries 要游戏目录 */
    req.on_line = loader_raw_line;
    req.on_libraries = loader_on_libraries;
    req.on_mappings = loader_on_mappings;

    sxcl_loader_install_result res;
    const int rc = sxcl_loader_install(&req, &res);
    if (rc < 0) {
        fprintf(stderr, "参数错误: %s\n", res.error[0] ? res.error : "(没有说明)");
        return 2;
    }
    printf("\n阶段=%s 回退方式B=%s 沙箱=%s 进度=%d%% 安装器退出码=%d\n",
           sxcl_loader_fail_stage_name(res.fail_stage), res.used_fallback ? "是" : "否",
           res.used_sandbox ? "是" : "否", res.percent, res.exit_code);
    if (!res.ok) {
        fprintf(stderr, "%s 安装失败: %s\n", sxcl_loader_kind_name(kind), res.error);
        return 1;
    }
    printf("%s 安装完成: %s\n", sxcl_loader_kind_name(kind), res.version_dir);
    return 0;
}

#if defined(_WIN32)
/* Windows 上 CRT 给 main 的 argv 是按**当前 ANSI 代码页**解码的,中文实参(实例名、路径)会变乱码。
 * 这里用 GetCommandLineW + CommandLineToArgvW 拿到 UTF-16,再转成 UTF-8 重建 argv ——
 * 只影响 CLI 自身;GUI 直接调 sxcl_* 接口本来就走 UTF-8,不受影响。 */
static void rebuild_argv_utf8(int *argc_io, char ***argv_io)
{
    static char storage[8192];
    static char *utf8_argv[128];
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        return;
    }
    size_t used = 0;
    int n = 0;
    for (int i = 0; i < wargc && n < (int)(sizeof(utf8_argv) / sizeof(utf8_argv[0])) - 1; ++i) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (need <= 0 || used + (size_t)need > sizeof(storage)) {
            break;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, storage + used, need, NULL, NULL);
        utf8_argv[n++] = storage + used;
        used += (size_t)need;
    }
    LocalFree(wargv);
    if (n > 0) {
        utf8_argv[n] = NULL;
        *argc_io = n;
        *argv_io = utf8_argv;
    }
}
#endif

/* ── auth 子命令:正版登录的端到端人工验收入口 ──
 *
 *   sxcl-dl auth login [--device-code] [--client-id ID] [--tenant T] [--no-browser]
 *   sxcl-dl auth status | refresh | logout | bedrock | help
 *
 * 纪律:**绝不打印 token 明文** —— 只打前 6 位 + 过期时间(见 sxcl_auth_mask_token)。
 */
typedef struct auth_cli_ctx {
    int verbose;
    int got_user_code;
    char token_file[SXCL_AUTH_STORE_PATH_MAX];
    char settings_file[SXCL_AUTH_STORE_PATH_MAX];
} auth_cli_ctx;

static void auth_cli_on_user_code(void *userdata, const char *user_code, const char *verification_uri,
                                  const char *message)
{
    auth_cli_ctx *ctx = (auth_cli_ctx *)userdata;
    ctx->got_user_code = 1;
    printf("\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("  请用浏览器打开：%s\n", verification_uri ? verification_uri : "(未给出)");
    printf("  然后输入这个代码：\n\n");
    printf("        %s\n\n", user_code ? user_code : "(未给出)");
    if (message && message[0]) {
        printf("  服务端提示：%s\n", message);
    }
    printf("  输入完成后本程序会自动继续（正在轮询，请勿关闭）。\n");
    printf("════════════════════════════════════════════════════════════\n\n");
    fflush(stdout);
}

static void auth_cli_on_device_code_info(void *userdata, int interval_seconds, int expires_in_seconds)
{
    (void)userdata;
    printf("  设备码有效期：%d 秒（约 %d 分钟）；轮询间隔：%d 秒（按服务端要求，server 要求 slow_down 时自动加大）\n",
           expires_in_seconds, expires_in_seconds / 60, interval_seconds);
    fflush(stdout);
}

static void auth_cli_on_status(void *userdata, const char *message)
{
    (void)userdata;
    printf("  · %s\n", message ? message : "");
    fflush(stdout);
}

static void auth_cli_on_open_url(void *userdata, const char *url)
{
    (void)userdata;
    printf("  授权链接（如果浏览器没自动打开，请手动复制）：\n    %s\n", url ? url : "");
    fflush(stdout);
}

/* 建一个主线程用的传输后端(CLI 的 HTTPS 走 Qt Network;见根 CMakeLists 的说明) */
static sxcl_transport *auth_cli_transport(void)
{
#if defined(SXCL_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    return sxcl_transport_qt_create();
#else
    return NULL;
#endif
}

static void auth_cli_fill_ctx(auth_cli_ctx *ctx, int argc, char **argv)
{
    memset(ctx, 0, sizeof(*ctx));
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_store_default_path(ctx->token_file, sizeof(ctx->token_file), err, sizeof(err)) !=
        SXCL_AUTH_OK) {
        ctx->token_file[0] = '\0';
    }
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    if (sxcl_auth_config_dir(dir, sizeof(dir), err, sizeof(err)) == SXCL_AUTH_OK) {
        (void)snprintf(ctx->settings_file, sizeof(ctx->settings_file), "%s/settings.txt", dir);
    }
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--verbose") == 0) {
            ctx->verbose = 1;
        } else if (strcmp(a, "--token-file") == 0 && v) {
            (void)snprintf(ctx->token_file, sizeof(ctx->token_file), "%s", v);
            ++i;
        } else if (strcmp(a, "--settings") == 0 && v) {
            (void)snprintf(ctx->settings_file, sizeof(ctx->settings_file), "%s", v);
            ++i;
        }
    }
}

static const char *auth_cli_opt(int argc, char **argv, const char *name)
{
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], name) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int auth_cli_has(int argc, char **argv, const char *name)
{
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int64_t auth_cli_opt_i64(int argc, char **argv, const char *name, int64_t def)
{
    const char *v = auth_cli_opt(argc, argv, name);
    if (v == NULL) {
        return def;
    }
    return (int64_t)atoll(v);
}

static void auth_cli_print_time(const char *label, int64_t when)
{
    if (when <= 0) {
        printf("  %s：未知\n", label);
        return;
    }
    const int64_t now = sxcl_auth_now();
    const int64_t left = when - now;
    char stamp[64];
#if defined(_WIN32)
    struct tm tmv;
    const time_t tt = (time_t)when;
    if (gmtime_s(&tmv, &tt) == 0) {
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", &tmv);
    } else {
        (void)snprintf(stamp, sizeof(stamp), "%lld", (long long)when);
    }
#else
    struct tm tmv;
    const time_t tt = (time_t)when;
    if (gmtime_r(&tt, &tmv) != NULL) {
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", &tmv);
    } else {
        (void)snprintf(stamp, sizeof(stamp), "%lld", (long long)when);
    }
#endif
    printf("  %s：%s（%s %lld 秒）\n", label, stamp, left >= 0 ? "还剩" : "已过期",
           (long long)(left >= 0 ? left : -left));
}

static int auth_cli_save(const auth_cli_ctx *ctx, const sxcl_auth_session *session)
{
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const sxcl_auth_store_kind kind = sxcl_auth_store_backend();
    if (sxcl_auth_store_save(ctx->token_file, session, err, sizeof(err)) != SXCL_AUTH_OK) {
        printf("  [失败] 令牌加密落盘失败：%s\n", err);
        return 1;
    }
    printf("  · 令牌已加密保存到：%s（后端：%s）\n", ctx->token_file,
           sxcl_auth_store_kind_name(kind));
    return 0;
}

static int auth_cli_login(int argc, char **argv)
{
    auth_cli_ctx ctx;
    auth_cli_fill_ctx(&ctx, argc, argv);
    sxcl_transport *tr = auth_cli_transport();
    if (tr == NULL) {
        printf("错误：没有可用的传输后端（本构建没编 Qt Network），无法联网登录。\n");
        return 1;
    }
    sxcl_settings *settings = sxcl_settings_open(ctx.settings_file);
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.settings = settings;
    opts.client_id = auth_cli_opt(argc, argv, "--client-id");
    opts.tenant = auth_cli_opt(argc, argv, "--tenant");
    opts.device_code = auth_cli_has(argc, argv, "--device-code") ? 1 : 0;
    opts.no_browser = auth_cli_has(argc, argv, "--no-browser") ? 1 : 0;
    opts.http_timeout_ms = auth_cli_opt_i64(argc, argv, "--http-timeout", 30000);
    opts.loopback_timeout_ms = auth_cli_opt_i64(argc, argv, "--wait", 300000);
    /* 设备码流默认给足 15 分钟(微软自己的 expires_in 就是 900 秒) */
    opts.poll_timeout_ms = auth_cli_opt_i64(argc, argv, "--poll-ms", 0);
    opts.cb.on_user_code = auth_cli_on_user_code;
    opts.cb.on_device_code_info = auth_cli_on_device_code_info;
    opts.cb.on_status = auth_cli_on_status;
    opts.cb.on_open_url = auth_cli_on_open_url;
    opts.cb.userdata = &ctx;

    char cidbuf[SXCL_AUTH_CLIENT_ID_MAX];
    char tenbuf[SXCL_AUTH_TENANT_MAX];
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_resolve_client_id(opts.client_id, settings, cidbuf, sizeof(cidbuf)) ==
        SXCL_AUTH_OK) {
        opts.client_id = cidbuf;
    }
    if (sxcl_auth_resolve_tenant(opts.tenant, settings, tenbuf, sizeof(tenbuf)) == SXCL_AUTH_OK) {
        opts.tenant = tenbuf;
    }
    printf("SXCL-C 正版登录（%s）\n",
           opts.device_code ? "设备码流" : "授权码流 + PKCE + 环回重定向");
    printf("  client_id = %s\n", opts.client_id ? opts.client_id : "(空)");
    printf("  租户段    = %s（端点 https://login.microsoftonline.com/%s/oauth2/v2.0/…）\n",
           opts.tenant ? opts.tenant : "(空)", opts.tenant ? opts.tenant : "");
    printf("  提示：授权页面上出现的是我们注册的应用名“Silent X Craft Launcher”。\n\n");
    fflush(stdout);

    sxcl_auth_session session;
    const int rc = sxcl_auth_login(tr, &opts, &session, err, sizeof(err));
    if (rc != SXCL_AUTH_OK) {
        printf("\n[失败] 登录没有完成（返回码 %d）\n  %s\n", rc, err);
        /* **关键**:微软那段(token/refresh)是用户花时间换来的,后面任何一跳失败都不该把它丢掉。
         * 落盘之后"修好配置再重试"只要 sxcl-dl auth refresh,不用再让用户输一次设备码。
         * (第一版只在整链成功时才存,结果第 ⑥ 跳失败把用户的一次登录白扔了 —— 实测踩过。) */
        if (session.ms.refresh_token[0] != '\0') {
            printf("\n  注意：微软登录本身是成功的，凭据已保存 —— 修好问题后用下面的命令重试，**不用再输一次设备码**：\n");
            printf("      sxcl-dl auth refresh\n");
            (void)auth_cli_save(&ctx, &session);
        }
        if (rc == SXCL_AUTH_ERR_XBOX && session.last_error_xerr != 0) {
            char human[SXCL_AUTH_MESSAGE_MAX];
            (void)sxcl_auth_xsts_error_message((uint64_t)session.last_error_xerr, human, sizeof(human));
            printf("  XSTS 原因：%s\n", human);
        }
        printf("\n  如果报的是 AADSTS 系列（应用配置问题）：\n");
        printf("    · AADSTS50059 / AADSTS500011 / AADSTS700016 → 应用的“受支持的帐户类型”\n");
        printf("      与租户段（默认 consumers）没对上；改 Azure 设置或用 --tenant 指定。\n");
        printf("    · AADSTS70002 → 应用没被标记成“公共客户端/移动应用”（设备码流与环回流都会被拒）。\n");
        printf("    · AADSTS900971 → 重定向地址没注册（要加 http://localhost）。\n");
        printf("    · AADSTS7000012 → refresh token 是另一个租户段换来的（登录与续期必须同一个租户段）。\n");
        printf("    · AADSTS70008 / AADSTS700082 → refresh token 过期，重新登录即可。\n");
        if (settings != NULL) {
            sxcl_settings_free(settings);
        }
        tr->destroy(tr->ctx);
        return 1;
    }

    printf("\n[成功] 登录完成（client_id=%s，租户段=%s）\n", session.client_id, session.tenant);
    printf("  账号      ：%s\n", session.account_name[0] ? session.account_name : "(未取到)");
    printf("  Xbox uhs  ：%s\n", session.xbox.user_hash);
    printf("  Xbox XUID ：%llu\n", (unsigned long long)session.xbox.xuid);
    printf("  Java 版   ：name=%s  uuid=%s\n", session.mc.name, session.mc.uuid);
    printf("  权益      ：%s（mcstore 条目数 %d）\n",
           session.mc.entitlement_count > 0 ? "拥有 Java 版权益" : "mcstore 里没有条目",
           session.mc.entitlement_count);
    char masked[160];
    (void)sxcl_auth_mask_token(session.ms.access_token, masked, sizeof(masked));
    printf("  微软令牌  ：%s\n", masked);
    auth_cli_print_time("微软令牌过期", session.ms.expires_at);
    (void)sxcl_auth_mask_token(session.mc.access_token, masked, sizeof(masked));
    printf("  MC 令牌   ：%s\n", masked);
    auth_cli_print_time("MC 令牌过期", session.mc.expires_at);
    (void)sxcl_auth_mask_token(session.ms.refresh_token, masked, sizeof(masked));
    printf("  refresh   ：%s（已加密落盘，续期用）\n", masked);
    (void)auth_cli_save(&ctx, &session);
    if (settings != NULL) {
        sxcl_settings_free(settings);
    }
    tr->destroy(tr->ctx);
    return 0;
}

static int auth_cli_load(const auth_cli_ctx *ctx, sxcl_auth_session *session)
{
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_store_load(ctx->token_file, session, err, sizeof(err)) != SXCL_AUTH_OK) {
        printf("读取令牌失败：%s\n", err);
        return 1;
    }
    return 0;
}

static int auth_cli_status(int argc, char **argv)
{
    auth_cli_ctx ctx;
    auth_cli_fill_ctx(&ctx, argc, argv);
    const sxcl_auth_store_kind kind = sxcl_auth_store_backend();
    printf("SXCL-C 正版登录状态\n");
    printf("  令牌文件  ：%s（%s）\n", ctx.token_file,
           sxcl_auth_store_exists(ctx.token_file) ? "存在" : "不存在（还没登录过）");
    printf("  加密后端  ：%s\n", sxcl_auth_store_kind_name(kind));
    printf("  后端说明  ：%s\n", sxcl_auth_store_kind_note(kind));
    sxcl_settings *settings = sxcl_settings_open(ctx.settings_file);
    char cid[SXCL_AUTH_CLIENT_ID_MAX];
    char ten[SXCL_AUTH_TENANT_MAX];
    (void)sxcl_auth_resolve_client_id(NULL, settings, cid, sizeof(cid));
    (void)sxcl_auth_resolve_tenant(NULL, settings, ten, sizeof(ten));
    printf("  client_id ：%s\n", cid);
    printf("  租户段    ：%s\n", ten);
    if (!sxcl_auth_store_exists(ctx.token_file)) {
        printf("\n  还没有登录过。先跑：sxcl-dl auth login --device-code\n");
        if (settings != NULL) {
            sxcl_settings_free(settings);
        }
        return 0;
    }
    sxcl_auth_session session;
    if (auth_cli_load(&ctx, &session) != 0) {
        if (settings != NULL) {
            sxcl_settings_free(settings);
        }
        return 1;
    }
    char masked[160];
    printf("\n  账号      ：%s\n", session.account_name[0] ? session.account_name : "(未取到)");
    printf("  Java 版   ：name=%s  uuid=%s\n", session.mc.name[0] ? session.mc.name : "(空)",
           session.mc.uuid[0] ? session.mc.uuid : "(空)");
    printf("  Java 权益 ：%s（%d 条）\n",
           session.mc.entitlement_count > 0 ? "有" : "没有/没查到", session.mc.entitlement_count);
    printf("  基岩联机链：%s（%d 段）\n",
           session.bedrock.chain_count > 0 ? "有" : "没有（跑 sxcl-dl auth bedrock 获取）",
           session.bedrock.chain_count);
    (void)sxcl_auth_mask_token(session.ms.access_token, masked, sizeof(masked));
    printf("  微软令牌  ：%s%s\n", masked,
           sxcl_auth_ms_expired(&session, sxcl_auth_now(), 60) ? "  已过期" : "");
    auth_cli_print_time("微软令牌过期", session.ms.expires_at);
    (void)sxcl_auth_mask_token(session.ms.refresh_token, masked, sizeof(masked));
    printf("  refresh   ：%s\n", masked);
    (void)sxcl_auth_mask_token(session.mc.access_token, masked, sizeof(masked));
    printf("  MC 令牌   ：%s%s\n", masked, sxcl_auth_mc_expired(&session, sxcl_auth_now(), 60) ? "  已过期" : "");
    auth_cli_print_time("MC 令牌过期", session.mc.expires_at);
    if (settings != NULL) {
        sxcl_settings_free(settings);
    }
    return 0;
}

static int auth_cli_refresh(int argc, char **argv)
{
    auth_cli_ctx ctx;
    auth_cli_fill_ctx(&ctx, argc, argv);
    sxcl_auth_session session;
    if (auth_cli_load(&ctx, &session) != 0) {
        return 1;
    }
    if (session.ms.refresh_token[0] == '\0') {
        printf("本地没有 refresh token，免密续期不可能：请重新登录。\n");
        return 1;
    }
    sxcl_transport *tr = auth_cli_transport();
    if (tr == NULL) {
        printf("错误：没有可用的传输后端。\n");
        return 1;
    }
    sxcl_settings *settings = sxcl_settings_open(ctx.settings_file);
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.settings = settings;
    opts.client_id = auth_cli_opt(argc, argv, "--client-id");
    opts.tenant = auth_cli_opt(argc, argv, "--tenant");
    opts.http_timeout_ms = auth_cli_opt_i64(argc, argv, "--http-timeout", 30000);
    opts.cb.on_status = auth_cli_on_status;
    opts.cb.userdata = &ctx;

    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    printf("用 refresh token 免密续期…\n");
    const int rc = sxcl_auth_refresh(tr, &opts, &session, err, sizeof(err));
    if (rc != SXCL_AUTH_OK) {
        printf("\n[失败] 续期没有完成（返回码 %d）\n  %s\n", rc, err);
        if (rc == SXCL_AUTH_ERR_EXPIRED) {
            printf("  refresh token 过期/被撤销 → 需要重新登录（auth login）。\n");
        }
        if (settings != NULL) {
            sxcl_settings_free(settings);
        }
        tr->destroy(tr->ctx);
        return 1;
    }
    printf("[成功] 免密续期完成（没有让用户重新登录）\n");
    char masked[160];
    (void)sxcl_auth_mask_token(session.ms.access_token, masked, sizeof(masked));
    printf("  新微软令牌：%s\n", masked);
    auth_cli_print_time("微软令牌过期", session.ms.expires_at);
    printf("  Java 版   ：name=%s  uuid=%s\n", session.mc.name, session.mc.uuid);
    (void)auth_cli_save(&ctx, &session);
    if (settings != NULL) {
        sxcl_settings_free(settings);
    }
    tr->destroy(tr->ctx);
    return 0;
}

static int auth_cli_logout(int argc, char **argv)
{
    auth_cli_ctx ctx;
    auth_cli_fill_ctx(&ctx, argc, argv);
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int existed = sxcl_auth_store_exists(ctx.token_file);
    if (sxcl_auth_store_clear(ctx.token_file, err, sizeof(err)) != SXCL_AUTH_OK) {
        printf("退出登录失败：%s\n", err);
        return 1;
    }
    printf("已退出登录：%s（%s）\n", ctx.token_file,
           existed ? "令牌文件已删除" : "本来就没有令牌文件");
    /* 顺手把盐文件也留着(它不含令牌,删了反而会让下次的密钥变),只说明一下 */
    return 0;
}

static int auth_cli_bedrock(int argc, char **argv)
{
    auth_cli_ctx ctx;
    auth_cli_fill_ctx(&ctx, argc, argv);
    sxcl_auth_session session;
    if (auth_cli_load(&ctx, &session) != 0) {
        return 1;
    }
    sxcl_transport *tr = auth_cli_transport();
    if (tr == NULL) {
        printf("错误：没有可用的传输后端。\n");
        return 1;
    }
    sxcl_settings *settings = sxcl_settings_open(ctx.settings_file);
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.settings = settings;
    opts.client_id = auth_cli_opt(argc, argv, "--client-id");
    opts.tenant = auth_cli_opt(argc, argv, "--tenant");
    opts.http_timeout_ms = auth_cli_opt_i64(argc, argv, "--http-timeout", 30000);
    opts.cb.on_status = auth_cli_on_status;
    opts.cb.userdata = &ctx;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    printf("跑基岩链（XSTS 中继方 = https://multiplayer.minecraft.net/）…\n");
    const int rc = sxcl_auth_session_bedrock(tr, &opts, &session, err, sizeof(err));
    if (rc != SXCL_AUTH_OK) {
        printf("\n[失败] 基岩链没有完成（返回码 %d）\n  %s\n", rc, err);
        if (rc == SXCL_AUTH_ERR_NO_ENTITLE) {
            printf("  结论：这个账号**没有基岩版权益**（Java 版与基岩版是分开购买的，不能互相推断）。\n");
        }
        if (settings != NULL) {
            sxcl_settings_free(settings);
        }
        tr->destroy(tr->ctx);
        return 1;
    }
    printf("[成功] 基岩联机证书链已取回：%d 段\n", session.bedrock.chain_count);
    printf("  身份公钥  ：%s\n", session.bedrock.identity_public_key);
    printf("  基岩权益  ：%s（与 Java 版权益各自独立判断）\n",
           session.bedrock.entitled ? "有" : "没有");
    printf("  说明：链只打印长度与前缀，不打印全文（它是凭据）。\n");
    for (int i = 0; i < session.bedrock.chain_count; ++i) {
        char masked[160];
        (void)sxcl_auth_mask_token(session.bedrock.chain[i], masked, sizeof(masked));
        printf("    chain[%d] = %s\n", i, masked);
    }
    (void)auth_cli_save(&ctx, &session);
    if (settings != NULL) {
        sxcl_settings_free(settings);
    }
    tr->destroy(tr->ctx);
    return 0;
}

static int cmd_auth(int argc, char **argv)
{
    if (argc < 3) {
        printf("sxcl-dl auth <login|status|refresh|logout|bedrock> [选项]\n"
               "  login   [--device-code] [--client-id ID] [--tenant T] [--no-browser]\n"
               "          [--wait MS] [--poll-ms MS] [--http-timeout MS] [--token-file PATH]\n"
               "          [--settings PATH]\n"
               "          └ 授权码流（PKCE + 127.0.0.1 环回）；--device-code 走设备码兜底\n"
               "  status  [--token-file PATH]     看当前登录状态（只打令牌前缀与过期时间）\n"
               "  refresh [--client-id ID]        用 refresh token 免密续期并重跑后半条链\n"
               "  logout  [--token-file PATH]     删除加密令牌文件\n"
               "  bedrock [--token-file PATH]     跑基岩链（multiplayer.minecraft.net）\n"
               "\n"
               "配置优先级：命令行 > 环境变量 SXCL_AUTH_CLIENT_ID / SXCL_AUTH_TENANT >\n"
               "           设置项 auth.client_id / auth.tenant > 内置默认\n"
               "本命令**永不打印 token 明文**。\n");
        return 0;
    }
    const char *sub = argv[2];
    if (strcmp(sub, "login") == 0) {
        return auth_cli_login(argc, argv);
    }
    if (strcmp(sub, "status") == 0) {
        return auth_cli_status(argc, argv);
    }
    if (strcmp(sub, "refresh") == 0) {
        return auth_cli_refresh(argc, argv);
    }
    if (strcmp(sub, "logout") == 0) {
        return auth_cli_logout(argc, argv);
    }
    if (strcmp(sub, "bedrock") == 0) {
        return auth_cli_bedrock(argc, argv);
    }
    return cmd_auth(1, NULL);
}

/* ══════════════════════ java:官方 JRE 运行时(预置/一次装齐) ══════════════════════ */

/* ── 传输后端是**编译期**定的:没有 Qt,整个 java 下载子系统就整体不编 ──
 *
 * 以前这里是"函数体照编、中途 #else 里 return 1",于是 return 之后的每一行都成了
 * 不可达代码 —— MSVC 在 /W4 /WX 下用 C4702 把 sxcl-dl 直接打挂
 * (README「CI 一次就抓出 5 个本机 MSVC 完全掩盖的真实缺陷」第 5 条记的就是它)。
 *
 * 改成"整体 #if 掉"之后:
 *   * 无 Qt 配置里**不存在**那条不可达分支 —— C4702 是从根上没有了,不是靠 pragma 静音;
 *   * java_cli_ctx / java_cli_progress / java_human_mb 这几个只服务于下载的辅助函数
 *     也一起不编:否则它们会变成"没人引用的 static 函数",被 C4505 同样在 /WX 下打挂;
 *   * 三个子命令在两种配置下**都存在**(调用点一行都不用改),无 Qt 时只回一条如实的错误。
 */
#if defined(SXCL_HAVE_QT_TRANSPORT)

/* 进度:每个组件单独一行,组件内原地刷新(百分比/速度/字节/当前文件)。
 * 回调可能来自引擎的多个工作线程,所以只 echo 不做重活。 */
typedef struct java_cli_ctx {
    int last_index;
    int printed_header;
    const char *root;
} java_cli_ctx;

static void java_cli_progress(void *ud, const sxcl_java_runtime_progress *p)
{
    java_cli_ctx *ctx = (java_cli_ctx *)ud;
    if (ctx == NULL || p == NULL) {
        return;
    }
    if (p->component_index != ctx->last_index) {
        if (ctx->last_index >= 0) {
            printf("\n");
        }
        ctx->last_index = p->component_index;
        printf("  [%d/%d] %s v%s\n", p->component_index + 1, p->component_total,
               p->component ? p->component : "?", p->version ? p->version : "?");
    }
    if (p->component_skipped) {
        printf("        已装好 -> 整个跳过(没联网、没下载)\n");
        fflush(stdout);
        return;
    }
    printf("\r        %-9s %3d%% | 整体 %3d%% | %7.2f/%7.2f MB | %s          ",
           p->stage_id ? p->stage_id : "?", p->percent, p->overall_percent,
           (double)p->bytes_done / (1024.0 * 1024.0), (double)p->bytes_total / (1024.0 * 1024.0),
           p->current ? p->current : "");
    fflush(stdout);
}

static const char *java_human_mb(int64_t bytes, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%8.2f MB", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

static int cmd_java_list(const cli_opts *o, const char *platform)
{
    sxcl_transport_qt_bootstrap();
    sxcl_java_runtime_query q;
    memset(&q, 0, sizeof(q));
    q.platform = platform;
    q.manifest_relaxed_mirror = 1;
    q.transport_factory = make_qt_transport;
    sxcl_java_runtime_component list[32];
    char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
    err[0] = '\0';
    const int n = sxcl_java_runtime_list(&q, list, 32, err, sizeof(err));
    if (n < 0) {
        fprintf(stderr, "取清单失败: %s\n", err);
        return 1;
    }
    printf("平台 %s:%d 个组件(已排除 minecraft-java-exe 与 gamma-snapshot)\n",
           sxcl_java_runtime_platform_key(), n);
    printf("  %-30s %-24s %-6s %s\n", "组件", "版本", "主版本", "清单");
    for (int i = 0; i < n; ++i) {
        printf("  %-30s %-24s %-6d %s\n", list[i].component, list[i].version, list[i].major,
               list[i].manifest_url);
    }
    (void)o;
    return 0;
}

static int cmd_java_plan(const cli_opts *o, const char *platform, const char *root,
                         const char *const *mc_versions, size_t mc_count, int include_newest)
{
    sxcl_transport_qt_bootstrap();
    sxcl_java_runtime_plan_request req;
    memset(&req, 0, sizeof(req));
    req.platform = platform;
    req.target_root = root;
    req.mc_versions = mc_versions;
    req.mc_version_count = mc_count;
    req.include_newest = include_newest;
    req.measure = 1;
    req.skip_installed = 0;
    req.transport_factory = make_qt_transport;
    sxcl_java_runtime_plan_item items[SXCL_JAVA_RUNTIME_PRESET_MAX];
    char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
    err[0] = '\0';
    const int n = sxcl_java_runtime_plan(&req, items, SXCL_JAVA_RUNTIME_PRESET_MAX, err, sizeof(err));
    if (n < 0) {
        fprintf(stderr, "出计划失败: %s\n", err);
        return 1;
    }
    printf("计划:%d 个组件(平台 %s,根目录 %s)\n", n, sxcl_java_runtime_platform_key(), root);
    printf("  %-24s %-22s %-5s %10s %7s %-8s %s\n", "组件", "版本", "Java", "大小", "文件", "状态",
           "落点");
    int64_t total = 0, need = 0;
    for (int i = 0; i < n; ++i) {
        char mb[32];
        total += items[i].bytes;
        if (!items[i].installed) {
            need += items[i].bytes;
        }
        printf("  %-24s %-22s %-5d %s %7d %-8s %s\n", items[i].component, items[i].version,
               items[i].major, java_human_mb(items[i].bytes, mb, sizeof(mb)), (int)items[i].files,
               items[i].installed ? "已装好" : "待安装", items[i].target_dir);
    }
    {
        char mb1[32], mb2[32];
        printf("  合计 %s;其中还要下载 %s\n", java_human_mb(total, mb1, sizeof(mb1)),
               java_human_mb(need, mb2, sizeof(mb2)));
    }
    (void)o;
    return 0;
}

static int cmd_java_preset(const cli_opts *o, const char *platform, const char *root,
                           const char *const *mc_versions, size_t mc_count, int include_newest,
                           int force)
{
    java_cli_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.last_index = -1;
    ctx.root = root;

    sxcl_java_runtime_preset_request req;
    memset(&req, 0, sizeof(req));
    req.platform = platform;
    req.target_root = root;
    req.mc_versions = mc_versions;
    req.mc_version_count = mc_count;
    req.include_newest = include_newest;
    req.force = force;
    req.use_mirror = o->prefer_mirror;
    req.on_progress = java_cli_progress;
    req.ud = &ctx;

    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = o->workers;
    opts.rate_bps = o->rate;
    opts.retry_per_source = 2;
    opts.cache_path = o->cache;
    sxcl_transport_qt_bootstrap();
    opts.transport_factory = make_qt_transport;
    req.transport_factory = make_qt_transport;
    req.engine_opts = &opts;

    const double t0 = (double)clock() / CLOCKS_PER_SEC;
    sxcl_java_runtime_preset_result res;
    const int rc = sxcl_java_runtime_install_preset(&req, &res);
    const double t1 = (double)clock() / CLOCKS_PER_SEC;
    if (ctx.last_index >= 0) {
        printf("\n");
    }

    printf("\n结果:计划 %d 个 · 新装 %d 个 · 已装跳过 %d 个 · 失败 %d 个\n", (int)res.planned,
           (int)res.installed, (int)res.skipped, (int)res.failed);
    {
        char mb1[32], mb2[32], mb3[32];
        printf("字节:计划下载 %s · 实下 %s · 落盘占用 %s\n",
               java_human_mb(res.planned_bytes, mb1, sizeof(mb1)),
               java_human_mb(res.downloaded_bytes, mb2, sizeof(mb2)),
               java_human_mb(res.on_disk_bytes, mb3, sizeof(mb3)));
        if (res.free_before >= 0) {
            char mb4[32];
            printf("开下之前磁盘可用 %s(位置 %s)\n", java_human_mb(res.free_before, mb4, sizeof(mb4)),
                   root);
        } else {
            printf("开下之前磁盘可用:拿不到(如实报未知,没有拦)\n");
        }
    }
    printf("  %-24s %-22s %-7s %-9s %10s %10s %8s\n", "组件", "版本", "状态", "文件(下/跳过/失败)",
           "下载", "落盘", "耗时");
    for (size_t i = 0; i < res.count; ++i) {
        const sxcl_java_runtime_item_result *it = &res.items[i];
        char mb[32];
        const char *state = it->code != 0 ? "失败" : (it->skipped ? "已装跳过" : "新装");
        char files[64];
        snprintf(files, sizeof(files), "%d/%d/%d", (int)it->files_done, (int)it->files_skipped,
                 (int)it->files_failed);
        printf("  %-24s %-22s %-7s %-17s %10.2f %s %7.1fs\n", it->component, it->version, state,
               files, (double)it->bytes_done / (1024.0 * 1024.0),
               java_human_mb(it->bytes_on_disk, mb, sizeof(mb)), it->seconds);
        printf("      落点 %s\n", it->java_home);
        if (it->code != 0 && it->error[0]) {
            printf("      失败原因 %s\n", it->error);
        }
    }
    printf("总耗时 %.1f 秒\n", t1 - t0);
    if (rc != SXCL_JAVA_RUNTIME_OK) {
        fprintf(stderr, "java preset 失败 [%s]: %s\n", sxcl_java_runtime_code_name(rc),
                res.error[0] ? res.error : "(没有原因)");
        return 1;
    }
    printf("java preset 完成\n");
    return 0;
}

#else /* !SXCL_HAVE_QT_TRANSPORT:没有传输后端,java 的下载类子命令只如实报错 */

/* 这三个壳子存在的唯一理由:让 cmd_java 的调用点在两种配置下**一模一样**。
 * 真正的实现整体在上面的 #if 里,不会编进这个产物 —— 也就没有不可达代码可报。 */
static int cmd_java_no_transport(const char *what)
{
    fprintf(stderr, "本产物没有传输后端(需要 Qt6::Network),%s\n", what);
    fprintf(stderr, "  —— 这是编译期决定的:重新配置时打开 -DSXCL_BUILD_QT_TRANSPORT=ON\n");
    return 1;
}

static int cmd_java_list(const cli_opts *o, const char *platform)
{
    (void)o;
    (void)platform;
    return cmd_java_no_transport("取不了清单");
}

static int cmd_java_plan(const cli_opts *o, const char *platform, const char *root,
                         const char *const *mc_versions, size_t mc_count, int include_newest)
{
    (void)o;
    (void)platform;
    (void)root;
    (void)mc_versions;
    (void)mc_count;
    (void)include_newest;
    return cmd_java_no_transport("取不了清单");
}

static int cmd_java_preset(const cli_opts *o, const char *platform, const char *root,
                           const char *const *mc_versions, size_t mc_count, int include_newest,
                           int force)
{
    (void)o;
    (void)platform;
    (void)root;
    (void)mc_versions;
    (void)mc_count;
    (void)include_newest;
    (void)force;
    return cmd_java_no_transport("装不了运行时");
}

#endif /* SXCL_HAVE_QT_TRANSPORT */

/* java <list|plan|preset> [--root DIR] [--mc 版本]... [--no-newest] [--force] */
static int cmd_java(int argc, char **argv, const cli_opts *o)
{
    if (argc < 3) {
        return usage();
    }
    const char *sub = argv[2];
    const char *platform = NULL;
    const char *root = NULL;
    const char *mcs[8];
    size_t mc_count = 0;
    int include_newest = 1;
    int force = 0;
    /* main() 已经把全局下载参数(workers/rate/source/…)解析进 o 了,这里见到它们
     * 只跳过 —— 否则 "java preset --workers 8" 会被当成"未知参数"直接用法报错。 */
    static const char *const kGlobalWithValue[] = { "--rate", "--workers", "--source", "--mirror",
                                                    "--limit", "--asset-mirror", "--cache" };
    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int consumed = 0;
        for (size_t gi = 0; gi < sizeof(kGlobalWithValue) / sizeof(kGlobalWithValue[0]); ++gi) {
            if (strcmp(a, kGlobalWithValue[gi]) == 0 && v != NULL) {
                ++i;
                consumed = 1;
                break;
            }
        }
        if (consumed) {
            continue;
        }
        if (strcmp(a, "--verbose") == 0 || strcmp(a, "--skip-assets") == 0 ||
            strcmp(a, "--no-cache") == 0) {
            continue;
        }
        if (strcmp(a, "--platform") == 0 && v) {
            platform = v;
            ++i;
        } else if (strcmp(a, "--root") == 0 && v) {
            root = v;
            ++i;
        } else if (strcmp(a, "--mc") == 0 && v) {
            if (mc_count < sizeof(mcs) / sizeof(mcs[0])) {
                mcs[mc_count++] = v;
            }
            ++i;
        } else if (strcmp(a, "--no-newest") == 0) {
            include_newest = 0;
        } else if (strcmp(a, "--force") == 0) {
            force = 1;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }
    if (platform == NULL) {
        platform = sxcl_java_runtime_platform_key();
    }
    if (root == NULL) {
        static char root_buf[SXCL_JAVA_RUNTIME_PATH_MAX];
        char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
        err[0] = '\0';
        if (sxcl_java_runtime_default_root(root_buf, sizeof(root_buf), err, sizeof(err)) !=
            SXCL_JAVA_RUNTIME_OK) {
            fprintf(stderr, "拿不到 runtime 根目录: %s(用 --root DIR 指定一个)\n", err);
            return 1;
        }
        root = root_buf;
        printf("没有给 --root,用默认运行时目录: %s\n", root);
    }
    if (strcmp(sub, "list") == 0) {
        return cmd_java_list(o, platform);
    }
    if (strcmp(sub, "plan") == 0) {
        return cmd_java_plan(o, platform, root, mcs, mc_count, include_newest);
    }
    if (strcmp(sub, "preset") == 0) {
        return cmd_java_preset(o, platform, root, mcs, mc_count, include_newest, force);
    }
    return usage();
}


/* ── crash 子命令:直接体检游戏目录里的取证文件(界面"查看详情"的命令行版)──
 *
 * 与启动层用的是**同一个**入口(sxcl_launch_scan_artifacts),所以命令行看到的
 * 原因键与界面里显示的一模一样 —— 排查时不会出现"CLI 说 A、界面说 B"。 */
static int cmd_crash(int argc, char **argv)
{
    const char *game_dir = argv[2];
    const char *lang = NULL;
    int want_debug = 1;
    sxcl_log_summary s;
    sxcl_crash_evidence ev;
    char err[SXCL_CRASH_ERROR_MAX];
    long long fed = 0;
    int i = 0;

    for (i = 3; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--lang") == 0 && v) {
            lang = v;
            ++i;
        } else if (strcmp(a, "--no-debug") == 0) {
            want_debug = 0;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }
    err[0] = '\0';
    sxcl_log_summary_init(&s);
    memset(&ev, 0, sizeof(ev));
    fed = sxcl_launch_scan_artifacts(game_dir, &s, &ev,
                                     want_debug ? SXCL_CRASH_SCAN_ALL : SXCL_CRASH_SCAN_REPORTS |
                                                                          SXCL_CRASH_SCAN_LATEST,
                                     0, err, sizeof(err));
    if (fed < 0) {
        fprintf(stderr, "崩溃取证失败: %s\n", err);
        return 1;
    }
    printf("游戏目录   : %s\n", game_dir);
    if (ev.report_path[0] != '\0') {
        printf("崩溃报告   : %s\n", ev.report_path);
        printf("             编码 %s(%s)· %lld 行 · crash-reports 里非空报告 %d 份\n",
               ev.report_encoding, sxcl_crash_encoding_name(ev.report_encoding),
               ev.report_lines, ev.reports_total);
        if (ev.report_description[0] != '\0') {
            printf("             Description: %s\n", ev.report_description);
        }
    } else {
        printf("崩溃报告   : (crash-reports 里没有非空报告)\n");
    }
    if (ev.latest_log_path[0] != '\0') {
        printf("latest.log : %s\n", ev.latest_log_path);
        printf("             编码 %s(%s)· 共 %lld 行%s · 喂入分析 %lld 行\n",
               ev.latest_log_encoding, sxcl_crash_encoding_name(ev.latest_log_encoding),
               ev.latest_log_lines, ev.latest_log_truncated ? "(只读了尾部)" : "",
               ev.latest_log_fed);
    } else {
        printf("latest.log : (没有这个文件)\n");
    }
    if (ev.used_debug_log) {
        printf("debug.log  : %s(编码 %s)· %lld 行 —— latest.log 缺失/为空,退到了它\n",
               ev.debug_log_path, sxcl_crash_encoding_name(ev.debug_log_encoding),
               ev.debug_log_lines);
    }
    if (ev.used_legacy_log) {
        printf("client-*.log: %s(编码 %s)· %lld 行 —— 1.7 之前的老版本命名,同样认\n",
               ev.legacy_log_path, sxcl_crash_encoding_name(ev.legacy_log_encoding),
               ev.legacy_log_lines);
    }
    printf("喂入分析   : %lld 行(与 stdout 同一套规则、同一张原因表)\n", fed);
    printf("结论       : %s\n", sxcl_log_conclusion_name(s.conclusion));
    printf("原因键     : %s\n", s.reason_key);
    printf("原因       : %s\n", sxcl_log_reason_name(s.reason, lang));
    printf("建议       : %s\n", sxcl_log_reason_advice(s.reason, lang));
    if (s.crash_reason[0] != '\0') {
        printf("日志首因   : %s\n", s.crash_reason);
    }
    return 0;
}

/* ── logs 子命令:一键导出日志包(与我们自己的 zip 读侧互逆)── */
static int cmd_logs(int argc, char **argv)
{
    sxcl_logs_export_request req;
    sxcl_logs_export_result res;
    char err[SXCL_LOGS_EXPORT_ERROR_MAX];
    const char *sub = (argc >= 3) ? argv[2] : NULL;
    int i = 0;

    if (sub == NULL || strcmp(sub, "export") != 0) {
        return usage();
    }
    if (argc < 5) {
        return usage(); /* logs export <游戏目录> <输出 zip> */
    }
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    req.game_dir = argv[3];
    req.out_zip = argv[4];
    for (i = 5; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--log-dir") == 0 && v) {
            req.log_dir = v;
            ++i;
        } else if (strcmp(a, "--debug") == 0) {
            req.include_debug_log = 1;
        } else if (strcmp(a, "--max-logs") == 0 && v) {
            req.max_launcher_logs = atoi(v);
            ++i;
        } else if (strcmp(a, "--max-reports") == 0 && v) {
            req.max_crash_reports = atoi(v);
            ++i;
        } else if (strcmp(a, "--max-bytes") == 0 && v) {
            req.max_file_bytes = (size_t)strtoull(v, NULL, 10);
            ++i;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }
    if (sxcl_logs_export(&req, &res, err, sizeof(err)) != 0) {
        fprintf(stderr, "导出失败: %s\n", err);
        return 1;
    }
    printf("导出完成   : %s\n", res.out_path);
    printf("打包条目   : %d 个文件 + 说明文件(%d 条);跳过 %d 个;原样 %lld 字节 -> zip %lld 字节\n",
           res.files, res.entries, res.skipped, res.raw_bytes, res.zip_bytes);
    printf("说明文件   : %s(包内条目名)\n", res.manifest_path);
    printf("打码       : token / UUID / 玩家名 / 路径里的用户名 -> ***\n");
    printf("压缩方式   : store(不压缩,任何解压工具都能打开;仓库只有 zip 读侧)\n");
    if (res.note[0] != '\0') {
        printf("注意       : %s\n", res.note);
    }
    return 0;
}

/** 收尾:把游戏输出的缓冲与日志文件冲刷干净(atexit,所有返回路径都覆盖)。 */
static void cli_log_atexit(void)
{
    sxcl_log_shutdown();
}

/* ── mods:模组资源来源层(Modrinth)的端到端验收入口（docs/22 的 A1） ──
 *
 *   sxcl-dl mods search <关键词> [--mc 1.20.1] [--loader fabric] [--limit N]
 *   sxcl-dl mods files <工程 id|短名> [--mc 1.20.1] [--loader fabric]
 *
 * 取 JSON 走**我们自己的引擎**（官方/镜像候选、UA、缓存都在里面），解析走 sxcl/mods.h ——
 * 与界面点"搜索/安装"是同一条路,所以这个命令就是这一层的真机验收。
 */
static int cmd_mods(int argc, char **argv, const cli_opts *o) {
    if (argc < 3) {
        return usage();
    }
    const char *sub = argv[2];
    const char *text = NULL;
    const char *project = NULL;
    const char *mc = NULL;
    const char *loader = NULL;
    const char *type = NULL;
    int limit = 20;
    if (strcmp(sub, "search") == 0) {
        if (argc < 4) {
            return usage();
        }
        text = argv[3];
    } else if (strcmp(sub, "files") == 0) {
        if (argc < 4) {
            return usage();
        }
        project = argv[3];
    } else {
        return usage();
    }
    for (int i = 4; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--mc") == 0 && v) {
            mc = v;
            ++i;
        } else if (strcmp(a, "--loader") == 0 && v) {
            loader = v;
            ++i;
        } else if (strcmp(a, "--limit") == 0 && v) {
            limit = atoi(v);
            ++i;
        } else if (strcmp(a, "--type") == 0 && v) {
            type = v;
            ++i;
        } else if (strcmp(a, "--rate") == 0 || strcmp(a, "--workers") == 0 ||
                   strcmp(a, "--conn") == 0 || strcmp(a, "--cache") == 0 ||
                   strcmp(a, "--mirror") == 0 || strcmp(a, "--source") == 0) {
            ++i;
        } else if (strcmp(a, "--no-cache") == 0 || strcmp(a, "--verbose") == 0) {
            /* 无值参数 */
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            return usage();
        }
    }
    char url[1200];
    int is_files = 0;
    if (project != NULL) {
        if (sxcl_mods_modrinth_versions_url(project, mc, loader, url, sizeof(url)) != 1) {
            fprintf(stderr, "版本列表 URL 拼不出来(工程 id 或筛选太长?)\n");
            return 2;
        }
        is_files = 1;
    } else {
        sxcl_mods_query q;
        memset(&q, 0, sizeof(q));
        q.text = text;
        q.game_version = mc;
        q.loader = loader;
        q.project_type = type;
        q.limit = limit;
        if (sxcl_mods_modrinth_search_url(&q, url, sizeof(url)) != 0) {
            fprintf(stderr, "搜索 URL 拼不出来(条件太长?)\n");
            return 2;
        }
    }
    printf("请求: %s\n", url);
    fflush(stdout);
    /* 用引擎取文本(与版本清单同一条路) */
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s/sxcl-mods-%ld.json", o->cache ? o->cache : ".", (long)time(NULL));
    cli_state st;
    memset(&st, 0, sizeof(st));
    st.verbose = o->verbose;
    sxcl_engine *engine = NULL;
    if (make_engine(o, &st, &engine) != 0) {
        fprintf(stderr, "下载引擎起不来\n");
        return 1;
    }
    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = tmp;
    task.urls[0] = url;
    task.priority = 0;
    task.label = "mods.json";
    const int state = run_one(engine, &task);
    sxcl_engine_destroy(engine);
    if (state != SXCL_TASK_DONE) {
        fprintf(stderr, "取 Modrinth 响应失败: %s\n", task.error);
        return 1;
    }
    /* 读回响应(CLI 里没有"整份读文本"的现成助手,这儿自己读) */
    char *body = NULL;
    {
        FILE *fh = fopen(tmp, "rb");
        if (fh != NULL) {
            (void)fseek(fh, 0, SEEK_END);
            const long n = ftell(fh);
            (void)fseek(fh, 0, SEEK_SET);
            if (n > 0 && n < (long)(64L * 1024L * 1024L)) {
                body = (char *)malloc((size_t)n + 1);
                if (body != NULL) {
                    const size_t got = fread(body, 1, (size_t)n, fh);
                    body[got] = '\0';
                }
            }
            (void)fclose(fh);
        }
    }
    remove(tmp);
    if (body == NULL) {
        fprintf(stderr, "读不回刚才下的响应\n");
        return 1;
    }
    char err[192];
    err[0] = '\0';
    if (is_files) {
        sxcl_mod_file files[32];
        size_t count = 0;
        if (sxcl_mods_modrinth_versions_parse(body, strlen(body), files, 32, &count, err,
                                              sizeof(err)) != 0) {
            fprintf(stderr, "解析版本列表失败: %s\n", err);
            free(body);
            return 1;
        }
        printf("共 %d 个版本:\n", (int)count);
        for (size_t i = 0; i < count; ++i) {
            printf("  [%d] %s | %s | %s | %lld 字节\n", (int)i + 1, files[i].version_number,
                   files[i].loaders, files[i].filename, (long long)files[i].size);
        }
        sxcl_mod_file picked;
        memset(&picked, 0, sizeof(picked));
        if (count > 0 && sxcl_mods_pick_file(files, count, mc, loader, &picked) == 0) {
            printf("挑中(装这个): %s\n  URL: %s\n", picked.filename, picked.url);
            if (picked.required_deps[0] != '\0') {
                printf("  依赖(只展示不装): %s\n", picked.required_deps);
            }
        } else {
            printf("没有适合这个实例的版本（版本/加载器对不上，PCL 口径不自动换加载器）\n");
        }
    } else {
        sxcl_mod_page page;
        if (sxcl_mods_modrinth_search_parse(body, strlen(body), &page, err, sizeof(err)) != 0) {
            fprintf(stderr, "解析搜索结果失败: %s\n", err);
            free(body);
            return 1;
        }
        printf("共命中 %d 条(这一页 %d 条):\n", (int)page.total, (int)page.count);
        for (size_t i = 0; i < page.count; ++i) {
            const sxcl_mod_hit *h = &page.items[i];
            printf("  [%d] %s（%s） 下载 %lld | %s\n      id=%s | 支持 %s\n", (int)i + 1, h->title,
                   h->author, (long long)h->downloads, h->description, h->id, h->versions);
        }
        sxcl_mods_page_free(&page);
    }
    free(body);
    return 0;
}

int main(int argc, char **argv)
{
    /* 无缓冲输出:崩溃时不会把最后一段输出留在缓冲区里丢掉(排查跨平台崩溃吃过这个亏) */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 命令行也要有 logs/sxcl-*.log:游戏输出落盘、崩溃取证、一键导出都挂在它上面。
     * 目录口径与界面一致(SXCL_LOG_DIR > <配置目录>/logs);打不开只降级,不影响命令。 */
    {
        char log_err[SXCL_LOG_ERROR_MAX];
        log_err[0] = '\0';
        if (sxcl_log_init(NULL, log_err, sizeof(log_err)) != SXCL_LOG_OK) {
            fprintf(stderr, "提示:运行日志文件不可用(%s),日志只走终端\n",
                    log_err[0] != '\0' ? log_err : "未知原因");
        } else {
            (void)atexit(cli_log_atexit);
        }
    }
#if defined(_WIN32)
    rebuild_argv_utf8(&argc, &argv); /* 必须在解析参数之前:否则中文实例名/路径就是乱码 */
#endif
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, sxcl_veh_handler); /* 第一现场,栈还没展开 */
    SetUnhandledExceptionFilter(sxcl_crash_handler);
#endif
    if (argc < 2) {
        return usage();
    }
    cli_opts o;
    memset(&o, 0, sizeof(o));
    o.prefer_mirror = 1; /* --source 默认 bmclapi:镜像优先(用户要求"默认走 bmc,官方不好使自动切") */
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--rate") == 0 && v) {
            o.rate = sxcl_limiter_parse_rate(v);
            ++i;
        } else if (strcmp(a, "--workers") == 0 && v) {
            o.workers = atoi(v);
            ++i;
        } else if (strcmp(a, "--source") == 0 && v) {
            if (strcmp(v, "mojang") == 0) {
                o.prefer_mirror = 0;
            } else if (strcmp(v, "bmclapi") == 0 || strcmp(v, "auto") == 0) {
                o.prefer_mirror = 1;
            } else {
                fprintf(stderr, "认不出的下载源: %s(可用 bmclapi|mojang|auto)\n", v);
                return usage();
            }
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
    if (strcmp(argv[1], "loader") == 0) {
        return cmd_loader(argc, argv, &o);
    }
    if (strcmp(argv[1], "launch") == 0) {
        return cmd_launch(argc, argv, &o);
    }
    if (strcmp(argv[1], "mods") == 0) {
        return cmd_mods(argc, argv, &o);
    }
    if (strcmp(argv[1], "crash") == 0) {
        if (argc < 3) {
            return usage();
        }
        return cmd_crash(argc, argv);
    }
    if (strcmp(argv[1], "logs") == 0) {
        return cmd_logs(argc, argv);
    }
    if (strcmp(argv[1], "auth") == 0) {
        return cmd_auth(argc, argv);
    }
    if (strcmp(argv[1], "java") == 0) {
        return cmd_java(argc, argv, &o);
    }
    return usage();
}
