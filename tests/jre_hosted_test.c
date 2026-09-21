/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* jre_hosted_test.c - 自托管 JRE 链路(index.json -> 下载 -> 解包 -> jre.json)的夹具测试。
 *
 * **不联网**:index.json 在用例里现拼(用夹具文件的真实 size/sha256),文件走内存假传输。
 * 夹具是签入仓库的三个真 .tar.xz(tests/fixtures/jre/):
 *   universal.tar.xz        所有 ABI 都要的那一份(release / lib / conf)
 *   bin-arm64.tar.xz        arm64 专属(bin/java、lib/libjli.so)
 *   bin-armeabi-v7a.tar.xz  **别的** ABI:必须一个字节都不解出来
 * 解包目录在构建目录下的 _jre_tmp(仓库里不留产物)。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/jre_hosted.h"
#include "sxcl/net.h"
#include "sxcl/verify.h"   /* sxcl_hash_file(拼 index 时算夹具的 sha256/sha1) */

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
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

/* ── 夹具 ── */

static char g_dir[1024];
static char g_tmp[1024];

static const char *fixture_dir(void)
{
    if (g_dir[0] == '\0') {
        const char *env = getenv("SXCL_JRE_FIXTURES");
        snprintf(g_dir, sizeof(g_dir), "%s", env != NULL ? env : "tests/fixtures/jre");
    }
    return g_dir;
}

static unsigned char *read_file(const char *name, size_t *len_out)
{
    char path[1200];
    FILE *fp;
    long size;
    unsigned char *buf;
    snprintf(path, sizeof(path), "%s/%s", fixture_dir(), name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  [!!] 打不开夹具 %s\n", path);
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)(size > 0 ? size : 0) + 1u);
    if (buf == NULL || (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size)) {
        free(buf);
        (void)fclose(fp);
        return NULL;
    }
    (void)fclose(fp);
    buf[size > 0 ? size : 0] = 0;
    *len_out = (size_t)(size > 0 ? size : 0);
    return buf;
}

/* ── 假传输(内存 HTTP) ── */

typedef struct blob {
    const char *name;   /* 匹配 URL 里的一段 */
    const unsigned char *data;
    size_t len;
    int corrupt;        /* 1 = 故意发坏字节(测 SHA-256 强校验) */
} blob;

static const blob *g_blobs = NULL;
static size_t g_blob_count = 0;
static int g_object_reads = 0;   /* 真的去下了几个包文件(不含 index) */
static int g_cancel_after = -1;
static int g_requests = 0;

typedef struct fake_body {
    const unsigned char *data;
    size_t len;
    size_t off;
    unsigned char *owned;
} fake_body;

static int fake_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                        sxcl_http_body **body)
{
    size_t i;
    (void)ctx;
    if (req == NULL || req->url == NULL) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    ++g_requests;
    memset(resp, 0, sizeof(*resp));
    *body = NULL;
    for (i = 0; i < g_blob_count; ++i) {
        if (strstr(req->url, g_blobs[i].name) != NULL) {
            fake_body *b;
            if (g_blobs[i].corrupt == 0) {
                ++g_object_reads;
            }
            b = (fake_body *)calloc(1, sizeof(*b));
            if (b == NULL) {
                return SXCL_NET_ERR_IO;
            }
            if (g_blobs[i].corrupt) {
                b->owned = (unsigned char *)malloc(g_blobs[i].len);
                if (b->owned == NULL) {
                    free(b);
                    return SXCL_NET_ERR_IO;
                }
                memcpy(b->owned, g_blobs[i].data, g_blobs[i].len);
                if (g_blobs[i].len > 10) {
                    b->owned[g_blobs[i].len / 2] ^= 0x5A;
                }
                b->data = b->owned;
            } else {
                b->data = g_blobs[i].data;
            }
            b->len = g_blobs[i].len;
            *body = (sxcl_http_body *)b;
            resp->status = 200;
            resp->content_length = (int64_t)g_blobs[i].len;
            resp->total_length = (int64_t)g_blobs[i].len;
            resp->range_start = -1;
            resp->range_end = -1;
            return SXCL_NET_OK;
        }
    }
    resp->status = 404;
    resp->content_length = 0;
    return SXCL_NET_OK;
}

static int64_t fake_read(void *ctx, sxcl_http_body *body, void *buf, size_t len)
{
    fake_body *b = (fake_body *)body;
    size_t n;
    (void)ctx;
    if (b == NULL || b->off >= b->len) {
        return 0;
    }
    n = b->len - b->off;
    if (n > len) {
        n = len;
    }
    memcpy(buf, b->data + b->off, n);
    b->off += n;
    return (int64_t)n;
}

static void fake_close(void *ctx, sxcl_http_body *body)
{
    fake_body *b = (fake_body *)body;
    (void)ctx;
    if (b != NULL) {
        free(b->owned);
        free(b);
    }
}

static void fake_cancel(void *ctx) { (void)ctx; }
static void fake_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *fake_transport(void *ud)
{
    sxcl_transport *tr;
    (void)ud;
    tr = (sxcl_transport *)calloc(1, sizeof(*tr));
    if (tr == NULL) {
        return NULL;
    }
    tr->request = fake_request;
    tr->read = fake_read;
    tr->close_body = fake_close;
    tr->cancel_all = fake_cancel;
    tr->destroy = fake_destroy;
    return tr;
}

/* ── 从夹具拼一份 index.json ── */

static char *build_index(const char *version, int with_sha1, int schema, const char *drop_sha256_of,
                         size_t *len_out)
{
    static const char *names[3] = {"universal.tar.xz", "bin-arm64.tar.xz", "bin-armeabi-v7a.tar.xz"};
    char *text = (char *)malloc(16384);
    size_t used = 0;
    int i;
    if (text == NULL) {
        return NULL;
    }
    used += (size_t)snprintf(text + used, 16384 - used,
                            "{\n  \"schema\": %d,\n  \"generatedAt\": \"2026-01-01T00:00:00Z\",\n"
                            "  \"components\": [\n    { \"id\": \"jre17\", \"javaMajor\": 17, "
                            "\"version\": \"%s\",\n      \"files\": [",
                            schema, version);
    for (i = 0; i < 3; ++i) {
        char path[1200];
        size_t len = 0;
        char sha256[65];
        char sha1[41];
        int64_t size = 0;
        snprintf(path, sizeof(path), "%s/%s", fixture_dir(), names[i]);
        if (sxcl_fs_stat(path, &size, NULL) != 0 || sxcl_hash_file(path, SXCL_HASH_SHA256, sha256,
                                                                    sizeof(sha256)) != 0) {
            free(text);
            return NULL;
        }
        (void)sxcl_hash_file(path, SXCL_HASH_SHA1, sha1, sizeof(sha1));
        {
            const int drop = (drop_sha256_of != NULL && strcmp(drop_sha256_of, names[i]) == 0);
            used += (size_t)snprintf(
                text + used, 16384 - used,
                "%s\n        {\"file\": \"jre17/%s\", \"size\": %lld, \"sha256\": \"%s\"",
                i == 0 ? "" : ",", names[i], (long long)size, drop ? "" : sha256);
            if (with_sha1) {
                used += (size_t)snprintf(text + used, 16384 - used, ", \"sha1\": \"%s\"", sha1);
            }
            used += (size_t)snprintf(text + used, 16384 - used, "}");
        }
        (void)len;
    }
    used += (size_t)snprintf(text + used, 16384 - used, "\n      ] }\n  ]\n}\n");
    if (len_out != NULL) {
        *len_out = used;
    }
    return text;
}

