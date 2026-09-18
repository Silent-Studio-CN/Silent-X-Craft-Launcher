/* 基岩版链:与 Java 版**同源不同链**。
 *
 *   微软 access_token
 *     → XBL 用户认证(同一跳,可以复用)
 *     → XSTS,**中继方换成 https://multiplayer.minecraft.net/**(必须重新要一次,不能拿 Java 那份)
 *     → POST https://multiplayer.minecraft.net/authentication
 *          {"identityPublicKey":"<base64 P-384 未压缩公钥>","certificate":null,
 *           "token":"XBL3.0 x=<uhs>;<XSTS token>"}
 *        → {"chain":["<JWT>", …]}  ← 这就是进官方基岩客户端要用的证书链
 *
 * 权益为什么必须分开谈:
 *   Java 版和基岩版是**两次独立购买**。买了 Java 不代表有基岩(反之亦然)。
 *   基岩这边没有 mcstore 那样的公开权益接口:能不能拿到 chain 本身就是判据 ——
 *   账号没买基岩时 multiplayer.minecraft.net 会拒绝(常见 403，错误正文里带原因)。
 *   所以我们把 entitlement_checked/entitled 单独记在 bedrock 段里,不去看 Java 段的结论。
 *
 * 诚实边界(写在文档里的同一条):
 *   这条链能拿到 chain,但**把 chain 送进官方基岩客户端**是另一件事 —— 客户端的身份
 *   私钥签名后续的握手包,而安卓上的官方客户端不接受外来的 chain(签名/密钥库都不对)。
 *   本模块只负责"把链取回来",不假装能替官方客户端登录。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void bedrock_error_text(const sxcl_auth_http *r, char *err, size_t err_len)
{
    char detail[SXCL_AUTH_ERROR_MAX];
    detail[0] = '\0';
    if (r->body != NULL && r->body_len > 0) {
        char jerr[64];
        jerr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(r->body, r->body_len, jerr, sizeof(jerr));
        if (doc != NULL) {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const char *msg = sxcl_json_get_string(root, "errorMessage", "");
            if (msg[0] == '\0') {
                msg = sxcl_json_get_string(root, "message", "");
            }
            if (msg[0] == '\0') {
                msg = sxcl_json_get_string(root, "error", "");
            }
            (void)sxcl_auth_copy(detail, sizeof(detail), msg);
            sxcl_json_free(doc);
        }
        if (detail[0] == '\0' && r->body_len < 200u) {
            (void)sxcl_auth_copy(detail, sizeof(detail), r->body);
        }
    }
    if (r->status == 403) {
        /* 最常见就是"这个账号没有基岩版权益",但也有可能是区域/策略限制 —— 两种都说清楚 */
        sxcl_auth_err(err, err_len,
                      "基岩版联机认证被拒绝（HTTP 403）：常见原因是这个微软账号**没有基岩版"
                      "(Minecraft for Windows/主机版)的权益** —— Java 版买了不等于基岩版买了，"
                      "两者是分开购买的。%s%s",
                      detail[0] ? "服务端说明：" : "", detail);
    } else if (r->status == 401) {
        sxcl_auth_err(err, err_len, "基岩版联机认证被拒绝（HTTP 401）：XSTS 或身份公钥不被接受。%s%s",
                      detail[0] ? "服务端说明：" : "", detail);
    } else if (r->status == 429) {
        sxcl_auth_err(err, err_len, "基岩版联机认证被限流（HTTP 429），稍后再试");
    } else {
        char message[SXCL_HTTP_ERROR_MAX];
        (void)sxcl_http_status_message(r->status, message, sizeof(message));
        sxcl_auth_err(err, err_len, "基岩版联机认证失败：%s%s%s", message,
                      detail[0] ? "：" : "", detail);
    }
}

