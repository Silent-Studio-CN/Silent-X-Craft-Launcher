/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "sxcl/auth.h"
#include "auth_internal.h"

#include "auth_fake.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET t_sock_t;
#  define T_BAD_SOCK INVALID_SOCKET
#  define t_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int t_sock_t;
#  define T_BAD_SOCK (-1)
#  define t_close close
#endif

/* ASCII 大小写不敏感查找(HTTP 头名字大小写不固定) */
static int contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL) {
        return 0;
    }
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != 0; ++p) {
        size_t i = 0;
        while (i < nlen) {
            char a = p[i];
            char b = needle[i];
            if (a == 0) {
                return 0;
            }
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) {
                break;
            }
            ++i;
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

static void t_net_boot(void)
{
#if defined(_WIN32)
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        (void)WSAStartup(MAKEWORD(2, 2), &wsa);
        done = 1;
    }
#endif
}

/* ── 测试自己的小客户端 ──
 * 注意顺序:**先把请求发出去,再调 wait()**,最后才读响应。
 * 反过来(发完就 recv)会死锁 —— 服务端要等 wait() 才会 accept/回包,
 * 而单线程里 recv 会把整条测试挂死(第一版就是这么挂的,记在这里免得再犯)。
 * 连接会先排在内核的 accept 队列里,不需要额外开线程。 */
static t_sock_t t_connect_and_send(int port, const char *target)
{
    t_net_boot();
    const t_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == T_BAD_SOCK) {
        return T_BAD_SOCK;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    if (connect(s, (const struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
        t_close(s);
        return T_BAD_SOCK;
    }
    char req[1024];
    const int n = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\nHost: localhost:%d\r\n"
                           "User-Agent: sxcl-test-client\r\nAccept: */*\r\n\r\n",
                           target, port);
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        t_close(s);
        return T_BAD_SOCK;
    }
    const int sent = (int)send(s, req, n, 0);
    if (sent != n) {
        t_close(s);
        return T_BAD_SOCK;
    }
    return s;
}

/* 发一次就关(不需要看服务端回话的场合);成功返回 0 */
static int t_send_request(int port, const char *target)
{
    const t_sock_t s = t_connect_and_send(port, target);
    if (s == T_BAD_SOCK) {
        return -1;
    }
    t_close(s);
    return 0;
}

/* 读服务端回给浏览器的页面(wait() 之后调用);返回读到的字节数,-1 表示错 */
static int t_read_response(t_sock_t s, char *buf, size_t cap)
{
    if (s == T_BAD_SOCK || buf == NULL || cap == 0) {
        return -1;
    }
    const int n = (int)recv(s, buf, (int)(cap - 1u), 0);
    if (n <= 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[n] = '\0';
    return n;
}

static void test_start_and_redirect_uri(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    t_check(sxcl_auth_loopback_start(&lb, err, sizeof(err)) == SXCL_AUTH_OK, "环回服务器起得来");
    if (lb == NULL) {
        printf("       起不来的原因: %s\n", err);
        return;
    }
    const int port = sxcl_auth_loopback_port(lb);
    t_check(port > 0, "拿到一个端口");
    t_check(port >= 1024, "端口在动态端口区间(不是硬编码的固定端口)");
    const char *uri = sxcl_auth_loopback_redirect_uri(lb);
    char want[128];
    (void)snprintf(want, sizeof(want), "http://localhost:%d/callback", port);
    t_check_str(uri, want, "redirect_uri = http://localhost:<端口>/callback");
    printf("  (本次环回端口 = %d)\n", port);
    sxcl_auth_loopback_destroy(lb);

    /* 再起一次:端口不该撞(系统分配) */
    sxcl_auth_loopback *lb2 = NULL;
    t_check(sxcl_auth_loopback_start(&lb2, err, sizeof(err)) == SXCL_AUTH_OK, "第二次也起得来");
    if (lb2 != NULL) {
        printf("  (第二次环回端口 = %d)\n", sxcl_auth_loopback_port(lb2));
        t_check(sxcl_auth_loopback_port(lb2) >= 1024, "第二次端口也在动态区间");
        sxcl_auth_loopback_destroy(lb2);
    }
}

static void test_success_callback(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(成功回调用例)");
        return;
    }
    const int port = sxcl_auth_loopback_port(lb);
    /* 真实浏览器会这么打过来:先把请求发出去(进 accept 队列),再等 */
    const t_sock_t client = t_connect_and_send(port, "/callback?code=THE-AUTH-CODE-123&state=STATE-OK");
    t_check(client != T_BAD_SOCK, "测试客户端把回调请求发出去了");

    char code[SXCL_AUTH_TOKEN_MAX];
    char state[64];
    char got_err[64];
    code[0] = state[0] = got_err[0] = '\0';
    const int rc = sxcl_auth_loopback_wait(lb, "STATE-OK", code, sizeof(code), state, sizeof(state),
                                           got_err, sizeof(got_err), 5000, NULL, NULL, err,
                                           sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "等到回调并返回成功");
    t_check_str(code, "THE-AUTH-CODE-123", "从 query 里取到授权码");
    t_check_str(state, "STATE-OK", "取到 state");
    /* 浏览器看到的那个页面也要对:否则用户会以为登录没成功 */
    char page[2048];
    t_check(t_read_response(client, page, sizeof(page)) > 0, "服务端回了 HTTP 响应给浏览器");
    t_check_contains(page, "HTTP/1.1 200", "回的是 200");
    t_check_contains(page, "text/html; charset=utf-8", "声明了 UTF-8(中文页面不会乱码)");
    t_check_contains(page, "登录成功", "页面上写着“登录成功，可以关闭此页面”");
    t_check(contains_ci(page, "Content-Length") == 1, "带了 Content-Length(浏览器能正常收尾)");
    t_close(client);
    sxcl_auth_loopback_destroy(lb);
}