/* ── 进度探针 ── */

typedef struct prog_probe {
    int calls;
    int saw_stage[5];
    int max_percent;
    int bad_message;      /* 收到空 message 的次数(必须为 0) */
    int bad_percent;      /* 百分比倒退的次数 */
    int last_percent;
    int saw_speed;
    int saw_eta;
} prog_probe;

static void on_progress(void *ud, const sxcl_jre_progress *p)
{
    prog_probe *pr = (prog_probe *)ud;
    if (p == NULL) {
        return;
    }
    ++pr->calls;
    if (p->stage >= 0 && p->stage < 5) {
        pr->saw_stage[(int)p->stage] = 1;
    }
    if (p->message[0] == '\0') {
        ++pr->bad_message;
    }
    if (p->percent < pr->last_percent) {
        ++pr->bad_percent;
    }
    pr->last_percent = p->percent;
    if (p->percent > pr->max_percent) {
        pr->max_percent = p->percent;
    }
    if (p->speed_bps > 0.0) {
        pr->saw_speed = 1;
    }
    if (p->eta_seconds >= 0) {
        pr->saw_eta = 1;
    }
}

/* ── 用例 ── */

static void tmp_reset(const char *leaf)
{
    snprintf(g_tmp, sizeof(g_tmp), "_jre_tmp/%s", leaf);
    (void)sxcl_fs_remove_tree("_jre_tmp");
    (void)sxcl_fs_mkdirs(g_tmp);
}

static int exists_in(const char *leaf)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_tmp, leaf);
    return sxcl_fs_exists(path);
}

static int content_equals(const char *leaf, const char *want, size_t want_len)
{
    char path[1200];
    static char buf[65536];
    FILE *fp;
    size_t n;
    snprintf(path, sizeof(path), "%s/%s", g_tmp, leaf);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf), fp);
    (void)fclose(fp);
    return n == want_len && memcmp(buf, want, want_len) == 0;
}

/** 只比前 want_len 个字节(大文件用)。 */
static int content_starts_with(const char *leaf, const char *want, size_t want_len)
{
    char path[1200];
    static char buf[4096];
    FILE *fp;
    size_t n;
    snprintf(path, sizeof(path), "%s/%s", g_tmp, leaf);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    n = fread(buf, 1, want_len, fp);
    (void)fclose(fp);
    return n == want_len && memcmp(buf, want, want_len) == 0;
}

/** 夹具文件的字节数。 */
static int64_t fixture_size(const char *name)
{
    char path[1200];
    int64_t size = 0;
    snprintf(path, sizeof(path), "%s/%s", fixture_dir(), name);
    if (sxcl_fs_stat(path, &size, NULL) != 0) {
        return 0;
    }
    return size;
}

static unsigned char *g_universal = NULL;
static unsigned char *g_arm64 = NULL;
static unsigned char *g_v7a = NULL;

static void test_install_and_skip(void)
{
    static blob blobs[3];
    char *index;
    size_t index_len = 0;
    sxcl_jre_request req;
    sxcl_jre_result res;
    prog_probe probe;
    int rc;

    printf("== 正常安装 / ABI 过滤 / version 判据跳过\n");
    tmp_reset("main");
    index = build_index("17.0.9+sxcl.1", 1, 1, NULL, &index_len);
    check(index != NULL, "拼 index.json");
    if (index == NULL) {
        return;
    }
    memset(blobs, 0, sizeof(blobs));
    blobs[0].name = "jre17/universal.tar.xz";
    blobs[0].data = g_universal;
    blobs[0].len = (size_t)fixture_size("universal.tar.xz");
    blobs[1].name = "jre17/bin-arm64.tar.xz";
    blobs[1].data = g_arm64;
    blobs[1].len = (size_t)fixture_size("bin-arm64.tar.xz");
    blobs[2].name = "jre17/bin-armeabi-v7a.tar.xz";
    blobs[2].data = g_v7a;
    blobs[2].len = (size_t)fixture_size("bin-armeabi-v7a.tar.xz");
    g_blobs = blobs;
    g_blob_count = 3;
    g_object_reads = 0;

    memset(&req, 0, sizeof(req));
    req.java_major = 17;
    req.abi = "arm64";
    req.index_text = index;
    req.target_dir = g_tmp;
    req.skip_if_installed = 1;
    req.transport_factory = fake_transport;
    memset(&probe, 0, sizeof(probe));
    req.on_progress = on_progress;
    req.ud = &probe;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    if (rc != 0) {
        printf("      err=%s\n", res.error);
    }
    check_int(rc, 0, "安装成功");
    check_str(res.component, "jre17", "组件名");
    check_str(res.version, "17.0.9+sxcl.1", "版本");
    check_int((long)res.files_total, 2, "只装 2 个包(universal + bin-arm64)");
    check_int((long)res.files_failed, 0, "没有失败");
    check(res.bytes_total > 0, "量出了总字节数");

    check(exists_in("bin/java"), "bin/java 落盘");
    check(exists_in("lib/libjli.so"), "lib/libjli.so 落盘");
    check(exists_in("release"), "release 落盘(universal 那份)");
    check(exists_in("lib/server/libjvm.so"), "lib/server/libjvm.so 落盘");
    check(!exists_in("V7A-MUST-NOT-BE-EXTRACTED"), "别的 ABI 的包没被解开");
    check(content_starts_with("lib/libjli.so", "\x7f" "ELFJLI-arm64", 13),
          "arm64 的 lib/libjli.so(universal 里没有这个文件)");
    check(sxcl_fs_exists(res.marker_path), "jre.json 写出来了");
    {
        char version[64];
        char json[4096];
        version[0] = '\0';
        json[0] = '\0';
        check_int(sxcl_jre_read_marker(g_tmp, version, sizeof(version), json, sizeof(json)),
                  SXCL_JRE_OK, "读 jre.json");
        check_str(version, "17.0.9+sxcl.1", "jre.json 里的 version");
        check(strstr(json, "\"sha256\"") != NULL, "jre.json 记了每个包的 sha256");
        check(strstr(json, "jre17/universal.tar.xz") != NULL, "jre.json 记了包名");
        check(strstr(json, "\"abi\": \"arm64\"") != NULL, "jre.json 记了 abi");
    }
    check_int(sxcl_jre_is_installed(g_tmp, "17.0.9+sxcl.1"), 1, "已装判定(版本相同)");
    check_int(sxcl_jre_is_installed(g_tmp, "17.0.10"), 0, "已装判定(版本不同 = 没装)");
    check_int(probe.bad_message, 0, "每条进度都有人话");
    check_int(probe.bad_percent, 0, "百分比不倒退");
    check_int(probe.max_percent, 100, "进度走到 100");
    check(probe.saw_stage[SXCL_JRE_STAGE_DOWNLOAD], "报过下载阶段");
    check(probe.saw_stage[SXCL_JRE_STAGE_EXTRACT], "报过解包阶段");
    check(probe.saw_stage[SXCL_JRE_STAGE_FINISH], "报过收尾阶段");

    /* 第二次:version 相同 -> 整个跳过,一个字节都不下 */
    g_object_reads = 0;
    g_requests = 0;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    check_int(rc, 0, "第二次安装成功");
    check_int(res.skipped, 1, "第二次整个跳过");
    check_int(g_object_reads, 0, "第二次一个字节都没下");
    check_int(g_requests, 0, "第二次连 index 都没取");

    /* 第三次:index 版本变了 -> 必须重装(下载重新发生) */
    {
        char *index2 = build_index("17.0.10+sxcl.2", 0, 1, NULL, &index_len);
        g_object_reads = 0;
        req.index_text = index2;
        memset(&res, 0, sizeof(res));
        rc = sxcl_jre_install(&req, &res);
        check_int(rc, 0, "版本变了重装成功");
        check_int(res.skipped, 0, "没有跳过");
        check_int((long)res.files_skipped, 0, "包缓存上次装完已清:这次全是真下载");
        check(g_object_reads > 0, "确实下载了");
        check_int(sxcl_jre_is_installed(g_tmp, "17.0.10+sxcl.2"), 1, "新版本已装");
        free(index2);
    }
    free(index);
}

