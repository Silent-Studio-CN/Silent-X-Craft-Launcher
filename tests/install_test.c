/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1 /* 测试里用 fopen 造夹具,MSVC 会标记弃用 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/install.h"
#include "sxcl/manifest.h" /* SXCL_MIRROR_BMCLAPI_BASE / SXCL_ASSET_OBJECTS_BASE(下载源用例要用) */
#include "sxcl/net.h"

#define TMP_ROOT  "build-c/_install_tmp"
#define INSTANCE  "1.20.1"
#define LOADER_INSTANCE "1.20.1-forge-47.2.0"

#define CLIENT_REL   "libraries/org/ow2/asm/asm/9.5/asm-9.5.jar"
#define LIB2_REL     "libraries/org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar"
#define INDEX_REL    "assets/indexes/5.json"
#define OBJ1_REL     "assets/objects/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OBJ2_REL     "assets/objects/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

/* ── 夹具文本(不是真 Mojang 数据,只求字段齐、路径可预测) ── */

static const char kDummy[] = "0123456789abcdef0123456789abcdef"; /* 32 字节,与 fixtures 里的 size 一致 */

static const char kManifest[] =
    "{\n"
    "  \"latest\": { \"release\": \"1.20.1\", \"snapshot\": \"1.20.1\" },\n"
    "  \"versions\": [\n"
    "    { \"id\": \"1.20.1\", \"type\": \"release\",\n"
    "      \"url\": \"https://piston-meta.mojang.com/v1/packages/deadbeef/1.20.1.json\",\n"
    "      \"sha1\": \"5555555555555555555555555555555555555555\", \"size\": 32 }\n"
    "  ]\n"
    "}\n";

static const char kVersionJson[] =
    "{\n"
    "  \"id\": \"1.20.1\",\n"
    "  \"mainClass\": \"net.minecraft.client.main.Main\",\n"
    "  \"assetIndex\": { \"id\": \"5\",\n"
    "     \"url\": \"https://piston-meta.mojang.com/v1/packages/idx/5.json\",\n"
    "     \"sha1\": \"1111111111111111111111111111111111111111\", \"size\": 32 },\n"
    "  \"downloads\": { \"client\": {\n"
    "     \"url\": \"https://piston-data.mojang.com/v1/objects/abc/client.jar\",\n"
    "     \"sha1\": \"2222222222222222222222222222222222222222\", \"size\": 32 } },\n"
    "  \"libraries\": [\n"
    "    { \"name\": \"org.ow2.asm:asm:9.5\", \"downloads\": { \"artifact\": {\n"
    "        \"path\": \"org/ow2/asm/asm/9.5/asm-9.5.jar\",\n"
    "        \"url\": \"https://libraries.minecraft.net/org/ow2/asm/asm/9.5/asm-9.5.jar\",\n"
    "        \"sha1\": \"3333333333333333333333333333333333333333\", \"size\": 32 } } },\n"
    "    { \"name\": \"org.lwjgl:lwjgl:3.3.3\", \"downloads\": { \"artifact\": {\n"
    "        \"path\": \"org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar\",\n"
    "        \"url\": \"https://libraries.minecraft.net/org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar\",\n"
    "        \"sha1\": \"4444444444444444444444444444444444444444\", \"size\": 32 } } }\n"
    "  ]\n"
    "}\n";

static const char kAssetIndex[] =
    "{\n"
    "  \"objects\": {\n"
    "    \"minecraft/sounds/a.ogg\": { \"hash\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", \"size\": 32 },\n"
    "    \"minecraft/textures/b.png\": { \"hash\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\", \"size\": 32 }\n"
    "  }\n"
    "}\n";

/* ── 断言小工具 ── */

static int g_pass = 0;
static int g_fail = 0;
static int g_group_pass = 0;
static int g_group_fail = 0;
static const char *g_group = "(未分组)";

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
        ++g_group_pass;
    } else {
        ++g_fail;
        ++g_group_fail;
        printf("  [!!] %s | %s\n", g_group, what);
    }
}

