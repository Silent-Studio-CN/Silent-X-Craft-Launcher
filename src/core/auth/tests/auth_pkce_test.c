/* PKCE 对拍 + 配置解析 + 错误码人话 + 端点拼接。
 *
 * 核心那一条:**RFC 7636 附录 B 的官方向量**
 *   verifier  = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
 *   challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
 * 算错一位,微软就会在换 token 那一步回 invalid_grant —— 那时代码看着"能跑",
 * 实际永远登不上,所以这条必须逐字符对拍。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/auth.h"
#include "sxcl/auth_store.h"
#include "auth_internal.h"

#include "auth_fake.h"

static void test_rfc7636_vector(void)
{
    const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const char *want = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    char got[128];
    got[0] = '\0';
    t_check(sxcl_auth_pkce_challenge(verifier, got, sizeof(got)) == SXCL_AUTH_OK,
            "S256 challenge 计算成功");
    t_check_str(got, want, "RFC 7636 附录 B 的 challenge 逐字符一致");

    /* 长度约束:RFC 7636 §4.1 是 43..128 */
    char short_v[8] = "abc";
    t_check(sxcl_auth_pkce_challenge(short_v, got, sizeof(got)) == SXCL_AUTH_ERR_ARG,
            "verifier 太短(<43)→ 拒绝");
    char long_v[140];
    memset(long_v, 'a', sizeof(long_v) - 1u);
    long_v[sizeof(long_v) - 1u] = '\0';
    t_check(sxcl_auth_pkce_challenge(long_v, got, sizeof(got)) == SXCL_AUTH_ERR_ARG,
            "verifier 太长(>128)→ 拒绝");
}

static void test_rfc7636_plain_vector(void)
{
    /* RFC 7636 附录 B 的另一半:plain 方法的 challenge 就等于 verifier。
     * 我们只用 S256,但用来说明"这条向量确实来自那份文档"(challenge 不是随便编的)。 */
    const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    char got[128];
    t_check(sxcl_auth_pkce_challenge(verifier, got, sizeof(got)) == SXCL_AUTH_OK, "重算一次");
    t_check(strcmp(got, verifier) != 0, "S256 的 challenge 与 verifier 不同(不是 plain)");
}

static void test_generate(void)
{
    char v1[128], c1[128], v2[128], c2[128];
    t_check(sxcl_auth_pkce_generate(v1, sizeof(v1), c1, sizeof(c1)) == SXCL_AUTH_OK,
            "随机生成 PKCE 成功");
    t_check(strlen(v1) == 64, "verifier 长度 = 64(在 43..128 内)");
    t_check(strlen(c1) == 43, "S256 challenge 长度 = 43(base64url 无填充的 32 字节)");
    t_check(sxcl_auth_pkce_generate(v2, sizeof(v2), c2, sizeof(c2)) == SXCL_AUTH_OK,
            "第二次生成成功");
    t_check(strcmp(v1, v2) != 0, "两次生成的 verifier 不同(用了真随机)");
    /* verifier 只能含 unreserved 字符 */
    for (const char *p = v1; *p != 0; ++p) {
        const char c = *p;
        const int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (!ok) {
            t_check(0, "verifier 含非法字符");
            return;
        }
    }
    t_check(1, "verifier 只含 RFC 7636 允许的 unreserved 字符");
}

static void test_entropy_deterministic(void)
{
    /* 同熵 → 同 verifier/challenge:证明 from_entropy 是纯函数(测试才可能对拍) */
    unsigned char entropy[48];
    for (size_t i = 0; i < sizeof(entropy); ++i) {
        entropy[i] = (unsigned char)i;
    }
    char v1[128], c1[128], v2[128], c2[128];
    t_check(sxcl_auth_pkce_from_entropy(entropy, sizeof(entropy), v1, sizeof(v1), c1, sizeof(c1)) ==
                SXCL_AUTH_OK,
            "由固定熵生成 PKCE 成功");
    t_check(sxcl_auth_pkce_from_entropy(entropy, sizeof(entropy), v2, sizeof(v2), c2, sizeof(c2)) ==
                SXCL_AUTH_OK,
            "同样熵再算一次成功");
    t_check_str(v1, v2, "同熵 → 同 verifier");
    t_check_str(c1, c2, "同熵 → 同 challenge");
}

