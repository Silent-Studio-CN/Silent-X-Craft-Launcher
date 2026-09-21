/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
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
#include "sxcl/java_runtime.h"
#include "sxcl/verify.h"

static int g_pass = 0, g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got && want && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

/* 路径比较:忽略分隔符差异(核心库对外用 '/',而 sxcl_java_inspect 返回本机分隔符) */
static int same_path(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void check_path(const char *got, const char *want, const char *what)
{
    if (same_path(got, want)) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
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

/* ── 假传输(内存 HTTP) ──
 * 每个 worker 线程各要一个实例(引擎的约定),但这份假实现无状态:请求体上下文随 resp 走,
 * 全局只有"只读的夹具表"+ 测试用的两个计数器(计数是给断言用的,加个锁也不值当)。 */

static char g_all_json[8192];
static char g_manifest[8192];
static char g_payload_java[256];
static char g_payload_release[256];
static char g_payload_dll[256];
static int g_requests = 0;       /* 传输次数(含清单) */
static int g_payload_reads = 0;  /* 真下到文件的次数 */
static int g_cancel_after = -1;  /* >=0 时:下到第 N 个文件后用户取消 */

/* 引擎配置:单线程。假传输的计数器没有加锁,单线程才没有数据竞争,
 * 也让"取消发生在第 N 个文件之后"这种时序断言可复现。 */
static sxcl_engine_opts g_engine_opts;

/* 取消探针(放在前面声明:下面几个用例都要用) */
static int cancel_probe(void *ud);

typedef struct fake_body {
    const char *data;
    size_t len;
    size_t off;
} fake_body;

static const char *fixture_lookup(const char *url, size_t *len_out)
{
    if (!url) {
        return NULL;
    }
    if (strstr(url, "all.json")) {
        *len_out = strlen(g_all_json);
        return g_all_json;
    }
    if (strstr(url, "delta.json") || strstr(url, "gamma.json") || strstr(url, "legacy.json")) {
        *len_out = strlen(g_manifest);
        return g_manifest;
    }
    if (strstr(url, "bin/java")) {
        *len_out = strlen(g_payload_java);
        return g_payload_java;
    }
    if (strstr(url, "release")) {
        *len_out = strlen(g_payload_release);
        return g_payload_release;
    }
    if (strstr(url, "java.dll")) {
        *len_out = strlen(g_payload_dll);
        return g_payload_dll;
    }
    return NULL;
}

static int fake_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                        sxcl_http_body **body)
{
    (void)ctx;
    if (!req || !req->url || !*req->url) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    ++g_requests;
    size_t len = 0;
    const char *data = fixture_lookup(req->url, &len);
    memset(resp, 0, sizeof(*resp));
    if (!data) {
        resp->status = 404;
        resp->content_length = 0;
        *body = NULL;
        return SXCL_NET_OK;
    }
    ++g_payload_reads;
    fake_body *b = (fake_body *)calloc(1, sizeof(*b));
    if (!b) {
        return SXCL_NET_ERR_IO;
    }
    b->data = data;
    b->len = len;
    *body = (sxcl_http_body *)b;
    resp->status = 200;
    resp->content_length = (int64_t)len;
    resp->total_length = (int64_t)len;
    resp->range_start = -1;
    resp->range_end = -1;
    return SXCL_NET_OK;
}

static int64_t fake_read(void *ctx, sxcl_http_body *body, void *buf, size_t len)
{
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

static void fake_close(void *ctx, sxcl_http_body *body)
{
    (void)ctx;
    free(body);
}

static void fake_cancel(void *ctx) { (void)ctx; }
static void fake_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *fake_factory(void *ud)
{
    static sxcl_transport transport;
    (void)ud;
    memset(&transport, 0, sizeof(transport));
    transport.request = fake_request;
    transport.read = fake_read;
    transport.close_body = fake_close;
    transport.cancel_all = fake_cancel;
    transport.destroy = fake_destroy;
    return &transport;
}

/* 文件内容:假装是 java 可执行文件 / release / 一个共享库(用真实内容算 sha1,清单才对得上) */
static void build_payloads(void)
{
    snprintf(g_payload_java, sizeof(g_payload_java),
             "MZ fake java launcher binary; payload id 1; %s", "0123456789abcdef");
    snprintf(g_payload_release, sizeof(g_payload_release),
             "JAVA_VERSION=\"21.0.5\"\nOS_NAME=\"Windows\"\nOS_ARCH=\"x86_64\"\nIMPLEMENTOR=\"SXCL\"\n");
    snprintf(g_payload_dll, sizeof(g_payload_dll), "fake shared library payload 3");
}

static void sha1_of(const char *text, char *out, size_t out_len)
{
    (void)sxcl_hash_digest(SXCL_HASH_SHA1, text, strlen(text), out, out_len);
}

/* 组件清单:bin/java[.exe](executable)、lib/modules(directory,不该被下载)、release、
 * lib/java.dll;外加一条 type=link(不该被下载,也不该让安装失败)。 */
static void build_manifest(void)
{
    char sha_java[48], sha_release[48], sha_dll[48];
    sha1_of(g_payload_java, sha_java, sizeof(sha_java));
    sha1_of(g_payload_release, sha_release, sizeof(sha_release));
    sha1_of(g_payload_dll, sha_dll, sizeof(sha_dll));
#if defined(_WIN32)
    const char *java_rel = "bin/java.exe";
#else
    const char *java_rel = "bin/java";
#endif
    snprintf(g_manifest, sizeof(g_manifest),
             "{\n"
             "  \"files\": {\n"
             "    \"%s\": { \"type\": \"file\", \"executable\": true,\n"
             "      \"downloads\": { \"raw\": { \"url\": \"https://piston-data.mojang.com/v1/objects/aa/bin/java\","
             " \"sha1\": \"%s\", \"size\": %d } } },\n"
             "    \"lib/modules\": { \"type\": \"directory\" },\n"
             "    \"release\": { \"type\": \"file\", \"executable\": false,\n"
             "      \"downloads\": { \"raw\": { \"url\": \"https://piston-data.mojang.com/v1/objects/bb/release\","
             " \"sha1\": \"%s\", \"size\": %d } } },\n"
             "    \"lib/java.dll\": { \"type\": \"file\", \"executable\": false,\n"
             "      \"downloads\": { \"raw\": { \"url\": \"https://piston-data.mojang.com/v1/objects/cc/lib/java.dll\","
             " \"sha1\": \"%s\", \"size\": %d } } },\n"
             "    \"lib/link\": { \"type\": \"link\", \"target\": \"../lib/modules\" }\n"
             "  }, \"_version\": 1\n"
             "}\n",
             java_rel, sha_java, (int)strlen(g_payload_java), sha_release,
             (int)strlen(g_payload_release), sha_dll, (int)strlen(g_payload_dll));
}

/* all.json:本平台三个组件 + 两个必须被排除的 + 另一个平台(测平台选择) */
static void build_all_json(void)
{
    char sha_manifest[48];
    sha1_of(g_manifest, sha_manifest, sizeof(sha_manifest));
    const int size = (int)strlen(g_manifest);
    snprintf(g_all_json, sizeof(g_all_json),
             "{\n"
             "  \"windows-x64\": {\n"
             "    \"jre-legacy\": [ { \"manifest\": { \"url\": \"https://piston-meta.mojang.com/v1/legacy.json\","
             " \"sha1\": \"%s\", \"size\": %d }, \"version\": { \"name\": \"1.8.0_402\","
             " \"released\": \"2024-01-01\" } } ],\n"
             "    \"java-runtime-gamma\": [ { \"manifest\": { \"url\":"
             " \"https://piston-meta.mojang.com/v1/gamma.json\", \"sha1\": \"%s\", \"size\": %d },"
             " \"version\": { \"name\": \"17.0.8\" } } ],\n"
             "    \"java-runtime-delta\": [ { \"manifest\": { \"url\":"
             " \"https://piston-meta.mojang.com/v1/delta.json\", \"sha1\": \"%s\", \"size\": %d },"
             " \"version\": { \"name\": \"21.0.5\" } } ],\n"
             "    \"minecraft-java-exe\": [ { \"manifest\": { \"url\":"
             " \"https://piston-meta.mojang.com/v1/exe.json\", \"sha1\": \"%s\", \"size\": 1 },"
             " \"version\": { \"name\": \"1.0\" } } ],\n"
             "    \"java-runtime-gamma-snapshot\": [ { \"manifest\": { \"url\":"
             " \"https://piston-meta.mojang.com/v1/snap.json\", \"sha1\": \"%s\", \"size\": 1 },"
             " \"version\": { \"name\": \"17.0.9\" } } ]\n"
             "  },\n"
             "  \"linux\": {\n"
             "    \"java-runtime-delta\": [ { \"manifest\": { \"url\":"
             " \"https://piston-meta.mojang.com/v1/delta.json\", \"sha1\": \"%s\", \"size\": %d },"
             " \"version\": { \"name\": \"21.0.5\" } } ]\n"
             "  }\n"
             "}\n",
             sha_manifest, size, sha_manifest, size, sha_manifest, size, sha_manifest, sha_manifest,
             sha_manifest, size);
}

/* ── 进度回调:记录阶段顺序与百分比单调性 ── */
typedef struct progress_log {
    int stages[8];
    size_t stage_count;
    int last_percent;
    int monotonic;
    int max_percent;
    int messages_nonempty;
    size_t calls;
    char last_message[192];
} progress_log;

static void on_progress(void *ud, const sxcl_java_runtime_progress *p)
{
    progress_log *log = (progress_log *)ud;
    ++log->calls;
    if (log->stage_count == 0 || log->stages[log->stage_count - 1] != (int)p->stage) {
        if (log->stage_count < 8) {
            log->stages[log->stage_count++] = (int)p->stage;
        }
    }
    if (p->percent < log->last_percent) {
        log->monotonic = 0;
    }
    log->last_percent = p->percent;
    if (p->percent > log->max_percent) {
        log->max_percent = p->percent;
    }
    if (p->message[0] != '\0') {
        ++log->messages_nonempty;
    }
    snprintf(log->last_message, sizeof(log->last_message), "%s", p->message);
}

/* ── 用例 ── */

static void test_platform_and_choose(void)
{
    printf("-- 平台键 / MC 版本 -> Java 主版本 / 组件挑选\n");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_WINDOWS, "x64"), "windows-x64", "Win x64");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_WINDOWS, "arm64"), "windows-arm64",
              "Win arm64");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_WINDOWS, "x86"), "windows-x86",
              "Win x86(老 32 位)");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_MACOS, "arm64"), "mac-os-arm64",
              "macOS arm64");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_MACOS, "x64"), "mac-os", "macOS x64");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_LINUX, "x64"), "linux", "Linux x64");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_LINUX, "x86"), "linux-i386",
              "Linux i386");
    check_str(sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_ANDROID, "x64"), "linux",
              "Android 按 linux(与 rules 口径一致)");
    check(sxcl_java_runtime_platform_key() != NULL, "本机平台键非空");

    check_int(sxcl_java_runtime_required_major("1.16.5"), 8, "1.16.5 -> Java 8");
    check_int(sxcl_java_runtime_required_major("1.17"), 17, "1.17 -> Java 17");
    check_int(sxcl_java_runtime_required_major("1.20.4"), 17, "1.20.4 -> Java 17");
    check_int(sxcl_java_runtime_required_major("1.20.5"), 21, "1.20.5 -> Java 21");
    check_int(sxcl_java_runtime_required_major("1.21.4"), 21, "1.21.4 -> Java 21");
    check_int(sxcl_java_runtime_required_major("26.1"), 25, "26.1 -> Java 25");
    check_int(sxcl_java_runtime_required_major(""), 21, "空 -> 21");
    check_int(sxcl_java_runtime_required_major("abc"), 21, "认不出 -> 21");

    check_int((long)sxcl_java_runtime_preset_count(), 3, "三个组件候选(Python COMPONENT_PREVIEW)");
    check_str(sxcl_java_runtime_preset_at(0)->component, "jre-legacy", "候选 0 = jre-legacy");
    check_str(sxcl_java_runtime_preset_at(2)->component, "java-runtime-delta", "候选 2 = delta");

    char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
    sxcl_java_runtime_query q;
    memset(&q, 0, sizeof(q));
    q.all_json_text = g_all_json;
    q.platform = "windows-x64";

    sxcl_java_runtime_component list[8];
    const int n = sxcl_java_runtime_list(&q, list, 8, err, sizeof(err));
    check_int(n, 3, "windows-x64 上枚举出 3 个组件(排除 exe 与 gamma-snapshot)");
    int found_excluded = 0;
    for (int i = 0; i < (n > 0 ? n : 0); ++i) {
        if (strcmp(list[i].component, "minecraft-java-exe") == 0 ||
            strcmp(list[i].component, "java-runtime-gamma-snapshot") == 0) {
            found_excluded = 1;
        }
    }
    check_int(found_excluded, 0, "被排除的组件不在列表里");

    sxcl_json *doc = sxcl_java_runtime_fetch_all(&q, err, sizeof(err));
    check(doc != NULL, "all.json 夹具解析成功");
    if (doc) {
        sxcl_java_runtime_component chosen;
        check_int(sxcl_java_runtime_choose(doc, "windows-x64", 8, &chosen), 0, "required=8 挑得出");
        check_str(chosen.component, "jre-legacy", "required=8 -> jre-legacy");
        check_int(chosen.major, 8, "主版本从 version.name 解析出来");
        check(sxcl_java_runtime_choose(doc, "windows-x64", 17, &chosen) == 0, "required=17 挑得出");
        check_str(chosen.component, "java-runtime-gamma", "required=17 -> gamma(不是更高的 delta)");
        check(sxcl_java_runtime_choose(doc, "windows-x64", 21, &chosen) == 0, "required=21 挑得出");
        check_str(chosen.component, "java-runtime-delta", "required=21 -> delta");
        check(sxcl_java_runtime_choose(doc, "windows-x64", 25, &chosen) == 0, "required=25 也挑得出");
        check_str(chosen.component, "java-runtime-delta", "没有 25 的组件 -> 用能拿到的最新的");
        check_str(chosen.version, "21.0.5", "版本串带上");
        check_int(sxcl_java_runtime_choose(doc, "mac-os", 17, &chosen),
                  SXCL_JAVA_RUNTIME_ERR_MANIFEST, "没有这个平台 -> ERR_MANIFEST");
        sxcl_json_free(doc);
    }
}

