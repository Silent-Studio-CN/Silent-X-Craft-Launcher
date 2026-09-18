/* 微软 OAuth2 三条路:授权码(+PKCE+环回)、refresh_token 续期、设备码轮询。
 *
 * 端点(租户段可配置,见 auth.h):
 *   GET  {authority}/{tenant}/oauth2/v2.0/authorize  ?response_type=code&client_id=…
 *         &redirect_uri=http://localhost:<随机端口>/callback&scope=XboxLive.signin offline_access
 *         &code_challenge=…&code_challenge_method=S256&state=…
 *   POST {authority}/{tenant}/oauth2/v2.0/token       (application/x-www-form-urlencoded)
 *   POST {authority}/{tenant}/oauth2/v2.0/devicecode  (application/x-www-form-urlencoded)
 *
 * 设备码流的流程控制全在 4xx 正文里(见 http_auth.c 的说明):
 *   authorization_pending → 继续等;slow_down → 间隔 +5s;expired_token → 重新登录。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double g_poll_scale = 1.0;

void sxcl_auth_set_poll_scale_for_test(double scale)
{
    g_poll_scale = (scale > 0.0) ? scale : 1.0;
}

/* ── AADSTS 码 → 中文排查建议 ──
 * 这些码藏在 error_description 里("AADSTS50059: No tenant-identifying information found…"),
 * 而它们是**配置类**问题(应用注册/租户类型),不是代码问题 —— 必须一眼能看出来,
 * 否则复测的人会以为是登录实现写坏了。 */
const char *sxcl_auth_aadsts_hint(int64_t aadsts_code)
{
    switch (aadsts_code) {
        case 50059:
            return "应用被注册成了“仅我的组织（单租户）”，而请求走的是 consumers/common 端点。"
                   "去 Azure 门户 → 应用注册 → 身份验证 → 受支持的帐户类型，改成"
                   "“任何组织目录中的帐户和个人 Microsoft 帐户”或“仅个人 Microsoft 帐户”，"
                   "并确认租户段（默认 consumers）与所选类型一致。";
        case 500011:
            return "在目标租户里找不到这个应用（或应用与租户段对不上）：单租户应用不能用 consumers 端点。"
                   "把受支持的帐户类型改成含个人帐户，或把租户段设成应用所在租户的 id/域名。";
        case 700016:
            return "这个租户里没有该 client_id 的应用：要么 client_id 抄错了，要么应用注册在另一个租户。"
                   "核对 Azure 里的“应用程序(客户端) ID”，必要时用 SXCL_AUTH_CLIENT_ID 覆盖。";
        case 50020:
            return "该 Microsoft 帐户不属于这个租户：单租户应用只能用本租户的账号。"
                   "改用 consumers/common 端点（或换成个人帐户可用的应用类型）。";
        case 65001:
            return "还没有同意所需的权限：在授权页面上点同意，或让租户管理员授予管理员同意。";
        case 65004:
            return "用户在同意页面上点了拒绝：重新登录并同意 XboxLive.signin 权限。";
        case 70008:
        case 700082:
            return "刷新令牌已过期（微软的 refresh token 有有效期，长期不用就会失效）：重新登录一次即可。";
        case 50173:
            return "授权已被撤销（改密码、管理员撤销、或令牌被回收）：需要重新登录。";
        case 70002:
            return "应用没有被标记成“公共客户端/移动应用”，所以设备码流与环回重定向流都被拒。"
                   "去 Azure 门户 → 应用注册 → 身份验证 → 添加平台 → “移动和桌面应用程序”"
                   "（勾上 http://localhost），并把“允许公共客户端流”设为是；"
                   "或在清单里设 \"allowPublicClient\": true 与 \"publicClient\": { \"redirectUris\": [ \"http://localhost\" ] }。";
        case 7000012:
            return "这个 refresh token 是用**另一个租户段**换来的：登录与续期必须用同一个租户段"
                   "（consumers 登录就用 consumers 续期）。检查 --tenant / SXCL_AUTH_TENANT / auth.tenant 有没有被改过。";
        case 7000215:
            return "client_secret 不对：公共客户端不该带 secret（我们也不带）。如果你在 Azure 里给这个应用建了 secret，"
                   "说明它被当成了机密客户端 —— 删掉 secret 并按公共客户端重新配置。";
        case 50089:
            return "请求里的 redirect_uri 与注册的不一致：环回流要在“移动和桌面应用程序”平台下注册 http://localhost。";
        case 9002325:
        case 9002313:
            return "公共客户端必须用 PKCE 且不能带 client_secret：确认请求里带了 code_challenge(S256)。";
        case 900971:
            return "重定向地址没注册：在 Azure 应用注册的“移动和桌面应用程序”平台里加上 http://localhost。";
        case 1001:
        case 1003:
            return "设备码流的请求参数组合不合法（常见：把 client_id 与 scope 配错了）。";
        default:
            return NULL;
    }
}