static void check_int(long got, long want, const char *what) {
    if (got == want) {
        ++g_pass;
        ++g_group_pass;
    } else {
        ++g_fail;
        ++g_group_fail;
        printf("  [!!] %s | %s: got %ld want %ld\n", g_group, what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) {
        ++g_pass;
        ++g_group_pass;
    } else {
        ++g_fail;
        ++g_group_fail;
        printf("  [!!] %s | %s: got '%s' want '%s'\n", g_group, what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

static void group(const char *name) {
    if (g_group_pass + g_group_fail > 0) {
        printf("  [%s] 通过 %d 项, 失败 %d 项\n", g_group, g_group_pass, g_group_fail);
    }
    g_group = name;
    g_group_pass = 0;
    g_group_fail = 0;
}

/* ── 文件小工具 ── */

static int write_file(const char *path, const char *text) {
    sxcl_fs_mkdirs_for_file(path);
    FILE *fh = fopen(path, "wb");
    if (!fh) {
        return -1;
    }
    const size_t n = strlen(text);
    const int ok = fwrite(text, 1, n, fh) == n;
    fclose(fh);
    return ok ? 0 : -1;
}

static int file_contains(const char *path, const char *needle) {
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return 0;
    }
    char buf[4096];
    const size_t got = fread(buf, 1, sizeof(buf) - 1, fh);
    buf[got] = '\0';
    fclose(fh);
    return strstr(buf, needle) != NULL;
}

/* ── 调用序列记录(假实现都往这里写) ── */

typedef enum fake_call_kind {
    CALL_FETCH = 0,
    CALL_DOWNLOAD,
    CALL_LOADER,
    CALL_NATIVES
} fake_call_kind;

typedef struct fake_call {
    fake_call_kind kind;
    int stage;
} fake_call;

#define MAX_CALLS 128

typedef struct fake {
    /* 记录 */
    fake_call calls[MAX_CALLS];
    size_t call_count;
    size_t fetch_calls;
    size_t download_calls;
    size_t download_tasks;
    size_t download_requests; /* 真的"要下"的文件数(跳过的不算) */
    size_t skip_reports;      /* 报"已存在且校验通过"的文件数 */
    size_t natives_calls;
    size_t loader_calls;
    size_t loader_lib_tasks;
    int natives_count;
    int stopped_early;
    char natives_dir_seen[1024];
    char natives_game_dir[1024];
    /* 下载源(镜像优先)用例:假下载器看到的候选顺序 */
    char want_prefix[256];    /* 每个任务 urls[0] 必须以它开头(空 = 不检查) */
    char forbid_prefix[256];  /* 每个任务 urls[0] 不许以它开头(空 = 不检查) */
    size_t url0_unexpected;   /* 第一候选不符合 want_prefix 的任务数 */
    size_t url0_forbidden;    /* 第一候选撞上 forbid_prefix 的任务数 */
    size_t missing_second;    /* 没有第二候选(官方退路)的任务数 */
    char fetch_url[1024];     /* 第一次 fetch_text 拿到的 URL */
    /* 假加载器收到的请求 */
    char loader_game_dir[1024];
    char loader_instance[256];
    char loader_base[64];
    char loader_installer[1024];
    char loader_java[256];
    char loader_version[64];
    int loader_kind;
    /* 剧本开关 */
    int fail_stage;       /* 让哪个阶段的下载全失败(SXCL_INSTALL_STAGE_END = 不失败) */
    int natives_fail;
    int loader_fail;
    int loader_call_libraries;
} fake;

static fake g_fake;

typedef struct progress_log {
    int events;
    int stage_begins;
    int stage_ends;
    int cleanups;
    int dones;
    int percent_dropped;
    int stage_percent_dropped;
    int empty_status;
    int last_percent;
    int last_stage_percent;
    int done_percent;
    int stage_index_mismatch;
    sxcl_install_stage begin_seq[SXCL_INSTALL_STAGE_COUNT + 2];
    size_t begin_count;
    size_t download_events_with_files;
    size_t download_events_with_bytes;
    int mid_stage_progress; /* 阶段内出现过 0<x<100 的进度(进度条真的会动) */
    char stage_status[SXCL_INSTALL_STAGE_COUNT][SXCL_INSTALL_STATUS_MAX];
} progress_log;

static progress_log g_log;

/* 取消控制:进度回调看到某个阶段开始后,下一次 is_cancelled 就返回 1 */
typedef struct cancel_ctl {
    int arm_stage;   /* 看到这个阶段开始就上膛;-1 = 不上膛 */
    int armed;
    int fired;
    int calls;
} cancel_ctl;

static cancel_ctl g_cancel;

static void log_progress(void *userdata, const sxcl_install_progress *p) {
    progress_log *log = (progress_log *)userdata;
    ++log->events;

    if (!p->status[0]) {
        ++log->empty_status;
    }
    if (p->percent < log->last_percent) {
        ++log->percent_dropped;
    }
    log->last_percent = p->percent;

    if (p->event == SXCL_INSTALL_EVENT_STAGE_BEGIN) {
        ++log->stage_begins;
        if (log->begin_count < sizeof(log->begin_seq) / sizeof(log->begin_seq[0])) {
            log->begin_seq[log->begin_count++] = p->stage;
        } else {
            ++log->stage_index_mismatch;
        }
        if (p->stage_index != log->begin_count - 1 || p->stage_total == 0) {
            /* 计划阶段表里的下标应当与开始顺序一一对应 */
            ++log->stage_index_mismatch;
        }
        log->last_stage_percent = 0;
        if (g_cancel.arm_stage >= 0 && (int)p->stage == g_cancel.arm_stage) {
            g_cancel.armed = 1;
        }
        return;
    }
    if (p->event == SXCL_INSTALL_EVENT_STAGE_END) {
        ++log->stage_ends;
        if ((int)p->stage >= 0 && (int)p->stage < SXCL_INSTALL_STAGE_COUNT) {
            snprintf(log->stage_status[(int)p->stage], SXCL_INSTALL_STATUS_MAX, "%s", p->status);
        }
        return;
    }
    if (p->event == SXCL_INSTALL_EVENT_CLEANUP) {
        ++log->cleanups;
        return;
    }
    if (p->event == SXCL_INSTALL_EVENT_DONE) {
        ++log->dones;
        log->done_percent = p->percent;
        return;
    }
    if (p->event == SXCL_INSTALL_EVENT_STAGE_PROGRESS) {
        if (p->stage_percent > 0 && p->stage_percent < 100) {
            log->mid_stage_progress++;
        }
        if (p->stage_percent < log->last_stage_percent) {
            ++log->stage_percent_dropped;
        }
        log->last_stage_percent = p->stage_percent;
        if (p->files_total > 0) {
            ++log->download_events_with_files;
        }
        if (p->bytes_total > 0) {
            ++log->download_events_with_bytes;
        }
    }
}

static int is_cancelled(void *userdata) {
    cancel_ctl *c = (cancel_ctl *)userdata;
    ++c->calls;
    if (c->armed && !c->fired) {
        c->fired = 1;
        return 1;
    }
    return 0;
}

static void record_call(fake_call_kind kind, int stage) {
    if (g_fake.call_count < MAX_CALLS) {
        g_fake.calls[g_fake.call_count].kind = kind;
        g_fake.calls[g_fake.call_count].stage = stage;
        ++g_fake.call_count;
    }
}

static int write_payload(const char *dest) {
    const char *text = kDummy;
    if (strstr(dest, "assets/indexes/") != NULL) {
        text = kAssetIndex;
    } else if (strstr(dest, ".json") != NULL) {
        text = kVersionJson;
    }
    return write_file(dest, text);
}

/* ── 假实现 ── */

static int fake_fetch_text(void *userdata, const char *url, char **out_text, char *err, size_t err_len) {
    fake *f = (fake *)userdata;
    ++f->fetch_calls;
    if (url && !f->fetch_url[0]) {
        snprintf(f->fetch_url, sizeof(f->fetch_url), "%s", url); /* 记下第一条被取的路 */
    }
    record_call(CALL_FETCH, SXCL_INSTALL_STAGE_MANIFEST);
    if (err && err_len) {
        err[0] = '\0';
    }
    const size_t n = strlen(kManifest);
    char *text = (char *)malloc(n + 1);
    if (!text) {
        return -1;
    }
    memcpy(text, kManifest, n + 1);
    *out_text = text;
    return 0;
}

static int fake_download(void *userdata, const sxcl_install_download *request,
                         sxcl_install_download_stats *stats, char *err, size_t err_len) {
    fake *f = (fake *)userdata;
    ++f->download_calls;
    f->download_tasks += request->count;
    record_call(CALL_DOWNLOAD, (int)request->stage);
    if (err && err_len) {
        err[0] = '\0';
    }

    size_t done = 0;
    size_t failed = 0;
    size_t skipped = 0;
    int64_t bytes_done = 0;
    int64_t bytes_total = 0;
    const int want_fail = (f->fail_stage == (int)request->stage);

    for (size_t i = 0; i < request->count; ++i) {
        sxcl_task *t = request->tasks[i];
        if (!t) {
            continue;
        }
        /* 候选顺序(下载源)记账:第一候选是谁、有没有留官方退路 */
        if (t->urls[0] != NULL) {
            if (f->want_prefix[0] != '\0' &&
                strncmp(t->urls[0], f->want_prefix, strlen(f->want_prefix)) != 0) {
                ++f->url0_unexpected;
            }
            if (f->forbid_prefix[0] != '\0' &&
                strncmp(t->urls[0], f->forbid_prefix, strlen(f->forbid_prefix)) == 0) {
                ++f->url0_forbidden;
            }
        }
        if (t->urls[1] == NULL) {
            ++f->missing_second;
        }
        if (t->size > 0) {
            bytes_total += t->size;
        }
        if (want_fail) {
            t->state = SXCL_TASK_FAILED;
            snprintf(t->error, sizeof(t->error), "假下载器:按剧本失败");
            ++failed;
        } else if (sxcl_fs_exists(t->dest)) {
            /* 与引擎快路径等价:文件在且(这个假实现里)大小也对 -> 一个字节都不下 */
            t->state = SXCL_TASK_DONE;
            snprintf(t->error, sizeof(t->error), "已存在且校验通过");
            t->bytes_done = t->size > 0 ? t->size : 32;
            ++skipped;
            f->skip_reports += 1;
        } else {
            ++f->download_requests;
            if (write_payload(t->dest) != 0) {
                t->state = SXCL_TASK_FAILED;
                snprintf(t->error, sizeof(t->error), "假下载器:写不进去 %s", t->dest);
                ++failed;
            } else {
                t->state = SXCL_TASK_DONE;
                t->bytes_done = t->size > 0 ? t->size : 32;
            }
        }
        if (t->state == SXCL_TASK_DONE) {
            ++done;
        }
        bytes_done += t->bytes_done;
        if (request->report) {
            const int stop = request->report(request->report_userdata, t, i, done + failed, request->count);
            if (stop != 0) {
                f->stopped_early = 1;
                break;
            }
        }
    }

    if (stats) {
        stats->files_total = request->count;
        stats->files_done = done;
        stats->files_skipped = skipped;
        stats->files_failed = failed;
        stats->bytes_total = bytes_total;
        stats->bytes_done = bytes_done;
    }
    return (int)failed;
}

static int fake_natives(void *userdata, const sxcl_json *version_json, const char *game_dir,
                        const char *natives_dir, int *out_count, char *err, size_t err_len) {
    fake *f = (fake *)userdata;
    ++f->natives_calls;
    record_call(CALL_NATIVES, SXCL_INSTALL_STAGE_NATIVES);
    snprintf(f->natives_dir_seen, sizeof(f->natives_dir_seen), "%s", natives_dir);
    snprintf(f->natives_game_dir, sizeof(f->natives_game_dir), "%s", game_dir);
    if (!version_json) {
        if (err && err_len) {
            snprintf(err, err_len, "假 natives:没有版本 JSON");
        }
        return -1;
    }
    if (f->natives_fail) {
        if (err && err_len) {
            snprintf(err, err_len, "假 natives:按剧本失败");
        }
        return -1;
    }
    if (out_count) {
        *out_count = f->natives_count;
    }
    return 0;
}

static int fake_loader(void *userdata, const sxcl_loader_install_request *request,
                       sxcl_loader_install_result *out) {
    fake *f = (fake *)userdata;
    ++f->loader_calls;
    record_call(CALL_LOADER, SXCL_INSTALL_STAGE_LOADER_RUN);
    snprintf(f->loader_game_dir, sizeof(f->loader_game_dir), "%s",
             request->game_dir ? request->game_dir : "");
    snprintf(f->loader_instance, sizeof(f->loader_instance), "%s",
             request->instance_name ? request->instance_name : "");
    snprintf(f->loader_base, sizeof(f->loader_base), "%s", request->base_version ? request->base_version : "");
    snprintf(f->loader_installer, sizeof(f->loader_installer), "%s",
             request->installer_jar ? request->installer_jar : "");
    snprintf(f->loader_java, sizeof(f->loader_java), "%s", request->java_path ? request->java_path : "");
    snprintf(f->loader_version, sizeof(f->loader_version), "%s",
             request->loader_version ? request->loader_version : "");
    f->loader_kind = (int)request->kind;

    if (request->on_progress) {
        request->on_progress(request->userdata, 42, "Downloading libraries");
    }
    if (f->loader_call_libraries && request->on_libraries) {
        sxcl_loader_library lib;
        memset(&lib, 0, sizeof(lib));
        snprintf(lib.name, sizeof(lib.name), "%s", "net.minecraftforge:forge:1.20.1-47.2.0:universal");
        snprintf(lib.path, sizeof(lib.path), "%s",
                 "net/minecraftforge/forge/1.20.1-47.2.0/forge-1.20.1-47.2.0-universal.jar");
        snprintf(lib.url, sizeof(lib.url), "%s", "https://maven.minecraftforge.net/");
        if (request->on_libraries(request->userdata, &lib, 1) != 0) {
            out->ok = 0;
            out->fail_stage = SXCL_LOADER_FAIL_EXTRACT;
            snprintf(out->error, sizeof(out->error), "假加载器:依赖库下载失败");
            return 1;
        }
        ++f->loader_lib_tasks;
    }
    if (f->loader_fail) {
        out->ok = 0;
        out->fail_stage = SXCL_LOADER_FAIL_RUN_INSTALLER;
        snprintf(out->error, sizeof(out->error), "假加载器安装器崩了");
        return 1;
    }
    out->ok = 1;
    out->fail_stage = SXCL_LOADER_FAIL_NONE;
    out->percent = 100;
    return 0;
}

static const sxcl_install_io kFakeIo = {
    fake_fetch_text,
    fake_download,
    fake_loader,
    fake_natives,
    &g_fake,
};

/* ── 夹具目录 ── */

static const char *kPurgeRel[] = {
    "versions/" INSTANCE "/" INSTANCE ".json",
    "versions/" INSTANCE "/" INSTANCE ".jar",
    "versions/" LOADER_INSTANCE "/" LOADER_INSTANCE ".json",
    "versions/" LOADER_INSTANCE "/" LOADER_INSTANCE ".jar",
    "versions/" LOADER_INSTANCE "/forge-installer.jar",
    INDEX_REL,
    CLIENT_REL,
    LIB2_REL,
    OBJ1_REL,
    OBJ2_REL,
};
#define PURGE_COUNT (sizeof(kPurgeRel) / sizeof(kPurgeRel[0]))

/** 每个用例一个干净目录:把上次跑留下的文件删掉,免得"跳过"用例被上次的产物污染。 */
static void case_dir(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", TMP_ROOT, name);
    if (sxcl_fs_mkdirs(out) != 0) {
        printf("  [!!] 建不出夹具目录: %s\n", out);
    }
    for (size_t i = 0; i < PURGE_COUNT; ++i) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", out, kPurgeRel[i]);
        (void)sxcl_fs_remove(path);
        snprintf(path, sizeof(path), "%s/%s.part", out, kPurgeRel[i]);
        (void)sxcl_fs_remove(path);
        snprintf(path, sizeof(path), "%s/%s.part.json", out, kPurgeRel[i]);
        (void)sxcl_fs_remove(path);
    }
}

static void reset_all(void) {
    memset(&g_fake, 0, sizeof(g_fake));
    memset(&g_log, 0, sizeof(g_log));
    memset(&g_cancel, 0, sizeof(g_cancel));
    g_cancel.arm_stage = -1;
    g_fake.fail_stage = SXCL_INSTALL_STAGE_END;
    g_fake.natives_count = 7;
    g_log.last_stage_percent = 0;
}

static void base_plan(sxcl_install_plan *plan, const char *game_dir) {
    memset(plan, 0, sizeof(*plan));
    plan->game_dir = game_dir;
    plan->version_id = INSTANCE;
    plan->assets = SXCL_INSTALL_ASSETS_DEFAULT; /* 零初始化也等于它 */
}

static int run_install(const sxcl_install_plan *plan, sxcl_install_result *res) {
    sxcl_install_request req;
    memset(&req, 0, sizeof(req));
    req.plan = plan;
    req.io = &kFakeIo;
    req.on_progress = log_progress;
    req.userdata = &g_log;
    if (g_cancel.arm_stage >= 0) {
        req.is_cancelled = is_cancelled;
        req.cancel_userdata = &g_cancel;
    }
    return sxcl_install_run(&req, res);
}

/* ── 1) 阶段表 ── */

static void test_stage_table(void) {
    group("阶段表");
    check_int(SXCL_INSTALL_STAGE_COUNT, 10, "阶段数量固定为 10");

    static const char *const ids[] = {"manifest", "version_json", "client_jar", "libraries",
                                      "asset_index", "asset_objects", "loader_installer", "loader_run",
                                      "natives", "finish"};
    static const char *const names[] = {"获取版本清单", "下载并解析版本 JSON", "下载客户端 jar", "下载依赖库",
                                        "下载资源索引", "下载资源对象", "下载加载器安装器", "执行加载器安装",
                                        "解压 natives", "整理文件"};
    for (int i = 0; i < SXCL_INSTALL_STAGE_COUNT; ++i) {
        check_str(sxcl_install_stage_id((sxcl_install_stage)i), ids[i], "阶段稳定名");
        check_str(sxcl_install_stage_name((sxcl_install_stage)i), names[i], "阶段中文显示名");
    }
    check_str(sxcl_install_stage_id(SXCL_INSTALL_STAGE_END), "none", "哨兵阶段名");
    check_str(sxcl_install_stage_id((sxcl_install_stage)99), "unknown", "越界阶段名");
    check_str(sxcl_install_stage_name((sxcl_install_stage)99), "未知阶段", "越界中文名");
    check(sxcl_install_stage_is_loader(SXCL_INSTALL_STAGE_LOADER_INSTALLER) == 1, "6 是加载器阶段");
    check(sxcl_install_stage_is_loader(SXCL_INSTALL_STAGE_LOADER_RUN) == 1, "7 是加载器阶段");
    check(sxcl_install_stage_is_loader(SXCL_INSTALL_STAGE_NATIVES) == 0, "8 不是加载器阶段");

    /* 枚举数值必须单调(UI/日志按"数值 = 顺序"使用) */
    check((int)SXCL_INSTALL_STAGE_ASSET_OBJECTS < (int)SXCL_INSTALL_STAGE_LOADER_INSTALLER,
          "资源对象在加载器之前");
    check((int)SXCL_INSTALL_STAGE_LOADER_RUN < (int)SXCL_INSTALL_STAGE_NATIVES, "加载器安装在 natives 之前");

    sxcl_install_plan plan;
    base_plan(&plan, "x");
    check_int((long)sxcl_install_plan_stage_count(&plan), 8, "原版 + 全量资源 = 8 个阶段");
    static const int vanilla[] = {0, 1, 2, 3, 4, 5, 8, 9};
    for (int i = 0; i < 8; ++i) {
        check_int((long)sxcl_install_plan_stage_at(&plan, (size_t)i), vanilla[i], "原版阶段表顺序");
    }
    check_str(sxcl_install_plan_instance(&plan), INSTANCE, "实例名默认用版本号");
    check(sxcl_install_plan_has_loader(&plan) == 0, "原版计划没有加载器");

    plan.loader = SXCL_LOADER_FORGE;
    plan.loader_version = "47.2.0";
    plan.instance_name = LOADER_INSTANCE;
    check_int((long)sxcl_install_plan_stage_count(&plan), 10, "含加载器 = 10 个阶段");
    static const int with_loader[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 0; i < 10; ++i) {
        check_int((long)sxcl_install_plan_stage_at(&plan, (size_t)i), with_loader[i], "含加载器阶段表顺序");
    }
    check_str(sxcl_install_plan_instance(&plan), LOADER_INSTANCE, "自定义实例名");
    check(sxcl_install_plan_has_loader(&plan) == 1, "含加载器的计划");

    plan.assets = SXCL_INSTALL_ASSETS_NONE;
    check_int((long)sxcl_install_plan_stage_count(&plan), 8, "不装资源:去掉索引与对象,仍是 8 个(含加载器)");
    check(sxcl_install_plan_stage_included(&plan, SXCL_INSTALL_STAGE_ASSET_INDEX) == 0, "NONE 不排资源索引");
    plan.assets = SXCL_INSTALL_ASSETS_INDEX;
    check(sxcl_install_plan_stage_included(&plan, SXCL_INSTALL_STAGE_ASSET_INDEX) == 1, "INDEX 排资源索引");
    check(sxcl_install_plan_stage_included(&plan, SXCL_INSTALL_STAGE_ASSET_OBJECTS) == 0, "INDEX 不排资源对象");
    check_str(sxcl_install_stage_id((sxcl_install_stage)sxcl_install_plan_stage_at(&plan, 99)), "none",
              "越界取阶段 = 哨兵");
}

/* ── 2) 原版全流程 ── */

static void test_vanilla_run(void) {
    group("原版全流程");
    reset_all();
    char game[1024];
    case_dir("vanilla", game, sizeof(game));

    sxcl_install_plan plan;
    base_plan(&plan, game);
    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_OK, "返回 0");
    check_int(res.code, SXCL_INSTALL_OK, "结果是成功");
    check_int((long)res.stages_done, 8, "8 个阶段全部跑完");
    check_int(res.percent, 100, "成功时整体进度 100");

    /* 阶段顺序(= 枚举顺序,逐阶段) */
    static const int want[] = {0, 1, 2, 3, 4, 5, 8, 9};
    check_int((long)g_log.begin_count, 8, "发了 8 次阶段开始");
    for (size_t i = 0; i < g_log.begin_count && i < 8; ++i) {
        check_int((long)g_log.begin_seq[i], want[i], "阶段开始顺序");
    }
    check_int(g_log.stage_ends, 8, "8 次阶段结束");

    /* 进度单调 + 状态非空 */
    check_int(g_log.percent_dropped, 0, "整体进度从不回退");
    check_int(g_log.stage_percent_dropped, 0, "阶段内进度从不回退");
    check_int(g_log.empty_status, 0, "每条状态都非空");
    check_int(g_log.dones, 1, "只发一次结束事件");
    check_int(g_log.done_percent, 100, "结束事件进度 100");
    check(g_log.download_events_with_files >= 5, "下载阶段报出了文件数");
    check(g_log.download_events_with_bytes >= 5, "下载阶段报出了字节数");
    check(g_log.mid_stage_progress >= 2, "阶段内进度真的在推进(不是只在阶段结束时跳 100)");
    check_int(g_log.stage_index_mismatch, 0, "阶段下标与开始顺序一致");

    /* 假实现调用序列:取清单 -> 版本 JSON -> jar -> 库 -> 索引 -> 对象 -> natives */
    check_int((long)g_fake.fetch_calls, 1, "取了一次版本清单");
    check_int((long)g_fake.download_calls, 5, "5 次下载(版本 JSON/jar/库/索引/对象)");
    /* 假实现看到的调用顺序:取清单 -> 版本 JSON -> jar -> 库 -> 索引 -> 对象 -> natives */
    static const int want_calls[][2] = {
        {CALL_FETCH, 0},          {CALL_DOWNLOAD, 1}, {CALL_DOWNLOAD, 2},
        {CALL_DOWNLOAD, 3},       {CALL_DOWNLOAD, 4}, {CALL_DOWNLOAD, 5},
        {CALL_NATIVES, 8},
    };
    check_int((long)g_fake.call_count, 7, "假实现一共被调 7 次");
    for (size_t i = 0; i < g_fake.call_count && i < 7; ++i) {
        check_int((long)g_fake.calls[i].kind, want_calls[i][0], "调用种类顺序");
        check_int(g_fake.calls[i].stage, want_calls[i][1], "调用所属阶段顺序");
    }
    check_int((long)g_fake.download_tasks, 7, "一共 7 个文件(1+1+2+1+2)");
    check_int((long)g_fake.natives_calls, 1, "natives 被调用一次");
    check_int((long)g_fake.loader_calls, 0, "原版不碰加载器");
    check_int(res.natives_files, 7, "natives 就绪文件数带回来");
    check(strstr(g_fake.natives_dir_seen, "/versions/" INSTANCE "/" INSTANCE "-natives") != NULL,
          "natives 目录 = versions/<实例>/<实例>-natives");
    check(strcmp(g_fake.natives_game_dir, game) == 0, "natives 拿到的是游戏目录");
    check(sxcl_fs_exists(res.version_json_path) == 1, "版本 JSON 真的落盘了");
    check(file_contains(res.version_json_path, "\"id\": \"1.20.1\"") == 1, "落盘的是版本 JSON 内容");
    check_int((long)res.files_failed, 0, "没有失败文件");
    check_int((long)res.files_skipped, 0, "第一次跑没有可跳过的");
    check(res.retryable == 0, "成功时没有可重试标记");
    check_str(res.fail_stage_id, "none", "成功时失败阶段是 none");
    check(res.bytes_done > 0, "字节数被统计");
    check(strstr(g_log.stage_status[SXCL_INSTALL_STAGE_CLIENT_JAR], "客户端 jar") != NULL,
          "客户端 jar 阶段有总结文案");
}

/* ── 3) 跳过:文件已在且校验通过 ── */

static void test_skip_existing(void) {
    group("不重复下载");
    reset_all();
    char game[1024];
    case_dir("skip", game, sizeof(game));

    char path[1200];
    snprintf(path, sizeof(path), "%s/versions/" INSTANCE "/" INSTANCE ".json", game);
    check(write_file(path, kVersionJson) == 0, "先摆好版本 JSON");
    snprintf(path, sizeof(path), "%s/" INDEX_REL, game);
    check(write_file(path, kAssetIndex) == 0, "先摆好资源索引");
    snprintf(path, sizeof(path), "%s/versions/" INSTANCE "/" INSTANCE ".jar", game);
    check(write_file(path, kDummy) == 0, "先摆好客户端 jar");
    snprintf(path, sizeof(path), "%s/" CLIENT_REL, game);
    check(write_file(path, kDummy) == 0, "先摆好依赖库 1");
    snprintf(path, sizeof(path), "%s/" LIB2_REL, game);
    check(write_file(path, kDummy) == 0, "先摆好依赖库 2");
    snprintf(path, sizeof(path), "%s/" OBJ1_REL, game);
    check(write_file(path, kDummy) == 0, "先摆好资源对象 1");
    snprintf(path, sizeof(path), "%s/" OBJ2_REL, game);
    check(write_file(path, kDummy) == 0, "先摆好资源对象 2");

    sxcl_install_plan plan;
    base_plan(&plan, game);
    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_OK, "返回 0");
    check_int((long)g_fake.download_requests, 0, "真下载请求数 = 0(已有且校验通过的文件绝不下第二次)");
    check_int((long)g_fake.skip_reports, 7, "7 个文件全部按跳过处理");
    check_int((long)res.files_skipped, 7, "结果里记录了跳过数");
    check_int((long)g_fake.download_tasks, 7, "任务照样交给下载器(没绕过引擎快路径)");
    check_int(res.percent, 100, "跳过也要跑到 100%");
}