static void test_install_ok(void)
{
    printf("-- 正常安装(夹具清单 + 内存假传输)\n");
    const char *root = "build/_jr_root_ok";
    (void)sxcl_fs_remove_tree(root);
    (void)sxcl_fs_mkdirs(root);

    sxcl_java_runtime_request req;
    memset(&req, 0, sizeof(req));
    req.component = "java-runtime-delta";
    req.target_root = root;
    req.all_json_text = g_all_json;
    req.manifest_text = g_manifest;
    req.transport_factory = fake_factory;
    req.engine_opts = &g_engine_opts;
    req.use_mirror = 1; /* 假传输按路径匹配,镜像候选也能命中 */
    sxcl_java_runtime_result res;
    progress_log log;
    memset(&log, 0, sizeof(log));
    log.monotonic = 1;
    req.on_progress = on_progress;
    req.ud = &log;

    g_requests = 0;
    g_payload_reads = 0;

    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_OK, "安装成功");
    check_str(res.component, "java-runtime-delta", "组件名回填");
    check_str(res.platform, sxcl_java_runtime_platform_key(), "平台键回填");
    check_str(res.version, "21.0.5", "版本回填");
    check_int((long)res.files_total, 3, "3 个文件(directory/link 不算)");
    check_int((long)res.files_done, 3, "3 个都完成");
    check_int((long)res.files_skipped, 0, "首次没有跳过");
    check_int((long)res.files_failed, 0, "没有失败");
    check(res.bytes_total > 0, "总字节数已知");
    check_int((long)res.bytes_done, (long)res.bytes_total, "字节数对得上");

    char expect_home[SXCL_JAVA_RUNTIME_PATH_MAX];
    snprintf(expect_home, sizeof(expect_home), "%s/java-runtime-delta-%s", root,
             sxcl_java_runtime_platform_key());
    check_str(res.java_home, expect_home, "装到 <root>/<组件>-<平台>");
    check(sxcl_fs_exists(res.java_path) == 1, "<home>/bin/java[.exe] 真的在");
    check(sxcl_fs_is_dir(res.java_home) == 1, "java_home 是目录");

    char lib_path[SXCL_JAVA_RUNTIME_PATH_MAX];
    snprintf(lib_path, sizeof(lib_path), "%s/lib/modules", res.java_home);
    check(sxcl_fs_is_dir(lib_path) == 1, "清单里的 directory 条目建出来了");
    snprintf(lib_path, sizeof(lib_path), "%s/lib/java.dll", res.java_home);
    check(sxcl_fs_exists(lib_path) == 1, "嵌套子目录里的文件也落了盘");
    snprintf(lib_path, sizeof(lib_path), "%s/lib/link", res.java_home);
    check(sxcl_fs_exists(lib_path) == 0, "type=link 的条目不会被当成文件下载");

    check(res.marker_path[0] != '\0', "写了标记文件");
    check(sxcl_fs_exists(res.marker_path) == 1, "标记文件真的在");
    char marker[2048];
    FILE *fh = fopen(res.marker_path, "rb");
    check(fh != NULL, "能读标记文件");
    if (fh) {
        const size_t got = fread(marker, 1, sizeof(marker) - 1, fh);
        marker[got] = '\0';
        fclose(fh);
        check(strstr(marker, "\"component\": \"java-runtime-delta\"") != NULL, "标记里有组件名");
        check(strstr(marker, "\"version\": \"21.0.5\"") != NULL, "标记里有版本");
        check(strstr(marker, "java_home") != NULL, "标记里有 java_home(Python 版认这个文件)");
    }

    /* 进度:阶段顺序、单调、收尾 100、文案非空 */
    check(log.stage_count >= 4, "四个阶段都报过");
    check_int(log.stages[0], SXCL_JAVA_RUNTIME_STAGE_QUERY, "第一个阶段是 query");
    check_int(log.stages[log.stage_count - 1], SXCL_JAVA_RUNTIME_STAGE_FINISH, "最后一个阶段是 finish");
    check_int(log.monotonic, 1, "百分比单调不减");
    check_int(log.max_percent, 100, "结束时 100%");
    check(log.messages_nonempty == (int)log.calls, "每次回调都有人话文案");
    check_str(sxcl_java_runtime_stage_id(SXCL_JAVA_RUNTIME_STAGE_END), "none", "END 的阶段名是 none");
    check_str(sxcl_java_runtime_code_name(SXCL_JAVA_RUNTIME_ERR_DOWNLOAD), "download", "码名稳定");

    printf("     进度回调 %d 次,最后一条: %s\n", (int)log.calls, log.last_message);

    /* find:按 Python find_installed_runtimes 的口径扫出来 */
    sxcl_java_info found[8];
    const size_t n = sxcl_java_runtime_find(root, found, 8);
    check_int((long)n, 1, "find 扫到 1 份已装运行时");
    if (n >= 1) {
        check_path(found[0].path, res.java_path, "扫出来的就是刚装的那个 java");
        check_str(found[0].source, "Runtime", "来源标记 = Runtime");
    }
    check_int(sxcl_java_runtime_is_installed(res.java_home), 1, "is_installed = 1");
    check_int(sxcl_java_runtime_is_installed("build/_jr_root_ok/根本没有这个目录"), 0,
              "不存在的目录 = 0");
}

