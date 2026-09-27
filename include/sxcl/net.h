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

/** 把 Qt 全局线程池里的活儿等完(域名解析就在那个池子上跑)。
 *  纯 C 的调用方(如 sxcl-dl)**在进程退出前**调一次:否则 Qt 的静态析构会在池子还有等待线程时
 *  打印 "QWaitCondition: Destroyed while threads are still waiting"(看着像我们的线程泄漏)。
 *  timeout_ms <= 0 用默认值(1500ms)。没引导过 Qt 时是空操作。 */
void sxcl_transport_qt_drain(int timeout_ms);

/** libcurl 后端(Win/Linux/Android;性能后端,HTTP/2 多路复用)。 */
sxcl_transport *sxcl_transport_libcurl_create(void);

/** Windows 辅助:只用 WinHTTP 解析 PAC/系统代理,不做传输(见 net 设计说明)。 */
sxcl_transport *sxcl_transport_winhttp_create(void);

/* ══════════════════════════════════════════════════════════════════════════════
 * 网络取证、统计与提速(2026-09-27)
 *
 * 为什么放在传输层:模组搜索/图标/清单全都从这里过,而且"每次请求都新建一个
 * QNetworkAccessManager"的结构问题也只有这里能改。上层(引擎/界面)一行都不用动。
 *
 * 环境变量(全部可选,产品默认值就是最快的那个):
 *   SXCL_NET_TRACE=1          每次请求打一行 NETTRACE:URL/DNS/连接/首字节/总耗时/
 *                             是否复用/压缩前后字节数/协议/缓存来源。
 *   SXCL_NET_CACHE=0          关掉结果缓存(查询 60 秒内存、图标内存+磁盘两级)。
 *   SXCL_NET_PREWARM=0        关掉预热(默认开:模组页要用的 host 先解析 + 先建连)。
 *   SXCL_NET_POOL=0           关掉共享网络线程池(默认开:连接按 host 粘住复用)。
 *   SXCL_NET_CACHE_DIR=<dir>  图标磁盘缓存的根目录(默认用户缓存目录)。
 *   SXCL_NET_QUERY_TTL_MS     查询缓存存活毫秒(默认 60000)。
 *   SXCL_NET_CONNECT_MS       连接(响应头)阶段超时毫秒(默认 8000;到点就换下一个源)。
 *   SXCL_NET_LEGACY=1         退回 2026-09-27 之前的旧行为(每次请求一个新实例、
 *                             不缓存、不预热、不重试)。基准对照用,产品路径不用。
 *
 * 下面这些接口**不调用也不影响传输**:纯取证/纯提速。
 * ══════════════════════════════════════════════════════════════════════════════ */

/** 取证样本里的 URL 上限(原值,不打码)。 */
#define SXCL_NET_URL_MAX 1024
#define SXCL_NET_ENCODING_MAX 32

/** 一次请求的取证样本。全部是实测值,测不到就是 -1(不编数)。 */
typedef struct sxcl_net_sample {
    char url[SXCL_NET_URL_MAX];        /**< 原始 URL */
    int status;                        /**< HTTP 状态码;0 = 没拿到响应 */
    int http2;                         /**< 1 = 协商到 HTTP/2 */
    int reused;                        /**< 1 = 复用了连接(没做 TLS 握手);-1 = 测不到(明文 http) */
    int from_cache;                    /**< 0 = 真网络;1 = 内存缓存;2 = 磁盘缓存 */
    int attempts;                      /**< 实际尝试次数(含重试) */
    int64_t dns_ms;                    /**< 本进程对这台主机的解析耗时;-1 = 没测到(已解析或未采样) */
    int64_t conn_ms;                   /**< 发起 -> TLS 握手完成;-1 = 没握手(连接复用)或测不到 */
    int64_t ttfb_ms;                   /**< 发起 -> 响应头/首字节到达 */
    int64_t total_ms;                  /**< 发起 -> 正文读完(上层 close_body 时定稿) */
    int64_t wire_bytes;                /**< 线上字节(压缩后);-1 = 未知 */
    int64_t body_bytes;                /**< 交给上层的字节(解压后) */
    char content_encoding[SXCL_NET_ENCODING_MAX]; /**< 线上编码(gzip/deflate/空) */
} sxcl_net_sample;

/** 进程级累计统计(线程安全)。 */
typedef struct sxcl_net_stats {
    int64_t requests;        /**< 真发到网络上的请求数 */
    int64_t cache_hits_mem;  /**< 内存缓存命中 */
    int64_t cache_hits_disk; /**< 磁盘缓存命中(图标) */
    int64_t cache_stores;    /**< 写进缓存的响应数 */
    int64_t prewarm_ok;      /**< 预热成功的 host 数 */
    int64_t prewarm_fail;    /**< 预热失败的 host 数 */
    int64_t retries;         /**< 重试次数 */
    int64_t throttled;       /**< 因同源并发上限等过槽的次数 */
    int64_t bytes_wire;      /**< 累计线上字节 */
    int64_t bytes_body;      /**< 累计解压后字节 */
} sxcl_net_stats;

/** 取**本线程**最近一次完成的请求样本(没有则返回 0)。 */
int sxcl_net_trace_last(sxcl_net_sample *out);

/** 取进程级统计 / 清零。 */
void sxcl_net_stats_get(sxcl_net_stats *out);
void sxcl_net_stats_reset(void);

/** 预热:把一批 URL 的主机先解析、先建连(不阻塞调用方,失败了下次照样直连)。
 *  返回真正排上队的 URL 数。 */
int sxcl_net_prewarm(const char *const *urls, size_t count);

/** 预热模组页要用的那一组主机(搜索 API / 图标 CDN / 版本清单)。
 *  幂等,重复调用只补还没热的那些。 */
int sxcl_net_prewarm_defaults(void);

/** 清空结果缓存(内存 + 磁盘)。返回清掉的条目数。 */
int sxcl_net_cache_clear(void);

/** 指定图标磁盘缓存根目录(空串 = 恢复默认)。返回 0 成功。 */
int sxcl_net_cache_dir_set(const char *dir);

/** 当前缓存条目数(内存 / 磁盘),给验收脚本读数用。 */
void sxcl_net_cache_counts(int64_t *mem_entries, int64_t *disk_files);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_NET_H */