/* ── 4) 失败:客户端 jar 失败 = 整体中断 ── */

static void test_client_jar_failure(void) {
    group("客户端 jar 失败");
    reset_all();
    char game[1024];
    case_dir("jarfail", game, sizeof(game));
    g_fake.fail_stage = SXCL_INSTALL_STAGE_CLIENT_JAR;

    /* 先放一个 .part,失败清理应该把它删掉 */
    char part[1200];
    snprintf(part, sizeof(part), "%s/versions/" INSTANCE "/" INSTANCE ".jar.part", game);
    check(write_file(part, "half-downloaded") == 0, "先摆一个半成品 .part");

    sxcl_install_plan plan;
    base_plan(&plan, game);
    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_ERR_CLIENT_JAR, "返回码 = 客户端 jar 失败");
    check_int(res.code, SXCL_INSTALL_ERR_CLIENT_JAR, "结果码一致");
    check_int((int)res.fail_stage, (int)SXCL_INSTALL_STAGE_CLIENT_JAR, "失败阶段正确");
    check_str(res.fail_stage_id, "client_jar", "失败阶段稳定名正确");
    check_str(sxcl_install_code_name(res.code), "client_jar", "码名可解析");
    check(res.error[0] != '\0', "有人话原因");
    check(strstr(res.error, "客户端 jar") != NULL, "原因里点到了客户端 jar");
    check_int(res.retryable, 1, "网络类失败建议重试");
    check_int((long)res.stages_done, 2, "只跑完前两个阶段");
    check_int((long)g_fake.natives_calls, 0, "客户端 jar 失败后不再跑后续阶段(natives)");
    check_int((long)g_log.begin_count, 3, "阶段只开始到客户端 jar");
    check(g_log.cleanups >= 1, "清理被调用过");
    check_int(sxcl_fs_exists(part), 0, "未完成产物 .part 被清掉");
    check(g_log.done_percent < 100, "失败时进度不该假装到 100");
    check(strstr(g_log.stage_status[SXCL_INSTALL_STAGE_MANIFEST], "清单") != NULL, "清单阶段有总结");
    check_int((long)g_log.stage_ends, 2, "失败阶段不发结束事件");
}

