/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* 事故夹具(三件套)—— 证明"一唤醒就自动续装并覆盖用户已有版本"这一类缺陷不再发生:
 *
 *   ① 目标已存在(versions/<名>/<名>.json 已在且可解析)-> 安装必须**拒绝**并给
 *      SXCL_INSTALL_ERR_TARGET_EXISTS,而不是把用户的文件覆盖掉;
 *   ② .part 已经被预分配到**最终大小**(内容是错的)+ .part.json 里显式记着 written=<n>
 *      -> 续传起点必须是那个 n(不是文件大小,不会拼出 Range: bytes=<final>- 而 416 删片);
 *   ③ size=0 且无哈希的 loader 库残留 -> **不许**被算成"已存在且校验通过"(一个字节都不下的那种)。
 *
 * 夹具全部落在 CWD 下的临时目录 _sxcl_target_fixture_<pid> 里(带进程号 = 并行跑也互不踩;
 * 跑完自己清掉):不联网、**不碰任何真实游戏目录**。 */

#define _CRT_SECURE_NO_WARNINGS 1 /* 夹具用 fopen 造文件,MSVC 会标记弃用 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/install.h"
#include "sxcl/net.h"
#include "sxcl/verify.h"

/* 夹具根目录 = 这个名字 + **进程号**:同一棵树里并行跑两份 ctest 各用各的,互不踩。
 * (不靠"跑的时候别并发";跑完在 main 末尾把自己那份删掉。)*/
#define TMP_ROOT_BASE "_sxcl_target_fixture"
#if defined(_WIN32)
#  include <process.h>
#  define SXCL_TEST_PID() ((long)_getpid())
#else
#  include <unistd.h>
#  define SXCL_TEST_PID() ((long)getpid())
#endif
static char g_root[256];

static int g_pass = 0;
static int g_fail = 0;
static const char *g_group = "(未分组)";

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s | %s\n", g_group, what);
    }
}

static void check_int(long got, long want, const char *what) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s | %s: got %ld want %ld\n", g_group, what, got, want);
    }
}

static void group(const char *name) {
    if (g_group[0] != '(') {
        printf("  [%s] 结束\n", g_group);
    }
    g_group = name;
    printf("\n== %s ==\n", name);
}

/* ── 文件小工具 ── */

static int write_file(const char *path, const void *data, size_t len) {
    sxcl_fs_mkdirs_for_file(path);
    FILE *fh = sxcl_fs_fopen(path, "wb");
    if (!fh) {
        return -1;
    }
    const int ok = (len == 0) || (fwrite(data, 1, len, fh) == len);
    fclose(fh);
    return ok ? 0 : -1;
}

static int read_file(const char *path, char *out, size_t cap) {
    FILE *fh = sxcl_fs_fopen(path, "rb");
    if (!fh) {
        return -1;
    }
    const size_t got = fread(out, 1, cap - 1, fh);
    out[got] = '\0';
    fclose(fh);
    return (int)got;
}

static int file_has(const char *path, const char *needle) {
    char buf[8192];
    const int got = read_file(path, buf, sizeof(buf));
    return got > 0 && strstr(buf, needle) != NULL;
}

static int file_equals(const char *path, const void *want, size_t len) {
    char buf[8192];
    if (len + 1 > sizeof(buf)) {
        return 0;
    }
    const int got = read_file(path, buf, sizeof(buf));
    return got == (int)len && memcmp(buf, want, len) == 0;
}

static void fixture_dir(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_root, name);
    if (sxcl_fs_mkdirs(out) != 0) {
        printf("  [!!] 建不出夹具目录: %s\n", out);
    }
}

/* ───────────────────────── ① 目标已存在:必须拒装,绝不覆盖 ───────────────────────── */

/* 只在预检之前会被调到的钩子:预检拒绝 = 这些计数必须全是 0(一个字节都没联网)。 */
typedef struct io_hits {
    int fetch;
    int download;
    int loader;
    int natives;
} io_hits;

