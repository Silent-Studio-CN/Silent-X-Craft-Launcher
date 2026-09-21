/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── XErr → 中文人话 ── */
const char *sxcl_auth_xsts_error_text(uint64_t xerr)
{
    switch (xerr) {
        case 2148916233ULL:
            return "这个微软账号还没有 Xbox 档案。请先在 xbox.com（或任意 Xbox 应用）里登录一次、"
                   "创建 Xbox 档案，再回来登录。";
        case 2148916235ULL:
            return "这个账号所在的国家/地区不支持 Xbox Live（Xbox Live 在当地不可用），"
                   "换一个受支持地区的账号，或改用离线模式。";
        case 2148916238ULL:
            return "这是未成年（儿童）账号：需要由一个成人微软账号把它加入 Xbox 家庭组并同意，"
                   "之后才能登录。";
        case 2148916227ULL:
            return "这个账号已被 Xbox Live 封禁。";
        case 2148916229ULL:
        case 2148916236ULL:
        case 2148916237ULL:
            return "需要先完成成人验证（韩国等地区要求），请到 Xbox 官网上完成验证后再登录。";
        default:
            return NULL;
    }
}

int sxcl_auth_xsts_error_message(uint64_t xerr, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    const char *text = sxcl_auth_xsts_error_text(xerr);
    int n;
    if (text != NULL) {
        n = snprintf(out, out_len, "%s（XErr %llu）", text, (unsigned long long)xerr);
    } else {
        /* 认不出的码**原样带出**,不吞掉:用户报错时我们能拿这个数字去查 */
        n = snprintf(out, out_len,
                     "Xbox Live 拒绝了这次登录（XErr %llu，微软没有给出我们能翻译的原因；"
                     "把这个码连同日志一起反馈）",
                     (unsigned long long)xerr);
    }
    return n > 0 ? n : 0;
}

/* ISO8601("2026-02-14T12:34:56.7890123Z")→ Unix 秒。解析不出返回 -1。 */
static int64_t parse_iso8601(const char *s)
{
    if (s == NULL) {
        return -1;
    }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return -1;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) {
        return -1;
    }
    /* 公历 → 纪元日(Howard Hinnant 的 days_from_civil,整数运算,无时区表) */
    int yy = y;
    yy -= (mo <= 2) ? 1 : 0;
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (unsigned)((153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u);
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + sec;
}