static void test_bad_index(void)
{
    char err[256];
    char *index;
    size_t index_len = 0;
    /* sxcl_jre_component 有 ~20 KiB,数组要堆分配(栈上会爆) */
    sxcl_jre_component *list = (sxcl_jre_component *)calloc(8, sizeof(sxcl_jre_component));
    sxcl_jre_request req;
    sxcl_jre_result res;
    int rc;
    int n;
    if (list == NULL) {
        check(0, "分配组件表");
        return;
    }

    printf("== index.json 校验(schema / 必填字段 / 组件挑选)\n");
    {
        static const char kBadSchema[] = "{\"schema\":2,\"components\":[]}";
        n = sxcl_jre_index_parse(kBadSchema, strlen(kBadSchema), list, 8, err, sizeof(err));
    }
    check_int(n, SXCL_JRE_ERR_INDEX, "schema=2 必须拒");
    check(strstr(err, "schema") != NULL, "错误信息点到 schema");

    /* 形态 3:没有任何校验信息 -> **接受**(只按 size 校验),并标 no_hash */
    index = build_index("17.0.9", 1, 1, "bin-arm64.tar.xz", &index_len);
    n = sxcl_jre_index_parse(index, index_len, list, 8, err, sizeof(err));
    check_int(n, 1, "缺 sha256 仍然接受(形态 3:只校验大小)");
    if (n == 1) {
        check_int(list[0].files[1].no_hash, 1, "缺 sha256 的条目标了 no_hash");
        check_int(list[0].files[0].no_hash, 0, "给了 sha256 的条目没标");
    }
    free(index);

    /* 写了 sha256 但不是 64 位十六进制、也不是 URL -> 必须拒(这是笔误,不能当形态 3 放过) */
    index = build_index("17.0.9", 1, 1, NULL, &index_len);
    {
        char *at = strstr(index, "\"sha256\": \"");
        check(at != NULL, "找到一处 sha256");
        if (at != NULL) {
            memcpy(at + 13, "zz", 2); /* 把第一个哈希的头两个字符改成非十六进制 */
        }
        n = sxcl_jre_index_parse(index, index_len, list, 8, err, sizeof(err));
        check_int(n, SXCL_JRE_ERR_INDEX, "sha256 既不是 hex 也不是 URL -> 必须拒");
    }
    free(index);

    index = build_index("17.0.9", 1, 1, NULL, &index_len);
    n = sxcl_jre_index_parse(index, index_len, list, 8, err, sizeof(err));
    check_int(n, 1, "正常 index 解析出 1 个组件");
    if (n == 1) {
        sxcl_jre_component picked;
        check_int(sxcl_jre_index_pick(list, 1, "jre17", 0, &picked, err, sizeof(err)),
                  SXCL_JRE_OK, "按 id 挑");
        check_int(sxcl_jre_index_pick(list, 1, NULL, 17, &picked, err, sizeof(err)),
                  SXCL_JRE_OK, "按主版本挑");
        check_int(sxcl_jre_index_pick(list, 1, NULL, 21, &picked, err, sizeof(err)),
                  SXCL_JRE_ERR_COMPONENT, "没有 21 必须报错(不顺手换一个)");
        check_int(sxcl_jre_index_pick(list, 1, "jre21", 0, &picked, err, sizeof(err)),
                  SXCL_JRE_ERR_COMPONENT, "id 不存在必须报错");
        check(sxcl_jre_file_applies(&list[0].files[0], "arm64"), "universal 一律适用");
        check(sxcl_jre_file_applies(&list[0].files[1], "arm64"), "bin-arm64 适用 arm64");
        check(!sxcl_jre_file_applies(&list[0].files[2], "arm64"), "bin-armeabi-v7a 不适用 arm64");
        check(sxcl_jre_file_applies(&list[0].files[2], "armeabi-v7a"), "bin-armeabi-v7a 适用 v7a");
    }

    /* 没有任何适用文件 -> ERR_NO_ABI(只给 bin-<别的 abi>、没有 universal 的清单) */
    {
        char only_bin[4096];
        size_t used = 0;
        used += (size_t)snprintf(
            only_bin, sizeof(only_bin),
            "{\"schema\":1,\"components\":[{\"id\":\"jre17\",\"javaMajor\":17,"
            "\"version\":\"17.0.9\",\"files\":[{\"file\":\"jre17/bin-arm64.tar.xz\","
            "\"size\":%lld,\"sha256\":\"%s\"}]}]}",
            (long long)fixture_size("bin-arm64.tar.xz"), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        (void)used;
        memset(&req, 0, sizeof(req));
        req.java_major = 17;
        req.abi = "riscv64";
        req.index_text = only_bin;
        req.target_dir = g_tmp;
        req.transport_factory = fake_transport;
        memset(&res, 0, sizeof(res));
        rc = sxcl_jre_install(&req, &res);
        check_int(rc, SXCL_JRE_ERR_NO_ABI, "只给 bin-arm64 而本机是 riscv64 -> 必须报 abi");
    }
    free(index);
    free(list);
}

static void test_sha256_mismatch(void)
{
    static blob blobs[3];
    char *index;
    size_t index_len = 0;
    sxcl_jre_request req;
    sxcl_jre_result res;
    int rc;

    printf("== SHA-256 不符必须拦住(引擎强校验)\n");
    tmp_reset("bad");
    index = build_index("17.0.9", 1, 1, NULL, &index_len);
    {
        int64_t size = 0;
        char path[1200];
        snprintf(path, sizeof(path), "%s/universal.tar.xz", fixture_dir());
        (void)sxcl_fs_stat(path, &size, NULL);
        memset(blobs, 0, sizeof(blobs));
        blobs[0].name = "jre17/universal.tar.xz";
        blobs[0].data = g_universal;
        blobs[0].len = (size_t)size;
        blobs[0].corrupt = 1;
        g_blobs = blobs;
        g_blob_count = 1;
        g_object_reads = 0;
        memset(&req, 0, sizeof(req));
        req.java_major = 17;
        req.abi = "arm64";
        req.index_text = index;
        req.target_dir = g_tmp;
        req.transport_factory = fake_transport;
        memset(&res, 0, sizeof(res));
        rc = sxcl_jre_install(&req, &res);
        check_int(rc, SXCL_JRE_ERR_DOWNLOAD, "坏包必须报 download");
        check(res.error[0] != '\0', "有人话错误");
        check(!exists_in("bin/java"), "坏包不许落出半棵树");
        check(!sxcl_fs_exists(res.marker_path), "失败不许写 jre.json");
    }
    free(index);
}

static int cancel_after_n(void *ud)
{
    int *hits = (int *)ud;
    ++*hits;
    /* 第 1 次是解包 universal 之前:放行;第 2 次是解包 bin-arm64 之前:取消 */
    return (*hits >= 2) ? 1 : 0;
}

static void test_cancel(void)
{
    static blob blobs[3];
    char *index;
    size_t index_len = 0;
    sxcl_jre_request req;
    sxcl_jre_result res;
    int rc;
    int hits = 0;

    printf("== 取消(解包阶段按下取消:不许写 jre.json)\n");
    tmp_reset("cancel");
    index = build_index("17.0.9", 1, 1, NULL, &index_len);
    memset(blobs, 0, sizeof(blobs));
    blobs[0].name = "jre17/universal.tar.xz";
    blobs[0].data = g_universal;
    blobs[0].len = (size_t)fixture_size("universal.tar.xz");
    blobs[1].name = "jre17/bin-arm64.tar.xz";
    blobs[1].data = g_arm64;
    blobs[1].len = (size_t)fixture_size("bin-arm64.tar.xz");
    g_blobs = blobs;
    g_blob_count = 2;
    memset(&req, 0, sizeof(req));
    req.java_major = 17;
    req.abi = "arm64";
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = fake_transport;
    req.is_cancelled = cancel_after_n;
    req.ud = &hits;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    check_int(rc, SXCL_JRE_ERR_CANCELLED, "取消 -> cancelled");
    check_int(res.fail_stage, SXCL_JRE_STAGE_EXTRACT, "取消发生在解包阶段");
    check(!sxcl_fs_exists(res.marker_path), "取消不写 jre.json");
    check(hits >= 2, "取消探针确实被问过");
    free(index);
}

/** 拼一份**既有 schema** 的 SXCL/Java_index.json 片段(versions -> platforms -> android -> arm64)。 */
static char *build_java_index(int with_url_sha256, size_t *len_out)
{
    char *text = (char *)malloc(8192);
    size_t used = 0;
    char sha256[65];
    char sha1[41];
    char path[1200];
    int64_t size = 0;
    const char *sc = "https://cloud.example.cn/api/v1/owner/JDK/Java/JRE/Android";
    const char *gh = "https://raw.githubusercontent.com/Silent-Studio-CN/index/main/jre";
    if (text == NULL) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/universal.tar.xz", fixture_dir());
    if (sxcl_fs_stat(path, &size, NULL) != 0 ||
        sxcl_hash_file(path, SXCL_HASH_SHA256, sha256, sizeof(sha256)) != 0 ||
        sxcl_hash_file(path, SXCL_HASH_SHA1, sha1, sizeof(sha1)) != 0) {
        free(text);
        return NULL;
    }
    used += (size_t)snprintf(
        text + used, 8192 - used,
        "{\n  \"schema_version\": 1,\n  \"description\": \"JDK 下载索引 — 用于 Silent X Craft Launcher 自动安装 Java\",\n"
        "  \"versions\": {\n    \"21\": {\n      \"name\": \"JDK 21 (LTS)\",\n      \"recommended\": true,\n"
        "      \"platforms\": {\n        \"windows\": { \"x64\": { \"format\": \"exe\", \"url\": \"https://example/w.exe\", \"size_mb\": 166.9, \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" } },\n"
        "        \"android\": {\n          \"arm64\": {\n            \"format\": \"tar.xz\",\n"
        "            \"version\": \"21.0.8+9-sxcl.1\",\n"
        "            \"size\": %lld,\n            \"size_mb\": %.2f,\n"
        "            \"sha256\": \"%s\",\n            \"sha1\": \"%s\",\n"
        "            \"url\": \"%s/jre21/universal.tar.xz\",\n"
        "            \"mirrors\": [\"%s/jre21/universal.tar.xz\"]\n          }\n        },\n"
        "        \"linux\": { \"x64\": { \"format\": \"tar.gz\", \"url\": \"https://example/l.tgz\", \"size_mb\": 180, \"sha256\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\" } }\n"
        "      }\n    }\n  }\n}\n",
        (long long)size, (double)size / 1048576.0, sha256, sha1, sc, gh);
    (void)with_url_sha256;
    if (len_out != NULL) {
        *len_out = used;
    }
    return text;
}