static io_hits g_hits;

static int hit_fetch(void *userdata, const char *url, char **out_text, char *err, size_t err_len) {
    (void)userdata;
    (void)url;
    (void)out_text;
    ++g_hits.fetch;
    if (err && err_len) {
        snprintf(err, err_len, "夹具:不该被调用");
    }
    return -1;
}

static int hit_download(void *userdata, const sxcl_install_download *request,
                        sxcl_install_download_stats *stats, char *err, size_t err_len) {
    (void)userdata;
    (void)request;
    (void)stats;
    ++g_hits.download;
    if (err && err_len) {
        snprintf(err, err_len, "夹具:不该被调用");
    }
    return -1;
}

static int hit_loader(void *userdata, const sxcl_loader_install_request *request,
                      sxcl_loader_install_result *out) {
    (void)userdata;
    (void)request;
    (void)out;
    ++g_hits.loader;
    return -1;
}

static int hit_natives(void *userdata, const sxcl_json *version_json, const char *game_dir,
                       const char *natives_dir, int *out_count, char *err, size_t err_len) {
    (void)userdata;
    (void)version_json;
    (void)game_dir;
    (void)natives_dir;
    (void)out_count;
    ++g_hits.natives;
    if (err && err_len) {
        snprintf(err, err_len, "夹具:不该被调用");
    }
    return -1;
}

static const sxcl_install_io kHitIo = {hit_fetch, hit_download, hit_loader, hit_natives, NULL};

/* 用户真实在用的那个版本 JSON(带一个只属于它的标记:*绝不能*被安装覆盖)。 */
static const char kUserVersionJson[] =
    "{\n  \"id\": \"1.20.1\",\n  \"USER-KEPT-MARKER\": \"my-real-install\",\n"
    "  \"mainClass\": \"net.minecraft.client.main.Main\"\n}\n";

static const char kBrokenJson[] = "{ \"id\": \"1.20.1\", 半截";

static void test_target_probe(void) {
    group("①a 共用判定:sxcl_install_target_probe");
    char dir[1024];
    char path[1200];
    fixture_dir("probe", dir, sizeof(dir));
    (void)sxcl_fs_remove_tree(dir);
    fixture_dir("probe", dir, sizeof(dir));

    check_int(sxcl_install_target_probe(dir, "1.20.1"), SXCL_INSTALL_TARGET_NONE,
              "空目录 = 什么都没有");
    check_int(sxcl_install_target_probe(dir, NULL), SXCL_INSTALL_TARGET_NONE,
              "实例名为空 = 什么都没有(不越界)");

    snprintf(path, sizeof(path), "%s/versions/1.20.1/1.20.1.jar", dir);
    check(write_file(path, "JAR", 3) == 0, "摆一个 jar 残骸");
    check_int(sxcl_install_target_probe(dir, "1.20.1"), SXCL_INSTALL_TARGET_JAR,
              "只有 jar = 残骸(不是'装过')");

    snprintf(path, sizeof(path), "%s/versions/1.20.1/1.20.1.json", dir);
    check(write_file(path, kBrokenJson, strlen(kBrokenJson)) == 0, "摆一个解析不了的 JSON");
    check_int(sxcl_install_target_probe(dir, "1.20.1"), SXCL_INSTALL_TARGET_JAR,
              "半截 JSON 不算'装过'(还是只有 jar 那个标志)");

    check(write_file(path, kUserVersionJson, strlen(kUserVersionJson)) == 0, "摆一个能解析的 JSON");
    check_int(sxcl_install_target_probe(dir, "1.20.1"), SXCL_INSTALL_TARGET_JSON,
              "JSON 能解析 = 装过(JSON 标志优先,JAR 只是残骸的说法)");

    char why[512];
    check_int(sxcl_install_target_describe(why, sizeof(why), dir, "1.20.1"),
              SXCL_INSTALL_TARGET_JSON, "人话文案与判定同源");
    check(strstr(why, "已经装过") != NULL, "错误文案是人话(这个版本已经装过了…)");
}