static void test_authorize_url(void)
{
    char url[SXCL_AUTH_URL_MAX];
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const char *cid = "aa4e81c8-f550-4720-bc51-c9b126f456d1";
    t_check(sxcl_auth_build_authorize_url_tenant(cid, "consumers",
                                                 "http://localhost:51234/callback", "CHALLENGE123",
                                                 "STATE456", url, sizeof(url), err, sizeof(err)) ==
                SXCL_AUTH_OK,
            "拼授权 URL 成功");
    t_check_contains(url,
                     "https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize?client_id=",
                     "授权端点是 consumers/oauth2/v2.0/authorize");
    t_check_contains(url, "response_type=code", "带 response_type=code");
    t_check_contains(url, "redirect_uri=http%3A%2F%2Flocalhost%3A51234%2Fcallback",
                     "redirect_uri 已 URL 编码(环回 + 随机端口)");
    t_check_contains(url, "scope=XboxLive.signin%20offline_access",
                     "scope 是 XboxLive.signin offline_access(空格编码成 %20)");
    t_check_contains(url, "code_challenge=CHALLENGE123", "带 PKCE challenge");
    t_check_contains(url, "code_challenge_method=S256", "challenge 方法固定 S256");
    t_check_contains(url, "state=STATE456", "带 state(防串号)");
    t_check(strstr(url, "client_secret") == NULL, "URL 里没有 client_secret(公共客户端)");

    /* 默认租户版本应该和 consumers 版本一致 */
    char url2[SXCL_AUTH_URL_MAX];
    t_check(sxcl_auth_build_authorize_url(cid, "http://localhost:51234/callback", "CHALLENGE123",
                                          "STATE456", url2, sizeof(url2), err, sizeof(err)) ==
                SXCL_AUTH_OK,
            "默认租户版本成功");
    t_check_str(url2, url, "默认租户 = consumers,两个 URL 完全一致");

    /* 单租户应用(用户自己的 tenant id):端点要跟着换 */
    char url3[SXCL_AUTH_URL_MAX];
    t_check(sxcl_auth_build_authorize_url_tenant(cid, "4343d779-841f-46aa-a50c-435c7b52b3ac",
                                                 "http://localhost:1/callback", "C", "S", url3,
                                                 sizeof(url3), err, sizeof(err)) == SXCL_AUTH_OK,
            "自定义租户版本成功");
    t_check_contains(url3, "/4343d779-841f-46aa-a50c-435c7b52b3ac/oauth2/v2.0/authorize",
                     "自定义租户段出现在端点里");

    /* 非法租户段必须挡住(不能让人用 "../" 改写端点路径) */
    char bad[SXCL_AUTH_URL_MAX];
    t_check(sxcl_auth_build_authorize_url_tenant(cid, "../../evil", "http://localhost/callback", "C",
                                                 "S", bad, sizeof(bad), err, sizeof(err)) !=
                SXCL_AUTH_OK,
            "租户段里的 / 与 . 被挡住(不能改写端点路径)");
    t_check(sxcl_auth_tenant_valid("common") == 1, "common 是合法租户段");
    t_check(sxcl_auth_tenant_valid("organizations") == 1, "organizations 是合法租户段");
    t_check(sxcl_auth_tenant_valid("") == 0, "空租户段非法");
    t_check(sxcl_auth_tenant_valid("a/b") == 0, "含 / 的租户段非法");
}

static void test_client_id_validation(void)
{
    t_check(sxcl_auth_client_id_valid(SXCL_AUTH_DEFAULT_CLIENT_ID) == 1,
            "内置默认 client_id 合法");
    t_check(sxcl_auth_client_id_valid(SXCL_AUTH_COMMUNITY_CLIENT_ID) == 1,
            "社区公共 client_id 依然可用(排障对照)");
    t_check(sxcl_auth_client_id_valid("0000") == 0, "太短 → 非法");
    t_check(sxcl_auth_client_id_valid("has space in it") == 0, "含空格 → 非法");
    t_check(sxcl_auth_client_id_valid(NULL) == 0, "NULL → 非法");
}

