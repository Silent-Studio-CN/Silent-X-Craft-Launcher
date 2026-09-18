/* auth_replay.c —— 登录对话框取证用的**夹具回放传输**(实现见 auth_replay.h)。
 *
 * 纪律:它**只在** SXCL_UI_AUTH_REPLAY=<夹具目录> 显式设了时被建出来,
 * 产品路径(不设变量)仍然走 sxcl_transport_qt_create() 打真网络。
 * 回放的路由与 tests/fixtures/auth/README.md 记的用例一一对应:
 *   设备码申请 → 200;轮询 → 400 authorization_pending ×2 → 200 成功;
 *   XBL → XSTS → login_with_xbox(可选 403)→ mcstore → profile。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* fopen/snprintf:MSVC 会标成"不安全",/WX 下会断构建 */
#endif

#include "auth_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_MAX_ROUTES 8
#define REPLAY_MAX_STEPS 6
#define REPLAY_PATH_MAX 900

typedef struct replay_step {
    int status;
    const char *fixture; /* NULL = 空正文 */
} replay_step;

typedef struct replay_route {
    const char *suffix; /* 与请求 URL 的**结尾**比较:租户段是可配的,不能按整串比 */
    replay_step steps[REPLAY_MAX_STEPS];
    size_t step_count;
    size_t cursor;
} replay_route;

typedef struct replay_body {
    char *data;
    size_t len;
    size_t pos;
} replay_body;

typedef struct replay_transport {
    sxcl_transport pub;
    char fixture_dir[REPLAY_PATH_MAX];
    replay_route routes[REPLAY_MAX_ROUTES];
    size_t route_count;
} replay_transport;

static int ends_with(const char *text, const char *suffix)
{
    if (text == NULL || suffix == NULL) {
        return 0;
    }
    const size_t lt = strlen(text);
    const size_t ls = strlen(suffix);
    if (ls > lt) {
        return 0;
    }
    return strcmp(text + (lt - ls), suffix) == 0;
}

/* 读一个夹具文件(整读进 malloc 缓冲,NUL 结尾);读不到返回 NULL(调用方报传输故障) */
static char *read_fixture(const replay_transport *rt, const char *name)
{
    char path[REPLAY_PATH_MAX * 2];
    (void)snprintf(path, sizeof(path), "%s/%s", rt->fixture_dir, name);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    if (size < 0 || size > 1024L * 1024L) {
        (void)fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        (void)fclose(fp);
        return NULL;
    }
    const size_t got = fread(buf, 1u, (size_t)size, fp);
    (void)fclose(fp);
    buf[got] = '\0';
    return buf;
}

static void route_add(replay_transport *rt, const char *suffix, const replay_step *steps, size_t count)
{
    if (rt->route_count >= REPLAY_MAX_ROUTES) {
        return;
    }
    replay_route *r = &rt->routes[rt->route_count++];
    memset(r, 0, sizeof(*r));
    r->suffix = suffix;
    for (size_t i = 0; i < count && i < REPLAY_MAX_STEPS; ++i) {
        r->steps[i] = steps[i];
    }
    r->step_count = (count < REPLAY_MAX_STEPS) ? count : REPLAY_MAX_STEPS;
}

static void route_add1(replay_transport *rt, const char *suffix, int status, const char *fixture)
{
    replay_step step;
    step.status = status;
    step.fixture = fixture;
    route_add(rt, suffix, &step, 1u);
}