static void test_target_exists_refuses(void) {
    group("①b 目标已存在 -> 拒装 + 绝不覆盖");
    char dir[1024];
    char path[1200];
    fixture_dir("exists", dir, sizeof(dir));
    (void)sxcl_fs_remove_tree(dir);
    fixture_dir("exists", dir, sizeof(dir));

    snprintf(path, sizeof(path), "%s/versions/1.20.1/1.20.1.json", dir);
    check(write_file(path, kUserVersionJson, strlen(kUserVersionJson)) == 0,
          "预置用户已有的版本 JSON(带 USER-KEPT-MARKER)");

    memset(&g_hits, 0, sizeof(g_hits));
    sxcl_install_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.game_dir = dir;
    plan.version_id = "1.20.1"; /* instance 缺省 = version_id -> versions/1.20.1/1.20.1.json */
    plan.assets = SXCL_INSTALL_ASSETS_DEFAULT;

    sxcl_install_request req;
    memset(&req, 0, sizeof(req));
    req.plan = &plan;
    req.io = &kHitIo;

    sxcl_install_result res;
    memset(&res, 0, sizeof(res));
    const int rc = sxcl_install_run(&req, &res);

    check_int(rc, SXCL_INSTALL_ERR_TARGET_EXISTS, "返回码 = SXCL_INSTALL_ERR_TARGET_EXISTS(-10)");
    check_int(res.code, SXCL_INSTALL_ERR_TARGET_EXISTS, "结果里的码也是它");
    check(strcmp(sxcl_install_code_name(res.code), "target_exists") == 0,
          "稳定名字 = target_exists(日志/界面可解析)");
    check_int(res.retryable, 0, "不可重试(重试也不会变)");
    check_int(res.cancelled, 0, "不是取消");
    check(strstr(res.error, "已经装过") != NULL, "错误文案写人话('这个版本已经装过了')");
    check_int((long)g_hits.fetch, 0, "一个字节都没联网(取文本 0 次)");
    check_int((long)g_hits.download, 0, "没有发起任何下载");
    check_int((long)g_hits.loader, 0, "没有跑加载器安装");
    check_int((long)g_hits.natives, 0, "没有解压 natives");
    check_int((long)res.stages_done, 0, "一个阶段都没跑");
    check(file_has(path, "USER-KEPT-MARKER") == 1, "用户的版本 JSON **原样还在**(没被覆盖)");
    check(file_has(path, "mainClass") == 1, "旧内容整份保留");
    printf("     证据:rc=%d code_name=%s retryable=%d stages_done=%zu 取文本=%d 下载=%d | "
           "用户 json 标记还在=%d\n",
           rc, sxcl_install_code_name(res.code), res.retryable, res.stages_done, g_hits.fetch,
           g_hits.download, file_has(path, "USER-KEPT-MARKER"));
    printf("     证据(错误文案):%s\n", res.error);

    /* 对照:把那个 JSON 删掉 -> 同一个计划就能跑(拒装是"目标已存在"造成的,不是别的) */
    (void)sxcl_fs_remove(path);
    memset(&res, 0, sizeof(res));
    const int rc2 = sxcl_install_run(&req, &res);
    check(rc2 != SXCL_INSTALL_ERR_TARGET_EXISTS,
          "目标清掉之后不再以'已存在'拒绝(证明拒装就是那条预检)");
    check_int((long)g_hits.fetch, 1, "这时才真的去取版本清单(夹具钩子被调用一次)");
}

/* ───────────────── ②/.part 续传起点 = .part.json 里显式记的字节数 ───────────────── */

/* 内存假传输:如实兑现 Range(206 + 正确的 range_start/total),Range 越界给 416。
 * 记下每个请求的 range_start —— 续传起点是不是"显式记录的字节数"看的就是它。 */