static void test_skip_existing(void)
{
    printf("-- 已存在跳过(重复安装一个字节都不下)\n");
    const char *root = "build/_jr_root_ok"; /* 上一节刚装好的那份 */
    sxcl_java_runtime_request req;
    memset(&req, 0, sizeof(req));
    req.component = "java-runtime-delta";
    req.target_root = root;
    req.all_json_text = g_all_json;
    req.manifest_text = g_manifest;
    req.transport_factory = fake_factory;
    req.engine_opts = &g_engine_opts;

    sxcl_java_runtime_result res;
    g_requests = 0;
    g_payload_reads = 0;
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_OK, "第二次安装也成功");
    check_int((long)res.files_skipped, 3, "3 个文件全部跳过(已存在且校验通过)");
    check_int((long)res.files_done, 3, "统计里仍算完成");
    check_int((long)res.files_failed, 0, "没有失败");
    check_int(g_payload_reads, 0, "一次传输都没发(复用既有引擎的快路径语义)");

    /* 覆盖安装:force 的等价物 = 直接把文件改坏,下一次会重下 */
    char victim[SXCL_JAVA_RUNTIME_PATH_MAX];
    snprintf(victim, sizeof(victim), "%s/release", res.java_home);
    FILE *fh = fopen(victim, "wb");
    check(fh != NULL, "把 release 改坏(模拟被改过的文件)");
    if (fh) {
        fputs("corrupted", fh);
        fclose(fh);
    }
    g_requests = 0;
    g_payload_reads = 0;
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_OK, "第三次安装修好了它");
    check_int((long)res.files_skipped, 2, "只有 2 个跳过");
    check_int(g_payload_reads, 1, "坏掉的那个被重下(校验不过就重下,不将就)");
}