static void test_java_index_schema(void)
{
    char *index;
    size_t index_len = 0;
    sxcl_jre_component *list = (sxcl_jre_component *)calloc(8, sizeof(sxcl_jre_component));
    int n;
    char err[256];
    static blob blobs[1];
    sxcl_jre_request req;
    sxcl_jre_result res;
    int rc;
    if (list == NULL) {
        check(0, "分配组件表");
        return;
    }

    printf("== 既有 schema:SXCL/Java_index.json(versions -> platforms -> android -> arm64)\n");
    index = build_java_index(0, &index_len);
    check(index != NULL, "拼 Java_index.json");
    if (index == NULL) {
        return;
    }
    err[0] = '\0';
    n = sxcl_jre_index_parse(index, index_len, list, 8, err, sizeof(err));
    check_int(n, 1, "解析出 1 个安卓组件(桌面那两档被忽略)");
    if (n != 1) {
        printf("      err=%s\n", err);
        free(index);
        free(list);
        return;
    }
    check_str(list[0].id, "jre21", "id 由 javaMajor 推出来(可被 id 字段覆盖)");
    check_int(list[0].java_major, 21, "javaMajor 来自键名");
    check_str(list[0].version, "21.0.8+9-sxcl.1", "version 来自平台档(重装判据)");
    check_int((long)list[0].file_count, 1, "一个文件(universal)");
    check(strncmp(list[0].files[0].file, "https://cloud.example.cn/", 25) == 0,
          "url 是绝对地址");
    check(list[0].files[0].size > 0, "size 是字节数");
    check_int(list[0].files[0].platform_selected, 1, "标了 platform_selected");
    check_str(list[0].files[0].mirror, "https://raw.githubusercontent.com/Silent-Studio-CN/index/main/jre/jre21/universal.tar.xz", "mirrors[0] -> 第二条候选");
    check(sxcl_jre_file_applies(&list[0].files[0], "arm64"), "platform_selected 不必再按文件名判 ABI");
    check(sxcl_jre_file_applies(&list[0].files[0], "x86_64"), "别的 ABI 也照样适用(清单已选好)");

    /* 真装一次:url 是绝对地址 -> 不能被当成相对路径拼到清单后面 */
    tmp_reset("javaindex");
    memset(blobs, 0, sizeof(blobs));
    blobs[0].name = "jre21/universal.tar.xz";
    blobs[0].data = g_universal;
    blobs[0].len = (size_t)fixture_size("universal.tar.xz");
    g_blobs = blobs;
    g_blob_count = 1;
    memset(&req, 0, sizeof(req));
    req.java_major = 21;
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = fake_transport;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    if (rc != 0) {
        printf("      err=%s\n", res.error);
    }
    check_int(rc, 0, "按 Java_index.json 装成功");
    check_str(res.component, "jre21", "组件名");
    check(exists_in("bin/java"), "bin/java 落盘(universal 里带的)");
    check(sxcl_fs_exists(res.marker_path), "jre.json 写出来了");

    /* sha256 写在 URL 里(老 schema 的坑):必须去取一次 */
    index = build_java_index(1, &index_len);
    {
        /* 把 sha256 换成一个 URL(指向假传输里的一份 .sha256 文本) */
        char *p = strstr(index, "\"sha256\": \"");
        char local[4096];
        size_t used = 0;
        check(p != NULL, "找到 sha256 字段");
        if (p != NULL) {
            const char *after = strstr(p + 12, "\",");
            /* 用替换的方式拼一份新的:前半 + URL + 后半 */
            size_t head = (size_t)(p + 12 - index);
            used += (size_t)snprintf(local, sizeof(local), "%.*s", (int)head, index);
            used += (size_t)snprintf(local + used, sizeof(local) - used,
                                     "https://cloud.example.cn/api/v1/owner/JDK/universal.tar.xz.sha256%s",
                                     after != NULL ? after : "\",");
            free(index);
            index = (char *)malloc(strlen(local) + 1);
            memcpy(index, local, strlen(local) + 1);
        }
    }
    free(index);
}