static int replay_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                          sxcl_http_body **body)
{
    replay_transport *rt = (replay_transport *)ctx;
    *body = NULL;
    memset(resp, 0, sizeof(*resp));
    resp->content_length = -1;
    resp->total_length = -1;
    resp->range_start = -1;
    resp->range_end = -1;

    if (req->on_header != NULL) {
        req->on_header(req->header_userdata, "Content-Type", "application/json");
    }

    const char *url = (req->url != NULL) ? req->url : "";
    for (size_t i = 0; i < rt->route_count; ++i) {
        replay_route *r = &rt->routes[i];
        if (!ends_with(url, r->suffix)) {
            continue;
        }
        const size_t idx = (r->cursor < r->step_count) ? r->cursor : (r->step_count - 1u);
        if (r->cursor < r->step_count) {
            r->cursor++;
        }
        const replay_step *step = &r->steps[idx];
        resp->status = step->status;

        replay_body *rb = (replay_body *)calloc(1u, sizeof(*rb));
        if (rb == NULL) {
            return SXCL_NET_ERR_IO;
        }
        if (step->fixture != NULL) {
            rb->data = read_fixture(rt, step->fixture);
            if (rb->data == NULL) {
                free(rb);
                return SXCL_NET_ERR_IO; /* 夹具读不到:如实报故障,不偷偷给个 200 */
            }
            rb->len = strlen(rb->data);
        } else {
            rb->data = (char *)calloc(1u, 1u);
            if (rb->data == NULL) {
                free(rb);
                return SXCL_NET_ERR_IO;
            }
            rb->len = 0;
        }
        resp->content_length = (int64_t)rb->len;
        *body = (sxcl_http_body *)rb;
        return SXCL_NET_OK;
    }
    return SXCL_NET_ERR_CONNECT; /* 没配过的 URL:报"连不上",让取证时立刻发现漏配 */
}

static int64_t replay_read(void *ctx, sxcl_http_body *body, void *buf, size_t len)
{
    (void)ctx;
    replay_body *rb = (replay_body *)body;
    if (rb == NULL || buf == NULL) {
        return SXCL_NET_ERR_BAD_ARG;
    }
    const size_t left = rb->len - rb->pos;
    const size_t take = (left < len) ? left : len;
    if (take > 0) {
        memcpy(buf, rb->data + rb->pos, take);
        rb->pos += take;
    }
    return (int64_t)take;
}

static void replay_close_body(void *ctx, sxcl_http_body *body)
{
    (void)ctx;
    replay_body *rb = (replay_body *)body;
    if (rb == NULL) {
        return;
    }
    free(rb->data);
    free(rb);
}

static void replay_cancel_all(void *ctx) { (void)ctx; }

static void replay_destroy(void *ctx)
{
    replay_transport *rt = (replay_transport *)ctx;
    free(rt);
}

sxcl_transport *sxcl_ui_auth_replay_create(const char *fixture_dir, int fail_at_hop6)
{
    if (fixture_dir == NULL || fixture_dir[0] == '\0') {
        return NULL;
    }
    replay_transport *rt = (replay_transport *)calloc(1u, sizeof(*rt));
    if (rt == NULL) {
        return NULL;
    }
    (void)snprintf(rt->fixture_dir, sizeof(rt->fixture_dir), "%s", fixture_dir);

    /* 设备码申请(成功) */
    route_add1(rt, "/oauth2/v2.0/devicecode", 200, "ms_devicecode.json");
    /* 设备码轮询:两轮 authorization_pending 之后再成功(与 auth_flow_test 同序) */
    {
        const replay_step poll[] = {
            {400, "ms_devicecode_pending.json"},
            {400, "ms_devicecode_pending.json"},
            {200, "ms_token_success.json"},
        };
        route_add(rt, "/oauth2/v2.0/token", poll, sizeof(poll) / sizeof(poll[0]));
    }
    route_add1(rt, "/user/authenticate", 200, "xbl_user_auth_success.json");
    route_add1(rt, "/xsts/authorize", 200, "xsts_success.json");
    if (fail_at_hop6) {
        route_add1(rt, "/authentication/login_with_xbox", 403,
                   "mc_login_403_app_not_registered.json");
    } else {
        route_add1(rt, "/authentication/login_with_xbox", 200, "mc_login_success.json");
    }
    route_add1(rt, "/entitlements/mcstore", 200, "mcstore_ok.json");
    route_add1(rt, "/minecraft/profile", 200, "mc_profile_ok.json");

    rt->pub.ctx = rt;
    rt->pub.request = replay_request;
    rt->pub.read = replay_read;
    rt->pub.close_body = replay_close_body;
    rt->pub.cancel_all = replay_cancel_all;
    rt->pub.destroy = replay_destroy;
    return &rt->pub;
}