/* ── 5) 失败:依赖库单文件失败不中断 ── */

static void test_library_partial_failure(void) {
    group("依赖库部分失败");
    reset_all();
    char game[1024];
    case_dir("libfail", game, sizeof(game));
    g_fake.fail_stage = SXCL_INSTALL_STAGE_LIBRARIES;

    sxcl_install_plan plan;
    base_plan(&plan, game);
    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_OK, "依赖库失败不中断整体安装");
    check_int((long)res.files_failed, 2, "失败文件数如实记录");
    check_int((long)res.stages_done, 8, "后面的阶段照常跑完");
    check_int((long)g_fake.natives_calls, 1, "natives 仍然跑了");
    check(strstr(g_log.stage_status[SXCL_INSTALL_STAGE_LIBRARIES], "失败") != NULL,
          "依赖库阶段的人话总结提到失败");
    check_int(g_log.empty_status, 0, "状态一直非空");
    check_int(res.retryable, 0, "成功就没有可重试标记");
}

/* ── 6) 取消 ── */

static void test_cancel(void) {
    group("取消");
    reset_all();
    char game[1024];
    case_dir("cancel", game, sizeof(game));
    g_cancel.arm_stage = (int)SXCL_INSTALL_STAGE_LIBRARIES;
    g_cancel.armed = 0;
    g_cancel.fired = 0;

    /* 取消后要清理的未完成产物 */
    char part[1200];
    snprintf(part, sizeof(part), "%s/" CLIENT_REL ".part", game);
    check(write_file(part, "half") == 0, "先摆一个半成品 .part");

    sxcl_install_plan plan;
    base_plan(&plan, game);
    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_ERR_CANCELLED, "返回码 = 已取消(可区分)");
    check_int(res.code, SXCL_INSTALL_ERR_CANCELLED, "结果码一致");
    check_int(res.cancelled, 1, "结果里标了已取消");
    check_int(res.retryable, 0, "取消不给重试建议");
    check_str(sxcl_install_code_name(res.code), "cancelled", "码名 cancelled");
    check(g_cancel.fired == 1, "取消回调真的被问过");
    check(g_cancel.calls > 0, "取消轮询有被调用");
    check_int((long)g_log.begin_count, 4, "只开始到取消时那个阶段(立刻停)");
    check_int((long)g_fake.natives_calls, 0, "取消后绝不跑 natives");
    check_int((long)g_fake.loader_calls, 0, "取消后绝不碰加载器");
    for (size_t i = 0; i < g_log.begin_count; ++i) {
        check(g_log.begin_seq[i] != SXCL_INSTALL_STAGE_ASSET_INDEX, "取消后没有后续阶段被调用(索引)");
        check(g_log.begin_seq[i] != SXCL_INSTALL_STAGE_NATIVES, "取消后没有后续阶段被调用(natives)");
    }
    check(g_log.cleanups >= 1, "清理被调用过");
    check_int(sxcl_fs_exists(part), 0, "半成品 .part 被删掉");
    check(res.error[0] != '\0', "取消也给人话原因");
    check_int((int)res.fail_stage, (int)SXCL_INSTALL_STAGE_LIBRARIES, "取消时的阶段 = 当时正在跑的阶段");
}