static void test_hash_mismatch(void)
{
    printf("-- 校验失败(服务端内容被改过)\n");
    const char *root = "build/_jr_root_bad";
    (void)sxcl_fs_remove_tree(root);
    (void)sxcl_fs_mkdirs(root);

    sxcl_java_runtime_request req;
    memset(&req, 0, sizeof(req));
    req.component = "java-runtime-delta";
    req.target_dir = "build/_jr_root_bad/target"; /* 直接指定目录,不走 <root>/<组件>-<平台> */
    req.all_json_text = g_all_json;
    req.manifest_text = g_manifest;
    req.transport_factory = fake_factory;
    req.engine_opts = &g_engine_opts;

    /* 让假传输服务端把 java 文件的内容换掉:清单里的 sha1 必然对不上 */
    char save[SXCL_JAVA_RUNTIME_PATH_MAX];
    snprintf(save, sizeof(save), "%s", g_payload_java);
    snprintf(g_payload_java, sizeof(g_payload_java), "TAMPERED-CONTENT-NOT-THE-REAL-FILE");
    sxcl_java_runtime_result res;
    const int rc = sxcl_java_runtime_install(&req, &res);
    snprintf(g_payload_java, sizeof(g_payload_java), "%s", save);

    check_int(rc, SXCL_JAVA_RUNTIME_ERR_DOWNLOAD, "校验失败 = ERR_DOWNLOAD");
    check_int(res.code, SXCL_JAVA_RUNTIME_ERR_DOWNLOAD, "结果码一致");
    check_int((int)res.fail_stage, (int)SXCL_JAVA_RUNTIME_STAGE_DOWNLOAD, "失败阶段 = download");
    check_str(res.fail_stage_id, "download", "失败阶段名");
    check(res.files_failed > 0, "至少一个文件失败");
    check(res.error[0] != '\0', "有人话原因");
    check(sxcl_fs_exists("build/_jr_root_bad/target/bin/java") == 0 &&
              sxcl_fs_exists("build/_jr_root_bad/target/bin/java.exe") == 0,
          "坏文件没被留在 bin/ 里(引擎查完校验才改名)");
}

