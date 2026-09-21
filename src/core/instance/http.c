/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/http.h"

#include "sxcl/hash.h"
#include "sxcl/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h> /* GetTickCount64:只用来算"这次请求用了多久" */
#else
#  include <sys/time.h>
#endif

#define SXCL_HTTP_INITIAL_CAP 8192u

/* 墙钟毫秒(只给日志算耗时用;Windows 用 GetTickCount64,不受系统时间调整影响)。 */
static int64_t http_now_ms(void)
{
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + (int64_t)(tv.tv_usec / 1000);
#endif
}

/* 一条网络日志。**控制面请求**(取清单/版本 JSON/加载器列表/登录)都从这里过,
 * 所以默认级别就能看到:方法 / 打码后的 URL / 状态码 / 字节数 / 耗时 / 失败原因。
 * 逐文件的下载明细不走这里(那条路在传输层,debug 级才记),见 docs/17-运行日志.md。
 * URL 用 sxcl_log_mask_url 打码:日志里不存在 token/授权码明文的第二份。 */
static void http_log_line(int level, const char *method, const char *url, int status, int64_t bytes,
                          int64_t started_ms, const char *result)
{
    char masked[SXCL_LOG_URL_MAX + 48];
    (void)sxcl_log_mask_url((url != NULL && url[0] != '\0') ? url : "(空)", masked, sizeof(masked));
    const int64_t elapsed = (started_ms > 0) ? (http_now_ms() - started_ms) : -1;
    sxcl_log_write(level, "net", "%s %s -> 状态=%d 字节=%lld 耗时=%lldms %s",
                   (method != NULL && method[0] != '\0') ? method : "GET", masked, status,
                   (long long)bytes, (long long)elapsed, (result != NULL) ? result : "");
}

static void http_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

const char *sxcl_http_status_text(int status)
{
    switch (status) {
        case 200: return "成功";
        case 204: return "成功（正文为空）";
        case 301:
        case 302:
        case 307:
        case 308: return "需要跳转";
        case 400: return "请求不合法";
        case 401: return "需要登录（凭据无效或已过期）";
        case 403: return "被服务器拒绝（可能是地区限制或反爬）";
        case 404: return "资源不存在";
        case 408: return "服务器超时";
        case 410: return "资源已被删除";
        case 416: return "请求的字节范围无效";
        case 429: return "请求过于频繁（被限流）";
        case 451: return "因法律原因不可用";
        case 500: return "服务器内部错误";
        case 502: return "网关错误";
        case 503: return "服务暂不可用（服务器维护或过载）";
        case 504: return "网关超时";
        default: return NULL;
    }
}

int sxcl_http_status_message(int status, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_HTTP_ERR_ARG;
    }
    const char *text = sxcl_http_status_text(status);
    const int written = (text != NULL)
                            ? snprintf(out, out_len, "%s（HTTP %d）", text, status)
                            : snprintf(out, out_len, "服务器返回了意外状态（HTTP %d）", status);
    if (written < 0) {
        out[0] = '\0';
        return SXCL_HTTP_ERR_ARG;
    }
    return written;
}

static int http_is_success(int status)
{
    return (status >= 200 && status < 300) ? 1 : 0;
}

/* 通用实现:get_text_ex / post_text_ex 都走它。
 *   method_default —— opts->method 为空时用什么方法("GET" / "POST");
 *   body/body_len  —— 非空 = 带请求体(覆盖 opts->body)。 */