static void test_url_decoding(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(URL 解码用例)");
        return;
    }
    const int port = sxcl_auth_loopback_port(lb);
    /* 微软的 code 里会有 '.'/'-'/'_' 之类;这里故意塞 %2E 与 %2D 验证解码 */
    t_check(t_send_request(port, "/callback?state=S1&code=AAA%2EBBB%2DCCC%5FDDD") == 0,
            "发出带百分号编码的回调");
    char code[SXCL_AUTH_TOKEN_MAX];
    char got_err[64];
    code[0] = got_err[0] = '\0';
    const int rc = sxcl_auth_loopback_wait(lb, "S1", code, sizeof(code), NULL, 0, got_err,
                                           sizeof(got_err), 5000, NULL, NULL, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "带编码的回调也成功");
    t_check_str(code, "AAA.BBB-CCC_DDD", "百分号编码被正确解码");
    sxcl_auth_loopback_destroy(lb);
}

static void test_wrong_state_is_ignored(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(state 校验用例)");
        return;
    }
    const int port = sxcl_auth_loopback_port(lb);
    /* 先来一个 state 不对的(CSRF/串号的典型场景),再来一个对的 */
    t_check(t_send_request(port, "/callback?code=EVIL-CODE&state=NOT-MINE") == 0, "发出 state 错误的回调");
    t_check(t_send_request(port, "/callback?code=GOOD-CODE&state=MY-STATE") == 0, "发出 state 正确的回调");
    char code[SXCL_AUTH_TOKEN_MAX];
    char got_err[64];
    code[0] = got_err[0] = '\0';
    const int rc = sxcl_auth_loopback_wait(lb, "MY-STATE", code, sizeof(code), NULL, 0, got_err,
                                           sizeof(got_err), 5000, NULL, NULL, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "state 不对的那次被忽略,继续等到对的那次");
    t_check_str(code, "GOOD-CODE", "**拿到的不是伪造的那条授权码**(state 校验生效)");
    sxcl_auth_loopback_destroy(lb);
}

static void test_other_paths_ignored(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(路径过滤用例)");
        return;
    }
    const int port = sxcl_auth_loopback_port(lb);
    /* 浏览器会顺手要 favicon;不能把它当回调 */
    t_check(t_send_request(port, "/favicon.ico") == 0, "发出 /favicon.ico 请求");
    t_check(t_send_request(port, "/") == 0, "发出 / 请求");
    t_check(t_send_request(port, "/callback?code=REAL&state=S") == 0, "发出真正的回调");
    char code[SXCL_AUTH_TOKEN_MAX];
    char got_err[64];
    code[0] = got_err[0] = '\0';
    const int rc = sxcl_auth_loopback_wait(lb, "S", code, sizeof(code), NULL, 0, got_err,
                                           sizeof(got_err), 5000, NULL, NULL, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "非 /callback 的请求被跳过,仍然等到真回调");
    t_check_str(code, "REAL", "授权码正确");
    sxcl_auth_loopback_destroy(lb);
}

static void test_denied_callback(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(拒绝用例)");
        return;
    }
    const int port = sxcl_auth_loopback_port(lb);
    t_check(t_send_request(port, "/callback?error=access_denied&error_description=User%20said%20no&state=S") == 0,
            "发出“用户拒绝”的回调");
    char code[SXCL_AUTH_TOKEN_MAX];
    char got_err[128];
    code[0] = got_err[0] = '\0';
    const int rc = sxcl_auth_loopback_wait(lb, "S", code, sizeof(code), NULL, 0, got_err,
                                           sizeof(got_err), 5000, NULL, NULL, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_DENIED, "用户在页面上点了拒绝 -> ERR_DENIED");
    t_check_str(got_err, "access_denied", "把 OAuth 的 error 原样带出来");
    t_check_contains(err, "拒绝", "人话说明“被拒绝”");
    sxcl_auth_loopback_destroy(lb);
}

