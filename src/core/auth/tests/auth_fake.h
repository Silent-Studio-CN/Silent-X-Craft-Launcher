/* 测试公用件:最小断言 + 夹具装载 + **假传输后端**(不联网也能把整条链跑完)。
 *
 * 假传输做的事很少,但正好够验登录链:
 *   - 按 URL 匹配"路由",把 tests/fixtures/auth/*.json 当响应正文返回(状态码在路由里写死);
 *   - 一条路由可以配**一串**响应(设备码轮询:pending → pending → slow_down → 成功);
 *   - 把每次请求的 method/url/body/Content-Type/Authorization 记下来,测试就能断言
 *     "RpsTicket 是不是 d=<token>""identityToken 是不是 XBL3.0 x=uhs;xsts";
 *   - 请求打到没配过的 URL 时**返回连接失败**,而不是偷偷给个 200 —— 免得测试自己骗自己。
 *
 * 这个头文件被多个测试 include,所有函数都是 static,不会重复符号。
 */
#ifndef SXCL_AUTH_TEST_FAKE_H
#define SXCL_AUTH_TEST_FAKE_H

/* 测试里要用 fopen/sscanf/snprintf:MSVC 会把它们标成"不安全"并在 /WX 下打断构建 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/auth.h"
#include "sxcl/auth_store.h"
#include "sxcl/net.h"

/* ── 最小断言 ── */
static int g_t_pass = 0;
static int g_t_fail = 0;

static void t_check(int ok, const char *what)
{
    if (ok) {
        ++g_t_pass;
    } else {
        ++g_t_fail;
        printf("  [!!] %s\n", what);
    }
}

static void t_check_str(const char *got, const char *want, const char *what)
{
    const int ok = (got != NULL && want != NULL && strcmp(got, want) == 0);
    if (ok) {
        ++g_t_pass;
    } else {
        ++g_t_fail;
        printf("  [!!] %s\n       期望: <%s>\n       实际: <%s>\n", what, want ? want : "(null)",
               got ? got : "(null)");
    }
}

static void t_check_contains(const char *haystack, const char *needle, const char *what)
{
    const int ok = (haystack != NULL && needle != NULL && strstr(haystack, needle) != NULL);
    if (ok) {
        ++g_t_pass;
    } else {
        ++g_t_fail;
        printf("  [!!] %s\n       要找: <%s>\n       在:   <%s>\n", what, needle ? needle : "(null)",
               haystack ? haystack : "(null)");
    }
}

static int t_report(const char *name)
{
    printf("%s: %d 通过, %d 失败\n", name, g_t_pass, g_t_fail);
    return g_t_fail == 0 ? 0 : 1;
}

/* ── 夹具目录:优先环境变量(ctest 会设),否则按源码树相对位置找 ── */
static const char *t_fixture_dir(void)
{
    const char *dir = getenv("SXCL_AUTH_FIXTURES");
    if (dir != NULL && dir[0] != '\0') {
        return dir;
    }
    return "tests/fixtures/auth";
}

/* 读一个夹具文件(整个读进 malloc 的缓冲,NUL 结尾);失败返回 NULL。 */
static char *t_read_fixture(const char *name)
{
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", t_fixture_dir(), name);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  [!!] 打不开夹具: %s\n", path);
        ++g_t_fail;
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    if (size < 0 || size > 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

/* ── 假传输 ── */
#define FAKE_MAX_STEPS 8
#define FAKE_MAX_ROUTES 12
#define FAKE_MAX_LOG 24

typedef struct fake_step {
    int status;
    const char *fixture;  /* NULL = 空正文 */
    int reject_after;     /* 占位:目前不用 */
} fake_step;

typedef struct fake_route {
    const char *url;
    fake_step steps[FAKE_MAX_STEPS];
    size_t step_count;
    size_t cursor;
} fake_route;

typedef struct fake_log {
    char method[16];
    char url[640];
    char body[4096];
    char content_type[128];
    char authorization[640];
    int status_returned;
} fake_log;

typedef struct fake_transport {
    sxcl_transport pub;
    fake_route routes[FAKE_MAX_ROUTES];
    size_t route_count;
    fake_log log[FAKE_MAX_LOG];
    size_t log_count;
} fake_transport;

typedef struct fake_body {
    char *data;
    size_t len;
    size_t pos;
} fake_body;

static fake_transport *fake_from_ctx(void *ctx)
{
    return (fake_transport *)ctx;
}

static int fake_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                        sxcl_http_body **body)
{
    fake_transport *ft = fake_from_ctx(ctx);
    *body = NULL;
    memset(resp, 0, sizeof(*resp));
    resp->content_length = -1;
    resp->total_length = -1;
    resp->range_start = -1;
    resp->range_end = -1;

    fake_log *entry = NULL;
    if (ft->log_count < FAKE_MAX_LOG) {
        entry = &ft->log[ft->log_count++];
        memset(entry, 0, sizeof(*entry));
        (void)snprintf(entry->method, sizeof(entry->method), "%s",
                       (req->method != NULL) ? req->method : "GET");
        (void)snprintf(entry->url, sizeof(entry->url), "%s", req->url != NULL ? req->url : "");
        if (req->body != NULL && req->body_len > 0) {
            const size_t n = (req->body_len < sizeof(entry->body) - 1u) ? req->body_len
                                                                        : sizeof(entry->body) - 1u;
            memcpy(entry->body, req->body, n);
            entry->body[n] = '\0';
        }
        if (req->extra_headers != NULL) {
            for (const char *const *h = req->extra_headers; *h != NULL; ++h) {
                if (strncmp(*h, "Content-Type:", 13) == 0) {
                    (void)snprintf(entry->content_type, sizeof(entry->content_type), "%s", *h + 13);
                } else if (strncmp(*h, "Authorization:", 14) == 0) {
                    (void)snprintf(entry->authorization, sizeof(entry->authorization), "%s", *h + 14);
                }
            }
        }
    }
    if (req->on_header != NULL) {
        req->on_header(req->header_userdata, "Content-Type", "application/json");
    }

    for (size_t i = 0; i < ft->route_count; ++i) {
        fake_route *r = &ft->routes[i];
        if (strcmp(r->url, req->url) != 0) {
            continue;
        }
        const size_t idx = (r->cursor < r->step_count) ? r->cursor : (r->step_count - 1u);
        if (r->cursor < r->step_count) {
            r->cursor++;
        }
        const fake_step *step = &r->steps[idx];
        resp->status = step->status;
        if (entry != NULL) {
            entry->status_returned = step->status;
        }
        fake_body *fb = (fake_body *)calloc(1, sizeof(*fb));
        if (fb == NULL) {
            return SXCL_NET_ERR_IO;
        }
        if (step->fixture != NULL) {
            fb->data = t_read_fixture(step->fixture);
            if (fb->data == NULL) {
                free(fb);
                return SXCL_NET_ERR_IO;
            }
            fb->len = strlen(fb->data);
        } else {
            fb->data = (char *)calloc(1, 1);
            fb->len = 0;
        }
        resp->content_length = (int64_t)fb->len;
        *body = (sxcl_http_body *)fb;
        return SXCL_NET_OK;
    }

    /* 没配过的 URL:如实报"连不上",让测试立刻发现漏配(而不是拿到一个假的 200) */
    printf("  [!!] 假传输收到未配置的请求: %s\n", req->url != NULL ? req->url : "(null)");
    ++g_t_fail;
    return SXCL_NET_ERR_CONNECT;
}