int sxcl_auth_bedrock_authorize(sxcl_transport *tr, const sxcl_auth_opts *opts,
                                const char *ms_access_token, sxcl_auth_xbox_tokens *xbox_out,
                                sxcl_auth_bedrock_tokens *out, char *err, size_t err_len)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (tr == NULL || out == NULL || ms_access_token == NULL || ms_access_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "没有微软 access_token，跑不了基岩链");
        return SXCL_AUTH_ERR_ARG;
    }
    const int64_t timeout =
        (opts != NULL && opts->http_timeout_ms > 0) ? opts->http_timeout_ms : 30000;

    /* 1) 身份密钥:调用方给了就用它(离线/自管密钥的场景),没给就现场生成一对 */
    if (opts != NULL && opts->bedrock_public_key != NULL && opts->bedrock_public_key[0] != '\0') {
        (void)sxcl_auth_copy(out->identity_public_key, sizeof(out->identity_public_key),
                             opts->bedrock_public_key);
    } else {
        unsigned char secret[48];
        const int krc = sxcl_auth_ec384_keygen(out->identity_public_key,
                                               sizeof(out->identity_public_key), secret);
        sxcl_auth_secure_zero(secret, sizeof(secret));
        if (krc != SXCL_AUTH_OK) {
            sxcl_auth_err(err, err_len, "生成 P-384 身份密钥失败（系统随机数不可用？）");
            return krc;
        }
    }

    /* 2) XBL 用户认证(与 Java 链同一跳;这里自己拿一次,不依赖调用方传进来的旧值) */
    char xbl[SXCL_AUTH_TOKEN_MAX];
    char uhs[SXCL_AUTH_NAME_MAX];
    int64_t xbl_expires = 0;
    memset(xbl, 0, sizeof(xbl));
    memset(uhs, 0, sizeof(uhs));
    if (opts != NULL && opts->cb.on_status != NULL) {
        opts->cb.on_status(opts->cb.userdata, "基岩链：Xbox Live 用户认证…");
    }
    int rc = sxcl_auth_xbox_user_authenticate(tr, ms_access_token, timeout, xbl, sizeof(xbl), uhs,
                                              sizeof(uhs), &xbl_expires, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        sxcl_auth_secure_zero(xbl, sizeof(xbl));
        return rc;
    }

    /* 3) XSTS:**中继方换成 multiplayer.minecraft.net** —— 这一步决定了拿到的授权给谁用 */
    if (opts != NULL && opts->cb.on_status != NULL) {
        opts->cb.on_status(opts->cb.userdata, "基岩链：XSTS 授权（中继方 = multiplayer.minecraft.net）…");
    }
    sxcl_auth_xbox_tokens xbox;
    memset(&xbox, 0, sizeof(xbox));
    uint64_t xerr = 0;
    rc = sxcl_auth_xsts_authorize(tr, xbl, SXCL_AUTH_RP_BEDROCK, timeout, &xbox, &xerr, err, err_len);
    sxcl_auth_secure_zero(xbl, sizeof(xbl));
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (xbox_out != NULL) {
        *xbox_out = xbox;
    }

    /* 4) /authentication:换证书链 */
    if (opts != NULL && opts->cb.on_status != NULL) {
        opts->cb.on_status(opts->cb.userdata, "基岩链：换取联机证书链…");
    }
    char identity[SXCL_AUTH_TOKEN_MAX + SXCL_AUTH_NAME_MAX + 16];
    const int n = snprintf(identity, sizeof(identity), "XBL3.0 x=%s;%s", xbox.user_hash,
                           xbox.xsts_token);
    if (n < 0 || (size_t)n >= sizeof(identity)) {
        sxcl_auth_err(err, err_len, "XSTS 令牌太长,拼不下 identityToken");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_buf body;
    sxcl_auth_buf_init(&body);
    (void)sxcl_auth_buf_append(&body, "{\"identityPublicKey\":\"");
    (void)sxcl_auth_json_escape(&body, out->identity_public_key);
    (void)sxcl_auth_buf_append(&body, "\",\"certificate\":null,\"token\":\"");
    (void)sxcl_auth_json_escape(&body, identity);
    (void)sxcl_auth_buf_append(&body, "\"}");
    sxcl_auth_secure_zero(identity, sizeof(identity));
    if (body.oom) {
        sxcl_auth_buf_free(&body);
        return SXCL_AUTH_ERR_NOMEM;
    }
    sxcl_auth_http resp;
    rc = sxcl_auth_http_call(tr, "POST", SXCL_AUTH_URL_BEDROCK_AUTH, "application/json",
                             sxcl_auth_buf_cstr(&body), body.len, NULL, timeout, &resp, err, err_len);
    sxcl_auth_buf_free(&body);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (resp.status != 200) {
        bedrock_error_text(&resp, err, err_len);
        out->entitlement_checked = 1;
        out->entitled = 0;
        /* 403 = 十有八九没买基岩版:用"没有权益"这个码,让调用方能和网络错误区分开 */
        const int code = (resp.status == 403) ? SXCL_AUTH_ERR_NO_ENTITLE
                                              : ((resp.status == 429) ? SXCL_AUTH_ERR_RATE_LIMIT
                                                                      : SXCL_AUTH_ERR_HTTP);
        sxcl_auth_http_free(&resp);
        return code;
    }
    if (resp.body == NULL || resp.body_len == 0) {
        sxcl_auth_http_free(&resp);
        sxcl_auth_err(err, err_len, "基岩版认证返回了空正文");
        return SXCL_AUTH_ERR_JSON;
    }
    char jerr[128];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(resp.body, resp.body_len, jerr, sizeof(jerr));
    if (doc == NULL) {
        sxcl_auth_http_free(&resp);
        sxcl_auth_err(err, err_len, "基岩版认证返回的不是合法 JSON：%s", jerr);
        return SXCL_AUTH_ERR_JSON;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *chain = (root != NULL) ? sxcl_json_get(root, "chain") : NULL;
    const size_t count = (chain != NULL) ? sxcl_json_size(chain) : 0;
    if (count == 0) {
        sxcl_json_free(doc);
        sxcl_auth_http_free(&resp);
        sxcl_auth_err(err, err_len, "基岩版认证没有返回证书链（chain 为空）");
        return SXCL_AUTH_ERR_JSON;
    }
    for (size_t i = 0; i < count && i < 3u; ++i) {
        const sxcl_json_value *item = sxcl_json_at(chain, i);
        if (item != NULL && sxcl_json_type_of(item) == SXCL_JSON_STRING) {
            (void)sxcl_auth_copy(out->chain[out->chain_count], SXCL_AUTH_TOKEN_MAX,
                                 sxcl_json_string(item));
            out->chain_count++;
        }
    }
    sxcl_json_free(doc);
    sxcl_auth_http_free(&resp);
    /* 拿到了链 = 这个账号有基岩版权益(服务端就是在这一步校验所有权的) */
    out->entitlement_checked = 1;
    out->entitled = 1;
    return SXCL_AUTH_OK;
}