typedef struct mem_body {
    const unsigned char *data;
    size_t len;
    size_t off;
} mem_body;

static const unsigned char *g_payload = NULL;
static size_t g_payload_len = 0;
static int g_requests = 0;
static int g_ranged_requests = 0;
static int g_full_requests = 0;
static int g_416 = 0;
static int64_t g_first_range_start = -1;

static int mt_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                      sxcl_http_body **body) {
    (void)ctx;
    memset(resp, 0, sizeof(*resp));
    *body = NULL;
    ++g_requests;

    const int64_t start = req->range_start;
    if (start > 0) {
        ++g_ranged_requests;
        if (g_first_range_start < 0) {
            g_first_range_start = start;
        }
    } else {
        ++g_full_requests;
    }
    if (start > (int64_t)g_payload_len) {
        ++g_416;
        resp->status = 416;
        resp->content_length = 0;
        resp->total_length = (int64_t)g_payload_len;
        resp->range_start = -1;
        resp->range_end = -1;
        return SXCL_NET_OK;
    }

    mem_body *b = (mem_body *)malloc(sizeof(mem_body));
    if (!b) {
        return SXCL_NET_ERR_IO;
    }
    b->data = g_payload + (start > 0 ? (size_t)start : 0);
    b->len = g_payload_len - (start > 0 ? (size_t)start : 0);
    b->off = 0;
    if (start > 0) {
        resp->status = 206;
        resp->range_start = start;
        resp->range_end = (int64_t)g_payload_len - 1;
        resp->content_length = (int64_t)b->len;
        resp->total_length = (int64_t)g_payload_len;
        resp->is_range_response = 1;
    } else {
        resp->status = 200;
        resp->range_start = -1;
        resp->range_end = -1;
        resp->content_length = (int64_t)b->len;
        resp->total_length = (int64_t)g_payload_len;
    }
    *body = (sxcl_http_body *)b;
    return SXCL_NET_OK;
}

static int64_t mt_read(void *ctx, sxcl_http_body *body, void *buf, size_t len) {
    (void)ctx;
    mem_body *b = (mem_body *)body;
    if (!b || b->off >= b->len) {
        return 0;
    }
    size_t n = b->len - b->off;
    if (n > len) {
        n = len;
    }
    memcpy(buf, b->data + b->off, n);
    b->off += n;
    return (int64_t)n;
}

static void mt_close(void *ctx, sxcl_http_body *body) {
    (void)ctx;
    free(body);
}

static void mt_cancel(void *ctx) { (void)ctx; }
static void mt_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *mt_create(void *userdata) {
    static sxcl_transport transport;
    (void)userdata;
    memset(&transport, 0, sizeof(transport));
    transport.ctx = NULL;
    transport.request = mt_request;
    transport.read = mt_read;
    transport.close_body = mt_close;
    transport.cancel_all = mt_cancel;
    transport.destroy = mt_destroy;
    return &transport;
}

static int run_one_task(sxcl_task *task) {
    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = 1;
    opts.retry_per_source = 1;
    opts.max_conn_per_file = 1; /* 单连接:本夹具针对的就是这条路径 */
    opts.transport_factory = mt_create;

    sxcl_engine *engine = sxcl_engine_create(&opts);
    if (!engine) {
        return -99;
    }
    if (sxcl_engine_submit(engine, task) != 0) {
        sxcl_engine_destroy(engine);
        return -98;
    }
    const int failed = sxcl_engine_run(engine);
    sxcl_engine_destroy(engine);
    return failed;
}

