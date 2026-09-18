/* 环回重定向服务器(授权码流的"接收端")—— 只监听 127.0.0.1,只活一次登录。
 *
 * 为什么走环回而不是"复制粘贴授权码":
 *   官方推荐的桌面公共客户端做法(RFC 8252):起一个**短命**的本地 HTTP 服务,
 *   把 redirect_uri 指到 http://localhost:<随机端口>/callback,浏览器授权后自动跳回来。
 *   用户不用手抄 code,也不会把 code 抄错。
 *
 * 安全上的三条硬规矩:
 *   1) **只 bind 127.0.0.1**(不是 0.0.0.0):局域网里的其它机器连不上这个端口;
 *   2) **随机端口**(bind 端口 0 让系统分配):避免和别的程序撞端口,也避免被猜到;
 *   3) **校验 state**:回调里的 state 必须和发起时一致,否则直接拒(防 CSRF/串号),
 *      而且**只接受 /callback 一条路径**。
 *
 * 生命周期:start(拿端口与 redirect_uri)→ wait(等一次回调,可超时/可取消)→ destroy。
 * 只服务一次登录,用完立刻关 —— 不留常驻端口。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET sxcl_sock_t;
#  define SXCL_BAD_SOCK INVALID_SOCKET
#  define sxcl_sock_close closesocket
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sxcl_sock_t;
#  define SXCL_BAD_SOCK (-1)
#  define sxcl_sock_close close
#endif

struct sxcl_auth_loopback {
    intptr_t sock;
    int port;
    char redirect_uri[SXCL_AUTH_URL_MAX];
};

static int net_boot(void)
{
#if defined(_WIN32)
    static int done = 0;
    if (done) {
        return 0;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }
    done = 1;
#endif
    return 0;
}

int sxcl_auth_loopback_start(sxcl_auth_loopback **out, char *err, size_t err_len)
{
    if (out == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    *out = NULL;
    if (net_boot() != 0) {
        sxcl_auth_err(err, err_len, "初始化网络套接字失败（WSAStartup）");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    sxcl_auth_loopback *lb = (sxcl_auth_loopback *)calloc(1, sizeof(*lb));
    if (lb == NULL) {
        sxcl_auth_err(err, err_len, "内存不足（环回服务器）");
        return SXCL_AUTH_ERR_NOMEM;
    }
    lb->sock = (intptr_t)SXCL_BAD_SOCK;
    lb->port = 0;

    const sxcl_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SXCL_BAD_SOCK) {
        free(lb);
        sxcl_auth_err(err, err_len, "建不了监听套接字（系统不允许）");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, (int)sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0; /* 0 = 让系统挑一个空闲端口(随机,不撞车) */
    /* 只绑环回地址:局域网连不进来 */
    addr.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */

    if (bind(s, (const struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
        sxcl_sock_close(s);
        free(lb);
        sxcl_auth_err(err, err_len, "绑不了 127.0.0.1 的本地端口（被防火墙/策略挡了？）");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    if (listen(s, 4) != 0) {
        sxcl_sock_close(s);
        free(lb);
        sxcl_auth_err(err, err_len, "监听本地端口失败");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    int alen = (int)sizeof(addr);
    if (getsockname(s, (struct sockaddr *)&addr, &alen) != 0) {
        sxcl_sock_close(s);
        free(lb);
        sxcl_auth_err(err, err_len, "拿不到本地端口号");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    lb->sock = (intptr_t)s;
    lb->port = (int)ntohs(addr.sin_port);
    /* 注意:redirect_uri 用 localhost(微软对环回地址的匹配规则认这个主机名,
     * 端口由我们自己指定 —— 这正是"随机端口"能通过重定向校验的原因)。 */
    (void)snprintf(lb->redirect_uri, sizeof(lb->redirect_uri),
                   "http://localhost:%d/callback", lb->port);
    *out = lb;
    return SXCL_AUTH_OK;
}

int sxcl_auth_loopback_port(const sxcl_auth_loopback *lb)
{
    return lb != NULL ? lb->port : 0;
}

const char *sxcl_auth_loopback_redirect_uri(const sxcl_auth_loopback *lb)
{
    return lb != NULL ? lb->redirect_uri : "";
}

void sxcl_auth_loopback_destroy(sxcl_auth_loopback *lb)
{
    if (lb == NULL) {
        return;
    }
    if (lb->sock != (intptr_t)SXCL_BAD_SOCK) {
        sxcl_sock_close((sxcl_sock_t)lb->sock);
        lb->sock = (intptr_t)SXCL_BAD_SOCK;
    }
    free(lb);
}

/* 从 "k=v&k2=v2" 里取一次 k(已做 URL 解码);找到返回 0。 */
static int query_get(const char *query, const char *key, char *out, size_t out_len)
{
    if (query == NULL || key == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    const size_t klen = strlen(key);
    const char *p = query;
    while (*p != 0) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (eq == NULL) {
            break;
        }
        if (amp != NULL && eq > amp) {
            p = amp + 1;
            continue;
        }
        const size_t nlen = (size_t)(eq - p);
        if (nlen == klen && strncmp(p, key, klen) == 0) {
            const char *vstart = eq + 1;
            const char *vend = (amp != NULL) ? amp : (vstart + strlen(vstart));
            size_t o = 0;
            for (const char *v = vstart; v < vend && o + 1u < out_len; ++v) {
                if (*v == '%' && v + 2u < vend) {
                    int hi = -1, lo = -1;
                    const char a = v[1], b = v[2];
                    if (a >= '0' && a <= '9') hi = a - '0';
                    else if ((a | 0x20) >= 'a' && (a | 0x20) <= 'f') hi = (a | 0x20) - 'a' + 10;
                    if (b >= '0' && b <= '9') lo = b - '0';
                    else if ((b | 0x20) >= 'a' && (b | 0x20) <= 'f') lo = (b | 0x20) - 'a' + 10;
                    if (hi >= 0 && lo >= 0) {
                        out[o++] = (char)((hi << 4) | lo);
                        v += 2;
                        continue;
                    }
                }
                out[o++] = (*v == '+') ? ' ' : *v;
            }
            out[o] = '\0';
            return 0;
        }
        p = (amp != NULL) ? amp + 1 : (p + strlen(p));
    }
    return -1;
}

/* 给浏览器一个"可以关掉了"的页面(中文;UTF-8 已声明,不会乱码) */
static void send_page(sxcl_sock_t c, const char *title, const char *detail, int ok)
{
    char body[1024];
    (void)snprintf(body, sizeof(body),
                   "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                   "<title>%s</title></head><body style=\"font-family:system-ui,sans-serif;"
                   "background:#1b1b1f;color:#eee;display:flex;align-items:center;"
                   "justify-content:center;height:100vh;margin:0\">"
                   "<div style=\"text-align:center\"><h1 style=\"color:%s\">%s</h1><p>%s</p></div>"
                   "</body></html>",
                   title, ok ? "#7ee787" : "#ff7b72", title, detail);
    char head[256];
    const int blen = (int)strlen(body);
    (void)snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: %d\r\nConnection: close\r\n\r\n",
                   blen);
    (void)send(c, head, (int)strlen(head), 0);
    (void)send(c, body, blen, 0);
}

int sxcl_auth_loopback_wait(sxcl_auth_loopback *lb, const char *expect_state,
                            char *code, size_t code_len, char *state, size_t state_len,
                            char *error_code, size_t error_code_len,
                            int64_t timeout_ms, int (*should_cancel)(void *), void *cancel_ud,
                            char *err, size_t err_len)
{
    if (lb == NULL || lb->sock == (intptr_t)SXCL_BAD_SOCK) {
        sxcl_auth_err(err, err_len, "内部错误:环回服务器没有启动");
        return SXCL_AUTH_ERR_ARG;
    }
    if (code != NULL && code_len > 0) code[0] = '\0';
    if (state != NULL && state_len > 0) state[0] = '\0';
    if (error_code != NULL && error_code_len > 0) error_code[0] = '\0';

    const int64_t deadline = timeout_ms > 0 ? (sxcl_auth_now() * 1000 + timeout_ms) : 0;
    /* 时间基准用真实毫秒(不能用 sxcl_auth_now:*_now 是"秒",测试里被钉死会死循环) */
    int64_t waited = 0;
    const int64_t slice = 200;
    const sxcl_sock_t s = (sxcl_sock_t)lb->sock;

    for (;;) {
        if (should_cancel != NULL && should_cancel(cancel_ud) != 0) {
            sxcl_auth_err(err, err_len, "已取消等待授权回调");
            return SXCL_AUTH_ERR_CANCELLED;
        }
        if (timeout_ms > 0 && waited >= timeout_ms) {
            sxcl_auth_err(err, err_len, "等了 %lld 秒也没等到浏览器回调（超时）",
                          (long long)(timeout_ms / 1000));
            return SXCL_AUTH_ERR_TIMEOUT;
        }
        (void)deadline;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        struct timeval tv;
        tv.tv_sec = (long)(slice / 1000);
        tv.tv_usec = (long)((slice % 1000) * 1000);
        const int ready = select((int)(s + 1), &rfds, NULL, NULL, &tv);
        waited += slice;
        if (ready <= 0) {
            continue; /* 超时/被打断:回到上面判取消与总超时 */
        }
        const sxcl_sock_t c = accept(s, NULL, NULL);
        if (c == SXCL_BAD_SOCK) {
            continue;
        }
        /* 读请求(只要请求行;浏览器可能分几次发,循环读到 \r\n 或缓冲满) */
        char req[4096];
        size_t used = 0;
        int got_line = 0;
        for (int guard = 0; guard < 64 && !got_line; ++guard) {
            if (used + 1u >= sizeof(req)) {
                break;
            }
            const int n = recv(c, req + used, (int)(sizeof(req) - 1u - used), 0);
            if (n <= 0) {
                break;
            }
            used += (size_t)n;
            req[used] = '\0';
            if (strstr(req, "\r\n") != NULL) {
                got_line = 1;
            }
        }
        req[used] = '\0';
        if (!got_line) {
            send_page(c, "请求不完整", "浏览器发来的请求读不完整，请重试。", 0);
            sxcl_sock_close(c);
            continue;
        }
        /* 第一行: METHOD SP TARGET SP VERSION */
        char target[SXCL_AUTH_URL_MAX];
        target[0] = '\0';
        (void)sscanf(req, "%*s %1023s", target);
        const char *q = strchr(target, '?');
        const char *path_end = (q != NULL) ? q : (target + strlen(target));
        const size_t path_len = (size_t)(path_end - target);
        if (path_len != 9u || strncmp(target, "/callback", 9u) != 0) {
            /* 浏览器会顺手要 /favicon.ico:给个空 404 别打扰用户 */
            const char *notfound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)send(c, notfound, (int)strlen(notfound), 0);
            sxcl_sock_close(c);
            continue;
        }
        const char *query = (q != NULL) ? q + 1 : "";
        char got_code[SXCL_AUTH_TOKEN_MAX];
        char got_state[256];
        char got_error[128];
        char got_desc[SXCL_AUTH_MESSAGE_MAX];
        got_code[0] = got_state[0] = got_error[0] = got_desc[0] = '\0';
        (void)query_get(query, "code", got_code, sizeof(got_code));
        (void)query_get(query, "state", got_state, sizeof(got_state));
        (void)query_get(query, "error", got_error, sizeof(got_error));
        (void)query_get(query, "error_description", got_desc, sizeof(got_desc));

        if (expect_state != NULL && expect_state[0] != '\0') {
            if (!sxcl_auth_ct_equal(got_state, expect_state)) {
                /* state 不对:可能是别的程序在往这个端口打请求(端口是随机的,几率极低),
                 * 也可能是 CSRF。**不把 code 交出去**,继续等真正的回调。 */
                send_page(c, "state 校验失败",
                          "回调里的 state 与本次登录不一致，已忽略这次请求。", 0);
                sxcl_sock_close(c);
                continue;
            }
        }
        if (got_error[0] != '\0') {
            send_page(c, "授权被拒绝", "已经收到微软的拒绝回调，可以关闭此页面。", 0);
            sxcl_sock_close(c);
            if (error_code != NULL && error_code_len > 0) {
                (void)sxcl_auth_copy(error_code, error_code_len, got_error);
            }
            if (err != NULL && err_len > 0) {
                sxcl_auth_err(err, err_len, "用户在授权页面上点了拒绝（%s%s%s）", got_error,
                              got_desc[0] ? "：" : "", got_desc);
            }
            return SXCL_AUTH_ERR_DENIED;
        }
        if (got_code[0] == '\0') {
            send_page(c, "没有拿到授权码", "回调里没有 code，请重试登录。", 0);
            sxcl_sock_close(c);
            continue;
        }
        send_page(c, "登录成功", "已收到授权码，可以关闭此页面回到启动器。", 1);
        sxcl_sock_close(c);
        if (code != NULL && code_len > 0) {
            (void)sxcl_auth_copy(code, code_len, got_code);
        }
        if (state != NULL && state_len > 0) {
            (void)sxcl_auth_copy(state, state_len, got_state);
        }
        return SXCL_AUTH_OK;
    }
}
