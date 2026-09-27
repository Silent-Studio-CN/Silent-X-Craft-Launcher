/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 *
 * 模组中文名表的单测（docs/29 的数据来源、docs/28 的云端契约）。**不联网**：
 * 夹具是内嵌的表文本，更新那条路用假 transport 喂真文本 —— 真解析、真 SHA-256、真原子替换、
 * 真目录，只有网络那一跳是假的。临时文件都写在构建目录下（仓库里不留夹具）。
 *
 * 断言覆盖用户点名的五件事：解析 / 查找 / 缺列 / 坏行 / 空表，外加
 * "大表上查表是 O(1)"（按走过的哈希槽位数断言，不是掐表）。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/modnames.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got != NULL ? got : "(null)",
               want != NULL ? want : "(null)");
    }
}

static void check_has(const char *hay, const char *needle, const char *what)
{
    if (hay != NULL && needle != NULL && strstr(hay, needle) != NULL) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: '%s' 里找不到 '%s'\n", what, hay != NULL ? hay : "(null)",
               needle != NULL ? needle : "(null)");
    }
}

/* ── 夹具 ── */

/* 四行真数据（名字是社区通用译名；这里只是夹具，随包表由 tools/modnames_build.py 生成） */
static const char *kBody =
    "sodium\t\t\tsodium\t钠\tSodium\tfixture\n"
    "jei\t238222\tjei\tjei\tJEI 物品管理器\tJust Enough Items\tfixture\n"
    "applied-energistics-2\t\tapplied-energistics-2\tae2\t应用能源 2\tApplied Energistics 2\tfixture\n"
    "iris\t\t\tiris\t鸢尾花光影\tIris Shaders\tfixture\n";

static size_t count_lines(const char *body)
{
    size_t n = 0;
    const char *p = body;
    if (body == NULL || *body == '\0') {
        return 0u;
    }
    for (; *p != '\0'; ++p) {
        if (*p == '\n') {
            n++;
        }
    }
    if (p > body && p[-1] != '\n') {
        n++;
    }
    return n;
}

/** 安全追加：缓冲不够就截断（宁可在断言里看到不一致，也不许把栈写坏）。 */
static void append_str(char *out, size_t cap, const char *s)
{
    const size_t used = strlen(out);
    if (used + 1u >= cap) {
        return;
    }
    (void)snprintf(out + used, cap - used, "%s", s);
}

/* flags: bit0 = 写 count, bit1 = 写 sha256, bit2 = 写 columns 行 */
static void build_table(char *out, size_t cap, long version, size_t count, const char *body, int flags)
{
    char sha[SXCL_MODNAMES_SHA256_MAX];
    char line[512];
    sha[0] = '\0';
    if ((flags & 2) != 0) {
        (void)sxcl_modnames_body_sha256(body, strlen(body), sha, sizeof(sha));
    }
    out[0] = '\0';
    (void)snprintf(line, sizeof(line), "# sxcl-modnames\t1\n# version\t%ld\n", version);
    append_str(out, cap, line);
    if ((flags & 1) != 0) {
        (void)snprintf(line, sizeof(line), "# count\t%lu\n", (unsigned long)count);
        append_str(out, cap, line);
    }
    if ((flags & 2) != 0) {
        (void)snprintf(line, sizeof(line), "# sha256\t%s\n", sha);
        append_str(out, cap, line);
    }
    if ((flags & 4) != 0) {
        append_str(out, cap, "# columns\tslug\tcf_id\tcf_slug\tmodid\tname_zh\tname_en\tsource\n");
    }
    append_str(out, cap, "# fixture\n");
    append_str(out, cap, body);
}

/* ── 假 transport（结构照 src/core/instance/tests/instance_test.c 那份，只留要用的字段） ── */

typedef struct fake_body {
    size_t pos;
} fake_body;

typedef struct fake_http {
    const char *body;
    size_t body_len;
    int status;
    size_t chunk;
    char last_url[512];
    char last_range[128];
    int calls;
} fake_http;

static int fake_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                        sxcl_http_body **body)
{
    fake_http *fake = (fake_http *)ctx;
    fake_body *state;
    if (fake == NULL || req == NULL || req->url == NULL || resp == NULL || body == NULL) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    fake->calls++;
    (void)snprintf(fake->last_url, sizeof(fake->last_url), "%s", req->url);
    fake->last_range[0] = '\0';
    if (req->extra_headers != NULL) {
        int i;
        for (i = 0; req->extra_headers[i] != NULL; ++i) {
            if (strncmp(req->extra_headers[i], "Range:", 6) == 0) {
                (void)snprintf(fake->last_range, sizeof(fake->last_range), "%s", req->extra_headers[i]);
            }
        }
    }
    (void)memset(resp, 0, sizeof(*resp));
    resp->status = fake->status;
    resp->content_length = (int64_t)fake->body_len;
    resp->total_length = (int64_t)fake->body_len;
    resp->range_start = -1;
    resp->range_end = -1;
    state = (fake_body *)malloc(sizeof(fake_body));
    if (state == NULL) {
        return SXCL_NET_ERR_IO;
    }
    state->pos = 0;
    *body = (sxcl_http_body *)state;
    return SXCL_NET_OK;
}