/* ── 7) 含加载器 ── */

static void test_loader_plan(void) {
    group("含加载器");
    reset_all();
    char game[1024];
    case_dir("loader", game, sizeof(game));
    g_fake.loader_call_libraries = 1;

    sxcl_install_plan plan;
    base_plan(&plan, game);
    plan.loader = SXCL_LOADER_FORGE;
    plan.loader_version = "47.2.0";
    plan.instance_name = LOADER_INSTANCE;
    plan.java_path = "C:/Java/bin/java.exe";
    plan.installer_url = "https://maven.minecraftforge.net/net/minecraftforge/forge/"
                         "1.20.1-47.2.0/forge-1.20.1-47.2.0-installer.jar";
    plan.loader_mirror_maven = "https://bmclapi2.bangbang93.com/maven";

    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_OK, "返回 0");
    check_int((long)res.stages_done, 10, "10 个阶段全部跑完");
    check_int((long)g_log.begin_count, 10, "发了 10 次阶段开始");
    static const int want[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (size_t i = 0; i < g_log.begin_count && i < 10; ++i) {
        check_int((long)g_log.begin_seq[i], want[i], "含加载器的阶段顺序");
    }
    /* 加载器阶段在资源之后、natives 之前 */
    size_t idx_assets = 0, idx_loader = 0, idx_natives = 0;
    for (size_t i = 0; i < g_log.begin_count; ++i) {
        if (g_log.begin_seq[i] == SXCL_INSTALL_STAGE_ASSET_OBJECTS) {
            idx_assets = i;
        }
        if (g_log.begin_seq[i] == SXCL_INSTALL_STAGE_LOADER_INSTALLER) {
            idx_loader = i;
        }
        if (g_log.begin_seq[i] == SXCL_INSTALL_STAGE_NATIVES) {
            idx_natives = i;
        }
    }
    check(idx_assets < idx_loader, "加载器在资源对象之后");
    check(idx_loader < idx_natives, "加载器在 natives 之前");

    check_int((long)g_fake.loader_calls, 1, "加载器安装被调用一次");
    check_str(g_fake.loader_instance, LOADER_INSTANCE, "实例名传对了");
    check_str(g_fake.loader_base, INSTANCE, "原版版本号传对了");
    check_str(g_fake.loader_java, "C:/Java/bin/java.exe", "Java 路径传对了");
    check_str(g_fake.loader_version, "47.2.0", "加载器版本传对了");
    check_int(g_fake.loader_kind, (int)SXCL_LOADER_FORGE, "加载器类型传对了");
    check(strstr(g_fake.loader_installer, "/versions/" LOADER_INSTANCE "/forge-installer.jar") != NULL,
          "安装器落在 versions/<实例>/<加载器>-installer.jar");
    check(strstr(g_fake.loader_installer, game) == g_fake.loader_installer, "安装器路径在游戏目录里");
    check_int((long)g_fake.loader_lib_tasks, 1, "方式 B 的依赖库回调走通了下载");
    check_int((long)g_fake.natives_calls, 1, "natives 照常跑");
    check_int(sxcl_fs_exists(g_fake.loader_installer), 0, "收尾阶段删掉了本次下到的安装器");
    check(strstr(g_log.stage_status[SXCL_INSTALL_STAGE_LOADER_RUN], "加载器安装完成") != NULL,
          "加载器阶段的人话总结");

    char installer_path[1024];
    snprintf(installer_path, sizeof(installer_path), "%s/versions/" LOADER_INSTANCE "/forge-installer.jar",
             game);

    /* 同一个用例再跑一次,这次保留安装器 */
    reset_all();
    sxcl_install_plan keep;
    base_plan(&keep, game);
    keep.loader = SXCL_LOADER_FORGE;
    keep.loader_version = "47.2.0";
    keep.instance_name = LOADER_INSTANCE;
    keep.java_path = "java";
    keep.installer_url = plan.installer_url;
    keep.keep_installer = 1;
    sxcl_install_result res2;
    check_int(run_install(&keep, &res2), SXCL_INSTALL_OK, "keep_installer 这轮也成功");
    check_int(sxcl_fs_exists(installer_path), 1, "keep_installer=1 时安装器留着");

    /* 已经下好的安装器:只核对存在,不再下 */
    reset_all();
    sxcl_install_plan reuse;
    base_plan(&reuse, game);
    reuse.loader = SXCL_LOADER_FORGE;
    reuse.loader_version = "47.2.0";
    reuse.instance_name = LOADER_INSTANCE;
    reuse.java_path = "java";
    reuse.installer_jar = installer_path;
    check_int(sxcl_fs_exists(installer_path), 1, "上一轮留下的安装器还在");
    sxcl_install_result res3;
    check_int(run_install(&reuse, &res3), SXCL_INSTALL_OK, "复用安装器这轮也成功");
    check_int((long)g_fake.download_calls, 5, "复用安装器时少一次下载(只剩 5 次)");
    check_str(g_fake.loader_installer, installer_path, "复用的安装器路径原样传给安装器");
    check_int(sxcl_fs_exists(installer_path), 1, "复用别人的安装器:收尾不许删它");
}

