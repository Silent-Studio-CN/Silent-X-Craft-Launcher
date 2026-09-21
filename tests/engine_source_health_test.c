/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* R3 源级熔断夹具(**不联网**,内存假传输;夹具目录在 CWD 下的临时目录):
 *
 *   ① 第一条候选是黑洞(连不上)、第二条正常:**第一个文件**先撞黑洞再换好源,
 *      **第二个文件**不许再先撞黑洞(候选顺序按源健康度重排);
 *   ② BMCLAPI 的 403 连续来 9 次也**不**把它判死(它高频请求就会 403,算失败会把好源拉黑);
 *   ③ 非 BMCLAPI 的 403 连续来 9 次 -> 判死 -> 后面的文件先走好源(判死=排最后,不删掉兜底)。
 *
 * 打印每个文件的候选请求顺序与 source_index(原始输出就是证据)。 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/net.h"

#define TMP_ROOT "_sxcl_source_health_fixture"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

/* ── 假传输:按 URL 里的关键字决定"黑洞 / 403 / 好源" ── */

typedef struct fake_body {
    const char *data;
    size_t len;
    size_t off;
} fake_body;

static const char kPayloadA[] = "PAYLOAD-A-0123456789-0123456789-0123456789-0123456789";
static const char kPayloadB[] = "PAYLOAD-B-9876543210-9876543210-9876543210-9876543210";
static const char kPayloadC[] = "PAYLOAD-C-abcdefghij-abcdefghij-abcdefghij-abcdefghij";

static int g_seq[32];       /* 每个请求:0 = 黑洞,1 = 403 源,2 = 好源 */
static const char *g_seq_url[32];
static int g_seq_count;
static int g_seq_mark;      /* "从这一条开始算这一段" */

static void reset_seq(void) {
    g_seq_count = 0;
    g_seq_mark = 0;
}

static int seq_has(int kind) {
    for (int i = g_seq_mark; i < g_seq_count; ++i) {
        if (g_seq[i] == kind) {
            return 1;
        }
    }
    return 0;
}

static int seq_first(int kind) {
    return g_seq_count > g_seq_mark && g_seq[g_seq_mark] == kind;
}

/** 这一段请求里,打到某个主机(URL 里含 needle)的次数。 */
static int seq_count_url(const char *needle) {
    int n = 0;
    for (int i = g_seq_mark; i < g_seq_count; ++i) {
        if (g_seq_url[i] != NULL && strstr(g_seq_url[i], needle) != NULL) {
            ++n;
        }
    }
    return n;
}

static int sh_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                      sxcl_http_body **body) {
    (void)ctx;
    memset(resp, 0, sizeof(*resp));
    *body = NULL;
    const char *url = req->url ? req->url : "";

    if (strstr(url, "blackhole") != NULL) {
        if (g_seq_count < 32) {
            g_seq[g_seq_count] = 0;
            g_seq_url[g_seq_count] = url;
            ++g_seq_count;
        }
        return SXCL_NET_ERR_CONNECT; /* 连不上 */
    }
    if (strstr(url, "403.example.invalid") != NULL ||
        strstr(url, "bmclapi2.bangbang93.com") != NULL) {
        if (g_seq_count < 32) {
            g_seq[g_seq_count] = 1;
            g_seq_url[g_seq_count] = url;
            ++g_seq_count;
        }
        resp->status = strstr(url, "bmclapi") != NULL ? 403 : 403;
        resp->content_length = 0;
        resp->total_length = -1;
        return SXCL_NET_OK;
    }

    const char *payload = kPayloadA;
    size_t len = strlen(kPayloadA);
    if (strstr(url, "b.bin") != NULL) {
        payload = kPayloadB;
        len = strlen(kPayloadB);
    } else if (strstr(url, "c.bin") != NULL || strstr(url, "many") != NULL) {
        payload = kPayloadC;
        len = strlen(kPayloadC);
    }
    if (g_seq_count < 32) {
        g_seq[g_seq_count] = 2;
        g_seq_url[g_seq_count] = url;
        ++g_seq_count;
    }
    fake_body *b = (fake_body *)malloc(sizeof(fake_body));
    if (!b) {
        return SXCL_NET_ERR_IO;
    }
    b->data = payload;
    b->len = len;
    b->off = 0;
    resp->status = 200;
    resp->content_length = (int64_t)len;
    resp->total_length = (int64_t)len;
    *body = (sxcl_http_body *)b;
    return SXCL_NET_OK;
}