static int http_fetch(sxcl_transport *tr, const char *url, const char *const *headers,
                      const char *method_default, const char *req_body, size_t req_body_len,
                      const sxcl_http_opts *opts, char **out, size_t *out_len,
                      char *err, size_t err_len)
{
    if (out == NULL || out_len == NULL) {
        http_err(err, err_len, "参数不合法（out/out_len 不能为空）");
        return SXCL_HTTP_ERR_ARG;
    }
    *out = NULL;
    *out_len = 0;
    if (opts != NULL && opts->status_out != NULL) {
        *opts->status_out = 0;
    }
    if (tr == NULL || tr->request == NULL || tr->read == NULL || tr->close_body == NULL ||
        url == NULL || url[0] == '\0') {
        http_err(err, err_len, "参数不合法（transport/url 不能为空）");
        return SXCL_HTTP_ERR_ARG;
    }

    size_t max_bytes = (opts != NULL && opts->max_bytes > 0) ? opts->max_bytes : SXCL_HTTP_DEFAULT_MAX_BYTES;
    const int64_t started_ms = http_now_ms();

    const char *method = (opts != NULL && opts->method != NULL) ? opts->method : method_default;
    if (req_body == NULL && opts != NULL) {
        req_body = opts->body;
        if (req_body != NULL) {
            req_body_len = opts->body_len;
        }
    }
    if (req_body != NULL && req_body_len == 0) {
        req_body_len = strlen(req_body); /* 文本体的便捷写法(length 字段是给二进制体准备的) */
    }
    if (req_body == NULL) {
        req_body_len = 0;
    }

    sxcl_http_request req;
    (void)memset(&req, 0, sizeof(req));
    req.url = url;
    req.method = method;
    req.range_start = -1;
    req.range_end = -1;
    req.extra_headers = headers;
    req.timeout_ms = (opts != NULL) ? opts->timeout_ms : 0;
    req.force_http1 = (opts != NULL) ? opts->force_http1 : 0;
    req.body = req_body;
    req.body_len = req_body_len;
    req.on_header = (opts != NULL) ? opts->on_header : NULL;
    req.header_userdata = (opts != NULL) ? opts->header_userdata : NULL;

    sxcl_http_response resp;
    (void)memset(&resp, 0, sizeof(resp));
    sxcl_http_body *body = NULL;
    const int rc = tr->request(tr->ctx, &req, &resp, &body);
    if (opts != NULL && opts->status_out != NULL) {
        *opts->status_out = rc == SXCL_NET_OK ? resp.status : 0;
    }
    if (rc != SXCL_NET_OK) {
        const char *why = "网络请求失败（连不上/超时/TLS 失败）";
        if (rc == SXCL_NET_ERR_IO) {
            why = "传输中途断开";
        } else if (rc == SXCL_NET_ERR_CANCELLED) {
            why = "已被取消";
        } else if (rc == SXCL_NET_ERR_UNSUPPORTED) {
            why = "当前传输后端不支持这个请求";
        }
        http_err(err, err_len, "%s：%s", why, url);
        http_log_line(SXCL_LOG_WARN, method, url, 0, 0, started_ms, why);
        return SXCL_HTTP_ERR_NET;
    }
    if (body == NULL) {
        http_err(err, err_len, "传输层没有返回响应体句柄：%s", url);
        http_log_line(SXCL_LOG_WARN, method, url, resp.status, 0, started_ms, "传输层没有返回响应体");
        return SXCL_HTTP_ERR_NET;
    }
    const int allow_error = (opts != NULL && opts->accept_error_status) ? 1 : 0;
    if (!http_is_success(resp.status) && !allow_error) {
        char message[SXCL_HTTP_ERROR_MAX];
        (void)sxcl_http_status_message(resp.status, message, sizeof(message));
        http_err(err, err_len, "%s：%s", message, url);
        http_log_line(SXCL_LOG_WARN, method, url, resp.status, 0, started_ms, message);
        tr->close_body(tr->ctx, body);
        return SXCL_HTTP_ERR_STATUS;
    }
    if (resp.content_length >= 0 && (uint64_t)resp.content_length > (uint64_t)max_bytes) {
        http_err(err, err_len, "响应体太大（%lld 字节，上限 %llu 字节），已放弃：%s",
                 (long long)resp.content_length, (unsigned long long)max_bytes, url);
        http_log_line(SXCL_LOG_WARN, method, url, resp.status, resp.content_length, started_ms,
                      "响应体超过上限,已放弃");
        tr->close_body(tr->ctx, body);
        return SXCL_HTTP_ERR_TOO_LARGE;
    }

    size_t cap = SXCL_HTTP_INITIAL_CAP;
    if ((size_t)max_bytes + 1u < cap) {
        cap = (size_t)max_bytes + 1u;
    }
    char *buf = (char *)malloc(cap + 1u);
    if (buf == NULL) {
        tr->close_body(tr->ctx, body);
        http_err(err, err_len, "内存不足（准备申请 %llu 字节）", (unsigned long long)(cap + 1u));
        http_log_line(SXCL_LOG_WARN, method, url, resp.status, 0, started_ms, "内存不足");
        return SXCL_HTTP_ERR_NOMEM;
    }
    size_t len = 0;
    for (;;) {
        if (len == cap) {
            if (cap > max_bytes) {
                break; /* 已经超过上限,交给下面的检查报错 */
            }
            size_t next = cap * 2u;
            if (next > (size_t)max_bytes + 1u) {
                next = (size_t)max_bytes + 1u;
            }
            if (next <= cap) {
                break;
            }
            char *grown = (char *)realloc(buf, next + 1u);
            if (grown == NULL) {
                free(buf);
                tr->close_body(tr->ctx, body);
                http_err(err, err_len, "内存不足（响应体已读 %llu 字节）", (unsigned long long)len);
                return SXCL_HTTP_ERR_NOMEM;
            }
            buf = grown;
            cap = next;
        }
        const size_t want = cap - len;
        const int64_t got = tr->read(tr->ctx, body, buf + len, want);
        if (got < 0) {
            free(buf);
            tr->close_body(tr->ctx, body);
            http_err(err, err_len, "读响应体出错（已读 %llu 字节）：%s", (unsigned long long)len, url);
            http_log_line(SXCL_LOG_WARN, method, url, resp.status, (int64_t)len, started_ms,
                          "读响应体中途出错");
            return SXCL_HTTP_ERR_IO;
        }
        if (got == 0) {
            break;
        }
        if ((uint64_t)got > (uint64_t)want) {
            free(buf);
            tr->close_body(tr->ctx, body);
            http_err(err, err_len, "传输层返回的字节数超过请求长度（实现有 bug）");
            return SXCL_HTTP_ERR_IO;
        }
        len += (size_t)got;
        if ((uint64_t)len > (uint64_t)max_bytes) {
            free(buf);
            tr->close_body(tr->ctx, body);
            http_err(err, err_len, "响应体超过上限（上限 %llu 字节），已放弃：%s",
                     (unsigned long long)max_bytes, url);
            http_log_line(SXCL_LOG_WARN, method, url, resp.status, (int64_t)len, started_ms,
                          "读到的字节数超过上限,已放弃");
            return SXCL_HTTP_ERR_TOO_LARGE;
        }
    }
    tr->close_body(tr->ctx, body);

    if ((uint64_t)len > (uint64_t)max_bytes) {
        free(buf);
        http_err(err, err_len, "响应体超过上限（上限 %llu 字节），已放弃：%s",
                 (unsigned long long)max_bytes, url);
        return SXCL_HTTP_ERR_TOO_LARGE;
    }

    if (opts != NULL && opts->expected_sha1 != NULL && opts->expected_sha1[0] != '\0') {
        char digest[65];
        if (sxcl_hash_digest(SXCL_HASH_SHA1, buf, len, digest, sizeof(digest)) != 0) {
            free(buf);
            http_err(err, err_len, "算不出 SHA-1（内部错误）");
            return SXCL_HTTP_ERR_SHA1;
        }
        if (!sxcl_hash_hex_equal(digest, opts->expected_sha1)) {
            http_err(err, err_len, "SHA-1 校验失败：期望 %s，实际 %s：%s",
                     opts->expected_sha1, digest, url);
            char sha1_note[160];
            (void)snprintf(sha1_note, sizeof(sha1_note), "SHA-1 不符(期望 %.16s… 实际 %.16s…)",
                           opts->expected_sha1, digest);
            http_log_line(SXCL_LOG_WARN, method, url, resp.status, (int64_t)len, started_ms, sha1_note);
            free(buf);
            return SXCL_HTTP_ERR_SHA1;
        }
    }

    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    http_log_line(SXCL_LOG_INFO, method, url, resp.status, (int64_t)len, started_ms, "OK");
    return SXCL_HTTP_OK;
}

