/* 登录链的统一 HTTP 出口 —— 只做三件事:发请求(可带 Bearer)、把状态码与正文一起拿回来、
 * 把 OAuth 的错误 JSON 翻成人话。
 *
 * 为什么要"非 2xx 也读正文":微软的 token/devicecode 端点把**正常流程控制**放在 4xx 里 ——
 *   400 {"error":"authorization_pending"}  用户还没在浏览器里点同意,继续轮询
 *   400 {"error":"slow_down"}              轮太快了,间隔 +5 秒
 *   400 {"error":"expired_token"}          设备码过期,重来
 * 用"只认 2xx"的 sxcl_http_get_text 会把这些全变成"服务器返回了意外状态(HTTP 400)",
 * 轮询逻辑就没法写了。所以这里走 accept_error_status=1 的路径。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sxcl_auth_http_free(sxcl_auth_http *r)
{
    if (r == NULL) {
        return;
    }
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->status = 0;
    r->retry_after_ms = -1;
    r->content_type[0] = '\0';
}

typedef struct header_sink {
    sxcl_auth_http *out;
} header_sink;

static void on_header_cb(void *userdata, const char *name, const char *value)
{
    header_sink *sink = (header_sink *)userdata;
    if (sink == NULL || sink->out == NULL || name == NULL || value == NULL) {
        return;
    }
    if (sxcl_auth_iequal(name, "Retry-After")) {
        /* 只要秒数形式(HTTP-date 形式对登录链没意义,直接忽略) */
        char *end = NULL;
        const long secs = strtol(value, &end, 10);
        if (end != value && secs >= 0 && secs <= 86400) {
            sink->out->retry_after_ms = (int64_t)secs * 1000;
        }
    } else if (sxcl_auth_iequal(name, "Content-Type")) {
        (void)sxcl_auth_copy(sink->out->content_type, sizeof(sink->out->content_type), value);
    }
}

int sxcl_auth_http_call(sxcl_transport *tr, const char *method, const char *url,
                        const char *content_type, const char *body, size_t body_len,
                        const char *bearer, int64_t timeout_ms,
                        sxcl_auth_http *out, char *err, size_t err_len)
{
    if (tr == NULL || url == NULL || out == NULL) {
        sxcl_auth_err(err, err_len, "内部错误:HTTP 调用参数为空");
        return SXCL_AUTH_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->retry_after_ms = -1;

    const char *headers[4];
    char ct_header[192];
    char auth_header[SXCL_AUTH_TOKEN_MAX + 32];
    int hn = 0;
    if (content_type != NULL && content_type[0] != '\0') {
        (void)snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);
        headers[hn++] = ct_header;
    }
    if (bearer != NULL && bearer[0] != '\0') {
        if (strlen(bearer) + 32u > sizeof(auth_header)) {
            sxcl_auth_err(err, err_len, "内部错误:令牌太长,放不下 Authorization 头");
            return SXCL_AUTH_ERR_ARG;
        }
        (void)snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", bearer);
        headers[hn++] = auth_header;
    }
    headers[hn++] = "Accept: application/json";
    headers[hn] = NULL;

    sxcl_http_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = timeout_ms;
    opts.accept_error_status = 1; /* 见文件头:4xx 的正文是流程控制信息 */
    opts.status_out = &out->status;
    header_sink sink;
    sink.out = out;
    opts.on_header = &on_header_cb;
    opts.header_userdata = &sink;
    /* 方法必须真的传下去:权益/档案是 GET,把 GET 发成 POST 会被服务端拒(实测夹具测试抓到过) */
    opts.method = (method != NULL && method[0] != '\0') ? method : "POST";

    char *text = NULL;
    size_t text_len = 0;
    char herr[SXCL_HTTP_ERROR_MAX];
    herr[0] = '\0';
    const int rc = sxcl_http_post_text_ex(tr, url, headers, body, body_len, &opts, &text, &text_len,
                                          herr, sizeof(herr));
    if (rc != SXCL_HTTP_OK) {
        if (rc == SXCL_HTTP_ERR_NET) {
            sxcl_auth_err(err, err_len, "连不上服务器（网络/DNS/TLS/超时）：%s", url);
            return SXCL_AUTH_ERR_NET;
        }
        if (rc == SXCL_HTTP_ERR_NOMEM) {
            sxcl_auth_err(err, err_len, "内存不足（读取 %s 的响应时）", url);
            return SXCL_AUTH_ERR_NOMEM;
        }
        sxcl_auth_err(err, err_len, "%s", herr[0] ? herr : "请求失败");
        return SXCL_AUTH_ERR_HTTP;
    }
    out->body = text;
    out->body_len = text_len;

    return SXCL_AUTH_OK;
}