static int64_t fake_read(void *ctx, sxcl_http_body *body, void *buf, size_t len)
{
    fake_http *fake = (fake_http *)ctx;
    fake_body *state = (fake_body *)body;
    size_t want;
    if (fake == NULL || state == NULL || buf == NULL) {
        return -1;
    }
    if (state->pos >= fake->body_len) {
        return 0;
    }
    want = fake->body_len - state->pos;
    if (want > len) {
        want = len;
    }
    if (fake->chunk > 0u && want > fake->chunk) {
        want = fake->chunk;
    }
    (void)memcpy(buf, fake->body + state->pos, want);
    state->pos += want;
    return (int64_t)want;
}

static void fake_close(void *ctx, sxcl_http_body *body)
{
    (void)ctx;
    free(body);
}

static void fake_cancel(void *ctx)
{
    (void)ctx;
}

static sxcl_transport make_fake_transport(fake_http *fake)
{
    sxcl_transport tr;
    (void)memset(&tr, 0, sizeof(tr));
    tr.ctx = fake;
    tr.request = fake_request;
    tr.read = fake_read;
    tr.close_body = fake_close;
    tr.cancel_all = fake_cancel;
    return tr;
}

/* ── 1) 解析 + 五个键各自的查找 ── */

static void test_parse_lookup(void)
{
    char table[4096];
    sxcl_modnames *t = NULL;
    char err[SXCL_MODNAMES_ERR_MAX];
    const sxcl_modname_meta *m;

    err[0] = '\0';
    build_table(table, sizeof(table), 7L, count_lines(kBody), kBody, 1 | 2 | 4);
    check_int(sxcl_modnames_parse(table, strlen(table), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
              "解析：正常表");
    check(t != NULL, "解析：拿到表");
    check_int((long)sxcl_modnames_count(t), 4, "解析：条数");
    check_int((long)sxcl_modnames_bad_lines(t), 0, "解析：没有坏行");
    check(sxcl_modnames_self_consistent(t) == 1, "解析：sha256/条数都对得上");
    m = sxcl_modnames_meta(t);
    check_int(m->version, 7, "解析：version");
    check_int((long)m->count, 4, "解析：表头 count");
    check_str(m->source, "", "解析：source 缺省为空");

    check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠", "查 slug");
    check_str(sxcl_modnames_lookup_slug(t, "SODIUM"), "钠", "查 slug：大小写不敏感");
    check_str(sxcl_modnames_lookup_slug(t, "  sodium  "), "钠", "查 slug：首尾空白不管");
    check_str(sxcl_modnames_lookup_cf_id(t, "238222"), "JEI 物品管理器", "查 CurseForge 数字 id");
    check_str(sxcl_modnames_lookup_cf_slug(t, "applied-energistics-2"), "应用能源 2",
              "查 CurseForge slug");
    check_str(sxcl_modnames_lookup_modid(t, "ae2"), "应用能源 2", "查 modid");
    check_str(sxcl_modnames_lookup_name(t, "Applied Energistics 2"), "应用能源 2", "按英文名查");
    check_str(sxcl_modnames_lookup_name(t, "applied-energistics-2"), "应用能源 2",
              "按英文名查：标点不参与比较");
    check_str(sxcl_modnames_lookup_name(t, "钠"), "钠", "按中文名查");
    check_str(sxcl_modnames_lookup(t, "jei", NULL, NULL), "JEI 物品管理器", "三键：只给 slug");
    check_str(sxcl_modnames_lookup(t, NULL, "238222", NULL), "JEI 物品管理器", "三键：只给 cf_id");
    check_str(sxcl_modnames_lookup(t, NULL, NULL, "Iris Shaders"), "鸢尾花光影", "三键：只给名字");
    check_str(sxcl_modnames_lookup(t, "applied-energistics-2", NULL, NULL), "应用能源 2",
              "三键：CF 的 slug 走 cf_slug 那一列");
    check(sxcl_modnames_lookup(t, "no-such-mod", NULL, NULL) == NULL, "查不到就是 NULL（不猜）");
    check(sxcl_modnames_lookup_name(t, "钠元素") == NULL, "按名字查：不许模糊命中");
    check(sxcl_modnames_lookup_slug(t, "") == NULL, "空键返回 NULL");
    check(sxcl_modnames_lookup_slug(t, "---") == NULL, "全是标点的键返回 NULL");

    /* apply：命中改写、没命中的一个字都不写 */
    {
        char out[128];
        (void)snprintf(out, sizeof(out), "%s", "Sodium");
        check_int(sxcl_modnames_apply(t, "sodium", NULL, "Sodium", out, sizeof(out)), 1,
                  "apply：命中返回 1");
        check_str(out, "钠", "apply：命中改写");
        (void)snprintf(out, sizeof(out), "%s", "Some Unknown Mod");
        check_int(sxcl_modnames_apply(t, "unknown-mod", "1", "Some Unknown Mod", out, sizeof(out)), 0,
                  "apply：没命中返回 0");
        check_str(out, "Some Unknown Mod", "apply：没命中一个字都不写");
        check_int(sxcl_modnames_apply(t, NULL, NULL, NULL, NULL, 0u), -1, "apply：参数不合法返回 -1");
    }

    /* 索引是哈希：命中/未命中都只走个位数个槽位 */
    (void)sxcl_modnames_lookup_slug(t, "sodium");
    check(sxcl_modnames_last_probes(t) <= 8u, "小表命中：探测槽位数是个位数");
    (void)sxcl_modnames_lookup_slug(t, "nope-nope");
    check(sxcl_modnames_last_probes(t) <= 8u, "小表未命中：探测槽位数是个位数");

    sxcl_modnames_free(t);
}

/* ── 2) 缺列 / 坏行 ── */

static void test_columns_and_bad_lines(void)
{
    char table[32768]; /* 里面有一行是"超长行"夹具（> SXCL_MODNAMES_LINE_MAX），表要比它大 */
    sxcl_modnames *t = NULL;
    char err[SXCL_MODNAMES_ERR_MAX];
    const char *body;

    err[0] = '\0';

    /* 自己的列顺序（表头用 columns 行钉住）；认不出的列名忽略，不许崩 */
    body = "钠\tSodium\tsodium\tignored-1\tignored-2\n"
           "应用能源 2\tApplied Energistics 2\tae2\tignored-3\tignored-4\n";
    {
        char head[512];
        (void)snprintf(head, sizeof(head),
                       "# sxcl-modnames\t1\n# version\t3\n# count\t2\n"
                       "# columns\tname_zh\tname_en\tmodid\n%s",
                       body);
        check_int(sxcl_modnames_parse(head, strlen(head), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
                  "缺列：columns 行钉住顺序");
        check_int((long)sxcl_modnames_count(t), 2, "缺列：两条都读到了");
        check_str(sxcl_modnames_lookup_modid(t, "ae2"), "应用能源 2", "缺列：按 columns 给的 modid 查");
        check_str(sxcl_modnames_lookup_name(t, "Sodium"), "钠", "缺列：按名字查");
        check(sxcl_modnames_lookup_slug(t, "ae2") == NULL, "缺列：没给的列查不到（不猜）");
        check(sxcl_modnames_self_consistent(t) == 1, "缺列：条数对得上");
        sxcl_modnames_free(t);
        t = NULL;
    }

    /* 坏行：列不够 / 中文名为空 / 超长行 —— 全跳过并计数，好行照收 */
    {
        static char huge[SXCL_MODNAMES_LINE_MAX + 64];
        char *body2 = (char *)malloc(1u << 16);
        (void)memset(huge, 'x', sizeof(huge) - 1u);
        huge[sizeof(huge) - 1u] = '\0';
        body2[0] = '\0';
        (void)strcat(body2, "sodium\n");                              /* 列不够（1 列） */
        (void)strcat(body2, "jei\t238222\tjei\tjei\t\tJust Enough Items\tbad\n"); /* 中文名空 */
        (void)strcat(body2, huge);
        (void)strcat(body2, "\n");
        (void)strcat(body2, "ae2\t\tapplied-energistics-2\tae2\t应用能源 2\tApplied Energistics 2\tok\n");
        build_table(table, sizeof(table), 5L, 1u, body2, 1 | 2 | 4);
        check_int(sxcl_modnames_parse(table, strlen(table), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
                  "坏行：照样解析成功");
        check_int((long)sxcl_modnames_count(t), 1, "坏行：只留下那一条好的");
        check_int((long)sxcl_modnames_bad_lines(t), 3, "坏行：数出来 3 条（不静默）");
        check_str(sxcl_modnames_lookup_slug(t, "ae2"), "应用能源 2", "坏行：好的那条还能查");
        check(sxcl_modnames_lookup_slug(t, "sodium") == NULL, "坏行：坏的那条没有名字");
        if (sxcl_modnames_self_consistent(t) != 1) {
            /* 表头声明的 count=1、正文摘要也是照含坏行的正文算的：都该对得上。
             * 这里**故意**做成断言，防止"跳过坏行"顺手把完整性判据弄坏。 */
            check(0, "坏行：条数与 sha256 仍然自洽");
        } else {
            check(1, "坏行：条数与 sha256 仍然自洽");
        }
        sxcl_modnames_free(t);
        t = NULL;
        free(body2);
    }

    /* 表头 count 与正文对不上 -> self_consistent = 0（但**不崩**，名字照查） */
    build_table(table, sizeof(table), 5L, 99u, kBody, 1 | 2 | 4);
    check_int(sxcl_modnames_parse(table, strlen(table), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
              "条数不符：解析成功");
    check_int((long)sxcl_modnames_count(t), 4, "条数不符：数据照读");
    check(sxcl_modnames_self_consistent(t) == 0, "条数不符：自洽判据为假");
    check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠", "条数不符：查表照旧");
    sxcl_modnames_free(t);
    t = NULL;

    /* 摘要不符 -> self_consistent = 0 */
    {
        char bad[4096];
        (void)snprintf(bad, sizeof(bad),
                       "# sxcl-modnames\t1\n# version\t6\n# count\t4\n"
                       "# sha256\t0000000000000000000000000000000000000000000000000000000000000000\n"
                       "# columns\tslug\tcf_id\tcf_slug\tmodid\tname_zh\tname_en\tsource\n%s",
                       kBody);
        check_int(sxcl_modnames_parse(bad, strlen(bad), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
                  "摘要不符：解析成功");
        check(sxcl_modnames_self_consistent(t) == 0, "摘要不符：自洽判据为假");
        check_str(sxcl_modnames_lookup_slug(t, "jei"), "JEI 物品管理器", "摘要不符：查表照旧");
        sxcl_modnames_free(t);
        t = NULL;
    }
}

/* ── 3) 空表 / 不是我们的表 / 空指针 ── */

static void test_empty_and_format(void)
{
    sxcl_modnames *t = NULL;
    char err[SXCL_MODNAMES_ERR_MAX];
    char table[4096];

    err[0] = '\0';

    check_int(sxcl_modnames_parse("", 0u, &t, err, sizeof(err)), SXCL_MODNAMES_OK, "空表：0 字节合法");
    check_int((long)sxcl_modnames_count(t), 0, "空表：0 条");
    check(sxcl_modnames_self_consistent(t) == 1, "空表：自洽");
    check(sxcl_modnames_lookup_slug(t, "sodium") == NULL, "空表：查什么都返回 NULL");
    check_int((long)sxcl_modnames_bad_lines(t), 0, "空表：没有坏行");
    sxcl_modnames_free(t);
    t = NULL;

    check_int(sxcl_modnames_parse(NULL, 0u, &t, err, sizeof(err)), SXCL_MODNAMES_OK,
              "空指针文本：当成空表");
    sxcl_modnames_free(t);
    t = NULL;

    build_table(table, sizeof(table), 1L, 0u, "", 1 | 2 | 4);
    check_int(sxcl_modnames_parse(table, strlen(table), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
              "只有表头：合法");
    check_int((long)sxcl_modnames_count(t), 0, "只有表头：0 条");
    sxcl_modnames_free(t);
    t = NULL;

    check_int(sxcl_modnames_parse("hello\nworld\n", 12u, &t, err, sizeof(err)),
              SXCL_MODNAMES_ERR_FORMAT, "不是我们的表：报格式错");
    check_has(err, "模组中文名表", "不是我们的表：错误里说清缺什么");
    check(t == NULL, "不是我们的表：不给半张表");

    check_int(sxcl_modnames_parse("# another-table\t1\nx\ty\n", 23u, &t, err, sizeof(err)),
              SXCL_MODNAMES_ERR_FORMAT, "别的表头：也算不是我们的表");

    check_int(sxcl_modnames_parse(kBody, strlen(kBody), NULL, err, sizeof(err)),
              SXCL_MODNAMES_ERR_ARG, "out 为空：参数错");

    /* 空表上的空指针容忍度 */
    check(sxcl_modnames_lookup_slug(NULL, "sodium") == NULL, "NULL 表：查不崩");
    check(sxcl_modnames_lookup(NULL, "a", "b", "c") == NULL, "NULL 表：三键查不崩");
    check_int((long)sxcl_modnames_count(NULL), 0, "NULL 表：条数 0");
    check_int((long)sxcl_modnames_bucket_count(NULL), 0, "NULL 表：槽数 0");
    check(sxcl_modnames_meta(NULL) != NULL, "NULL 表：meta 不是空指针");
    check_int(sxcl_modnames_entry(NULL, 0u) == NULL, 1, "NULL 表：entry 返回 NULL");
    sxcl_modnames_free(NULL); /* 不许崩 */
    check(1, "free(NULL) 不崩");
}

/* ── 4) 大表：查表是 O(1) ── */

static void test_big_table(void)
{
    const size_t rows = 20000u;
    const size_t cap = rows * 96u + 4096u;
    char *body = (char *)malloc(cap);
    char *table = (char *)malloc(cap + 4096u);
    sxcl_modnames *t = NULL;
    char err[SXCL_MODNAMES_ERR_MAX];
    size_t i, pos = 0;
    char key[64];
    char want[64];

    err[0] = '\0';
    if (body == NULL || table == NULL) {
        check(0, "大表：夹具内存分配失败");
        free(body);
        free(table);
        return;
    }
    for (i = 0; i < rows; ++i) {
        pos += (size_t)snprintf(body + pos, cap - pos,
                                "mod-%lu\t%lu\tmod-%lu\tmod%lu\t名字%lu\tMod Name %lu\tbig\n",
                                (unsigned long)i, (unsigned long)(1000000u + i), (unsigned long)i,
                                (unsigned long)i, (unsigned long)i, (unsigned long)i);
    }
    build_table(table, cap + 4096u, 42L, rows, body, 1 | 2 | 4);
    check_int(sxcl_modnames_parse(table, strlen(table), &t, err, sizeof(err)), SXCL_MODNAMES_OK,
              "大表：解析成功");
    check_int((long)sxcl_modnames_count(t), (long)rows, "大表：条数 20000");
    check(sxcl_modnames_self_consistent(t) == 1, "大表：自洽");
    check(sxcl_modnames_bucket_count(t) >= rows * 2u, "大表：哈希槽数 >= 2 倍条数（负载因子 <= 0.5）");

    (void)snprintf(key, sizeof(key), "mod-%lu", (unsigned long)(rows - 1u));
    (void)snprintf(want, sizeof(want), "名字%lu", (unsigned long)(rows - 1u));
    check_str(sxcl_modnames_lookup_slug(t, key), want, "大表：最后一条查得到");
    check(sxcl_modnames_last_probes(t) <= 8u, "大表：命中只走个位数个槽位");
    (void)snprintf(key, sizeof(key), "mod-%lu", (unsigned long)(rows / 2u));
    check(sxcl_modnames_lookup_modid(t, key) != NULL, "大表：modid 命中");
    check(sxcl_modnames_last_probes(t) <= 8u, "大表：modid 命中同样是个位数槽位");
    check(sxcl_modnames_lookup_slug(t, "mod-does-not-exist") == NULL, "大表：没命中返回 NULL");
    check(sxcl_modnames_last_probes(t) <= 8u, "大表：未命中也是走个位数个槽位");
    check_str(sxcl_modnames_lookup_name(t, "Mod Name 137"), "名字137", "大表：按名字查");

    sxcl_modnames_free(t);
    free(body);
    free(table);
}

/* ── 5) 路径 ── */

static void test_paths(void)
{
    char out[1024];

    check_int(sxcl_modnames_remote_url("https://sxcl.example.com", out, sizeof(out)), SXCL_MODNAMES_OK,
              "远端 URL：正常");
    check_str(out, "https://sxcl.example.com/meta/modnames.tsv", "远端 URL：拼成 /meta/modnames.tsv");
    check_int(sxcl_modnames_remote_url("https://sxcl.example.com///", out, sizeof(out)),
              SXCL_MODNAMES_OK, "远端 URL：末尾斜杠");
    check_str(out, "https://sxcl.example.com/meta/modnames.tsv", "远端 URL：末尾斜杠去掉");
    check_int(sxcl_modnames_remote_url("", out, sizeof(out)), SXCL_MODNAMES_ERR_ARG,
              "远端 URL：没给基址就报参数错（不编默认域名）");
    check_int(sxcl_modnames_remote_url("https://x/", out, 8u), SXCL_MODNAMES_ERR_ARG,
              "远端 URL：缓冲不够报参数错");

    check_int(sxcl_modnames_cache_path("C:/tmp/sxcl", out, sizeof(out)), SXCL_MODNAMES_OK,
              "缓存路径：正常");
    check_str(out, "C:/tmp/sxcl/meta/modnames.tsv", "缓存路径：<dir>/meta/modnames.tsv");
    check_int(sxcl_modnames_cache_path(NULL, out, sizeof(out)), SXCL_MODNAMES_ERR_ARG,
              "缓存路径：空目录报参数错");

    check_int(sxcl_modnames_bundled_path("C:/app/assets", out, sizeof(out)), SXCL_MODNAMES_OK,
              "随包路径：正常");
    check_str(out, "C:/app/assets/data/modnames.tsv", "随包路径：<assets>/data/modnames.tsv");

    /* 环境变量优先（取证/验收通路，和 SXCL_ICON_DIR 同一套口径） */
#if defined(_MSC_VER)
    (void)_putenv_s("SXCL_MODNAMES_FILE", "D:/fixture/modnames.tsv");
#else
    (void)setenv("SXCL_MODNAMES_FILE", "D:/fixture/modnames.tsv", 1);
#endif
    check_int(sxcl_modnames_bundled_path("C:/app/assets", out, sizeof(out)), SXCL_MODNAMES_OK,
              "随包路径：环境变量覆盖");
    check_str(out, "D:/fixture/modnames.tsv", "随包路径：SXCL_MODNAMES_FILE 优先");
#if defined(_MSC_VER)
    (void)_putenv_s("SXCL_MODNAMES_FILE", "");
#else
    (void)unsetenv("SXCL_MODNAMES_FILE");
#endif
}

/* ── 6) 缓存与随包的取舍 ── */

#define TMP_ROOT "_modnames_tmp"
#define TMP_BUNDLED TMP_ROOT "/bundled.tsv"
#define TMP_CACHE_DIR TMP_ROOT "/cache"

static int write_text_file(const char *path, const char *text)
{
    FILE *fh;
    const size_t len = strlen(text);
    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        return -1;
    }
    fh = sxcl_fs_fopen(path, "wb");
    if (fh == NULL) {
        return -1;
    }
    if (len > 0u && fwrite(text, 1u, len, fh) != len) {
        fclose(fh);
        return -1;
    }
    return (fclose(fh) == 0) ? 0 : -1;
}

static void test_cache(void)
{
    char bundled[4096];
    char cloud[2048];
    char err[SXCL_MODNAMES_ERR_MAX];
    char cache_path[1024];
    sxcl_modnames *t = NULL;
    sxcl_modnames_source src;

    err[0] = '\0';
    (void)sxcl_fs_remove_tree(TMP_ROOT);
    (void)sxcl_fs_mkdirs(TMP_ROOT);

    build_table(bundled, sizeof(bundled), 7L, count_lines(kBody), kBody, 1 | 2 | 4);
    check_int(write_text_file(TMP_BUNDLED, bundled), 0, "缓存：随包那份落盘");

    /* (1) 只有随包那份 */
    check_int(sxcl_modnames_load_cached(TMP_BUNDLED, TMP_CACHE_DIR, &t, &src, err, sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：只有随包那份");
    check_int(src.from_bundled, 1, "缓存：用的是随包那份");
    check_int(src.from_cache, 0, "缓存：没走缓存");
    check_int(src.empty, 0, "缓存：不是空表");
    check_int((long)sxcl_modnames_count(t), 4, "缓存：随包那份 4 条");
    check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠", "缓存：随包那份的名字");
    sxcl_modnames_free(t);
    t = NULL;

    /* (2) 云端更新过的新版：缓存赢 */
    build_table(cloud, sizeof(cloud), 9L, 1u, "sodium\t\t\tsodium\t钠-云端\tSodium\tcloud\n", 1 | 2 | 4);
    check_int(sxcl_modnames_cache_write(TMP_CACHE_DIR, cloud, strlen(cloud), cache_path,
                                        sizeof(cache_path), err, sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：写一份新版的进去");
    check_int(sxcl_fs_exists(cache_path), 1, "缓存：文件真的在");
    check_int(sxcl_modnames_load_cached(TMP_BUNDLED, TMP_CACHE_DIR, &t, &src, err, sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：有新版时照旧返回 OK");
    check_int(src.from_cache, 1, "缓存：用的是云端那份");
    check_int(sxcl_modnames_meta(t)->version, 9, "缓存：版本是 9");
    check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠-云端", "缓存：名字来自云端那份");
    sxcl_modnames_free(t);
    t = NULL;

    /* (3) 比随包旧的那份不算数 */
    build_table(cloud, sizeof(cloud), 3L, 1u, "sodium\t\t\tsodium\t钠-老\tSodium\told\n", 1 | 2 | 4);
    check_int(sxcl_modnames_cache_write(TMP_CACHE_DIR, cloud, strlen(cloud), NULL, 0u, err,
                                        sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：写一份旧版的进去");
    check_int(sxcl_modnames_load_cached(TMP_BUNDLED, TMP_CACHE_DIR, &t, &src, err, sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：旧版不换");
    check_int(src.from_bundled, 1, "缓存：退回随包那份");
    check_int(src.cache_rejected, 1, "缓存：如实报「缓存被否掉」");
    check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠", "缓存：名字来自随包那份");
    sxcl_modnames_free(t);
    t = NULL;

    /* (4) 坏掉的缓存（摘要对不上）不许用 */
    {
        char bad[4096];
        (void)snprintf(bad, sizeof(bad),
                       "# sxcl-modnames\t1\n# version\t99\n# count\t1\n"
                       "# sha256\t1111111111111111111111111111111111111111111111111111111111111111\n"
                       "# columns\tslug\tcf_id\tcf_slug\tmodid\tname_zh\tname_en\tsource\n"
                       "sodium\t\t\tsodium\t坏名字\tSodium\tbroken\n");
        check_int(sxcl_modnames_cache_write(TMP_CACHE_DIR, bad, strlen(bad), NULL, 0u, err,
                                            sizeof(err)),
                  SXCL_MODNAMES_OK, "缓存：写一份坏摘要的进去");
        check_int(sxcl_modnames_load_cached(TMP_BUNDLED, TMP_CACHE_DIR, &t, &src, err, sizeof(err)),
                  SXCL_MODNAMES_OK, "缓存：坏的那份不影响启动");
        check_int(src.from_bundled, 1, "缓存：坏的那份被否掉，用随包");
        check_int(src.cache_rejected, 1, "缓存：如实记下被否");
        check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠", "缓存：绝不拿坏名字顶上");
        sxcl_modnames_free(t);
        t = NULL;
    }

    /* (5) 两份都没有 = 空表（不是错误） */
    check_int(sxcl_modnames_load_cached(TMP_ROOT "/nope.tsv", TMP_ROOT "/nocache", &t, &src, err,
                                        sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：两份都没有也返回 OK");
    check_int(src.empty, 1, "缓存：如实标空");
    check_int((long)sxcl_modnames_count(t), 0, "缓存：0 条");
    check(sxcl_modnames_lookup_slug(t, "sodium") == NULL, "缓存：空表查不到就是查不到");
    check_has(src.note, "没有", "缓存：空表有人说话");
    sxcl_modnames_free(t);
    t = NULL;

    /* (6) bundled_path 给 NULL = 只有缓存这条路（先把一份好的写回缓存） */
    build_table(cloud, sizeof(cloud), 9L, 1u, "sodium\t\t\tsodium\t钠-云端\tSodium\tcloud\n", 1 | 2 | 4);
    check_int(sxcl_modnames_cache_write(TMP_CACHE_DIR, cloud, strlen(cloud), NULL, 0u, err,
                                        sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：写回一份好的");
    check_int(sxcl_modnames_load_cached(NULL, TMP_CACHE_DIR, &t, &src, err, sizeof(err)),
              SXCL_MODNAMES_OK, "缓存：不给随包路径也能起");
    check_int(src.from_cache, 1, "缓存：走缓存那份");
    check_str(sxcl_modnames_lookup_slug(t, "sodium"), "钠-云端", "缓存：只有缓存时名字也对");
    sxcl_modnames_free(t);
    t = NULL;
}

/* ── 7) 云端更新（假 transport 喂真文本） ── */

static void test_cloud(void)
{
    const char *cache_dir = TMP_ROOT "/cloudcache";
    char table[4096];
    char err[SXCL_MODNAMES_ERR_MAX];
    char cache_path[1024];
    sxcl_modnames_update res;
    sxcl_modnames *cur = NULL;
    fake_http fake;
    sxcl_transport tr;
    int rc;

    err[0] = '\0';
    (void)sxcl_fs_remove_tree(cache_dir);
    (void)sxcl_modnames_cache_path(cache_dir, cache_path, sizeof(cache_path));

    /* (1) 正常：一份完整的新表 */
    build_table(table, sizeof(table), 9L, 1u, "sodium\t\t\tsodium\t钠-云端\tSodium\tcloud\n", 1 | 2 | 4);
    (void)memset(&fake, 0, sizeof(fake));
    fake.body = table;
    fake.body_len = strlen(table);
    fake.status = 200;
    fake.chunk = 7u;
    tr = make_fake_transport(&fake);
    rc = sxcl_modnames_update_from_cloud(&tr, "https://sxcl.example.com/", cache_dir, NULL, &res, err,
                                         sizeof(err));
    check_int(rc, SXCL_MODNAMES_OK, "云端：正常更新不报错");
    check_int(res.updated, 1, "云端：换上了云端那份");
    check_int(res.version, 9, "云端：版本 9");
    check_int((long)res.count, 1, "云端：条数 1");
    check_str(fake.last_url, "https://sxcl.example.com/meta/modnames.tsv",
              "云端：请求的就是 <base>/meta/modnames.tsv");
    check_int(sxcl_fs_exists(cache_path), 1, "云端：原子替换后缓存文件在");
    check_has(res.note, "version 9", "云端：note 里写了换成哪一版");

    /* 更新完立刻重新挑一份：应当就吃上缓存了 */
    check_int(sxcl_modnames_load_cached(NULL, cache_dir, &cur, NULL, err, sizeof(err)),
              SXCL_MODNAMES_OK, "云端：更新后能读回缓存");
    check_str(sxcl_modnames_lookup_slug(cur, "sodium"), "钠-云端", "云端：读回的就是云端那份");

    /* (2) 版本没前进：不换，也不改缓存 */
    (void)memset(&fake, 0, sizeof(fake));
    fake.body = table;
    fake.body_len = strlen(table);
    fake.status = 200;
    tr = make_fake_transport(&fake);
    rc = sxcl_modnames_update_from_cloud(&tr, "https://sxcl.example.com", cache_dir, cur, &res, err,
                                         sizeof(err));
    check_int(rc, SXCL_MODNAMES_OK, "云端：版本没前进不算错");
    check_int(res.updated, 0, "云端：版本没前进就不换");
    check_has(res.note, "没有比手上", "云端：说清为什么不换");
    sxcl_modnames_free(cur);
    cur = NULL;

    /* (3) 摘要对不上：拒，缓存原样不动 */
    build_table(table, sizeof(table), 10L, 1u, "sodium\t\t\tsodium\t坏名字\tSodium\tbroken\n", 1 | 4);
    (void)memset(&fake, 0, sizeof(fake));
    fake.body = table;
    fake.body_len = strlen(table);
    fake.status = 200;
    tr = make_fake_transport(&fake);
    rc = sxcl_modnames_update_from_cloud(&tr, "https://sxcl.example.com", cache_dir, NULL, &res, err,
                                         sizeof(err));
    check_int(rc, SXCL_MODNAMES_OK, "云端：摘要对不上不算传输错");
    check_int(res.updated, 0, "云端：摘要对不上就不换");
    check_has(res.note, "没带", "云端：缺 sha256/条数要说明白");

    /* (4) 摘要写了但对不上 */
    (void)snprintf(table, sizeof(table),
                   "# sxcl-modnames\t1\n# version\t11\n# count\t1\n"
                   "# sha256\t2222222222222222222222222222222222222222222222222222222222222222\n"
                   "# columns\tslug\tcf_id\tcf_slug\tmodid\tname_zh\tname_en\tsource\n"
                   "sodium\t\t\tsodium\t坏名字\tSodium\tbroken\n");
    (void)memset(&fake, 0, sizeof(fake));
    fake.body = table;
    fake.body_len = strlen(table);
    fake.status = 200;
    tr = make_fake_transport(&fake);
    rc = sxcl_modnames_update_from_cloud(&tr, "https://sxcl.example.com", cache_dir, NULL, &res, err,
                                         sizeof(err));
    check_int(rc, SXCL_MODNAMES_OK, "云端：摘要不符不算传输错");
    check_int(res.updated, 0, "云端：摘要不符就不换");
    check_has(res.note, "对不上", "云端：摘要不符要说清");

    /* 缓存里还是第 (1) 步那份（header 里的 version 9） */
    check_int(sxcl_modnames_load_cached(NULL, cache_dir, &cur, NULL, err, sizeof(err)),
              SXCL_MODNAMES_OK, "云端：被拒之后缓存还在");
    check_int(sxcl_modnames_meta(cur)->version, 9, "云端：被拒之后缓存里的版本没动");
    sxcl_modnames_free(cur);
    cur = NULL;

    /* (5) HTTP 500：如实报失败，不换 */
    (void)memset(&fake, 0, sizeof(fake));
    fake.body = "boom";
    fake.body_len = 4u;
    fake.status = 503;
    tr = make_fake_transport(&fake);
    rc = sxcl_modnames_update_from_cloud(&tr, "https://sxcl.example.com", cache_dir, NULL, &res, err,
                                         sizeof(err));
    check_int(rc, SXCL_MODNAMES_OK, "云端：503 不算致命错（不阻塞启动）");
    check_int(res.updated, 0, "云端：503 不换");
    check_int(res.status, 503, "云端：如实带回状态码");
    check_has(res.note, "失败", "云端：说清是取失败");

    /* (6) 没有传输后端 */
    rc = sxcl_modnames_update_from_cloud(NULL, "https://sxcl.example.com", cache_dir, NULL, &res, err,
                                         sizeof(err));
    check_int(rc, SXCL_MODNAMES_OK, "云端：没后端也不报错");
    check_int(res.updated, 0, "云端：没后端就不换");
    check_has(res.note, "传输后端", "云端：说清没有传输后端");

    /* (7) 参数错 */
    check_int(sxcl_modnames_update_from_cloud(&tr, "", cache_dir, NULL, &res, err, sizeof(err)),
              SXCL_MODNAMES_ERR_ARG, "云端：没给基址报参数错");
    check_int(sxcl_modnames_update_from_cloud(&tr, "https://x", "", NULL, &res, err, sizeof(err)),
              SXCL_MODNAMES_ERR_ARG, "云端：没给缓存目录报参数错");

    /* (8) 只取表头那一步（可选优化） */
    build_table(table, sizeof(table), 12L, 1u, "sodium\t\t\tsodium\t钠-十二\tSodium\tv12\n", 1 | 2 | 4);
    (void)memset(&fake, 0, sizeof(fake));
    fake.body = table;
    fake.body_len = strlen(table);
    fake.status = 200;
    tr = make_fake_transport(&fake);
    {
        long v = 0;
        check_int(sxcl_modnames_probe_remote(&tr, "https://sxcl.example.com", NULL, &v, err,
                                             sizeof(err)),
                  0, "探表头：有新版本要更新");
        check_int(v, 12, "探表头：读到 version 12");
        check_has(fake.last_range, "bytes=0-4095", "探表头：带了 Range 头");
        check_int(sxcl_modnames_probe_remote(&tr, "https://sxcl.example.com", NULL, &v, err,
                                             sizeof(err)) != 0 || v == 12,
                  1, "探表头：第二次也一致");
    }
    {
        sxcl_modnames *v12 = NULL;
        long v = 0;
        check_int(sxcl_modnames_parse(table, strlen(table), &v12, err, sizeof(err)), SXCL_MODNAMES_OK,
                  "探表头：夹具能解析");
        check_int(sxcl_modnames_probe_remote(&tr, "https://sxcl.example.com", v12, &v, err,
                                             sizeof(err)),
                  1, "探表头：跟手上同版本就不用更新");
        sxcl_modnames_free(v12);
    }
    {
        long v = 0;
        (void)memset(&fake, 0, sizeof(fake));
        fake.body = "hello";
        fake.body_len = 5u;
        fake.status = 200;
        tr = make_fake_transport(&fake);
        check_int(sxcl_modnames_probe_remote(&tr, "https://sxcl.example.com", NULL, &v, err,
                                             sizeof(err)),
                  -1, "探表头：不是我们的表就报「不知道」");
        check_int(sxcl_modnames_probe_remote(NULL, "https://x", NULL, &v, err, sizeof(err)),
                  SXCL_MODNAMES_ERR_ARG, "探表头：没后端报参数错");
    }

    (void)sxcl_fs_remove_tree(TMP_ROOT);
}

/* ── 8) 随包那张**真表**（存在才编进来） ── */

static void test_bundled_table(void)
{
#ifdef SXCL_MODNAMES_BUNDLED_FIXTURE
    sxcl_modnames *t = NULL;
    char err[SXCL_MODNAMES_ERR_MAX];
    const sxcl_modname_meta *m;
    err[0] = '\0';
    check_int(sxcl_modnames_load_file(SXCL_MODNAMES_BUNDLED_FIXTURE, &t, err, sizeof(err)),
              SXCL_MODNAMES_OK, "随包表：读得出来");
    if (t == NULL) {
        return;
    }
    m = sxcl_modnames_meta(t);
    check_int((long)sxcl_modnames_count(t), (long)m->count, "随包表：条数与表头声明一致");
    check(sxcl_modnames_count(t) > 10000u, "随包表：一万条以上");
    check(m->version >= 20260101L, "随包表：version 像个日期");
    check(sxcl_modnames_self_consistent(t) == 1, "随包表：sha256 与条数都对得上（生成器写的摘要是真的）");
    check_int((long)sxcl_modnames_bad_lines(t), 0, "随包表：生成器产出的表一个坏行都没有");
    check_str(m->source, "HMCL mod_data.txt (GPL-3.0; 数据出处 mcmod.cn,每行保留 mcmod 编号)",
              "随包表：出处写在表头里（换数据源别忘改它）");
    check_str(sxcl_modnames_lookup_cf_slug(t, "sodium"), "钠", "随包表：sodium -> 钠");
    check_str(sxcl_modnames_lookup_modid(t, "jei"), "JEI物品管理器", "随包表：modid jei -> JEI物品管理器");
    check_str(sxcl_modnames_lookup_name(t, "Mod Menu"), "模组菜单", "随包表：按英文名查 Mod Menu");
    check_str(sxcl_modnames_lookup_name(t, "AppleSkin"), "苹果皮", "随包表：按英文名查 AppleSkin");
    check(sxcl_modnames_lookup_cf_id(t, "238222") == NULL, "随包表：没填 cf_id 的键查不到（不猜）");
    check(sxcl_modnames_lookup_slug(t, "definitely-not-a-mod-xyz") == NULL, "随包表：查不到就是 NULL");
    check(sxcl_modnames_last_probes(t) <= 8u, "随包表：真表上查表仍是 O(1)（个位数个槽位）");
    sxcl_modnames_free(t);
#else
    check(1, "随包表：这个构建没钉进 assets/data/modnames.tsv（跳过）");
#endif
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* 崩了也要看得见走到哪一步 */
    printf("== 模组中文名表（随包 + 云端可更新）单测 ==\n");
    printf("[1] 解析/查找\n");
    test_parse_lookup();
    printf("[2] 缺列/坏行\n");
    test_columns_and_bad_lines();
    printf("[3] 空表/格式\n");
    test_empty_and_format();
    printf("[4] 大表 O(1)\n");
    test_big_table();
    printf("[5] 路径\n");
    test_paths();
    printf("[6] 缓存\n");
    test_cache();
    printf("[7] 云端更新\n");
    test_cloud();
    printf("[8] 随包真表\n");
    test_bundled_table();
    printf("modnames: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