static void test_resolve_client_id(void)
{
    char out[SXCL_AUTH_CLIENT_ID_MAX];
    /* 显式参数优先 */
    t_check(sxcl_auth_resolve_client_id("explicit-id-1234", NULL, out, sizeof(out)) == SXCL_AUTH_OK,
            "解析 client_id 成功");
    t_check_str(out, "explicit-id-1234", "显式参数优先");

    /* 环境变量次之 */
#if defined(_WIN32)
    _putenv_s("SXCL_AUTH_CLIENT_ID", "env-id-56789012");
#else
    setenv("SXCL_AUTH_CLIENT_ID", "env-id-56789012", 1);
#endif
    t_check(sxcl_auth_resolve_client_id(NULL, NULL, out, sizeof(out)) == SXCL_AUTH_OK, "再解析一次");
    t_check_str(out, "env-id-56789012", "环境变量覆盖默认值");
    t_check(sxcl_auth_resolve_client_id("explicit-id-1234", NULL, out, sizeof(out)) == SXCL_AUTH_OK,
            "显式参数仍然优先于环境变量");
    t_check_str(out, "explicit-id-1234", "显式 > 环境变量");

    /* settings 再次之 */
#if defined(_WIN32)
    _putenv_s("SXCL_AUTH_CLIENT_ID", "");
#else
    unsetenv("SXCL_AUTH_CLIENT_ID");
#endif
    sxcl_settings *st = sxcl_settings_open("");
    if (st != NULL) {
        t_check(sxcl_settings_set(st, SXCL_AUTH_SETTINGS_CLIENT_ID, "settings-id-12345678") == 0,
                "写设置项成功");
        t_check(sxcl_auth_resolve_client_id(NULL, st, out, sizeof(out)) == SXCL_AUTH_OK, "再解析");
        t_check_str(out, "settings-id-12345678", "settings 覆盖默认值");
        t_check(sxcl_settings_remove(st, SXCL_AUTH_SETTINGS_CLIENT_ID) == 0, "删设置项成功");
        t_check(sxcl_auth_resolve_client_id(NULL, st, out, sizeof(out)) == SXCL_AUTH_OK, "再解析");
        t_check_str(out, SXCL_AUTH_DEFAULT_CLIENT_ID, "都没有 → 落到内置默认值");
        sxcl_settings_free(st);
    }

    /* 租户段:显式 > 环境变量 > 默认 */
    t_check(sxcl_auth_resolve_tenant("common", NULL, out, sizeof(out)) == SXCL_AUTH_OK, "租户解析");
    t_check_str(out, "common", "显式租户段优先");
    t_check(sxcl_auth_resolve_tenant(NULL, NULL, out, sizeof(out)) == SXCL_AUTH_OK, "租户解析");
    t_check_str(out, SXCL_AUTH_TENANT_DEFAULT, "默认租户段是 consumers");
    t_check(sxcl_auth_resolve_tenant("bad/tenant", NULL, out, sizeof(out)) == SXCL_AUTH_ERR_ARG,
            "非法租户段被拒");
}

static void test_xsts_error_text(void)
{
    /* 三条必须有人话的码(题目点名要求) */
    const char *t33 = sxcl_auth_xsts_error_text(SXCL_AUTH_XERR_NO_XBOX_ACCOUNT);
    const char *t35 = sxcl_auth_xsts_error_text(SXCL_AUTH_XERR_REGION);
    const char *t38 = sxcl_auth_xsts_error_text(SXCL_AUTH_XERR_CHILD);
    t_check(t33 != NULL && strstr(t33, "Xbox 档案") != NULL,
            "2148916233 → “没有 Xbox 档案”(中文人话)");
    t_check(t35 != NULL && strstr(t35, "地区") != NULL, "2148916235 → “所在地区不支持”");
    t_check(t38 != NULL && strstr(t38, "未成年") != NULL, "2148916238 → “未成年账号需成人同意”");
    t_check(sxcl_auth_xsts_error_text(2148916250) == NULL, "认不出的码返回 NULL(交给调用方原样带出)");

    char buf[256];
    t_check(sxcl_auth_xsts_error_message(2148916250, buf, sizeof(buf)) > 0, "未知码也能出人话");
    t_check_contains(buf, "2148916250", "未知码**原样带出数字**(不吞掉)");
    t_check(sxcl_auth_xsts_error_message(SXCL_AUTH_XERR_REGION, buf, sizeof(buf)) > 0, "已知码出人话");
    t_check_contains(buf, "2148916235", "已知码也带上原始数字");
    t_check_contains(buf, "地区", "已知码带中文原因");
}