static void test_manifest_sha1(void)
{
    printf("-- 组件清单 SHA-1 不符\n");
    /* 造一份 manifest.sha1 是错的 all.json:清单本身必须过校验,不能"不看 sha1 硬上" */
    char bad_all[8192];
    snprintf(bad_all, sizeof(bad_all), "%s", g_all_json);
    char *pos = strstr(bad_all, "\"sha1\": \"");
    check(pos != NULL, "夹具里有 sha1 字段");
    if (pos) {
        /* 把第一个 sha1 的第一位数字改掉(位置固定,不会破坏 JSON 结构) */
        char *digit = pos + strlen("\"sha1\": \"");
        *digit = (*digit == '0') ? '1' : '0';
    }

    char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
    sxcl_java_runtime_request req;
    memset(&req, 0, sizeof(req));
    req.component = "jre-legacy"; /* 第一个组件 = 被改坏的那条 */
    req.target_dir = "build/_jr_root_sha/target";
    req.all_json_text = bad_all;
    req.transport_factory = fake_factory; /* manifest_text 为空 -> 真去"下载",并校验 sha1 */
    req.engine_opts = &g_engine_opts;
    req.use_mirror = 1;
    sxcl_java_runtime_result res;
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_ERR_MANIFEST,
              "清单 SHA-1 不符 = ERR_MANIFEST");
    check_int((int)res.fail_stage, (int)SXCL_JAVA_RUNTIME_STAGE_MANIFEST, "失败阶段 = manifest");
    check(res.error[0] != '\0', "有人话原因");
    (void)err;
}