static void test_resume_from_written_record(void) {
    group("② .part 到最终大小也不能拿文件大小当续传起点");
    char dir[1024];
    char dest[1200];
    char part[1300];
    char pj[1400];
    char expected[1200];
    fixture_dir("resume", dir, sizeof(dir));
    (void)sxcl_fs_remove_tree(dir);
    fixture_dir("resume", dir, sizeof(dir));
    snprintf(dest, sizeof(dest), "%s/payload.bin", dir);
    snprintf(part, sizeof(part), "%s.part", dest);
    snprintf(pj, sizeof(pj), "%s.part.json", dest);
    snprintf(expected, sizeof(expected), "%s/expected.bin", dir);

    const size_t total = 4096;
    unsigned char *payload = (unsigned char *)malloc(total);
    unsigned char *wrong = (unsigned char *)malloc(total);
    check(payload != NULL && wrong != NULL, "夹具缓冲分配");
    if (!payload || !wrong) {
        free(payload);
        free(wrong);
        return;
    }
    for (size_t i = 0; i < total; ++i) {
        payload[i] = (unsigned char)('A' + (int)(i % 26));
        wrong[i] = (unsigned char)'X';
    }
    g_payload = payload;
    g_payload_len = total;

    /* 真摘要:拿正确的负载算出来(强校验路径被真正走到) */
    char sha[65];
    sha[0] = '\0';
    check(write_file(expected, payload, total) == 0, "写正确负载(算摘要用)");
    check(sxcl_hash_file(expected, SXCL_HASH_SHA1, sha, sizeof(sha)) == 0, "算出 SHA-1");
    check(strlen(sha) == 40, "SHA-1 是 40 个字符");
    (void)sxcl_fs_remove(expected);

    /* 夹具:**已到最终大小**的 .part(预分配的结果),前 written 字节是已经下好的正确前缀,
     * 后面那段是没下过的垃圾 —— "文件大小 = 进度"这句话在这里彻底不成立。
     * 同时 .part.json 显式记着 written=1536,续传起点只能来自它。 */
    const size_t written = 1536;
    memcpy(wrong, payload, written); /* 前缀:真的下好的那一段 */
    check(write_file(part, wrong, total) == 0, "预置一个已到最终大小的 .part(尾部是垃圾)");
    int64_t part_size = 0;
    check(sxcl_fs_stat(part, &part_size, NULL) == 0 && part_size == (int64_t)total,
          "夹具前提:.part 的文件大小 == 最终大小(所以它绝不能当进度用)");
    char js[128];
    snprintf(js, sizeof(js), "sxcl-part 1\nsize=%llu\nwritten=%llu\n", (unsigned long long)total,
             (unsigned long long)written);
    check(write_file(pj, js, strlen(js)) == 0, "预置 .part.json:written=1536");

    g_requests = 0;
    g_ranged_requests = 0;
    g_full_requests = 0;
    g_416 = 0;
    g_first_range_start = -1;

    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = dest;
    task.urls[0] = "https://example.invalid/payload.bin";
    task.size = (int64_t)total;
    task.sha1 = sha;
    task.algo = SXCL_HASH_SHA1;
    task.priority = 10;
    task.label = "payload";

    const int failed = run_one_task(&task);

    check_int(failed, 0, "引擎跑完没有失败任务");
    check_int((int)task.state, SXCL_TASK_DONE, "任务 DONE");
    check_int((long)task.resume_from, (long)written, "引擎自报的续传起点 = 1536(显式记录)");
    check_int((long)g_first_range_start, (long)written,
              "第一个请求的 Range 起点 = 1536(**不是** 4096 = 文件大小)");
    check_int(g_416, 0, "没有出现 416(Range 不可满足)");
    check_int(g_ranged_requests, 1, "只发了一次带 Range 的请求(剩下的字节一次拿完)");
    check_int(g_full_requests, 0, "没有从头全量重下");
    check_int((int)task.skipped_existing, 0, "这不是'已存在'快路径(目标文件本来就不在)");
    check(file_equals(dest, payload, total) == 1, "落位的文件内容 = 正确负载(续传拼对了)");
    check(sxcl_fs_exists(pj) == 0, "完成后 .part.json 被清掉(不留过期进度)");
    check(sxcl_fs_exists(part) == 0, "完成后 .part 已改名为正式文件");
    printf("     证据:.part 文件大小=%lld(最终大小) 显式记录 written=%llu -> 实际 Range 起点=%lld | "
           "带 Range 请求=%d 全量请求=%d 416=%d | task.resume_from=%lld\n",
           (long long)part_size, (unsigned long long)written, (long long)g_first_range_start,
           g_ranged_requests, g_full_requests, g_416, (long long)task.resume_from);

    free(payload);
    free(wrong);
    g_payload = NULL;
    g_payload_len = 0;
}