int64_t sxcl_auth_aadsts_extract(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    const char *p = text;
    while ((p = strstr(p, "AADSTS")) != NULL) {
        p += 6;
        int64_t v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 9) {
            v = v * 10 + (*p - '0');
            ++p;
            ++digits;
        }
        if (digits >= 4) {
            return v; /* AADSTS 码至少 4 位,少于此的多半是别的文本 */
        }
    }
    return 0;
}

/* ── 端点拼接 ── */
int sxcl_auth_endpoint_url(sxcl_auth_endpoint endpoint, const char *tenant,
                           char *out, size_t out_len)
{
    const char *t = (tenant != NULL && tenant[0] != '\0') ? tenant : SXCL_AUTH_TENANT_DEFAULT;
    const char *tail = NULL;
    switch (endpoint) {
        case SXCL_AUTH_EP_AUTHORIZE: tail = "/authorize"; break;
        case SXCL_AUTH_EP_TOKEN: tail = "/token"; break;
        case SXCL_AUTH_EP_DEVICECODE: tail = "/devicecode"; break;
        default: return SXCL_AUTH_ERR_ARG;
    }
    if (sxcl_auth_tenant_valid(t) != 1) {
        return SXCL_AUTH_ERR_ARG;
    }
    const int n = snprintf(out, out_len, "%s/%s/oauth2/v2.0%s", SXCL_AUTH_AUTHORITY_HOST, t, tail);
    if (n < 0 || (size_t)n >= out_len) {
        return SXCL_AUTH_ERR_ARG;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_tenant_valid(const char *tenant)
{
    if (tenant == NULL || tenant[0] == '\0' || strlen(tenant) >= SXCL_AUTH_TENANT_MAX) {
        return 0;
    }
    for (const char *p = tenant; *p != 0; ++p) {
        const char c = *p;
        const int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       c == '.' || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

int sxcl_auth_client_id_valid(const char *client_id)
{
    if (client_id == NULL) {
        return 0;
    }
    const size_t n = strlen(client_id);
    if (n < 8u || n >= SXCL_AUTH_CLIENT_ID_MAX) {
        return 0;
    }
    for (const char *p = client_id; *p != 0; ++p) {
        const char c = *p;
        const int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       c == '.' || c == '_' || c == '~' || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* ── 授权码 URL ── */
int sxcl_auth_build_authorize_url(const char *client_id, const char *redirect_uri,
                                  const char *code_challenge, const char *state,
                                  char *out, size_t out_len, char *err, size_t err_len)
{
    return sxcl_auth_build_authorize_url_tenant(client_id, SXCL_AUTH_TENANT_DEFAULT, redirect_uri,
                                                code_challenge, state, out, out_len, err, err_len);
}

int sxcl_auth_build_authorize_url_tenant(const char *client_id, const char *tenant,
                                         const char *redirect_uri, const char *code_challenge,
                                         const char *state, char *out, size_t out_len,
                                         char *err, size_t err_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    out[0] = '\0';
    if (!sxcl_auth_client_id_valid(client_id)) {
        sxcl_auth_err(err, err_len, "client_id 不合法（只允许字母数字与 . _ ~ -，长度 8..127）");
        return SXCL_AUTH_ERR_ARG;
    }
    if (redirect_uri == NULL || redirect_uri[0] == '\0') {
        sxcl_auth_err(err, err_len, "redirect_uri 为空（环回服务器没起来？）");
        return SXCL_AUTH_ERR_ARG;
    }
    if (code_challenge == NULL || code_challenge[0] == '\0') {
        sxcl_auth_err(err, err_len, "code_challenge 为空（PKCE 没生成？）");
        return SXCL_AUTH_ERR_ARG;
    }
    char base[256];
    if (sxcl_auth_endpoint_url(SXCL_AUTH_EP_AUTHORIZE, tenant, base, sizeof(base)) != SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len, "租户段不合法：%s", tenant != NULL ? tenant : "(空)");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_buf b;
    sxcl_auth_buf_init(&b);
    (void)sxcl_auth_buf_append(&b, base);
    (void)sxcl_auth_buf_append(&b, "?client_id=");
    (void)sxcl_auth_buf_append(&b, client_id);
    (void)sxcl_auth_buf_append(&b, "&response_type=code&response_mode=query&redirect_uri=");
    (void)sxcl_auth_url_encode(&b, redirect_uri);
    (void)sxcl_auth_buf_append(&b, "&scope=");
    (void)sxcl_auth_url_encode(&b, SXCL_AUTH_SCOPE);
    (void)sxcl_auth_buf_append(&b, "&code_challenge=");
    (void)sxcl_auth_buf_append(&b, code_challenge);
    (void)sxcl_auth_buf_append(&b, "&code_challenge_method=S256&prompt=select_account");
    if (state != NULL && state[0] != '\0') {
        (void)sxcl_auth_buf_append(&b, "&state=");
        (void)sxcl_auth_buf_append(&b, state);
    }
    if (b.oom || b.len + 1u > out_len) {
        sxcl_auth_buf_free(&b);
        sxcl_auth_err(err, err_len, "授权 URL 太长，放不下（缓冲 %llu 字节）",
                      (unsigned long long)out_len);
        return SXCL_AUTH_ERR_ARG;
    }
    (void)sxcl_auth_copy(out, out_len, sxcl_auth_buf_cstr(&b));
    sxcl_auth_buf_free(&b);
    return SXCL_AUTH_OK;
}

/* ── token 响应解析 ── */
static int parse_token_response(const sxcl_auth_http *r, sxcl_auth_ms_tokens *out,
                               char *err, size_t err_len, int64_t now)
{
    if (r == NULL || r->body == NULL || r->body_len == 0) {
        sxcl_auth_err(err, err_len, "登录接口返回了空正文（HTTP %d）", r != NULL ? r->status : 0);
        return SXCL_AUTH_ERR_JSON;
    }
    char jerr[128];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(r->body, r->body_len, jerr, sizeof(jerr));
    if (doc == NULL) {
        sxcl_auth_err(err, err_len, "登录接口返回的不是合法 JSON（HTTP %d）：%s", r->status, jerr);
        return SXCL_AUTH_ERR_JSON;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    if (root == NULL) {
        sxcl_json_free(doc);
        sxcl_auth_err(err, err_len, "登录接口返回了空 JSON（HTTP %d）", r->status);
        return SXCL_AUTH_ERR_JSON;
    }
    const char *access = sxcl_json_get_string(root, "access_token", "");
    if (access[0] == '\0') {
        sxcl_json_free(doc);
        char raw[128];
        (void)sxcl_auth_oauth_error(r, raw, sizeof(raw), err, err_len);
        return SXCL_AUTH_ERR_JSON;
    }
    (void)sxcl_auth_copy(out->access_token, sizeof(out->access_token), access);
    (void)sxcl_auth_copy(out->refresh_token, sizeof(out->refresh_token),
                         sxcl_json_get_string(root, "refresh_token", ""));
    (void)sxcl_auth_copy(out->id_token, sizeof(out->id_token),
                         sxcl_json_get_string(root, "id_token", ""));
    (void)sxcl_auth_copy(out->token_type, sizeof(out->token_type),
                         sxcl_json_get_string(root, "token_type", "Bearer"));
    (void)sxcl_auth_copy(out->scope, sizeof(out->scope), sxcl_json_get_string(root, "scope", ""));
    out->issued_at = now;
    out->expires_at = sxcl_auth_expiry_from(root, now, 3600);
    sxcl_json_free(doc);
    return SXCL_AUTH_OK;
}

/* ── 授权码换 token ── */
int sxcl_auth_exchange_code(sxcl_transport *tr, const char *client_id, const char *code,
                            const char *redirect_uri, const char *code_verifier,
                            int64_t timeout_ms, sxcl_auth_ms_tokens *out,
                            char *err, size_t err_len)
{
    return sxcl_auth_exchange_code_tenant(tr, client_id, SXCL_AUTH_TENANT_DEFAULT, code,
                                          redirect_uri, code_verifier, timeout_ms, out, err, err_len);
}

int sxcl_auth_exchange_code_tenant(sxcl_transport *tr, const char *client_id, const char *tenant,
                                   const char *code, const char *redirect_uri,
                                   const char *code_verifier, int64_t timeout_ms,
                                   sxcl_auth_ms_tokens *out, char *err, size_t err_len)
{
    if (tr == NULL || out == NULL || code == NULL || code[0] == '\0') {
        sxcl_auth_err(err, err_len, "内部错误:换 token 的参数不全（授权码为空？）");
        return SXCL_AUTH_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    char url[384];
    if (sxcl_auth_endpoint_url(SXCL_AUTH_EP_TOKEN, tenant, url, sizeof(url)) != SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len, "租户段不合法：%s", tenant != NULL ? tenant : "(空)");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_buf form;
    sxcl_auth_buf_init(&form);
    (void)sxcl_auth_form_append(&form, "client_id", client_id);
    (void)sxcl_auth_form_append(&form, "grant_type", "authorization_code");
    (void)sxcl_auth_form_append(&form, "code", code);
    (void)sxcl_auth_form_append(&form, "redirect_uri", redirect_uri);
    (void)sxcl_auth_form_append(&form, "code_verifier", code_verifier);
    (void)sxcl_auth_form_append(&form, "scope", SXCL_AUTH_SCOPE);
    if (form.oom) {
        sxcl_auth_buf_free(&form);
        sxcl_auth_err(err, err_len, "内存不足（拼换 token 的表单）");
        return SXCL_AUTH_ERR_NOMEM;
    }

    sxcl_auth_http resp;
    const int rc = sxcl_auth_http_call(tr, "POST", url, "application/x-www-form-urlencoded",
                                       sxcl_auth_buf_cstr(&form), form.len, NULL, timeout_ms,
                                       &resp, err, err_len);
    sxcl_auth_secure_zero(form.data, form.cap);
    sxcl_auth_buf_free(&form);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    int result = SXCL_AUTH_OK;
    if (resp.status != 200) {
        result = sxcl_auth_oauth_error(&resp, NULL, 0, err, err_len);
    } else {
        result = parse_token_response(&resp, out, err, err_len, sxcl_auth_now());
    }
    sxcl_auth_http_free(&resp);
    return result;
}

/* ── refresh token 续期 ── */
int sxcl_auth_refresh_ms(sxcl_transport *tr, const char *client_id, const char *refresh_token,
                         int64_t timeout_ms, sxcl_auth_ms_tokens *out, char *err, size_t err_len)
{
    return sxcl_auth_refresh_ms_tenant(tr, client_id, SXCL_AUTH_TENANT_DEFAULT, refresh_token,
                                       timeout_ms, out, err, err_len);
}

int sxcl_auth_refresh_ms_tenant(sxcl_transport *tr, const char *client_id, const char *tenant,
                                const char *refresh_token, int64_t timeout_ms,
                                sxcl_auth_ms_tokens *out, char *err, size_t err_len)
{
    if (tr == NULL || out == NULL || refresh_token == NULL || refresh_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "没有可用的 refresh token（需要重新登录一次）");
        return SXCL_AUTH_ERR_EXPIRED;
    }
    char url[384];
    if (sxcl_auth_endpoint_url(SXCL_AUTH_EP_TOKEN, tenant, url, sizeof(url)) != SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len, "租户段不合法：%s", tenant != NULL ? tenant : "(空)");
        return SXCL_AUTH_ERR_ARG;
    }
    char old_refresh[SXCL_AUTH_TOKEN_MAX];
    (void)sxcl_auth_copy(old_refresh, sizeof(old_refresh), refresh_token);

    sxcl_auth_buf form;
    sxcl_auth_buf_init(&form);
    (void)sxcl_auth_form_append(&form, "client_id", client_id);
    (void)sxcl_auth_form_append(&form, "grant_type", "refresh_token");
    (void)sxcl_auth_form_append(&form, "refresh_token", refresh_token);
    (void)sxcl_auth_form_append(&form, "scope", SXCL_AUTH_SCOPE);
    if (form.oom) {
        sxcl_auth_buf_free(&form);
        sxcl_auth_err(err, err_len, "内存不足（拼续期表单）");
        return SXCL_AUTH_ERR_NOMEM;
    }

    sxcl_auth_http resp;
    const int rc = sxcl_auth_http_call(tr, "POST", url, "application/x-www-form-urlencoded",
                                       sxcl_auth_buf_cstr(&form), form.len, NULL, timeout_ms,
                                       &resp, err, err_len);
    sxcl_auth_secure_zero(form.data, form.cap);
    sxcl_auth_buf_free(&form);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    memset(out, 0, sizeof(*out));
    int result = SXCL_AUTH_OK;
    if (resp.status != 200) {
        char raw[128];
        raw[0] = '\0';
        result = sxcl_auth_oauth_error(&resp, raw, sizeof(raw), err, err_len);
        /* 微软会在详情里塞 AADSTS 码:补一句"这是配置问题还是真过期"的判断 */
        char detail[SXCL_AUTH_ERROR_MAX];
        (void)sxcl_auth_copy(detail, sizeof(detail), err != NULL ? err : "");
        const int64_t aadsts = sxcl_auth_aadsts_extract(detail);
        const char *hint = (aadsts != 0) ? sxcl_auth_aadsts_hint(aadsts) : NULL;
        if (hint != NULL) {
            /* **建议放前面**:err 缓冲只有 512 字节,而微软的 error_description 又臭又长,
             * 把"怎么办"写在后面会被截掉 —— 那样人话就等于没写(实测踩过)。 */
            sxcl_auth_err(err, err_len, "（AADSTS%lld）%s\n微软原文：%s", (long long)aadsts, hint,
                          detail);
        }
    } else {
        result = parse_token_response(&resp, out, err, err_len, sxcl_auth_now());
        if (result == SXCL_AUTH_OK && out->refresh_token[0] == '\0') {
            /* 微软不一定每次都回新的 refresh token:那就沿用旧值(它仍然有效) */
            (void)sxcl_auth_copy(out->refresh_token, sizeof(out->refresh_token), old_refresh);
        }
    }
    sxcl_auth_http_free(&resp);
    return result;
}

/* ── 设备码流 ── */
typedef struct device_code_info {
    char device_code[SXCL_AUTH_TOKEN_MAX];
    char user_code[64];
    char verification_uri[256];
    int64_t interval_ms;
    int64_t expires_at;  /* Unix 秒 */
    int64_t expires_in;  /* 服务端给的原始秒数(给前端展示"这个码能用多久") */
} device_code_info;

static int device_code_request(sxcl_transport *tr, const char *client_id, const char *tenant,
                              int64_t timeout_ms, device_code_info *out,
                              char *err, size_t err_len)
{
    char url[384];
    if (sxcl_auth_endpoint_url(SXCL_AUTH_EP_DEVICECODE, tenant, url, sizeof(url)) != SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len, "租户段不合法：%s", tenant != NULL ? tenant : "(空)");
        return SXCL_AUTH_ERR_ARG;
    }
    sxcl_auth_buf form;
    sxcl_auth_buf_init(&form);
    (void)sxcl_auth_form_append(&form, "client_id", client_id);
    (void)sxcl_auth_form_append(&form, "scope", SXCL_AUTH_SCOPE);
    if (form.oom) {
        sxcl_auth_buf_free(&form);
        return SXCL_AUTH_ERR_NOMEM;
    }
    sxcl_auth_http resp;
    int rc = sxcl_auth_http_call(tr, "POST", url, "application/x-www-form-urlencoded",
                                 sxcl_auth_buf_cstr(&form), form.len, NULL, timeout_ms,
                                 &resp, err, err_len);
    sxcl_auth_buf_free(&form);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (resp.status != 200) {
        rc = sxcl_auth_oauth_error(&resp, NULL, 0, err, err_len);
        if (rc == SXCL_AUTH_ERR_HTTP) {
            /* 设备码申请失败基本是配置问题,把 AADSTS 建议带上 */
            const int64_t aadsts = sxcl_auth_aadsts_extract(resp.body);
            const char *hint = (aadsts != 0) ? sxcl_auth_aadsts_hint(aadsts) : NULL;
            if (hint != NULL) {
                char detail[SXCL_AUTH_ERROR_MAX];
                (void)sxcl_auth_copy(detail, sizeof(detail), err);
                sxcl_auth_err(err, err_len, "（AADSTS%lld）%s\n微软原文：%s", (long long)aadsts, hint,
                              detail);
            }
        }
        sxcl_auth_http_free(&resp);
        return rc;
    }
    char jerr[128];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(resp.body, resp.body_len, jerr, sizeof(jerr));
    if (doc == NULL) {
        sxcl_auth_http_free(&resp);
        sxcl_auth_err(err, err_len, "设备码接口返回的不是合法 JSON：%s", jerr);
        return SXCL_AUTH_ERR_JSON;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const char *dc = sxcl_json_get_string(root, "device_code", "");
    const char *uc = sxcl_json_get_string(root, "user_code", "");
    const char *vu = sxcl_json_get_string(root, "verification_uri", "");
    if (dc[0] == '\0' || uc[0] == '\0') {
        sxcl_json_free(doc);
        sxcl_auth_http_free(&resp);
        sxcl_auth_err(err, err_len, "设备码接口没有返回 device_code/user_code（HTTP %d）", resp.status);
        return SXCL_AUTH_ERR_JSON;
    }
    (void)sxcl_auth_copy(out->device_code, sizeof(out->device_code), dc);
    (void)sxcl_auth_copy(out->user_code, sizeof(out->user_code), uc);
    (void)sxcl_auth_copy(out->verification_uri, sizeof(out->verification_uri),
                         vu[0] ? vu : "https://microsoft.com/devicelogin");
    const sxcl_json_value *iv = sxcl_json_get(root, "interval");
    int64_t interval = 5; /* 微软的默认值;不许比它更小 */
    if (iv != NULL && sxcl_json_type_of(iv) == SXCL_JSON_NUMBER) {
        const double d = sxcl_json_number(iv);
        if (d >= 1.0 && d <= 60.0) {
            interval = (int64_t)d;
        }
    }
    const sxcl_json_value *ev = sxcl_json_get(root, "expires_in");
    int64_t expires_in = 900;
    if (ev != NULL && sxcl_json_type_of(ev) == SXCL_JSON_NUMBER) {
        const double d = sxcl_json_number(ev);
        if (d > 0.0 && d <= 86400.0) {
            expires_in = (int64_t)d;
        }
    }
    out->interval_ms = interval * 1000;
    out->expires_in = expires_in;
    out->expires_at = sxcl_auth_expiry_from(root, sxcl_auth_now(), expires_in);
    sxcl_json_free(doc);
    sxcl_auth_http_free(&resp);
    return SXCL_AUTH_OK;
}

int sxcl_auth_device_code_login(sxcl_transport *tr, const sxcl_auth_opts *opts, const char *client_id,
                                sxcl_auth_ms_tokens *out, char *err, size_t err_len)
{
    return sxcl_auth_device_code_login_tenant(tr, opts, client_id, SXCL_AUTH_TENANT_DEFAULT,
                                              out, err, err_len);
}

int sxcl_auth_device_code_login_tenant(sxcl_transport *tr, const sxcl_auth_opts *opts,
                                       const char *client_id, const char *tenant,
                                       sxcl_auth_ms_tokens *out, char *err, size_t err_len)
{
    if (tr == NULL || out == NULL || !sxcl_auth_client_id_valid(client_id)) {
        sxcl_auth_err(err, err_len, "client_id 不合法，无法发起设备码登录");
        return SXCL_AUTH_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    const int64_t http_timeout = (opts != NULL && opts->http_timeout_ms > 0) ? opts->http_timeout_ms : 30000;
    device_code_info info;
    memset(&info, 0, sizeof(info));
    const int rc = device_code_request(tr, client_id, tenant, http_timeout, &info, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (opts != NULL && opts->cb.on_user_code != NULL) {
        opts->cb.on_user_code(opts->cb.userdata, info.user_code, info.verification_uri, NULL);
    }
    if (opts != NULL && opts->cb.on_device_code_info != NULL) {
        opts->cb.on_device_code_info(opts->cb.userdata, (int)(info.interval_ms / 1000),
                                     (int)info.expires_in);
    }
    if (opts != NULL && opts->cb.on_status != NULL) {
        opts->cb.on_status(opts->cb.userdata, "等待用户在浏览器里完成授权…");
    }

    char token_url[384];
    if (sxcl_auth_endpoint_url(SXCL_AUTH_EP_TOKEN, tenant, token_url, sizeof(token_url)) != SXCL_AUTH_OK) {
        return SXCL_AUTH_ERR_ARG;
    }

    int64_t interval_ms = info.interval_ms > 0 ? info.interval_ms : 5000;
    if (g_poll_scale != 1.0) {
        interval_ms = (int64_t)((double)interval_ms * g_poll_scale);
        if (interval_ms < 1) {
            interval_ms = 1; /* 测试里缩放到 1ms 也不会变成"不睡"(那是空转) */
        }
    }
    const int64_t hard_deadline_ms =
        (opts != NULL && opts->poll_timeout_ms > 0) ? opts->poll_timeout_ms
                                                   : (info.expires_at - sxcl_auth_now()) * 1000;
    int64_t waited_ms = 0;
    int consecutive_errors = 0;

    for (;;) {
        if (opts != NULL && opts->should_cancel != NULL && opts->should_cancel(opts->cb.userdata) != 0) {
            sxcl_auth_err(err, err_len, "已取消设备码登录");
            return SXCL_AUTH_ERR_CANCELLED;
        }
        if (hard_deadline_ms > 0 && waited_ms >= hard_deadline_ms) {
            sxcl_auth_err(err, err_len, "设备码已过期（等了 %lld 秒），请重新发起登录",
                          (long long)(waited_ms / 1000));
            return SXCL_AUTH_ERR_EXPIRED;
        }
        /* 分片睡:取消/超时能在 200ms 内得到响应,不用等满一个 interval */
        int64_t slept = 0;
        while (slept < interval_ms) {
            const int64_t step = (interval_ms - slept < 200) ? (interval_ms - slept) : 200;
            sxcl_auth_sleep_ms(step);
            slept += step;
            waited_ms += step;
            if (opts != NULL && opts->should_cancel != NULL &&
                opts->should_cancel(opts->cb.userdata) != 0) {
                sxcl_auth_err(err, err_len, "已取消设备码登录");
                return SXCL_AUTH_ERR_CANCELLED;
            }
        }

        sxcl_auth_buf form;
        sxcl_auth_buf_init(&form);
        (void)sxcl_auth_form_append(&form, "client_id", client_id);
        (void)sxcl_auth_form_append(&form, "grant_type",
                                    "urn:ietf:params:oauth:grant-type:device_code");
        (void)sxcl_auth_form_append(&form, "device_code", info.device_code);
        sxcl_auth_http resp;
        char herr[SXCL_AUTH_ERROR_MAX];
        herr[0] = '\0';
        const int call_rc = sxcl_auth_http_call(tr, "POST", token_url,
                                                "application/x-www-form-urlencoded",
                                                sxcl_auth_buf_cstr(&form), form.len, NULL,
                                                http_timeout, &resp, herr, sizeof(herr));
        sxcl_auth_secure_zero(form.data, form.cap);
        sxcl_auth_buf_free(&form);
        if (call_rc != SXCL_AUTH_OK) {
            ++consecutive_errors;
            if (consecutive_errors >= 3) {
                sxcl_auth_err(err, err_len, "轮询设备码连续失败 %d 次：%s", consecutive_errors, herr);
                return call_rc;
            }
            continue; /* 网络抖动:继续轮询 */
        }
        if (resp.status == 200) {
            const int prc = parse_token_response(&resp, out, err, err_len, sxcl_auth_now());
            sxcl_auth_http_free(&resp);
            return prc;
        }
        char raw[128];
        raw[0] = '\0';
        const int erc = sxcl_auth_oauth_error(&resp, raw, sizeof(raw), err, err_len);
        if (resp.status == 429) {
            /* 被限流:按 Retry-After(没有就翻倍)退避,不消耗"连续错误"额度 */
            int64_t back = resp.retry_after_ms > 0 ? resp.retry_after_ms : interval_ms * 2;
            if (back > 60000) {
                back = 60000;
            }
            interval_ms = back;
            sxcl_auth_http_free(&resp);
            continue;
        }
        if (strcmp(raw, "authorization_pending") == 0) {
            consecutive_errors = 0;
            sxcl_auth_http_free(&resp);
            continue; /* 正常:用户还没点完,接着等 */
        }
        if (strcmp(raw, "slow_down") == 0) {
            /* 规范:slow_down 之后间隔 +5 秒(而且只增不减);测试里按同一比例缩放 */
            interval_ms += (int64_t)(5000.0 * g_poll_scale);
            consecutive_errors = 0;
            sxcl_auth_http_free(&resp);
            continue;
        }
        sxcl_auth_http_free(&resp);
        /* expired_token / authorization_declined / bad_verification_code / 其它:如实报错 */
        (void)erc;
        return erc;
    }
}
