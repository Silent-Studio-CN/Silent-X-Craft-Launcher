/* SXCL-C "一次取回一整段文本" 的 HTTP 小入口 —— net.h 只提供流式 transport,这里补齐
 * 版本清单 / 版本 JSON / 加载器列表 / JRE 清单都需要的那一层(元数据层)。
 *
 * 与 net.h 的分工:
 *   net.h 管"连上去、发请求、读字节";本模块只管"把响应体读进内存 + 把状态码翻成人话"。
 *   换后端(WinHTTP / Qt / libcurl)不需要动这里,因为用的就是 sxcl_transport 函数指针。
 *
 * 语义(逐条定死,别猜):
 *   - 成功返回 SXCL_HTTP_OK,*out 是 malloc 出来的缓冲,内容后面额外补一个 NUL(方便当 C 串用),
 *     *out_len 是**正文字节数**(不含补的 NUL);调用方 free(*out)。失败时 *out=NULL、*out_len=0。
 *   - 只接受 2xx。3xx/4xx/5xx 一律失败(SXCL_HTTP_ERR_STATUS),err 里是"状态码 + 人话"
 *     ("资源不存在(HTTP 404)" / "服务器错误(HTTP 503),请稍后再试")。
 *   - 默认最大 8 MiB,超过就报 SXCL_HTTP_ERR_TOO_LARGE 而不是把内存吃光:
 *     能提前知道(Content-Length)就提前拒,不知道也在读的过程中随时掐断。
 *   - 响应体里可以有 NUL 字节(get_text 这个名字只表示"一次全拿回来";文本/二进制都走它)。
 *   - 不抛异常、不 abort:所有失败都返回负错误码 + 人话 err。
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
