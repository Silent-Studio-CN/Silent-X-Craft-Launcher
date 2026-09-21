/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_HTTP_H
#define SXCL_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码(负数为错) */
#define SXCL_HTTP_OK              0
#define SXCL_HTTP_ERR_ARG       (-1)   /* 参数不合法 */
#define SXCL_HTTP_ERR_NET       (-2)   /* 传输层失败:连响应都没拿到(DNS/连接/TLS/超时) */
#define SXCL_HTTP_ERR_STATUS    (-3)   /* 拿到了响应,但不是 2xx(人话在 err 里) */
#define SXCL_HTTP_ERR_TOO_LARGE (-4)   /* 响应体超过上限,已主动放弃 */
#define SXCL_HTTP_ERR_NOMEM     (-5)   /* 内存不足 */
#define SXCL_HTTP_ERR_SHA1      (-6)   /* 下载完了,但 SHA-1 与期望值不符 */
#define SXCL_HTTP_ERR_IO        (-7)   /* 读响应体中途断了 */

/** 默认大小上限:8 MiB(版本清单 ~1 MiB、版本 JSON 几十 KB、JRE 清单 ~200 KB,都够) */
#define SXCL_HTTP_DEFAULT_MAX_BYTES (8u * 1024u * 1024u)
#define SXCL_HTTP_ERROR_MAX 256

/** 可选参数(整个结构可空 = 全默认)。 */
typedef struct sxcl_http_opts {
    size_t max_bytes;          /**< 0 = SXCL_HTTP_DEFAULT_MAX_BYTES */
    const char *expected_sha1; /**< 非空 = 读完校验 SHA-1(十六进制,大小写不敏感;不匹配报 ERR_SHA1) */
    int *status_out;           /**< 非空 = 无论成败都把 HTTP 状态码写进去(没拿到响应写 0) */
    int64_t timeout_ms;        /**< 单次请求超时;<= 0 用传输层默认值 */
    int force_http1;           /**< 1 = 强制 HTTP/1.1(某些 CDN 在 h2 下不兑现 Range;见 net.h) */

    /* ── 以下为正版登录(2026-02)新增,默认值下行为与从前完全一致 ── */
    const char *method;        /**< NULL = "GET"(get_text 系)/ "POST"(post_text 系) */
    const char *body;          /**< 非空 = 带请求体(POST/PUT);常配 "Content-Type: …" 头 */
    size_t body_len;           /**< 请求体字节数;0 = 用 strlen(body)(文本体专用) */
    int accept_error_status;   /**< 1 = 非 2xx 也当成功返回正文(状态码看 status_out)。
                                *   OAuth 的 token/devicecode 端点把错误写在 4xx 的 JSON 正文里
                                *   (authorization_pending / slow_down / expired_token),必须能读到。 */
    void (*on_header)(void *userdata, const char *name, const char *value); /**< 响应头回调,可空 */
    void *header_userdata;     /**< 传给 on_header */
} sxcl_http_opts;

/** 把一个 HTTP 状态码翻成人话(只翻状态码本身,不含 URL)。
 *  404 -> "资源不存在";403 -> "被服务器拒绝",429 -> "请求过于频繁"…
 *  认得出返回静态字符串(勿 free),认不出返回 NULL。 */
const char *sxcl_http_status_text(int status);

/** 状态码 -> 完整人话("资源不存在(HTTP 404)"),写进 out(始终 NUL 结尾)。
 *  返回写入的字节数(不含 NUL);参数不合法返回 SXCL_HTTP_ERR_ARG。缓冲不够就截断,不报错。 */
int sxcl_http_status_message(int status, char *out, size_t out_len);

/** 取一整段文本。成功返回 SXCL_HTTP_OK(见文件头语义);err 可空。 */
int sxcl_http_get_text(sxcl_transport *tr, const char *url, const char *const *headers,
                       char **out, size_t *out_len, char *err, size_t err_len);

/** 带可选项的版本(大小上限 / 期望 SHA-1 / 取状态码 / 超时)。opts 可空。 */
int sxcl_http_get_text_ex(sxcl_transport *tr, const char *url, const char *const *headers,
                          const sxcl_http_opts *opts, char **out, size_t *out_len,
                          char *err, size_t err_len);

/** POST(或 opts->method 指定的方法)一段请求体,把响应正文读回来。
 *  与 sxcl_http_get_text_ex 同一套语义/上限/错误码,只多了请求体:
 *    - body 非空时用它,body_len 为 0 就按 NUL 结尾的文本算长度;
 *    - opts->accept_error_status = 1 时 4xx/5xx 也算成功(正文照返回,状态码在 status_out);
 *    - Content-Type 由调用方放进 headers(本函数不猜)。 */
int sxcl_http_post_text_ex(sxcl_transport *tr, const char *url, const char *const *headers,
                           const char *body, size_t body_len, const sxcl_http_opts *opts,
                           char **out, size_t *out_len, char *err, size_t err_len);

/** "下载小文件并校验 SHA-1"的重载(版本 JSON 那类:URL 旁边就带着 sha1)。
 *  等价于 opts.expected_sha1 = expected_sha1 的 sxcl_http_get_text_ex;
 *  expected_sha1 为空串/NULL 时退化成不校验(调用方通常从版本清单里直接拿这个字段)。 */
int sxcl_http_get_text_sha1(sxcl_transport *tr, const char *url, const char *const *headers,
                            const char *expected_sha1, char **out, size_t *out_len,
                            char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_HTTP_H */
