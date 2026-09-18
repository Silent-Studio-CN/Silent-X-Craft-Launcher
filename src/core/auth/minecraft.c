/* Java 版链的后三跳:login_with_xbox → 权益(mcstore)→ 档案(profile)。
 *
 *   POST https://api.minecraftservices.com/authentication/login_with_xbox
 *        {"identityToken":"XBL3.0 x=<uhs>;<XSTS token>"}
 *        → {"username":"…","access_token":"…","token_type":"Bearer","expires_in":86400}
 *   GET  https://api.minecraftservices.com/entitlements/mcstore      (Bearer)
 *        → {"items":[{"name":"product_minecraft",…}],"signature":"…"}     ← 买了才有条目
 *   GET  https://api.minecraftservices.com/minecraft/profile         (Bearer)
 *        → {"id":"<32 位无横线 uuid>","name":"<玩家名>",…}
 *        → 404 = **这个账号没有 Java 版**,必须如实报错,不许拿默认名继续启动。
 *
 * 注意:第 6 跳的两条**不是**同一件事。mcstore 是"商店权益",profile 是"档案存在性";
 * 有的账号是"有档案但没有 ownership 记录"(老账号/迁移中),所以两条都查、分开报。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mc_parse_json(const sxcl_auth_http *r, const char *what, sxcl_json **out,
                         char *err, size_t err_len)
{
    *out = NULL;
    if (r->body == NULL || r->body_len == 0) {
        sxcl_auth_err(err, err_len, "%s 返回了空正文（HTTP %d）", what, r->status);
        return SXCL_AUTH_ERR_JSON;
    }
    char jerr[128];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(r->body, r->body_len, jerr, sizeof(jerr));
    if (doc == NULL) {
        sxcl_auth_err(err, err_len, "%s 返回的不是合法 JSON（HTTP %d）：%s", what, r->status, jerr);
        return SXCL_AUTH_ERR_JSON;
    }
    *out = doc;
    return SXCL_AUTH_OK;
}

/* Minecraft 服务端错误正文:{"path":…,"errorType":"NOT_FOUND","error":"NOT_FOUND","errorMessage":"…"} */
static void mc_error_text(const sxcl_auth_http *r, const char *what, char *err, size_t err_len)
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
            const char *type = sxcl_json_get_string(root, "error", "");
            (void)snprintf(detail, sizeof(detail), "%s%s%s", type, (msg[0] && type[0]) ? "：" : "", msg);
            sxcl_json_free(doc);
        }
    }
    if (r->status == 401) {
        sxcl_auth_err(err, err_len, "%s：登录凭据被拒绝（HTTP 401）%s%s", what,
                      detail[0] ? "：" : "", detail);
    } else if (r->status == 404) {
        sxcl_auth_err(err, err_len, "%s：Minecraft 服务说“没有这个东西”（HTTP 404）%s%s", what,
                      detail[0] ? "：" : "", detail);
    } else if (r->status == 429) {
        sxcl_auth_err(err, err_len, "%s：被 Minecraft 服务限流了（HTTP 429），稍后再试", what);
    } else {
        char message[SXCL_HTTP_ERROR_MAX];
        (void)sxcl_http_status_message(r->status, message, sizeof(message));
        sxcl_auth_err(err, err_len, "%s：%s%s%s", what, message, detail[0] ? "：" : "", detail);
    }
}