/* ── 8) 加载器失败 ── */

static void test_loader_failure(void) {
    group("加载器失败");
    reset_all();
    char game[1024];
    case_dir("loaderfail", game, sizeof(game));
    g_fake.loader_fail = 1;

    sxcl_install_plan plan;
    base_plan(&plan, game);
    plan.loader = SXCL_LOADER_NEOFORGE;
    plan.loader_version = "21.1.72";
    plan.java_path = "java";
    plan.installer_url = "https://maven.neoforged.net/releases/net/neoforged/neoforge/21.1.72/"
                         "neoforge-21.1.72-installer.jar";

    sxcl_install_result res;
    const int rc = run_install(&plan, &res);

    check_int(rc, SXCL_INSTALL_ERR_LOADER, "返回码 = 加载器失败");
    check_int((int)res.fail_stage, (int)SXCL_INSTALL_STAGE_LOADER_RUN, "失败阶段 = 执行加载器安装");
    check_str(res.fail_stage_id, "loader_run", "失败阶段稳定名");
    check(res.error[0] != '\0', "有人话原因");
    check(strstr(res.error, "假加载器安装器崩了") != NULL, "原因带上了安装器自己给的话");
    check(strstr(res.error, "run_installer") != NULL, "原因带上了安装器内部阶段名");
    check_int(res.retryable, 1, "加载器失败建议重试");
    check_int((long)g_fake.natives_calls, 0, "加载器失败后不再跑 natives");
    check(g_log.cleanups >= 1, "清理被调用过");
    check_int(res.natives_files, -1, "没跑到 natives:计数是 -1");
}

/* ── 9) 参数与边界 ── */