static int64_t fake_read(void *ctx, sxcl_http_body *body, void *buf, size_t len)
{
    (void)ctx;
    fake_body *fb = (fake_body *)body;
    if (fb == NULL || buf == NULL) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    const size_t left = fb->len - fb->pos;
    const size_t take = (left < len) ? left : len;
    if (take > 0) {
        memcpy(buf, fb->data + fb->pos, take);
        fb->pos += take;
    }
    return (int64_t)take;
}

static void fake_close_body(void *ctx, sxcl_http_body *body)
{
    (void)ctx;
    fake_body *fb = (fake_body *)body;
    if (fb == NULL) {
        return;
    }
    free(fb->data);
    free(fb);
}

static void fake_cancel_all(void *ctx) { (void)ctx; }
static void fake_destroy(void *ctx) { (void)ctx; }

static fake_transport *fake_create(void)
{
    fake_transport *ft = (fake_transport *)calloc(1, sizeof(*ft));
    if (ft == NULL) {
        return NULL;
    }
    ft->pub.ctx = ft;
    ft->pub.request = fake_request;
    ft->pub.read = fake_read;
    ft->pub.close_body = fake_close_body;
    ft->pub.cancel_all = fake_cancel_all;
    ft->pub.destroy = fake_destroy;
    return ft;
}

static void fake_free(fake_transport *ft) { free(ft); }

/* 配一条"单步"路由 */
static void fake_route1(fake_transport *ft, const char *url, int status, const char *fixture)
{
    fake_route *r = &ft->routes[ft->route_count++];
    memset(r, 0, sizeof(*r));
    r->url = url;
    r->steps[0].status = status;
    r->steps[0].fixture = fixture;
    r->step_count = 1;
}

/* 配一条"多步"路由(依次返回;用完后一直是最后一步) */
static void fake_route_seq(fake_transport *ft, const char *url, const fake_step *steps, size_t n)
{
    fake_route *r = &ft->routes[ft->route_count++];
    memset(r, 0, sizeof(*r));
    r->url = url;
    for (size_t i = 0; i < n && i < FAKE_MAX_STEPS; ++i) {
        r->steps[i] = steps[i];
    }
    r->step_count = (n < FAKE_MAX_STEPS) ? n : FAKE_MAX_STEPS;
}

/* 找"打到某个 URL 的第 n 条日志"(n 从 0 开始);找不到返回 NULL */
static const fake_log *fake_find(const fake_transport *ft, const char *url, size_t nth)
{
    size_t seen = 0;
    for (size_t i = 0; i < ft->log_count; ++i) {
        if (strcmp(ft->log[i].url, url) == 0) {
            if (seen == nth) {
                return &ft->log[i];
            }
            ++seen;
        }
    }
    return NULL;
}

/* 把所有端点都配上"标准成功链",供整链用例复用 */
static void fake_setup_java_chain(fake_transport *ft)
{
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_LOGIN, 200, "mc_login_success.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_ENTITLEMENTS, 200, "mcstore_ok.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_PROFILE, 200, "mc_profile_ok.json");
}

/* 夹具目录下建一个临时目录(存储测试用);返回 0 成功 */
static int t_make_tmpdir(const char *name, char *out, size_t out_len)
{
    char base[512];
    const char *env = getenv("SXCL_AUTH_FIXTURES");
    if (env != NULL && env[0] != '\0') {
        (void)snprintf(base, sizeof(base), "%s", env);
        char *slash = strrchr(base, '/');
        char *bslash = strrchr(base, '\\');
        if (bslash != NULL && (slash == NULL || bslash > slash)) {
            slash = bslash;
        }
        if (slash != NULL) {
            *slash = '\0';
        }
    } else {
        (void)snprintf(base, sizeof(base), ".");
    }
    (void)snprintf(out, out_len, "%s/_auth_tmp/%s", base, name);
    return 0;
}

#endif /* SXCL_AUTH_TEST_FAKE_H */