int sxcl_auth_minecraft_login(sxcl_transport *tr, const char *user_hash, const char *xsts_token,
                              int64_t timeout_ms, sxcl_auth_minecraft_tokens *out,
                              char *err, size_t err_len)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (tr == NULL || out == NULL || user_hash == NULL || user_hash[0] == '\0' ||
        xsts_token == NULL || xsts_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "内部错误:XSTS 结果不全（uhs 或 token 为空）");
        return SXCL_AUTH_ERR_ARG;
    }
    /* identityToken = "XBL3.0 x=<uhs>;<xsts>" —— 格式由 Minecraft 服务定死,别改动 */
    char identity[SXCL_AUTH_TOKEN_MAX + SXCL_AUTH_NAME_MAX + 16];
    const int n = snprintf(identity, sizeof(identity), "XBL3.0 x=%s;%s", user_hash, xsts_token);
    if (n < 0 || (size_t)n >= sizeof(identity)) {
        sxcl_auth_err(err, err_len, "内部错误:XSTS 令牌太长,identityToken 拼不下");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_buf body;
    sxcl_auth_buf_init(&body);
    (void)sxcl_auth_buf_append(&body, "{\"identityToken\":\"");
    (void)sxcl_auth_json_escape(&body, identity);
    (void)sxcl_auth_buf_append(&body, "\"}");
    sxcl_auth_secure_zero(identity, sizeof(identity));
    if (body.oom) {
        sxcl_auth_buf_free(&body);
        return SXCL_AUTH_ERR_NOMEM;
    }
    sxcl_auth_http resp;
    int rc = sxcl_auth_http_call(tr, "POST", SXCL_AUTH_URL_MC_LOGIN, "application/json",
                                 sxcl_auth_buf_cstr(&body), body.len, NULL, timeout_ms,
                                 &resp, err, err_len);
    sxcl_auth_secure_zero(body.data, body.cap);
    sxcl_auth_buf_free(&body);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (resp.status != 200) {
        mc_error_text(&resp, "用 Xbox 凭据换 Minecraft 令牌失败", err, err_len);
        sxcl_auth_http_free(&resp);
        return (resp.status == 429) ? SXCL_AUTH_ERR_RATE_LIMIT : SXCL_AUTH_ERR_HTTP;
    }
    sxcl_json *doc = NULL;
    rc = mc_parse_json(&resp, "Minecraft 登录", &doc, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        sxcl_auth_http_free(&resp);
        return rc;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const char *token = sxcl_json_get_string(root, "access_token", "");
    if (token[0] == '\0') {
        sxcl_json_free(doc);
        sxcl_auth_http_free(&resp);
        sxcl_auth_err(err, err_len, "Minecraft 登录响应里没有 access_token");
        return SXCL_AUTH_ERR_JSON;
    }
    (void)sxcl_auth_copy(out->access_token, sizeof(out->access_token), token);
    out->expires_at = sxcl_auth_expiry_from(root, sxcl_auth_now(), 86400);
    sxcl_json_free(doc);
    sxcl_auth_http_free(&resp);
    return SXCL_AUTH_OK;
}

int sxcl_auth_minecraft_entitlements(sxcl_transport *tr, const char *mc_access_token,
                                     int64_t timeout_ms, int *count_out, char *err, size_t err_len)
{
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (tr == NULL || mc_access_token == NULL || mc_access_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "没有 Minecraft 令牌，查不了权益");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_http resp;
    const int rc = sxcl_auth_http_call(tr, "GET", SXCL_AUTH_URL_MC_ENTITLEMENTS, NULL, NULL, 0,
                                       mc_access_token, timeout_ms, &resp, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    int result = SXCL_AUTH_OK;
    if (resp.status == 404) {
        /* 官方文档:没有所有权时 mcstore 返回 404 NOT_FOUND。这是"确实没买",不是错误。 */
        if (count_out != NULL) {
            *count_out = 0;
        }
        sxcl_auth_err(err, err_len,
                      "这个账号在 Minecraft 商店里没有任何 Java 版权益（mcstore 返回 404）");
        result = SXCL_AUTH_OK; /* 让调用方自己决定怎么说 —— 返回值区分"查到 0 条"与"查询失败" */
    } else if (resp.status != 200) {
        mc_error_text(&resp, "查询 Minecraft 权益失败", err, err_len);
        result = SXCL_AUTH_ERR_HTTP;
    } else {
        sxcl_json *doc = NULL;
        char jerr[128];
        jerr[0] = '\0';
        doc = sxcl_json_parse(resp.body, resp.body_len, jerr, sizeof(jerr));
        if (doc == NULL) {
            sxcl_auth_err(err, err_len, "权益接口返回的不是合法 JSON：%s", jerr);
            result = SXCL_AUTH_ERR_JSON;
        } else {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const sxcl_json_value *items = (root != NULL) ? sxcl_json_get(root, "items") : NULL;
            const size_t count = (items != NULL) ? sxcl_json_size(items) : 0;
            if (count_out != NULL) {
                *count_out = (int)count;
            }
            if (count == 0) {
                sxcl_auth_err(err, err_len,
                              "这个账号没有 Java 版权益（mcstore 的 items 是空的）");
            }
            sxcl_json_free(doc);
        }
    }
    sxcl_auth_http_free(&resp);
    return result;
}

int sxcl_auth_minecraft_profile(sxcl_transport *tr, const char *mc_access_token,
                                int64_t timeout_ms, char *uuid, size_t uuid_len,
                                char *name, size_t name_len, char *err, size_t err_len)
{
    if (uuid != NULL && uuid_len > 0) uuid[0] = '\0';
    if (name != NULL && name_len > 0) name[0] = '\0';
    if (tr == NULL || mc_access_token == NULL || mc_access_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "没有 Minecraft 令牌，查不了档案");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_http resp;
    const int rc = sxcl_auth_http_call(tr, "GET", SXCL_AUTH_URL_MC_PROFILE, NULL, NULL, 0,
                                       mc_access_token, timeout_ms, &resp, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    int result = SXCL_AUTH_OK;
    if (resp.status == 404) {
        /* 关键分支:404 = 这个账号**没有 Java 版档案**。要如实报,不许拿默认名继续启动。 */
        sxcl_auth_err(err, err_len,
                      "这个微软账号没有 Minecraft Java 版档案：登录是成功的，但账号没有购买/绑定 "
                      "Java 版（profile 接口返回 404）。请用已购买 Java 版的账号登录。");
        result = SXCL_AUTH_ERR_NO_PROFILE;
    } else if (resp.status != 200) {
        mc_error_text(&resp, "查询 Minecraft 档案失败", err, err_len);
        result = SXCL_AUTH_ERR_HTTP;
    } else {
        sxcl_json *doc = NULL;
        char jerr[128];
        jerr[0] = '\0';
        doc = sxcl_json_parse(resp.body, resp.body_len, jerr, sizeof(jerr));
        if (doc == NULL) {
            sxcl_auth_err(err, err_len, "档案接口返回的不是合法 JSON：%s", jerr);
            result = SXCL_AUTH_ERR_JSON;
        } else {
            const sxcl_json_value *root = sxcl_json_root(doc);
            const char *id = sxcl_json_get_string(root, "id", "");
            const char *nm = sxcl_json_get_string(root, "name", "");
            if (uuid != NULL && uuid_len > 0) {
                (void)sxcl_auth_copy(uuid, uuid_len, id);
            }
            if (name != NULL && name_len > 0) {
                (void)sxcl_auth_copy(name, name_len, nm);
            }
            if (nm[0] == '\0') {
                /* 200 但 name 为空:同样是"没有 Java 版"(极少数账号会这样返回),
                 * 依然如实报错 —— 空名字进游戏只会更难查。 */
                sxcl_auth_err(err, err_len,
                              "这个微软账号没有 Minecraft Java 版档案（profile 返回的 name 是空的）："
                              "登录成功，但账号没有 Java 版。");
                result = SXCL_AUTH_ERR_NO_PROFILE;
            }
            sxcl_json_free(doc);
        }
    }
    sxcl_auth_http_free(&resp);
    return result;
}