static void test_args(void) {
    group("参数校验");
    reset_all();
    char game[1024];
    case_dir("args", game, sizeof(game));

    sxcl_install_result res;
    sxcl_install_plan plan;
    base_plan(&plan, game);

    check_int(sxcl_install_run(NULL, &res), SXCL_INSTALL_ERR_ARG, "没有请求 = 参数错");
    check(res.error[0] != '\0' && sxcl_install_code_name(res.code) != NULL, "参数错也有人话原因");

    sxcl_install_request req;
    memset(&req, 0, sizeof(req));
    req.io = &kFakeIo;
    check_int(sxcl_install_run(&req, &res), SXCL_INSTALL_ERR_ARG, "没有计划 = 参数错");

    plan.game_dir = NULL;
    req.plan = &plan;
    check_int(sxcl_install_run(&req, &res), SXCL_INSTALL_ERR_ARG, "没有游戏目录 = 参数错");
    plan.game_dir = game;
    plan.version_id = "";
    check_int(sxcl_install_run(&req, &res), SXCL_INSTALL_ERR_ARG, "没有版本号 = 参数错");
    plan.version_id = INSTANCE;

    plan.loader = SXCL_LOADER_FORGE;
    check_int(sxcl_install_run(&req, &res), SXCL_INSTALL_ERR_ARG, "加载器计划没有 Java = 参数错");
    check(strstr(res.error, "Java") != NULL, "原因里点名 Java");
    plan.java_path = "java";
    plan.loader = SXCL_LOADER_QUILT;
    check_int(sxcl_install_run(&req, &res), SXCL_INSTALL_ERR_ARG, "还没实现的加载器 = 参数错");
    check(strstr(res.error, "Quilt") != NULL, "原因里点名 Quilt");
    plan.loader = SXCL_LOADER_FORGE;

    sxcl_install_io broken = kFakeIo;
    broken.download = NULL;
    req.io = &broken;
    check_int(sxcl_install_run(&req, &res), SXCL_INSTALL_ERR_ARG, "注入表不完整 = 参数错");
    req.io = &kFakeIo;

    /* 清单已在手:不该取网络 */
    reset_all();
    sxcl_install_plan offline;
    base_plan(&offline, game);
    offline.manifest_text = kManifest; /* 清单已在手 */
    sxcl_install_result res2;
    check_int(run_install(&offline, &res2), SXCL_INSTALL_OK, "清单文本走通");
    check_int((long)g_fake.fetch_calls, 0, "清单文本已在手时不联网");

    /* 清单里没有这个版本 */
    reset_all();
    sxcl_install_plan missing;
    base_plan(&missing, game);
    missing.version_id = "9.9.9";
    sxcl_install_result res3;
    check_int(run_install(&missing, &res3), SXCL_INSTALL_ERR_MANIFEST, "清单里没有的版本 = 清单阶段失败");
    check_str(res3.fail_stage_id, "manifest", "失败阶段 = 获取版本清单");
    check(strstr(res3.error, "9.9.9") != NULL, "原因里点名版本号");

    /* 代码名与可重试表 */
    check_str(sxcl_install_code_name(SXCL_INSTALL_OK), "ok", "码名 ok");
    check_str(sxcl_install_code_name(SXCL_INSTALL_ERR_NOMEM), "nomem", "码名 nomem");
    check_str(sxcl_install_code_name(-1234), "unknown", "未知码");
    check_int(sxcl_install_code_retryable(SXCL_INSTALL_ERR_MANIFEST), 1, "清单失败可重试");
    check_int(sxcl_install_code_retryable(SXCL_INSTALL_ERR_CANCELLED), 0, "取消不可重试");
    check_int(sxcl_install_code_retryable(SXCL_INSTALL_ERR_NATIVES), 0, "natives 失败不可重试");
}

/* ── 10) 取文本入口(默认实现):内存假传输,不联网 ── */

static char kHttpBody[] = "{\"hello\":\"world\",\"n\":2}";
static size_t g_http_off = 0;
static int g_http_status = 200;
static int g_http_fail_connect = 0;

static int ft_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                      sxcl_http_body **body) {
    (void)ctx;
    if (!req || !req->url || !*req->url) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    if (g_http_fail_connect) {
        return SXCL_NET_ERR_CONNECT;
    }
    memset(resp, 0, sizeof(*resp));
    resp->status = g_http_status;
    resp->content_length = (int64_t)strlen(kHttpBody);
    *body = (sxcl_http_body *)kHttpBody; /* 非空哨兵即可 */
    g_http_off = 0;
    return SXCL_NET_OK;
}

static int64_t ft_read(void *ctx, sxcl_http_body *body, void *buf, size_t len) {
    (void)ctx;
    (void)body;
    const size_t total = strlen(kHttpBody);
    if (g_http_off >= total) {
        return 0;
    }
    size_t n = total - g_http_off;
    if (n > len) {
        n = len;
    }
    memcpy(buf, kHttpBody + g_http_off, n);
    g_http_off += n;
    return (int64_t)n;
}

static void ft_close(void *ctx, sxcl_http_body *body) {
    (void)ctx;
    (void)body;
}
static void ft_cancel(void *ctx) { (void)ctx; }
static void ft_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *ft_create(void *userdata) {
    static sxcl_transport transport;
    (void)userdata;
    memset(&transport, 0, sizeof(transport));
    transport.ctx = NULL;
    transport.request = ft_request;
    transport.read = ft_read;
    transport.close_body = ft_close;
    transport.cancel_all = ft_cancel;
    transport.destroy = ft_destroy;
    return &transport;
}

static void test_http_text(void) {
    group("取文本入口");
    char err[SXCL_INSTALL_ERROR_MAX];
    char *text = NULL;
    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.transport_factory = ft_create;

    g_http_status = 200;
    g_http_fail_connect = 0;
    check_int(sxcl_install_http_get_text(&opts, "https://example.invalid/manifest.json", &text, err,
                                         sizeof(err)),
              0, "HTTP 200 = 成功");
    check(text != NULL && strcmp(text, kHttpBody) == 0, "内容原样拿回来");
    free(text);
    text = NULL;

    check_int(sxcl_install_http_get_text(&opts, NULL, &text, err, sizeof(err)), SXCL_NET_ERR_BAD_ARG,
              "空 URL = 参数错");
    check_int(sxcl_install_http_get_text(NULL, "https://x/y", &text, err, sizeof(err)),
              SXCL_NET_ERR_UNSUPPORTED, "没有传输后端 = 不支持");

    g_http_status = 404;
    check(sxcl_install_http_get_text(&opts, "https://x/y", &text, err, sizeof(err)) < 0, "404 = 失败");
    check(strstr(err, "404") != NULL, "原因里带上状态码");
    check(text == NULL, "失败不留半边结果");

    g_http_status = 200;
    g_http_fail_connect = 1;
    check(sxcl_install_http_get_text(&opts, "https://x/y", &text, err, sizeof(err)) < 0, "连不上 = 失败");
    check(err[0] != '\0', "连不上也有人话原因");
    g_http_fail_connect = 0;

    const sxcl_install_io *io = sxcl_install_default_io();
    check(io != NULL, "默认注入表存在");
    check(io && io->fetch_text && io->download && io->loader_install && io->natives_prepare,
          "默认注入表四个钩子齐全(真实现 = engine/loader/natives)");
}


/* ── 11) 默认下载钩子:真引擎 + 内存假传输(不联网) ── */

static char kEnginePayload[] = "SXCL-ENGINE-PAYLOAD-0123456789-abcdefghij";
static size_t g_engine_off = 0;
static int g_engine_requests = 0;

static int et_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                      sxcl_http_body **body) {
    (void)ctx;
    if (!req || !req->url || !*req->url) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    ++g_engine_requests;
    memset(resp, 0, sizeof(*resp));
    resp->status = 200;
    resp->content_length = (int64_t)strlen(kEnginePayload);
    resp->total_length = resp->content_length;
    resp->range_start = -1;
    resp->range_end = -1;
    *body = (sxcl_http_body *)kEnginePayload;
    g_engine_off = 0;
    return SXCL_NET_OK;
}

static int64_t et_read(void *ctx, sxcl_http_body *body, void *buf, size_t len) {
    (void)ctx;
    (void)body;
    const size_t total = strlen(kEnginePayload);
    if (g_engine_off >= total) {
        return 0;
    }
    size_t n = total - g_engine_off;
    if (n > len) {
        n = len;
    }
    memcpy(buf, kEnginePayload + g_engine_off, n);
    g_engine_off += n;
    return (int64_t)n;
}