int64_t sxcl_auth_expiry_from(const sxcl_json_value *obj, int64_t now, int64_t def_seconds)
{
    int64_t secs = def_seconds;
    if (obj != NULL) {
        const sxcl_json_value *v = sxcl_json_get(obj, "expires_in");
        if (v != NULL && sxcl_json_type_of(v) == SXCL_JSON_NUMBER) {
            const double d = sxcl_json_number(v);
            if (d > 0.0 && d < 315360000.0) { /* 10 年上限:防脏数据把过期时间算到天上去 */
                secs = (int64_t)d;
            }
        }
    }
    if (secs <= 0) {
        secs = def_seconds;
    }
    return now + secs;
}

/* OAuth 错误 → 人话(认得的是"流程语义",不是微软文案) */
int sxcl_auth_oauth_error(const sxcl_auth_http *r, char *raw_error, size_t raw_error_len,
                          char *err, size_t err_len)
{
    if (raw_error != NULL && raw_error_len > 0) {
        raw_error[0] = '\0';
    }
    if (r == NULL) {
        return SXCL_AUTH_ERR_HTTP;
    }
    char code[128];
    code[0] = '\0';
    char desc[SXCL_AUTH_ERROR_MAX];
    desc[0] = '\0';
    if (r->body != NULL && r->body_len > 0) {
        char jerr[128];
        jerr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(r->body, r->body_len, jerr, sizeof(jerr));
        if (doc != NULL) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            if (root != NULL) {
                (void)sxcl_auth_copy(code, sizeof(code), sxcl_json_get_string(root, "error", ""));
                (void)sxcl_auth_copy(desc, sizeof(desc),
                                     sxcl_json_get_string(root, "error_description", ""));
            }
            sxcl_json_free(doc);
        }
    }
    if (raw_error != NULL && raw_error_len > 0) {
        (void)sxcl_auth_copy(raw_error, raw_error_len, code);
    }

    /* 先给"人话骨架",再把微软的原文附在后面(原文里常有 AADSTS 码,排查时有用) */
    const char *head = NULL;
    int rc = SXCL_AUTH_ERR_HTTP;
    if (strcmp(code, "authorization_pending") == 0) {
        head = "用户还没在浏览器里完成授权（继续等待）";
        rc = SXCL_AUTH_ERR_TIMEOUT;
    } else if (strcmp(code, "slow_down") == 0) {
        head = "轮询太快了，需要加大间隔";
        rc = SXCL_AUTH_ERR_RATE_LIMIT;
    } else if (strcmp(code, "expired_token") == 0) {
        head = "设备码已过期，请重新发起登录";
        rc = SXCL_AUTH_ERR_EXPIRED;
    } else if (strcmp(code, "authorization_declined") == 0) {
        head = "用户在授权页面上点了拒绝";
        rc = SXCL_AUTH_ERR_DENIED;
    } else if (strcmp(code, "bad_verification_code") == 0) {
        head = "设备码不对（可能已经用过或抄错了）";
        rc = SXCL_AUTH_ERR_DENIED;
    } else if (strcmp(code, "invalid_grant") == 0) {
        head = "授权码/刷新令牌已失效（过期、被撤销，或已被用过一次）";
        rc = SXCL_AUTH_ERR_EXPIRED;
    } else if (strcmp(code, "interaction_required") == 0 ||
               strcmp(code, "login_required") == 0 ||
               strcmp(code, "consent_required") == 0) {
        head = "需要用户重新登录一次（免密续期已经不被接受）";
        rc = SXCL_AUTH_ERR_EXPIRED;
    } else if (strcmp(code, "invalid_client") == 0 ||
               strcmp(code, "unauthorized_client") == 0) {
        head = "client_id 不被接受（请检查是不是填错了/应用没注册成公共客户端）";
        rc = SXCL_AUTH_ERR_ARG;
    } else if (strcmp(code, "invalid_request") == 0) {
        head = "请求被微软拒绝（参数不合法）";
        rc = SXCL_AUTH_ERR_ARG;
    } else if (strcmp(code, "temporarily_unavailable") == 0) {
        head = "微软登录服务暂时不可用，稍后再试";
        rc = SXCL_AUTH_ERR_HTTP;
    } else if (r->status == 429) {
        head = "被限流了（请求太频繁）";
        rc = SXCL_AUTH_ERR_RATE_LIMIT;
    }

    if (head == NULL) {
        if (r->status >= 500) {
            head = "微软登录服务返回了服务器错误";
        } else {
            head = "微软登录接口返回了错误";
        }
    }
    if (desc[0] != '\0' && code[0] != '\0') {
        sxcl_auth_err(err, err_len, "%s：%s（%s，HTTP %d）", head, desc, code, r->status);
    } else if (code[0] != '\0') {
        sxcl_auth_err(err, err_len, "%s（%s，HTTP %d）", head, code, r->status);
    } else {
        sxcl_auth_err(err, err_len, "%s（HTTP %d）", head, r->status);
    }
    return rc;
}