/* 旧行为(拿文件大小当起点)的对照:同一份夹具,如果不看 .part.json 就会这么挂。 */
static void test_file_size_would_be_416(void) {
    group("②b 对照:拿文件大小当起点 = 416(这条证明夹具真的能抓住旧行为)");
    char dir[1024];
    char dest[1200];
    char part[1300];
    fixture_dir("would416", dir, sizeof(dir));
    (void)sxcl_fs_remove_tree(dir);
    fixture_dir("would416", dir, sizeof(dir));
    snprintf(dest, sizeof(dest), "%s/payload.bin", dir);
    snprintf(part, sizeof(part), "%s.part", dest);

    const size_t total = 2048;
    unsigned char *payload = (unsigned char *)malloc(total);
    unsigned char *wrong = (unsigned char *)malloc(total);
    if (!payload || !wrong) {
        free(payload);
        free(wrong);
        return;
    }
    memset(payload, 'P', total);
    memset(wrong, 'X', total);
    g_payload = payload;
    g_payload_len = total;

    /* 只有 .part(没有任何 .part.json 记录):引擎必须**从头下**,不许用文件大小 */
    check(write_file(part, wrong, total) == 0, "预置一个已到最终大小的 .part(没有 .part.json)");
    g_requests = 0;
    g_ranged_requests = 0;
    g_full_requests = 0;
    g_416 = 0;
    g_first_range_start = -1;

    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = dest;
    task.urls[0] = "https://example.invalid/payload.bin";
    task.size = (int64_t)total;
    task.algo = SXCL_HASH_SHA1;

    const int failed = run_one_task(&task);
    check_int(failed, 0, "引擎跑完没有失败任务");
    check_int((long)task.resume_from, 0, "没有记录 = 起点 0(全新下)");
    check_int(g_ranged_requests, 0, "没有任何带 Range 的请求(更不会有 416)");
    check_int(g_416, 0, "一个 416 都没有");
    check_int(g_full_requests, 1, "一次全量请求");
    check(file_equals(dest, payload, total) == 1, "最终内容正确");

    free(payload);
    free(wrong);
    g_payload = NULL;
    g_payload_len = 0;
}

/* ─────────────────── ③ 无校验信息(size=0/无哈希)的残留不许算"已存在" ─────────────────── */