static void test_aadsts_hint(void)
{
    t_check(sxcl_auth_aadsts_extract("AADSTS50059: No tenant-identifying information found") == 50059,
            "从 error_description 里抠出 AADSTS 码");
    t_check(sxcl_auth_aadsts_extract("invalid_grant") == 0, "没有 AADSTS 码时返回 0");
    const char *h = sxcl_auth_aadsts_hint(50059);
    t_check(h != NULL && strstr(h, "受支持的帐户类型") != NULL,
            "AADSTS50059 → 指向“应用的受支持账户类型”这个配置点");
    t_check(sxcl_auth_aadsts_hint(700082) != NULL, "AADSTS700082 → refresh token 过期的建议");
    t_check(sxcl_auth_aadsts_hint(999999) == NULL, "认不出的 AADSTS 码返回 NULL");
}

static void test_mask_token(void)
{
    char out[128];
    t_check(sxcl_auth_mask_token("eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiJ9.abc", out, sizeof(out)) > 0,
            "打码成功");
    t_check_contains(out, "eyJ0eX", "打码保留前 6 位便于肉眼核对");
    t_check(strstr(out, "OiJKV1QiLCJhbGciOiJSUzI1NiJ9") == NULL, "**正文没有被打出来**");
    (void)sxcl_auth_mask_token("", out, sizeof(out));
    t_check_str(out, "(无)", "空 token → (无)");
    (void)sxcl_auth_mask_token(NULL, out, sizeof(out));
    t_check_str(out, "(无)", "NULL token → (无)");
}

static void test_expiry(void)
{
    sxcl_auth_session s;
    memset(&s, 0, sizeof(s));
    t_check(sxcl_auth_ms_expired(&s, 1000, 60) == 1, "没有 token → 视为过期");
    (void)snprintf(s.ms.access_token, sizeof(s.ms.access_token), "token");
    s.ms.expires_at = 2000;
    t_check(sxcl_auth_ms_expired(&s, 1900, 60) == 0, "还剩 100 秒、余量 60 → 还没过期");
    t_check(sxcl_auth_ms_expired(&s, 1950, 60) == 1, "还剩 50 秒、余量 60 → 提前判过期(留刷新余量)");
    t_check(sxcl_auth_ms_expired(&s, 1800, 60) == 0, "还剩 200 秒 → 未过期");
    t_check(sxcl_auth_ms_expired(&s, 2500, 0) == 1, "已过期 → 过期");
    s.ms.expires_at = 0;
    t_check(sxcl_auth_ms_expired(&s, 999999, 0) == 0, "拿不到 expires_in(0)→ 不擅自判过期");
    t_check(sxcl_auth_session_has_ms(&s) == 1, "has_ms 认出 token");
    t_check(sxcl_auth_session_has_mc(&s) == 0, "has_mc 对空 mc 段返回 0");

    sxcl_auth_set_now_for_test(4242);
    t_check(sxcl_auth_now() == 4242, "测试时钟可以覆盖 sxcl_auth_now");
    sxcl_auth_set_now_for_test(-1);
    t_check(sxcl_auth_now() > 1700000000, "恢复真实时钟");
}

int main(void)
{
    test_rfc7636_vector();
    test_rfc7636_plain_vector();
    test_generate();
    test_entropy_deterministic();
    test_authorize_url();
    test_client_id_validation();
    test_resolve_client_id();
    test_xsts_error_text();
    test_aadsts_hint();
    test_mask_token();
    test_expiry();
    return t_report("auth_pkce_test");
}