int sxcl_http_get_text_ex(sxcl_transport *tr, const char *url, const char *const *headers,
                          const sxcl_http_opts *opts, char **out, size_t *out_len,
                          char *err, size_t err_len)
{
    return http_fetch(tr, url, headers, "GET", NULL, 0, opts, out, out_len, err, err_len);
}

int sxcl_http_post_text_ex(sxcl_transport *tr, const char *url, const char *const *headers,
                           const char *body, size_t body_len, const sxcl_http_opts *opts,
                           char **out, size_t *out_len, char *err, size_t err_len)
{
    return http_fetch(tr, url, headers, "POST", body, body_len, opts, out, out_len, err, err_len);
}

int sxcl_http_get_text(sxcl_transport *tr, const char *url, const char *const *headers,
                       char **out, size_t *out_len, char *err, size_t err_len)
{
    return http_fetch(tr, url, headers, "GET", NULL, 0, NULL, out, out_len, err, err_len);
}

int sxcl_http_get_text_sha1(sxcl_transport *tr, const char *url, const char *const *headers,
                            const char *expected_sha1, char **out, size_t *out_len,
                            char *err, size_t err_len)
{
    sxcl_http_opts opts;
    (void)memset(&opts, 0, sizeof(opts));
    opts.expected_sha1 = expected_sha1;
    return sxcl_http_get_text_ex(tr, url, headers, &opts, out, out_len, err, err_len);
}
