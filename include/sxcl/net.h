/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_NET_H
#define SXCL_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 错误码(负数为错,与 fs/verify 的约定一致)
 *
 * 约定:只要拿到了 HTTP 响应(哪怕 404/500)就返回 SXCL_NET_OK,
 * 状态码与头放在 resp 里由引擎判断(引擎要据 404/超时/慢速决定换哪条路);
 * 负数只表示"连响应都没拿到"这一类传输层故障。 */
#define SXCL_NET_OK 0
#define SXCL_NET_ERR_CONNECT (-1)     /* DNS/连接/TLS/超时:没拿到响应 */
#define SXCL_NET_ERR_IO (-2)          /* 传输中途断开 */
#define SXCL_NET_ERR_CANCELLED (-3)   /* 用户取消 */
#define SXCL_NET_ERR_BAD_ARG (-4)
#define SXCL_NET_ERR_UNSUPPORTED (-5) /* 后端不支持该能力 */

typedef struct sxcl_http_request {
    const char *url;
    const char *method;              /* NULL 视为 "GET" */
    int64_t range_start;             /* < 0 表示不带 Range 头 */
    int64_t range_end;               /* < 0 表示到文件末尾(start 有效时必须 >= start) */
    const char *const *extra_headers;/* 形如 "User-Agent: x" 的字符串数组,NULL 结尾,可空 */
    int64_t timeout_ms;              /* 单次请求超时;<= 0 用实现默认值 */
    int force_http1;                 /* 0=允许 HTTP/2(默认);1=强制 HTTP/1.1。
                                      * 用途:某些后端/CDN 在 h2 下不兑现 Range(实测 Qt h2 会忽略 Range
                                      * 返回 200 全量),这时必须按请求降级,否则续传会静默变成全量重下。 */
    /* ── 请求体(正版登录要 POST JSON / 表单;2026-02 加) ──
     * body 非空 = 带请求体,配合 method 用 POST/PUT。body_len 是字节数(允许 0)。
     * Content-Type 由调用方通过 extra_headers 给(下载引擎不需要它)。 */
    const void *body;
    size_t body_len;
    /* ── 响应头回调(可选;2026-02 加) ──
     * 每收到一个响应头调用一次(name/value 都是 NUL 结尾,大小写保留服务器原样,可以重复)。
     * 只在 request() 调用期间有效,回调里不要把指针存下来。NULL = 不要响应头。
     * 用途:登录链要读 Retry-After(限流退避)这类头,而固定字段塞不下所有头。 */
    void (*on_header)(void *userdata, const char *name, const char *value);
    void *header_userdata;
} sxcl_http_request;

typedef struct sxcl_http_response {
    int status;                      /* 200 / 206 / 404 ... */
    int64_t content_length;          /* 本次响应体长度;未知为 -1 */
    int64_t total_length;            /* 资源全长(Content-Range 的 total);未知为 -1 */
    int64_t range_start;             /* 实际生效的范围起点;未知为 -1 */
    int64_t range_end;               /* 实际生效的范围终点(含);未知为 -1 */
    int accept_ranges;               /* 是否声明 Accept-Ranges: bytes(0/1) */
    int is_range_response;           /* 是否 206 */
} sxcl_http_response;

/* 响应体读句柄(实现自定义内部结构) */
typedef struct sxcl_http_body sxcl_http_body;

typedef struct sxcl_transport {
    void *ctx;
    /* 发起请求。成功后 *body 非空,必须由 close_body 释放(即使 status 是 404:错误页也可能有正文)。
     * 返回 SXCL_NET_OK / 负错误码。 */
    int (*request)(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                   sxcl_http_body **body);
    /* 读最多 len 字节。>0 实读字节;0 表示响应体结束;<0 错误(可查询是否被取消)。 */
    int64_t (*read)(void *ctx, sxcl_http_body *body, void *buf, size_t len);
    /* 释放响应体。允许中途调用(等价于放弃剩余正文)。 */
    void (*close_body)(void *ctx, sxcl_http_body *body);
    /* 请求取消当前 transport 上所有进行中的 IO(线程安全;引擎的取消按钮走这条)。 */
    void (*cancel_all)(void *ctx);
    /* 释放后端实例。 */
    void (*destroy)(void *ctx);
} sxcl_transport;

/** Qt Network 后端(四平台通用:Win/Android/macOS/Linux)。
 *  依赖 Qt6::Network;TLS 走平台后端(SChannel / Secure Transport / OpenSSL),
 *  系统代理与 PAC 由 Qt 自动处理。
 *  线程约束:一个实例只属于创建它的那个线程,且必须在该线程里创建与使用
 *  (QNetworkAccessManager 有线程亲和性);工作线程池的每个线程各持一个实例。 */
sxcl_transport *sxcl_transport_qt_create(void);

/** 创建进程级 QCoreApplication(必须在主线程、创建任何 Qt 传输实例之前调用一次)。
 *  引擎的工作线程里跑嵌套事件循环依赖它;纯 C 调用方(如 sxcl-dl)靠这个函数引导。 */
void sxcl_transport_qt_bootstrap(void);

/** libcurl 后端(Win/Linux/Android;性能后端,HTTP/2 多路复用)。 */
sxcl_transport *sxcl_transport_libcurl_create(void);

/** Windows 辅助:只用 WinHTTP 解析 PAC/系统代理,不做传输(见 net 设计说明)。 */
sxcl_transport *sxcl_transport_winhttp_create(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_NET_H */