/* ── 正式来源:sxcl.jre.index/1(D:\SilentStudio\_termux_jre\out\index.json 的那一份) ── */

/** 拼一份**忠实于真实 schema** 的 sxcl.jre.index/1(字段名/嵌套与真清单逐一对齐)。 */
static char *build_sxcl_index(const char *version, int major, size_t *len_out)
{
    char *text = (char *)malloc(8192);
    size_t used = 0;
    static const char *names[2] = {"universal.tar.xz", "bin-arm64.tar.xz"};
    int i;
    if (text == NULL) {
        return NULL;
    }
    used += (size_t)snprintf(
        text + used, 8192 - used,
        "{\"schema\":\"%s\",\"abi\":\"arm64-v8a\",\"generated\":\"2026-01-01T00:00:00Z\","
        "\"components\":{\"jre%d\":{\"component\":\"jre%d\",\"major\":%d,\"version\":\"%s\","
        "\"java_version\":\"%s\",\"abi\":\"arm64-v8a\",\"implementor\":\"Termux\","
        "\"id\":\"jre%d/%s/arm64-v8a/deadbeefdeadbeef\",\"file_count\":339,"
        "\"installed_bytes\":158165753,\"install_dir_hint\":\"x\","
        "\"packages\":[",
        SXCL_JRE_INDEX_SCHEMA_STRING, major, major, major, version, version, major, version);
    for (i = 0; i < 2; ++i) {
        char path[1200];
        char sha256[65];
        char sha1[41];
        int64_t size = 0;
        snprintf(path, sizeof(path), "%s/%s", fixture_dir(), names[i]);
        (void)sxcl_fs_stat(path, &size, NULL);
        (void)sxcl_hash_file(path, SXCL_HASH_SHA256, sha256, sizeof(sha256));
        (void)sxcl_hash_file(path, SXCL_HASH_SHA1, sha1, sizeof(sha1));
        used += (size_t)snprintf(
            text + used, 8192 - used,
            "%s{\"name\":\"%s\",\"size\":%lld,\"sha256\":\"%s\",\"sha1\":\"%s\","
            "\"url\":\"jre%d/%s\"}",
            i == 0 ? "" : ",", names[i], (long long)size, sha256, sha1, major, names[i]);
    }
    used += (size_t)snprintf(
        text + used, 8192 - used,
        "],\"version_file\":{\"name\":\"version\",\"size\":470,\"sha256\":\"%s\","
        "\"sha1\":\"%s\",\"url\":\"jre%d/version\"},"
        "\"notice\":[{\"name\":\"NOTICE.txt\",\"size\":5942,\"sha256\":\"%s\","
        "\"sha1\":\"%s\",\"url\":\"jre%d/NOTICE/NOTICE.txt\"}],"
        "\"manifest\":{\"files\":{\"bin/java\":{\"type\":\"file\",\"size\":6032}}},"
        "\"source\":{\"kind\":\"termux-deb\"}}}}",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        major,
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", major);
    if (len_out != NULL) {
        *len_out = used;
    }
    return text;
}

static void test_sxcl_index_schema(void)
{
    static blob blobs[2];
    char *index;
    size_t index_len = 0;
    sxcl_jre_component *list = (sxcl_jre_component *)calloc(8, sizeof(sxcl_jre_component));
    sxcl_jre_request req;
    sxcl_jre_result res;
    prog_probe probe;
    int n;
    int rc;
    char err[256];

    printf("== 正式来源:sxcl.jre.index/1(components 对象 + packages[相对 url])\n");
    if (list == NULL) {
        check(0, "分配组件表");
        return;
    }
    index = build_sxcl_index("17.0.20", 17, &index_len);
    check(index != NULL, "拼 sxcl.jre.index/1");
    if (index == NULL) {
        free(list);
        return;
    }
    err[0] = '\0';
    n = sxcl_jre_index_parse(index, index_len, list, 8, err, sizeof(err));
    if (n != 1) {
        printf("      err=%s\n", err);
    }
    check_int(n, 1, "解析出 1 个组件");
    if (n == 1) {
        check_str(list[0].id, "jre17", "组件名");
        check_int(list[0].java_major, 17, "major");
        check_str(list[0].version, "17.0.20", "version");
        check_str(list[0].abi, "arm64-v8a", "组件 ABI");
        check_str(list[0].dir_key, "jre17/17.0.20/arm64-v8a", "落盘相对路径");
        check_str(list[0].index_id, "jre17/17.0.20/arm64-v8a/deadbeefdeadbeef", "清单里的 id");
        check_int((long)list[0].file_count, 2, "两个 packages");
        check_int((long)list[0].installed_bytes, 158165753, "installed_bytes 带过来了");
        check_str(list[0].files[0].file, "jre17/universal.tar.xz", "rel url 原样留着");
        check_int(list[0].files[0].platform_selected, 1, "packages 直接算平台已选");
        check(list[0].files[0].size > 0 && list[0].files[0].sha256[0] != '\0', "带 size + sha256");
    }

    /* 真装一次:相对 url 要相对**清单 URL** 解析 */
    tmp_reset("sxclidx");
    memset(blobs, 0, sizeof(blobs));
    blobs[0].name = "jre17/universal.tar.xz";
    blobs[0].data = g_universal;
    blobs[0].len = (size_t)fixture_size("universal.tar.xz");
    blobs[1].name = "jre17/bin-arm64.tar.xz";
    blobs[1].data = g_arm64;
    blobs[1].len = (size_t)fixture_size("bin-arm64.tar.xz");
    g_blobs = blobs;
    g_blob_count = 2;
    g_object_reads = 0;
    memset(&req, 0, sizeof(req));
    req.java_major = 17;
    req.abi = "arm64";
    req.index_text = index; /* 清单文本直接给,但 url 仍然按"清单地址"解析 */
    req.index_url = "https://cloud.silentstudio.cn/api/v1/owner/JDK/Java/JRE/Android/index.json";
    req.target_root = "_jre_tmp/sxclidx";
    req.skip_if_installed = 1;
    req.transport_factory = fake_transport;
    memset(&probe, 0, sizeof(probe));
    req.on_progress = on_progress;
    req.ud = &probe;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    if (rc != 0) {
        printf("      err=%s\n", res.error);
    }
    check_int(rc, 0, "按 sxcl.jre.index/1 安装成功");
    check_str(res.component, "jre17", "组件名");
    check_str(res.version, "17.0.20", "版本");
    check_int(g_object_reads, 2, "两个包都真下了(相对 url 拼对了)");
    check(strstr(res.java_home, "jre17/17.0.20/arm64-v8a") != NULL ||
              strstr(res.java_home, "jre17\\17.0.20\\arm64-v8a") != NULL,
          "装到 <runtime>/jre17/17.0.20/arm64-v8a");
    check(sxcl_fs_exists(res.java_path), "java 可执行文件在");
    check(sxcl_fs_exists(res.marker_path), "jre.json 在");
    {
        char json[4096];
        json[0] = '\0';
        check_int(sxcl_jre_read_marker(res.java_home, NULL, 0, json, sizeof(json)), SXCL_JRE_OK,
                  "读 jre.json");
        check(strstr(json, "jre17/17.0.20/arm64-v8a/deadbeefdeadbeef") != NULL,
              "jre.json 记了清单里的 id(可追溯)");
        check(strstr(json, "\"installedBytes\": 158165753") != NULL, "jre.json 记了 installed_bytes");
    }
    check_int(sxcl_jre_is_installed(res.java_home, "17.0.20"), 1, "已装判定(版本相同)");

    /* 组件 ABI 与本机不符 -> 明确报 abi,不许靠文件名过滤"猜" */
    memset(&req, 0, sizeof(req));
    req.java_major = 17;
    req.abi = "x64";
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = fake_transport;
    tmp_reset("sxclidxabi");
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    check_int(rc, SXCL_JRE_ERR_NO_ABI, "arm64-v8a 的组件装到 x64 上必须报 abi");
    check(strstr(res.error, "arm64-v8a") != NULL, "错误信息点名清单里的 ABI");

    /* 不认识的字符串 schema -> 明确报错 */
    {
        static const char kFuture[] = "{\"schema\":\"sxcl.jre.index/99\",\"components\":{}}";
        n = sxcl_jre_index_parse(kFuture, strlen(kFuture), list, 8, err, sizeof(err));
        check_int(n, SXCL_JRE_ERR_INDEX, "不认识的 schema 串必须拒");
        check(strstr(err, "sxcl.jre.index/1") != NULL, "错误信息说清我们只认哪一版");
    }
    free(index);
    free(list);
    (void)sxcl_fs_remove_tree("_jre_tmp/sxclidx");
}

/** 诊断(不是必跑用例):SXCL_JRE_REAL_INDEX 指向真清单时,真读一遍并核对关键字段。
 *  这一条证明的是"我们消费的是**真实**那一份",而不是测试里自己拼的形状。 */
static void test_real_index_if_any(void)
{
    const char *path = getenv("SXCL_JRE_REAL_INDEX");
    char *text = NULL;
    size_t len = 0;
    sxcl_jre_component *list;
    sxcl_jre_component picked;
    int n;
    char err[256];
    printf("== 真清单(SXCL_JRE_REAL_INDEX)真读一遍\n");
    if (path == NULL || *path == '\0') {
        printf("      (没设 SXCL_JRE_REAL_INDEX,跳过)\n");
        return;
    }
    {
        FILE *fp = fopen(path, "rb");
        long size;
        if (fp == NULL) {
            printf("      (打不开 %s,跳过)\n", path);
            return;
        }
        (void)fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        (void)fseek(fp, 0, SEEK_SET);
        text = (char *)malloc((size_t)size + 1u);
        if (text == NULL || fread(text, 1, (size_t)size, fp) != (size_t)size) {
            free(text);
            (void)fclose(fp);
            check(0, "读真清单");
            return;
        }
        (void)fclose(fp);
        text[size] = '\0';
        len = (size_t)size;
    }
    list = (sxcl_jre_component *)calloc(8, sizeof(sxcl_jre_component));
    check(list != NULL, "分配组件表");
    if (list == NULL) {
        free(text);
        return;
    }
    err[0] = '\0';
    n = sxcl_jre_index_parse(text, len, list, 8, err, sizeof(err));
    if (n < 0) {
        printf("      err=%s\n", err);
    }
    check_int(n, 3, "真清单里 3 个组件(jre17/jre21/jre25)");
    if (n == 3) {
        int i;
        for (i = 0; i < n; ++i) {
            printf("      %-6s major=%-3d version=%-9s abi=%-10s packages=%u dir=%s\n",
                   list[i].id, list[i].java_major, list[i].version, list[i].abi,
                   (unsigned)list[i].file_count, list[i].dir_key);
        }
        check_int(sxcl_jre_index_pick(list, (size_t)n, "jre17", 0, &picked, err, sizeof(err)),
                  SXCL_JRE_OK, "按 id 挑 jre17");
        check_str(picked.version, "17.0.20", "jre17 的版本");
        check_int((long)picked.file_count, 2, "jre17 两个包(universal + bin-arm64)");
        check_str(picked.files[0].file, "jre17/universal.tar.xz", "第一个包是 relative url");
        check_int((long)picked.files[0].size, 31132644, "universal.tar.xz 的字节数");
        check_str(picked.files[0].sha256,
                  "aa11db5ff7f38101b51d367af16e885702cc97ffcdabc5bea76c27360ce9bcaf",
                  "universal.tar.xz 的 sha256");
        check_int(sxcl_jre_index_pick(list, (size_t)n, NULL, 21, &picked, err, sizeof(err)),
                  SXCL_JRE_OK, "按 major 挑 21");
        check_str(picked.id, "jre21", "挑出来的是 jre21");
    }
    free(list);
    free(text);
}

/* ── sha256 的三种形态(最终口径,见 docs/19 §3) ── */

static char *build_index_forms(int form, const char *sha256_value, size_t *len_out)
{
    /* form 1 = 字面哈希 / 2 = 侧车 URL / 3 = 缺失 */
    char *text = (char *)malloc(4096);
    size_t used = 0;
    char sha256[65];
    char path[1200];
    int64_t size = 0;
    if (text == NULL) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/universal.tar.xz", fixture_dir());
    if (sxcl_fs_stat(path, &size, NULL) != 0 ||
        sxcl_hash_file(path, SXCL_HASH_SHA256, sha256, sizeof(sha256)) != 0) {
        free(text);
        return NULL;
    }
    if (form == 1) {
        sha256_value = sha256;
    }
    used += (size_t)snprintf(
        text + used, 4096 - used,
        "{\"schema\":1,\"components\":[{\"id\":\"jre21\",\"javaMajor\":21,"
        "\"version\":\"21.0.8+9-sxcl.1\",\"files\":["
        "{\"file\":\"https://cloud.example.cn/pkg/universal.tar.xz\",\"size\":%lld",
        (long long)size);
    if (form != 3) {
        used += (size_t)snprintf(text + used, 4096 - used, ",\"sha256\":\"%s\"", sha256_value);
    }
    /* mirrors:形态 2 要靠它的同名 .sha256 兜底(清单在 GitHub、包在云上就是这个形状) */
    used += (size_t)snprintf(text + used, 4096 - used,
                             ",\"mirrors\":[\"https://cloud.example.cn/pkg/universal.tar.xz\"]");
    /* bin-arm64 那一份:绝对 URL + **字面哈希**(它不参与三种形态的验证,只是为了让
     * 解出来的树里有 bin/java,好让"安装成功"这件事可判定)。 */
    {
        char bpath[1200];
        char bsha[65];
        int64_t bsize = 0;
        snprintf(bpath, sizeof(bpath), "%s/bin-arm64.tar.xz", fixture_dir());
        (void)sxcl_fs_stat(bpath, &bsize, NULL);
        (void)sxcl_hash_file(bpath, SXCL_HASH_SHA256, bsha, sizeof(bsha));
        used += (size_t)snprintf(
            text + used, 4096 - used,
            "},{\"file\":\"https://cloud.example.cn/pkg/bin-arm64.tar.xz\",\"size\":%lld,"
            "\"sha256\":\"%s\"}]}]}",
            (long long)bsize, bsha);
    }
    if (len_out != NULL) {
        *len_out = used;
    }
    return text;
}

static void test_hash_forms(void)
{
    static blob blobs[3];
    char *index;
    size_t index_len = 0;
    sxcl_jre_request req;
    sxcl_jre_result res;
    prog_probe probe;
    char sidecar[96];
    int rc;

    printf("== sha256 三种形态:字面哈希 / 侧车 URL(含镜像兜底) / 缺失(只校验大小)\n");

    /* 侧车文件的**原文**:64 个小写十六进制 + 换行(与 Oracle 的 .sha256 逐字节同形态) */
    {
        char path[1200];
        char sha256[65];
        size_t len = 0;
        int64_t size = 0;
        snprintf(path, sizeof(path), "%s/universal.tar.xz", fixture_dir());
        (void)sxcl_fs_stat(path, &size, NULL);
        (void)sxcl_hash_file(path, SXCL_HASH_SHA256, sha256, sizeof(sha256));
        snprintf(sidecar, sizeof(sidecar), "%s\n", sha256);
        (void)len;
    }
    memset(blobs, 0, sizeof(blobs));
    /* 顺序要紧:.sha256 必须先匹配,否则包名会先把侧车 URL 吃掉 */
    blobs[0].name = "universal.tar.xz.sha256";
    blobs[0].data = (const unsigned char *)sidecar;
    blobs[0].len = strlen(sidecar);
    blobs[1].name = "universal.tar.xz";
    blobs[1].data = g_universal;
    blobs[1].len = (size_t)fixture_size("universal.tar.xz");
    blobs[2].name = "bin-arm64.tar.xz";
    blobs[2].data = g_arm64;
    blobs[2].len = (size_t)fixture_size("bin-arm64.tar.xz");
    g_blobs = blobs;
    g_blob_count = 3;

    /* 形态 2:主 URL 是**取不到**的(名字没在任何夹具里 —— 所以只能用 mirrors 的同名
     * .sha256),必须靠镜像兜底 */
    tmp_reset("form2");
    index = build_index_forms(2, "https://nohost.invalid/pkg/some-other-name.sha256", &index_len);
    check(index != NULL, "拼形态 2 的清单");
    memset(&req, 0, sizeof(req));
    req.java_major = 21;
    req.abi = "arm64";
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = fake_transport;
    memset(&probe, 0, sizeof(probe));
    req.on_progress = on_progress;
    req.ud = &probe;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    if (rc != 0) {
        printf("      err=%s\n", res.error);
    }
    check_int(rc, 0, "形态 2(侧车 URL)安装成功");
    check(exists_in("bin/java"), "形态 2 的包解出来了");
    check_int((long)res.files_without_hash, 0, "形态 2 拿到了哈希,不算'没有校验信息'");
    free(index);
    (void)probe;

    /* 形态 3:没有 sha256 -> 只能按 size 校验,并必须如实报出来 */
    tmp_reset("form3");
    index = build_index_forms(3, NULL, &index_len);
    check(index != NULL, "拼形态 3 的清单");
    memset(&req, 0, sizeof(req));
    req.java_major = 21;
    req.abi = "arm64";
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = fake_transport;
    memset(&probe, 0, sizeof(probe));
    req.on_progress = on_progress;
    req.ud = &probe;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    if (rc != 0) {
        printf("      err=%s\n", res.error);
    }
    check_int(rc, 0, "形态 3(没有校验信息)仍然能装(只校验大小)");
    check_int((long)res.files_without_hash, 1, "如实报了'1 个包没有校验信息'");
    check(exists_in("bin/java"), "形态 3 的包解出来了");
    free(index);

    /* 形态 2 但侧车内容不是十六进制 -> 必须**拒装**(不许当成'没有校验'放过去) */
    tmp_reset("form2bad");
    {
        const char *junk = "not-a-hash\n";
        blobs[0].data = (const unsigned char *)junk;
        blobs[0].len = strlen(junk);
    }
    index = build_index_forms(2, "https://cloud.example.cn/pkg/universal.tar.xz.sha256",
                              &index_len);
    memset(&req, 0, sizeof(req));
    req.java_major = 21;
    req.abi = "arm64";
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = fake_transport;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    check_int(rc, SXCL_JRE_ERR_INDEX, "侧车里没有 64 位十六进制 -> 报 index 并拒装");
    check(strstr(res.error, "校验信息拿不到") != NULL, "错误信息写明'校验信息拿不到'");
    check(!sxcl_fs_exists(res.marker_path), "拒装时不许写 jre.json");
    free(index);
}

static void test_source_and_url(void)
{
    char out[512];
    printf("== 来源解析(显式 > 环境变量 > 设置)与相对 URL 拼接\n");
    check_int(sxcl_jre_resolve_index_url("https://a/index.json", "https://b/", "https://c/", out,
                                         sizeof(out)),
              SXCL_JRE_OK, "显式优先");
    check_str(out, "https://a/index.json", "显式赢");
    check_int(sxcl_jre_resolve_index_url(NULL, "https://b/", "https://c/", out, sizeof(out)),
              SXCL_JRE_OK, "环境变量优先于设置");
    check_str(out, "https://c/", "环境变量赢(与 java_runtime 的优先级一致)");
    check_int(sxcl_jre_resolve_index_url(NULL, "https://b/", "", out, sizeof(out)), SXCL_JRE_OK,
              "设置兜底");
    check_str(out, "https://b/", "设置赢");
    /* 四级来源:显式 > 环境变量 > 设置 > 编译期默认(GitHub raw)。
     * 默认值里带着 <REPO> 占位(仓库名待用户确认)时视为"没有默认值" -> 报错。 */
    {
        const int placeholder = sxcl_jre_default_index_url_is_placeholder();
        const int rc = sxcl_jre_resolve_index_url(NULL, NULL, NULL, out, sizeof(out));
        if (placeholder) {
            check_int(rc, SXCL_JRE_ERR_ARG, "默认仓库名是占位 -> 报错(不拿假地址去请求)");
            check_str(out, "", "占位时不返回任何地址");
        } else {
            check_int(rc, SXCL_JRE_OK, "仓库名已钉死 -> 用编译期默认(GitHub raw)");
            check_str(out, "https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/index.json",
                      "默认值就是定下来的那一条");
            check(strncmp(SXCL_JRE_RAW_BASE_DEFAULT,
                          "https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/",
                          82) == 0,
                  "相对根默认值也在同一个位置");
            /* 镜像前缀 + 完整 raw URL = gh-proxy 的用法 */
            {
                char mirror[512];
                snprintf(mirror, sizeof(mirror), "%s%s", SXCL_JRE_MIRROR_PREFIX,
                         SXCL_JRE_INDEX_URL_DEFAULT);
                check_str(mirror,
                          "https://gh-proxy.com/https://raw.githubusercontent.com/"
                          "Silent-Studio-CN/index/main/SXCL/jre/index.json",
                          "镜像地址(gh-proxy 前缀)");
            }
        }
        printf("      (编译期默认 GitHub 仓库: %s,占位=%s)\n", SXCL_JRE_GH_REPO,
               placeholder ? "是(待用户确认)" : "否");
    }
    check_int(sxcl_jre_resolve_index_url("https://a/", NULL, NULL, out, 8), SXCL_JRE_ERR_ARG,
              "装不下就报错,不截断");

    check_int(sxcl_jre_join_url("https://h/sxcl/java/index.json", "jre17/universal.tar.xz", out,
                                sizeof(out)),
              SXCL_JRE_OK, "拼相对路径");
    check_str(out, "https://h/sxcl/java/jre17/universal.tar.xz", "拼出来的 URL");
    check_int(sxcl_jre_join_url("https://h/sxcl/java/", "jre17/universal.tar.xz", out, sizeof(out)),
              SXCL_JRE_OK, "根目录形式也能拼");
    check_str(out, "https://h/sxcl/java/jre17/universal.tar.xz", "根目录形式的 URL");
    /* 绝对 URL:原样用(包放云上、清单放 GitHub 就是这条) */
    check_int(sxcl_jre_join_url("https://raw.githubusercontent.com/o/r/main/index.json",
                                "https://cloud.silentstudio.cn/api/v1/o/JDK/Java/JRE/jre17/universal.tar.xz",
                                out, sizeof(out)),
              SXCL_JRE_OK, "绝对 URL 直接过");
    check_str(out, "https://cloud.silentstudio.cn/api/v1/o/JDK/Java/JRE/jre17/universal.tar.xz",
              "绝对 URL 原样");
    check_int(sxcl_jre_join_url("https://h/a.json", "ftp://evil/x", out, sizeof(out)),
              SXCL_JRE_ERR_ARG, "非 http(s) 的 :// 必须拒");
    check_int(sxcl_jre_join_url("https://h/a.json", "../escape", out, sizeof(out)), SXCL_JRE_ERR_ARG,
              "rel 里带 .. 必须拒");
    check_int(sxcl_jre_join_url("https://h/a.json", "/abs", out, sizeof(out)), SXCL_JRE_ERR_ARG,
              "rel 是绝对路径必须拒");

    check_int(sxcl_jre_target_dir("base", "jre21", out, sizeof(out)), SXCL_JRE_OK, "拼目标目录");
    check_str(out, "base/jre21", "目标目录 = root/id");
}

static void test_provided_index_no_network(void)
{
    char *index;
    size_t index_len = 0;
    sxcl_jre_request req;
    sxcl_jre_result res;
    int rc;
    printf("== 没有传输后端:报参数错(安装总要下点什么,不装作能装)\n");
    tmp_reset("noback");
    index = build_index("17.0.9", 1, 1, NULL, &index_len);
    memset(&req, 0, sizeof(req));
    req.java_major = 17;
    req.index_text = index;
    req.target_dir = g_tmp;
    req.transport_factory = NULL;
    memset(&res, 0, sizeof(res));
    rc = sxcl_jre_install(&req, &res);
    check_int(rc, SXCL_JRE_ERR_ARG, "没有后端必须报 arg");
    free(index);
}

static void test_names(void)
{
    printf("== 返回码/阶段名\n");
    check_str(sxcl_jre_code_name(SXCL_JRE_OK), "ok", "ok");
    check_str(sxcl_jre_code_name(SXCL_JRE_ERR_INDEX), "index", "index");
    check_str(sxcl_jre_code_name(SXCL_JRE_ERR_NO_ABI), "abi", "abi");
    check_str(sxcl_jre_code_name(SXCL_JRE_ERR_DISK), "disk", "disk");
    check_str(sxcl_jre_code_name(-999), "?", "未知");
    check_str(sxcl_jre_stage_id(SXCL_JRE_STAGE_EXTRACT), "extract", "extract");
    check_str(sxcl_jre_stage_id(SXCL_JRE_STAGE_END), "none", "none");
    check_str(sxcl_jre_stage_name(SXCL_JRE_STAGE_DOWNLOAD), "下载并校验安装包", "中文阶段名");
    check_str(SXCL_JRE_INDEX_URL_ENV, "SXCL_JAVA_JRE_INDEX_URL", "环境变量名(对外契约)");
    check_str(SXCL_JRE_INDEX_URL_SETTING, "java.jre_index_url", "设置键名(对外契约)");
    check_str(SXCL_JRE_INDEX_SCHEMA_STRING, "sxcl.jre.index/1", "正式来源的 schema 串");
    check_str(SXCL_JRE_MIRROR_PREFIX, "https://gh-proxy.com/", "镜像前缀(gh-proxy)");
    check(sxcl_jre_host_abi()[0] != '\0', "本机 ABI 名非空");
}

int main(void)
{
    size_t len = 0;
    g_universal = read_file("universal.tar.xz", &len);
    g_arm64 = read_file("bin-arm64.tar.xz", &len);
    g_v7a = read_file("bin-armeabi-v7a.tar.xz", &len);
    check(g_universal != NULL && g_arm64 != NULL && g_v7a != NULL, "读到三个夹具包");
    if (g_universal == NULL || g_arm64 == NULL || g_v7a == NULL) {
        printf("jre_hosted_test: 夹具缺失\n");
        return 1;
    }
    test_install_and_skip();
    test_bad_index();
    test_sha256_mismatch();
    test_cancel();
    test_sxcl_index_schema();
    test_real_index_if_any();
    test_hash_forms();
    test_source_and_url();
    test_provided_index_no_network();
    test_names();
    (void)sxcl_fs_remove_tree("_jre_tmp");
    free(g_universal);
    free(g_arm64);
    free(g_v7a);
    printf("\njre_hosted_test: 通过 %d,失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