static void test_no_verify_info_not_skipped(void) {
    group("③ size=0/无哈希的 loader 库残留不许当'已存在且校验通过'");
    char dir[1024];
    char dest[1200];
    fixture_dir("noverify", dir, sizeof(dir));
    (void)sxcl_fs_remove_tree(dir);
    fixture_dir("noverify", dir, sizeof(dir));
    snprintf(dest, sizeof(dest), "%s/libraries/loader-lib.jar", dir);

    const size_t total = 512;
    unsigned char *payload = (unsigned char *)malloc(total);
    unsigned char *wrong = (unsigned char *)malloc(total);
    if (!payload || !wrong) {
        free(payload);
        free(wrong);
        return;
    }
    for (size_t i = 0; i < total; ++i) {
        payload[i] = (unsigned char)(i * 7 + 3);
        wrong[i] = (unsigned char)'Z';
    }
    g_payload = payload;
    g_payload_len = total;

    /* 夹具:磁盘上已经有同名文件(内容是错的)—— 加载器依赖库就是这样被建的:size=0、没哈希 */
    check(write_file(dest, wrong, total) == 0, "预置一个同名的错误残留");
    g_requests = 0;
    g_ranged_requests = 0;
    g_full_requests = 0;
    g_416 = 0;
    g_first_range_start = -1;

    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = dest;
    task.urls[0] = "https://example.invalid/loader-lib.jar";
    task.size = 0;    /* 加载器依赖库:没有官方哈希、也没有大小(install.c 就是这么建的) */
    task.sha1 = NULL; /* 无哈希 */
    task.algo = SXCL_HASH_SHA1;

    const int failed = run_one_task(&task);
    check_int(failed, 0, "引擎跑完没有失败任务");
    check_int((int)task.verify_state, SXCL_TASK_VERIFY_NONE,
              "引擎显式标出'无校验信息'(verify_state = NONE)");
    check_int((int)task.skipped_existing, 0, "**没有**被算成'已存在且校验通过'");
    check(g_requests >= 1, "真的发了请求(残留没有被当成熟文件跳过)");
    check_int((int)task.state, SXCL_TASK_DONE, "任务完成(内容被真下了一遍)");
    check(file_equals(dest, payload, total) == 1, "磁盘上的错误残留被正确内容替换");
    const int no_verify_requests = g_requests; /* 记下来:下面要清零给对照用 */

    /* 对照:同样的文件 + 有摘要 -> 才允许走"已存在且校验通过"的快路径(0 个请求) */
    g_requests = 0;
    g_ranged_requests = 0;
    g_full_requests = 0;
    char sha[65];
    sha[0] = '\0';
    char expected[1200];
    snprintf(expected, sizeof(expected), "%s/expected.bin", dir);
    check(write_file(expected, payload, total) == 0, "写正确负载(算摘要用)");
    check(sxcl_hash_file(expected, SXCL_HASH_SHA1, sha, sizeof(sha)) == 0, "算出 SHA-1");
    (void)sxcl_fs_remove(expected);

    sxcl_task again;
    memset(&again, 0, sizeof(again));
    again.dest = dest;
    again.urls[0] = "https://example.invalid/loader-lib.jar";
    again.size = (int64_t)total;
    again.sha1 = sha;
    again.algo = SXCL_HASH_SHA1;
    const int failed2 = run_one_task(&again);
    check_int(failed2, 0, "对照:跑完没有失败任务");
    check_int((int)again.verify_state, SXCL_TASK_VERIFY_HASH, "对照:有摘要 = 强校验");
    check_int((int)again.skipped_existing, 1, "对照:这才叫'已存在且校验通过'");
    check_int(g_requests, 0, "对照:一个请求都没发(真跳过)");
    printf("     证据(无校验信息):verify_state=%d(NONE=%d) skipped_existing=%d 请求数=%d | "
           "对照(有摘要):verify_state=%d skipped_existing=%d 请求数=%d\n",
           (int)task.verify_state, (int)SXCL_TASK_VERIFY_NONE, (int)task.skipped_existing,
           no_verify_requests, (int)again.verify_state, (int)again.skipped_existing, g_requests);

    free(payload);
    free(wrong);
    g_payload = NULL;
    g_payload_len = 0;
}

int main(void) {
    snprintf(g_root, sizeof(g_root), "%s_%ld", TMP_ROOT_BASE, SXCL_TEST_PID());
    printf("SXCL 目标已存在 / 续传起点 / 无校验信息 夹具(临时目录 %s,绝不碰真实游戏目录)\n",
           g_root);
    (void)sxcl_fs_mkdirs(g_root);

    test_target_probe();
    test_target_exists_refuses();
    test_resume_from_written_record();
    test_file_size_would_be_416();
    test_no_verify_info_not_skipped();

    (void)sxcl_fs_remove_tree(g_root); // 清掉自己这份(带进程号的)夹具目录
    printf("\n夹具结果:通过 %d 项,失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