/* 从 XSTS/XBL 的 JSON 里取 Token / NotAfter / uhs / xid(两跳的响应结构一样) */
static int parse_xbox_token_response(const sxcl_auth_http *r, const char *what,
                                     sxcl_auth_xbox_tokens *out, uint64_t *xerr_out,
                                     char *err, size_t err_len)
{
    if (r->body == NULL || r->body_len == 0) {
        sxcl_auth_err(err, err_len, "%s 返回了空正文（HTTP %d）", what, r->status);
        return SXCL_AUTH_ERR_JSON;
    }
    char jerr[128];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(r->body, r->body_len, jerr, sizeof(jerr));
    if (doc == NULL) {
        sxcl_auth_err(err, err_len, "%s 返回的不是合法 JSON：%s", what, jerr);
        return SXCL_AUTH_ERR_JSON;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    uint64_t xerr = 0;
    const sxcl_json_value *xv = (root != NULL) ? sxcl_json_get(root, "XErr") : NULL;
    if (xv != NULL && sxcl_json_type_of(xv) == SXCL_JSON_NUMBER) {
        xerr = (uint64_t)sxcl_json_number(xv);
    }
    if (xerr_out != NULL) {
        *xerr_out = xerr;
    }
    if (r->status != 200 || xerr != 0) {
        char msg[SXCL_AUTH_MESSAGE_MAX];
        msg[0] = '\0';
        if (xerr != 0) {
            (void)sxcl_auth_xsts_error_message(xerr, msg, sizeof(msg));
        }
        const char *upstream = (root != NULL) ? sxcl_json_get_string(root, "Message", "") : "";
        const char *redirect = (root != NULL) ? sxcl_json_get_string(root, "Redirect", "") : "";
        if (msg[0] == '\0') {
            (void)snprintf(msg, sizeof(msg), "%s 失败（HTTP %d）%s%s", what, r->status,
                           upstream[0] ? "：" : "", upstream);
        }
        if (redirect[0] != '\0') {
            char with_link[SXCL_AUTH_ERROR_MAX];
            (void)snprintf(with_link, sizeof(with_link), "%s 处理入口：%s", msg, redirect);
            sxcl_auth_err(err, err_len, "%s", with_link);
        } else {
            sxcl_auth_err(err, err_len, "%s", msg);
        }
        sxcl_json_free(doc);
        return (xerr != 0) ? SXCL_AUTH_ERR_XBOX : SXCL_AUTH_ERR_HTTP;
    }
    const char *token = sxcl_json_get_string(root, "Token", "");
    if (token[0] == '\0') {
        sxcl_json_free(doc);
        sxcl_auth_err(err, err_len, "%s 的响应里没有 Token 字段（HTTP %d）", what, r->status);
        return SXCL_AUTH_ERR_JSON;
    }
    (void)sxcl_auth_copy(out->xsts_token, sizeof(out->xsts_token), token);
    out->xsts_expires_at = parse_iso8601(sxcl_json_get_string(root, "NotAfter", ""));
    /* DisplayClaims.xui[0] = {uhs, xid, agg} */
    const sxcl_json_value *dc = sxcl_json_get(root, "DisplayClaims");
    const sxcl_json_value *xui = (dc != NULL) ? sxcl_json_get(dc, "xui") : NULL;
    const sxcl_json_value *first = (xui != NULL) ? sxcl_json_at(xui, 0) : NULL;
    if (first != NULL) {
        (void)sxcl_auth_copy(out->user_hash, sizeof(out->user_hash),
                             sxcl_json_get_string(first, "uhs", ""));
        const char *xid = sxcl_json_get_string(first, "xid", "");
        if (xid[0] != '\0') {
            out->xuid = strtoull(xid, NULL, 10);
        }
        (void)sxcl_auth_copy(out->gamertag, sizeof(out->gamertag),
                             sxcl_json_get_string(first, "gtg", ""));
    }
    sxcl_json_free(doc);
    if (out->user_hash[0] == '\0') {
        sxcl_auth_err(err, err_len, "%s 的响应里没有 uhs（DisplayClaims.xui[0].uhs 缺失）", what);
        return SXCL_AUTH_ERR_JSON;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_xbox_user_authenticate(sxcl_transport *tr, const char *ms_access_token,
                                     int64_t timeout_ms, char *xbl_token, size_t xbl_token_len,
                                     char *user_hash, size_t user_hash_len,
                                     int64_t *expires_at, char *err, size_t err_len)
{
    if (tr == NULL || ms_access_token == NULL || ms_access_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "没有微软 access_token，无法做 Xbox Live 用户认证");
        return SXCL_AUTH_ERR_ARG;
    }
    if (xbl_token == NULL || xbl_token_len == 0 || user_hash == NULL || user_hash_len == 0) {
        sxcl_auth_err(err, err_len, "内部错误:Xbox Live 用户认证的输出缓冲为空");
        return SXCL_AUTH_ERR_ARG;
    }
    if (strlen(ms_access_token) + 8u > SXCL_AUTH_TOKEN_MAX) {
        sxcl_auth_err(err, err_len, "微软 access_token 长得离谱（%llu 字节），拒绝继续",
                      (unsigned long long)strlen(ms_access_token));
        return SXCL_AUTH_ERR_ARG;
    }
    /* RpsTicket 必须是 "d=<token>"(d= 表示"这是微软账号的令牌") */
    char rps[SXCL_AUTH_TOKEN_MAX + 4];
    (void)snprintf(rps, sizeof(rps), "d=%s", ms_access_token);

    sxcl_auth_buf body;
    sxcl_auth_buf_init(&body);
    (void)sxcl_auth_buf_append(&body,
        "{\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\","
        "\"RpsTicket\":\"");
    (void)sxcl_auth_json_escape(&body, rps);
    (void)sxcl_auth_buf_append(&body, "\"},\"RelyingParty\":\"");
    (void)sxcl_auth_json_escape(&body, SXCL_AUTH_RP_XBOX);
    (void)sxcl_auth_buf_append(&body, "\",\"TokenType\":\"JWT\"}");
    sxcl_auth_secure_zero(rps, sizeof(rps));
    if (body.oom) {
        sxcl_auth_buf_free(&body);
        sxcl_auth_err(err, err_len, "内存不足（拼 Xbox Live 认证请求）");
        return SXCL_AUTH_ERR_NOMEM;
    }

    sxcl_auth_http resp;
    int rc = sxcl_auth_http_call(tr, "POST", SXCL_AUTH_URL_XBL_USER, "application/json",
                                 sxcl_auth_buf_cstr(&body), body.len, NULL, timeout_ms,
                                 &resp, err, err_len);
    sxcl_auth_secure_zero(body.data, body.cap);
    sxcl_auth_buf_free(&body);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    sxcl_auth_xbox_tokens tmp;
    memset(&tmp, 0, sizeof(tmp));
    uint64_t xerr = 0;
    rc = parse_xbox_token_response(&resp, "Xbox Live 用户认证", &tmp, &xerr, err, err_len);
    sxcl_auth_http_free(&resp);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    (void)sxcl_auth_copy(xbl_token, xbl_token_len, tmp.xsts_token);
    (void)sxcl_auth_copy(user_hash, user_hash_len, tmp.user_hash);
    if (expires_at != NULL) {
        *expires_at = tmp.xsts_expires_at;
    }
    sxcl_auth_secure_zero(&tmp, sizeof(tmp));
    return SXCL_AUTH_OK;
}

int sxcl_auth_xsts_authorize(sxcl_transport *tr, const char *xbl_token, const char *relying_party,
                             int64_t timeout_ms, sxcl_auth_xbox_tokens *out, uint64_t *xerr_out,
                             char *err, size_t err_len)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (xerr_out != NULL) {
        *xerr_out = 0;
    }
    if (tr == NULL || xbl_token == NULL || xbl_token[0] == '\0' || out == NULL) {
        sxcl_auth_err(err, err_len, "没有 Xbox Live 令牌，无法做 XSTS 授权");
        return SXCL_AUTH_ERR_ARG;
    }
    const char *rp = (relying_party != NULL && relying_party[0] != '\0') ? relying_party
                                                                          : SXCL_AUTH_RP_MINECRAFT;
    sxcl_auth_buf body;
    sxcl_auth_buf_init(&body);
    (void)sxcl_auth_buf_append(&body, "{\"Properties\":{\"SandboxId\":\"");
    (void)sxcl_auth_json_escape(&body, SXCL_AUTH_SANDBOX);
    (void)sxcl_auth_buf_append(&body, "\",\"UserTokens\":[\"");
    (void)sxcl_auth_json_escape(&body, xbl_token);
    (void)sxcl_auth_buf_append(&body, "\"]},\"RelyingParty\":\"");
    (void)sxcl_auth_json_escape(&body, rp);
    (void)sxcl_auth_buf_append(&body, "\",\"TokenType\":\"JWT\"}");
    if (body.oom) {
        sxcl_auth_buf_free(&body);
        sxcl_auth_err(err, err_len, "内存不足（拼 XSTS 请求）");
        return SXCL_AUTH_ERR_NOMEM;
    }
    sxcl_auth_http resp;
    int rc = sxcl_auth_http_call(tr, "POST", SXCL_AUTH_URL_XSTS, "application/json",
                                 sxcl_auth_buf_cstr(&body), body.len, NULL, timeout_ms,
                                 &resp, err, err_len);
    sxcl_auth_buf_free(&body);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    uint64_t xerr = 0;
    rc = parse_xbox_token_response(&resp, "XSTS 授权", out, &xerr, err, err_len);
    sxcl_auth_http_free(&resp);
    if (xerr_out != NULL) {
        *xerr_out = xerr;
    }
    return rc;
}