static void et_close(void *ctx, sxcl_http_body *body) {
    (void)ctx;
    (void)body;
}
static void et_cancel(void *ctx) { (void)ctx; }
static void et_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *et_create(void *userdata) {
    static sxcl_transport transport;
    (void)userdata;
    memset(&transport, 0, sizeof(transport));
    transport.ctx = NULL;
    transport.request = et_request;
    transport.read = et_read;
    transport.close_body = et_close;
    transport.cancel_all = et_cancel;
    transport.destroy = et_destroy;
    return &transport;
}

static void test_default_download_hook(void) {
    group("默认下载钩子");
    char game[1024];
    case_dir("engine", game, sizeof(game));

    char dest[1200];
    snprintf(dest, sizeof(dest), "%s/payload.bin", game);
    char part[1300];
    snprintf(part, sizeof(part), "%s.part", dest);
    (void)sxcl_fs_remove(dest);
    (void)sxcl_fs_remove(part);

    char sha1[41];
    check_int(sxcl_hash_digest(SXCL_HASH_SHA1, kEnginePayload, strlen(kEnginePayload), sha1, sizeof(sha1)),
              0, "先算夹具的 sha1");

    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.transport_factory = et_create;
    opts.workers = 1;
    opts.retry_per_source = 1;

    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = dest;
    task.urls[0] = "https://example.invalid/payload.bin";
    task.sha1 = sha1;
    task.algo = SXCL_HASH_SHA1;
    task.size = (int64_t)strlen(kEnginePayload);
    task.priority = 0;
    task.label = "payload.bin";

    sxcl_task *tasks[1];
    tasks[0] = &task;
    sxcl_install_download request;
    memset(&request, 0, sizeof(request));
    request.stage = SXCL_INSTALL_STAGE_CLIENT_JAR;
    request.label = "客户端 jar";
    request.tasks = (sxcl_task *const *)tasks;
    request.count = 1;
    request.fatal = 1;
    request.engine_opts = &opts;

    sxcl_install_download_stats stats;
    char err[SXCL_INSTALL_ERROR_MAX];
    err[0] = '\0';
    const sxcl_install_io *io = sxcl_install_default_io();

    memset(&stats, 0, sizeof(stats));
    g_engine_requests = 0;
    check_int(io->download(NULL, &request, &stats, err, sizeof(err)), 0, "首次下载(真引擎 + 假传输)成功");
    check_int((int)task.state, (int)SXCL_TASK_DONE, "任务状态 = 完成");
    check_int((long)stats.files_done, 1, "统计:完成 1 个");
    check_int((long)stats.files_skipped, 0, "首次不算跳过");
    check_int(g_engine_requests, 1, "真的走了一次传输");
    check(sxcl_fs_exists(dest) == 1, "文件落盘");
    check(file_contains(dest, "SXCL-ENGINE-PAYLOAD") == 1, "内容与传输的一致");
    check_int((long)task.bytes_done, (long)strlen(kEnginePayload), "字节数对得上");

    /* 第二次:引擎快路径(已存在且校验通过)—— 一个请求都不该发 */
    memset(&stats, 0, sizeof(stats));
    const int before = g_engine_requests;
    check_int(io->download(NULL, &request, &stats, err, sizeof(err)), 0, "第二次也成功");
    check_int(g_engine_requests, before, "快路径:一个传输请求都没发");
    check_int((long)stats.files_skipped, 1, "统计:跳过 1 个");
    check_int((long)stats.files_done, 1, "统计:完成 1 个");
    check_int(sxcl_fs_exists(part), 0, "不留 .part");

    /* 没有传输后端:必须失败并给人话原因,不能假装成功 */
    sxcl_engine_opts none;
    memset(&none, 0, sizeof(none));
    request.engine_opts = &none;
    memset(&stats, 0, sizeof(stats));
    check(io->download(NULL, &request, &stats, err, sizeof(err)) < 0, "没有传输后端 = 失败");
    check(err[0] != '\0', "失败有人话原因");
    check(strstr(err, "传输后端") != NULL, "原因说清楚了是传输后端");
}

/* ── 下载源:prefer_mirror 真的把镜像排到了第一候选 ── */

static void test_prefer_mirror(void) {
    group("下载源(镜像优先)");
    char game[1024];
    sxcl_install_plan plan;
    sxcl_install_result res;

    /* 1) 镜像优先:每个文件的第一候选都必须是镜像 */
    reset_all();
    case_dir("mirror_first", game, sizeof(game));
    base_plan(&plan, game);
    plan.mirror_base = "https://bmclapi2.bangbang93.com";
    plan.prefer_mirror = 1;
    snprintf(g_fake.want_prefix, sizeof(g_fake.want_prefix), "%s", plan.mirror_base);
    const int rc = run_install(&plan, &res);
    check_int(rc, SXCL_INSTALL_OK, "镜像优先:整次安装成功");
    check_int((long)g_fake.url0_unexpected, 0, "镜像优先:每个任务的第一候选都是镜像");
    check_int((long)g_fake.missing_second, 0, "镜像优先:每个任务都留了官方第二候选(不通用自动切)");
    check(g_fake.download_requests >= 6, "镜像优先:确实下载了文件(不是全跳过)");
    check(strncmp(g_fake.fetch_url, plan.mirror_base, strlen(plan.mirror_base)) == 0,
          "镜像优先:版本清单也是先走镜像");

    /* 2) 对照:官方优先(老行为)—— 第一候选不许是镜像,镜像仍作第二候选 */
    reset_all();
    case_dir("mojang_first", game, sizeof(game));
    base_plan(&plan, game);
    plan.mirror_base = "https://bmclapi2.bangbang93.com";
    plan.prefer_mirror = 0;
    snprintf(g_fake.forbid_prefix, sizeof(g_fake.forbid_prefix), "%s", plan.mirror_base);
    const int rc2 = run_install(&plan, &res);
    check_int(rc2, SXCL_INSTALL_OK, "官方优先:整次安装成功");
    check_int((long)g_fake.url0_forbidden, 0, "官方优先:没有任务把镜像排在第一");
    check_int((long)g_fake.missing_second, 0, "官方优先:镜像仍留着作第二候选");
    check(strncmp(g_fake.fetch_url, plan.mirror_base, strlen(plan.mirror_base)) != 0,
          "官方优先:版本清单先走官方");

    /* 3) prefer_mirror=1 且没给 mirror_base:用核心默认 BMCLAPI,不是"没有镜像" */
    reset_all();
    case_dir("mirror_default", game, sizeof(game));
    base_plan(&plan, game);
    plan.prefer_mirror = 1;
    snprintf(g_fake.want_prefix, sizeof(g_fake.want_prefix), "%s", SXCL_MIRROR_BMCLAPI_BASE);
    const int rc3 = run_install(&plan, &res);
    check_int(rc3, SXCL_INSTALL_OK, "默认镜像:整次安装成功");
    check_int((long)g_fake.url0_unexpected, 0, "默认镜像:不给 mirror_base 时默认用 BMCLAPI");
    check_int((long)g_fake.missing_second, 0, "默认镜像:官方仍是第二候选");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* 崩了也要看到已经跑过的用例 */
    ft_create(NULL); /* 让静态表先初始化好(不调用也安全) */
    if (sxcl_fs_mkdirs(TMP_ROOT) != 0) {
        printf("建不出临时根目录 %s\n", TMP_ROOT);
        return 1;
    }
    test_stage_table();
    test_vanilla_run();
    test_skip_existing();
    test_client_jar_failure();
    test_library_partial_failure();
    test_cancel();
    test_loader_plan();
    test_loader_failure();
    test_args();
    test_http_text();
    test_default_download_hook();
    test_prefer_mirror();
    group("(结束)");

    printf("install 编排测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