static void test_cancel(void)
{
    printf("-- 取消\n");
    const char *root = "build/_jr_root_cancel";
    (void)sxcl_fs_remove_tree(root);
    (void)sxcl_fs_mkdirs(root);

    sxcl_java_runtime_request req;
    memset(&req, 0, sizeof(req));
    req.component = "java-runtime-delta";
    req.target_dir = "build/_jr_root_cancel/target";
    req.all_json_text = g_all_json;
    req.manifest_text = g_manifest;
    req.transport_factory = fake_factory;
    req.engine_opts = &g_engine_opts; /* workers = 1:"第 1 个文件之后取消"才可复现 */

    /* 先跑一遍不取消的:确认这份夹具本来能装成功(否则"取消成功"没有说服力) */
    g_payload_reads = 0;
    g_cancel_after = -1;
    sxcl_java_runtime_result res;
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_OK, "没有取消回调时正常装完");

    /* 现在真的取消:下了第 1 个文件之后 is_cancelled 返回 1 */
    (void)sxcl_fs_remove_tree("build/_jr_root_cancel/target");
    g_payload_reads = 0;
    g_cancel_after = 1;
    req.is_cancelled = cancel_probe;
    const int rc = sxcl_java_runtime_install(&req, &res);
    check_int(rc, SXCL_JAVA_RUNTIME_ERR_CANCELLED, "取消 = ERR_CANCELLED(与失败可区分)");
    check_int(res.cancelled, 1, "结果里带 cancelled 标记");
    check_int((int)res.fail_stage, (int)SXCL_JAVA_RUNTIME_STAGE_DOWNLOAD, "取消发生在下载阶段");
    check_str(res.error, "取消", "人话原因 = 取消(lang 的 page.progress.cancel)");
    check_int((long)res.files_failed, 0, "取消不算失败文件数");
    g_cancel_after = -1;
}