static int64_t sh_read(void *ctx, sxcl_http_body *body, void *buf, size_t len) {
    (void)ctx;
    fake_body *b = (fake_body *)body;
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

static void sh_close(void *ctx, sxcl_http_body *body) {
    (void)ctx;
    free(body);
}
static void sh_cancel(void *ctx) { (void)ctx; }
static void sh_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *sh_create(void *userdata) {
    static sxcl_transport transport;
    (void)userdata;
    memset(&transport, 0, sizeof(transport));
    transport.request = sh_request;
    transport.read = sh_read;
    transport.close_body = sh_close;
    transport.cancel_all = sh_cancel;
    transport.destroy = sh_destroy;
    return &transport;
}

/* ── 跑一批任务(同一个引擎 = 同一张源健康表) ── */

static int run_batch(sxcl_task **tasks, int count) {
    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = 1;            /* 阈值 = Clamp(1,5,30)+2 = 7 */
    opts.retry_per_source = 1;
    opts.max_conn_per_file = 1;
    opts.transport_factory = sh_create;

    sxcl_engine *engine = sxcl_engine_create(&opts);
    if (!engine) {
        return -99;
    }
    for (int i = 0; i < count; ++i) {
        if (sxcl_engine_submit(engine, tasks[i]) != 0) {
            sxcl_engine_destroy(engine);
            return -98;
        }
    }
    const int failed = sxcl_engine_run(engine);
    sxcl_engine_destroy(engine);
    return failed;
}

static void print_seq(const char *title) {
    char line[512];
    int used = 0;
    line[0] = '\0';
    for (int i = g_seq_mark; i < g_seq_count && used < (int)sizeof(line) - 64; ++i) {
        const char *kind = g_seq[i] == 0 ? "黑洞" : (g_seq[i] == 1 ? "403源" : "好源");
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%s ", kind);
    }
    printf("     %s: 请求顺序 [%s]\n", title, line);
}

static void init_task(sxcl_task *t, const char *dest, const char *u0, const char *u1,
                      int64_t size) {
    memset(t, 0, sizeof(*t));
    t->dest = dest;
    t->urls[0] = u0;
    t->urls[1] = u1;
    t->size = size;
    t->algo = SXCL_HASH_SHA1;
    t->priority = 10;
}

static void test_blackhole_reorder(void) {
    printf("\n== ① 黑洞源:第二个文件不许再先撞它 ==\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/blackhole", TMP_ROOT);
    (void)sxcl_fs_remove_tree(dir);
    (void)sxcl_fs_mkdirs(dir);

    char dest1[600];
    char dest2[600];
    snprintf(dest1, sizeof(dest1), "%s/a.bin", dir);
    snprintf(dest2, sizeof(dest2), "%s/b.bin", dir);

    sxcl_task t1;
    sxcl_task t2;
    init_task(&t1, dest1, "https://blackhole.example.invalid/a.bin",
              "https://good.example.invalid/a.bin", (int64_t)strlen(kPayloadA));
    init_task(&t2, dest2, "https://blackhole.example.invalid/b.bin",
              "https://good.example.invalid/b.bin", (int64_t)strlen(kPayloadB));

    reset_seq();
    sxcl_task *batch[1];
    batch[0] = &t1;
    const int f1 = run_batch(batch, 1);
    print_seq("文件 1(黑洞在前)");
    printf("     文件 1: 状态=%d source_index=%d\n", (int)t1.state, t1.source_index);
    check(f1 == 0, "文件 1 最终成功(黑洞挂了之后换好源)");
    check(seq_first(0), "文件 1 的**第一个**请求就是黑洞(候选顺序照调用方给的来)");
    check(seq_has(1) == 0 && seq_has(2), "文件 1 换到好源后拿到了内容");
    check(t1.source_index == 1, "文件 1 的 source_index = 1(好源那一路)");

    /* 第二个文件:黑洞的失败数已经记在引擎里 —— 但它挂在**引擎**上,而上面那批已经销毁了。
     * 所以这里必须把两个文件放进**同一个引擎**(同一批)。下面的 mock 负责把两批合成一次跑。 */
    t1.state = SXCL_TASK_PENDING;
    (void)sxcl_fs_remove(dest1);
    (void)sxcl_fs_remove(dest2);
    reset_seq();
    sxcl_task *pair[2];
    pair[0] = &t1;
    pair[1] = &t2;
    const int f2 = run_batch(pair, 2);
    /* 前两条请求属于文件 1(黑洞、黑洞重试... 然后好源) */
    int hole_before_file2 = 0;
    int file2_start = -1;
    for (int i = 0; i < g_seq_count; ++i) {
        if (strstr(g_seq_url[i], "b.bin") != NULL) {
            file2_start = i;
            break;
        }
    }
    for (int i = 0; i < file2_start; ++i) {
        if (g_seq[i] == 0) {
            ++hole_before_file2;
        }
    }
    int holes_in_file2 = 0;
    for (int i = file2_start; i < g_seq_count && file2_start >= 0; ++i) {
        if (g_seq[i] == 0) {
            ++holes_in_file2;
        }
    }
    print_seq("同一批:文件 1 + 文件 2");
    printf("     文件 1: 状态=%d source_index=%d | 文件 2: 状态=%d source_index=%d\n",
           (int)t1.state, t1.source_index, (int)t2.state, t2.source_index);
    printf("     文件 1 段里的黑洞请求=%d,文件 2 段里的黑洞请求=%d(文件 2 从第 %d 条请求开始)\n",
           hole_before_file2, holes_in_file2, file2_start);
    check(f2 == 0, "两个文件最终都成功");
    check(t2.state == SXCL_TASK_DONE, "文件 2 完成");
    check(hole_before_file2 >= 1, "文件 1 确实撞过黑洞(夹具有效)");
    check(holes_in_file2 == 0, "**文件 2 一条黑洞请求都没有**(候选顺序被重排:好源在前)");
    check(t2.source_index == 1, "文件 2 的 source_index = 1(好源)");
}

static void test_bmclapi_403_not_counted(void) {
    printf("\n== ② BMCLAPI 的 403 连来 9 次也不判死(PCL 同款口径)==\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/forbidden", TMP_ROOT);
    (void)sxcl_fs_remove_tree(dir);
    (void)sxcl_fs_mkdirs(dir);

    sxcl_task tasks[10];
    char dests[10][600];
    for (int i = 0; i < 10; ++i) {
        snprintf(dests[i], sizeof(dests[i]), "%s/many-%d.bin", dir, i);
        init_task(&tasks[i], dests[i], "https://bmclapi2.bangbang93.com/many/x.bin",
                  "https://good.example.invalid/many/x.bin", (int64_t)strlen(kPayloadC));
    }
    sxcl_task *ptrs[10];
    for (int i = 0; i < 10; ++i) {
        ptrs[i] = &tasks[i];
    }
    reset_seq();
    const int failed = run_batch(ptrs, 10);
    check(failed == 0, "10 个文件全部成功(403 源失败后换好源)");
    /* 最后一个文件的**第一条请求**必须还是 BMCLAPI —— 它没被判死 */
    const int bmclapi_hits = seq_count_url("bmclapi2.bangbang93.com");
    print_seq("10 个文件(403 源在前)");
    printf("     打到 BMCLAPI 的请求数=%d(10 个文件各一次 = 一次都没被跳过)\n", bmclapi_hits);
    check(bmclapi_hits == 10,
          "10 个文件**每个都仍然先试 BMCLAPI**(403 不算失败,一次都没被判死/降级)");
    check(tasks[9].source_index == 1, "第 10 个文件最终用好源(source_index=1)");
}

static void test_real_403_gets_demoted(void) {
    printf("\n== ③ 非 BMCLAPI 的 403 源:失败一次就排到干净源之后,并且一直不再回头 ==\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/demote", TMP_ROOT);
    (void)sxcl_fs_remove_tree(dir);
    (void)sxcl_fs_mkdirs(dir);

    sxcl_task tasks[10];
    char dests[10][600];
    for (int i = 0; i < 10; ++i) {
        snprintf(dests[i], sizeof(dests[i]), "%s/many-%d.bin", dir, i);
        init_task(&tasks[i], dests[i], "https://403.example.invalid/many/x.bin",
                  "https://good.example.invalid/many/x.bin", (int64_t)strlen(kPayloadC));
    }
    sxcl_task *ptrs[10];
    for (int i = 0; i < 10; ++i) {
        ptrs[i] = &tasks[i];
    }
    reset_seq();
    const int failed = run_batch(ptrs, 10);
    check(failed == 0, "10 个文件全部成功");
    const int forbidden_hits = seq_count_url("403.example.invalid");
    print_seq("10 个文件(403 源在前)");
    printf("     打到 403 源的请求数=%d(第 1 个文件失败一次之后,它的失败数 1 > 干净源的 0,\n"
           "     后面 9 个文件**一个都不再撞它** —— 排序键 = (是否判死, 连续失败数),判死不删源)\n",
           forbidden_hits);
    check(forbidden_hits == 1, "失败过一次的源立刻排到干净源之后(后 9 个文件不再撞它)");
    check(seq_count_url("good.example.invalid") == 10, "10 个文件全部由好源完成");
}

int main(void) {
    printf("SXCL 源健康表(源级熔断)夹具 —— 临时目录 %s,不联网\n", TMP_ROOT);
    (void)sxcl_fs_mkdirs(TMP_ROOT);
    test_blackhole_reorder();
    test_bmclapi_403_not_counted();
    test_real_403_gets_demoted();
    printf("\n夹具结果:通过 %d 项,失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