static void test_timeout(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(超时用例)");
        return;
    }
    char code[SXCL_AUTH_TOKEN_MAX];
    code[0] = '\0';
    /* 没人来回调:600ms 后必须自己放弃(不能挂住 UI) */
    const int rc = sxcl_auth_loopback_wait(lb, "S", code, sizeof(code), NULL, 0, NULL, 0, 600, NULL,
                                           NULL, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_TIMEOUT, "没人回调 -> 到点报超时");
    t_check_contains(err, "超时", "人话说明等超时了");
    sxcl_auth_loopback_destroy(lb);
}

/* 取消回调:恒返回 1,应该立刻退出 */
static int cancel_always(void *ud)
{
    (void)ud;
    return 1;
}

static void test_cancel(void)
{
    sxcl_auth_loopback *lb = NULL;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    if (sxcl_auth_loopback_start(&lb, err, sizeof(err)) != SXCL_AUTH_OK) {
        t_check(0, "环回服务器起得来(取消用例)");
        return;
    }
    char code[SXCL_AUTH_TOKEN_MAX];
    code[0] = '\0';
    const int rc = sxcl_auth_loopback_wait(lb, "S", code, sizeof(code), NULL, 0, NULL, 0, 60000,
                                           cancel_always, NULL, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_CANCELLED, "should_cancel 返回非 0 -> 立刻取消(不等满超时)");
    sxcl_auth_loopback_destroy(lb);
}

/* 授权码流(不含浏览器那一步)的组装:PKCE → 环回 → 拼 URL → 收码 → 换 token。
 * 这一步把"授权码主路"的代码路径真正跑一遍(只有"开浏览器"是外部动作)。 */
static void test_interactive_flow_composition(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    char verifier[128];
    char challenge[128];
    char state[48];
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    t_check(sxcl_auth_pkce_generate(verifier, sizeof(verifier), challenge, sizeof(challenge)) ==
                SXCL_AUTH_OK,
            "① 生成 PKCE");
    t_check(sxcl_auth_random_string(state, sizeof(state)) == 0, "② 生成 state");
    sxcl_auth_loopback *lb = NULL;
    t_check(sxcl_auth_loopback_start(&lb, err, sizeof(err)) == SXCL_AUTH_OK, "③ 起环回服务器");
    if (lb == NULL) {
        fake_free(ft);
        return;
    }
    const char *redirect = sxcl_auth_loopback_redirect_uri(lb);
    char url[SXCL_AUTH_URL_MAX];
    t_check(sxcl_auth_build_authorize_url_tenant(SXCL_AUTH_DEFAULT_CLIENT_ID, SXCL_AUTH_TENANT_DEFAULT,
                                                 redirect, challenge, state, url, sizeof(url), err,
                                                 sizeof(err)) == SXCL_AUTH_OK,
            "④ 拼出授权 URL");
    t_check_contains(url, challenge, "URL 里带着 PKCE challenge");
    t_check_contains(url, state, "URL 里带着 state");
    t_check_contains(url, "localhost", "回调用的是环回地址");

    /* 模拟浏览器跳回来 */
    char target[SXCL_AUTH_URL_MAX];
    const int port = sxcl_auth_loopback_port(lb);
    (void)snprintf(target, sizeof(target), "/callback?code=COMPOSED-CODE&state=%s", state);
    t_check(t_send_request(port, target) == 0, "⑤ 模拟浏览器把 code 回调回来");
    char code[SXCL_AUTH_TOKEN_MAX];
    code[0] = '\0';
    t_check(sxcl_auth_loopback_wait(lb, state, code, sizeof(code), NULL, 0, NULL, 0, 5000, NULL, NULL,
                                    err, sizeof(err)) == SXCL_AUTH_OK,
            "⑥ 收到授权码");
    sxcl_auth_loopback_destroy(lb);

    sxcl_auth_ms_tokens tok;
    t_check(sxcl_auth_exchange_code_tenant(&ft->pub, SXCL_AUTH_DEFAULT_CLIENT_ID,
                                          SXCL_AUTH_TENANT_DEFAULT, code, redirect, verifier, 5000,
                                          &tok, err, sizeof(err)) == SXCL_AUTH_OK,
            "⑦ 用授权码 + verifier 换到 token");
    t_check(tok.access_token[0] != '\0' && tok.refresh_token[0] != '\0',
            "拿到 access + refresh token");
    const fake_log *log = fake_find(ft, SXCL_AUTH_URL_TOKEN, 0);
    if (log != NULL) {
        t_check_contains(log->body, "code=COMPOSED-CODE", "换 token 用的是回调里那个码");
        char vf[160];
        (void)snprintf(vf, sizeof(vf), "code_verifier=%s", verifier);
        t_check_contains(log->body, vf, "用的是同一个 verifier(否则微软会拒)");
    }
    fake_free(ft);
}

int main(void)
{
    test_start_and_redirect_uri();
    test_success_callback();
    test_url_decoding();
    test_wrong_state_is_ignored();
    test_other_paths_ignored();
    test_denied_callback();
    test_timeout();
    test_cancel();
    test_interactive_flow_composition();
    return t_report("auth_loopback_test");
}