/* 取消探针:g_cancel_after 个文件下完之后返回 1 */
static int cancel_probe(void *ud)
{
    (void)ud;
    if (g_cancel_after < 0) {
        return 0;
    }
    return g_payload_reads >= g_cancel_after ? 1 : 0;
}

static void test_default_root_and_errors(void)
{
    printf("-- runtime 根目录 / 参数校验\n");
    char out[SXCL_JAVA_RUNTIME_PATH_MAX];
    char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
#if defined(_WIN32)
    _putenv_s("SXCL_RUNTIME_DIR", "D:/runtimes");
#else
    setenv("SXCL_RUNTIME_DIR", "/tmp/runtimes", 1);
#endif
    check_int(sxcl_java_runtime_default_root(out, sizeof(out), err, sizeof(err)),
              SXCL_JAVA_RUNTIME_OK, "SXCL_RUNTIME_DIR 覆盖成功");
#if defined(_WIN32)
    check_str(out, "D:/runtimes", "取的是环境变量的值");
    _putenv_s("SXCL_RUNTIME_DIR", "");
#else
    check_str(out, "/tmp/runtimes", "取的是环境变量的值");
    unsetenv("SXCL_RUNTIME_DIR");
#endif
    check_int(sxcl_java_runtime_default_root(out, sizeof(out), err, sizeof(err)),
              SXCL_JAVA_RUNTIME_OK, "没有覆盖时也能拼出来");
    check(strstr(out, "runtime") != NULL, "末尾是 runtime");
    check(strstr(out, "SilentXCraftLauncher") != NULL || strstr(out, "silentxcraftlauncher") != NULL,
          "在配置目录下面(与设置文件同源)");

    /* 参数校验:一个必填都没有 */
    sxcl_java_runtime_result res;
    check_int(sxcl_java_runtime_install(NULL, &res), SXCL_JAVA_RUNTIME_ERR_ARG, "req 为空 = ERR_ARG");
    sxcl_java_runtime_request req;
    memset(&req, 0, sizeof(req));
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_ERR_ARG,
              "既没有清单也没有传输后端 = ERR_ARG(不会假装成功)");
    check_int(res.code, SXCL_JAVA_RUNTIME_ERR_ARG, "结果码一致");

    /* 组件不存在:明确报错,不"顺手换一个" */
    req.all_json_text = g_all_json;
    req.all_json_url = NULL;
    req.component = "java-runtime-不存在";
    req.target_dir = "build/_jr_root_arg/target";
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_ERR_MANIFEST,
              "点名要一个不存在的组件 = ERR_MANIFEST");
    check(strstr(res.error, "java-runtime-不存在") != NULL, "错误里点名是哪个组件");

    /* 清单文本坏了:解析失败也要有人话原因 */
    req.component = "java-runtime-delta";
    req.manifest_text = "{ 这不是 JSON";
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_ERR_MANIFEST,
              "清单解析失败 = ERR_MANIFEST");

    /* 没有传输后端又没给文本:必须失败,不能假装成功 */
    req.manifest_text = NULL;
    req.all_json_text = NULL;
    req.transport_factory = NULL;
    check_int(sxcl_java_runtime_install(&req, &res), SXCL_JAVA_RUNTIME_ERR_ARG,
              "清单来源与传输后端都没有 = ERR_ARG(压根不该开始)");

    sxcl_java_info info;
    check_int((long)sxcl_java_runtime_find(NULL, &info, 1), 0, "find(NULL) = 0");
    check_int((long)sxcl_java_runtime_find("build/根本没有这个目录", &info, 1), 0,
              "目录不存在 = 0 条(不是错误)");
}

int main(void)
{
    memset(&g_engine_opts, 0, sizeof(g_engine_opts));
    g_engine_opts.workers = 1;
    g_engine_opts.max_conn_per_file = 1;
    g_engine_opts.transport_factory = fake_factory;

    build_payloads();
    build_manifest();
    build_all_json();

    test_platform_and_choose();
    test_install_ok();
    test_skip_existing();
    test_hash_mismatch();
    test_manifest_sha1();
    test_cancel();
    test_default_root_and_errors();

    printf("java_runtime 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
