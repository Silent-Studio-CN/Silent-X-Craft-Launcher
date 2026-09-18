/* 登录链整链测试:**不联网**(夹具 + 假传输)把成功链与全部失败分支跑一遍。
 *
 * 覆盖(对应任务书"测试夹具"一节):
 *   成功链              MS token → XBL → XSTS → login_with_xbox → mcstore → profile
 *   各档 XSTS 错误码    2148916233 / 2148916235 / 2148916238 / 认不出的码(必须原样带出)
 *   name 为空           没买 Java 版 → 必须报错,**不许继续**
 *   refresh 过期        invalid_grant + AADSTS700082 → ERR_EXPIRED + 中文排查建议
 *   device code         pending / slow_down / expired / declined 四种流程控制
 *   PKCE 之外还要验:请求体是不是对的(RpsTicket="d=…"、identityToken="XBL3.0 x=uhs;xsts")
 */
#include <stdio.h>
#include <string.h>

#include "sxcl/auth.h"
#include "auth_internal.h"

#include "auth_fake.h"

#define TENANT SXCL_AUTH_TENANT_DEFAULT
#define CID    SXCL_AUTH_DEFAULT_CLIENT_ID

static void test_xbox_user_auth(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    char xbl[SXCL_AUTH_TOKEN_MAX];
    char uhs[SXCL_AUTH_NAME_MAX];
    int64_t exp = 0;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_xbox_user_authenticate(&ft->pub, "MS-ACCESS-TOKEN-XYZ", 5000, xbl,
                                                    sizeof(xbl), uhs, sizeof(uhs), &exp, err,
                                                    sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "第 3 跳 XBL 用户认证成功");
    t_check_str(uhs, "u0000000000000001", "取到 uhs");
    t_check(strlen(xbl) > 20, "取到 XBL token(长度合理)");
    t_check(exp > 1700000000, "解析出 NotAfter(ISO8601 → Unix 秒)");
    const fake_log *log = fake_find(ft, SXCL_AUTH_URL_XBL_USER, 0);
    t_check(log != NULL, "记下了这次请求");
    if (log != NULL) {
        t_check_str(log->method, "POST", "XBL 用 POST");
        t_check_contains(log->body, "\"RpsTicket\":\"d=MS-ACCESS-TOKEN-XYZ\"",
                         "RpsTicket 必须是 d=<微软 access token>");
        t_check_contains(log->body, "\"RelyingParty\":\"http://auth.xboxlive.com\"",
                         "RelyingParty = http://auth.xboxlive.com");
        t_check_contains(log->body, "\"TokenType\":\"JWT\"", "TokenType=JWT");
        t_check_contains(log->body, "\"SiteName\":\"user.auth.xboxlive.com\"", "SiteName 正确");
        t_check_contains(log->content_type, "application/json", "Content-Type 是 JSON");
    }
    fake_free(ft);
}

static void test_xsts_success(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    sxcl_auth_xbox_tokens tok;
    uint64_t xerr = 12345;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_xsts_authorize(&ft->pub, "XBL-TOKEN-ABC", SXCL_AUTH_RP_MINECRAFT, 5000,
                                            &tok, &xerr, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "第 4 跳 XSTS 成功");
    t_check(xerr == 0, "成功时 XErr 归零(不残留上一次的值)");
    t_check_str(tok.user_hash, "u0000000000000001", "XSTS 也带 uhs");
    t_check(tok.xuid == 2533274800000001ULL, "解析出 XUID");
    t_check_str(tok.gamertag, "", "没有 gtg 字段时 gamertag 为空(不编造)");
    t_check(tok.xsts_expires_at > 1700000000, "解析出 XSTS 过期时间");
    const fake_log *log = fake_find(ft, SXCL_AUTH_URL_XSTS, 0);
    t_check(log != NULL, "记下了这次 XSTS 请求");
    if (log != NULL) {
        t_check_contains(log->body, "\"RelyingParty\":\"rp://api.minecraftservices.com/\"",
                         "Java 链的 RelyingParty 固定是 rp://api.minecraftservices.com/");
        t_check_contains(log->body, "\"UserTokens\":[\"XBL-TOKEN-ABC\"]", "带上 XBL token");
        t_check_contains(log->body, "\"SandboxId\":\"RETAIL\"", "SandboxId=RETAIL");
    }
    fake_free(ft);
}

static void test_xsts_error_codes(void)
{
    struct {
        const char *fixture;
        uint64_t xerr;
        const char *must_contain;
        const char *what;
    } cases[] = {
        {"xsts_err_2148916233.json", 2148916233ULL, "Xbox 档案", "2148916233 → 没有 Xbox 档案"},
        {"xsts_err_2148916235.json", 2148916235ULL, "地区", "2148916235 → 地区不支持"},
        {"xsts_err_2148916238.json", 2148916238ULL, "未成年", "2148916238 → 未成年要成人同意"},
        {"xsts_err_unknown.json", 2148916250ULL, "2148916250", "认不出的码原样带出数字"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fake_transport *ft = fake_create();
        fake_route1(ft, SXCL_AUTH_URL_XSTS, 401, cases[i].fixture);
        sxcl_auth_xbox_tokens tok;
        uint64_t xerr = 0;
        char err[SXCL_AUTH_ERROR_MAX];
        err[0] = '\0';
        const int rc = sxcl_auth_xsts_authorize(&ft->pub, "XBL", SXCL_AUTH_RP_MINECRAFT, 5000, &tok,
                                                &xerr, err, sizeof(err));
        t_check(rc == SXCL_AUTH_ERR_XBOX, cases[i].what);
        t_check(xerr == cases[i].xerr, "XErr 数字被原样取出");
        t_check_contains(err, cases[i].must_contain, "err 里有人话原因");
        if (strcmp(cases[i].fixture, "xsts_err_2148916233.json") == 0) {
            t_check_contains(err, "xbox.com", "给出“去哪创建 Xbox 档案”的指引");
            t_check_contains(err, "CreateAccount", "带上服务端给的 Redirect 入口");
        }
        fake_free(ft);
    }
}

static void test_mc_login(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_MC_LOGIN, 200, "mc_login_success.json");
    sxcl_auth_minecraft_tokens mc;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_minecraft_login(&ft->pub, "u0000000000000001", "XSTS-TOKEN-QQQ", 5000,
                                             &mc, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "第 5 跳 login_with_xbox 成功");
    t_check(mc.access_token[0] != '\0', "拿到 MC access_token");
    t_check(mc.expires_at > 1700000000, "按 expires_in 算出过期时间");
    const fake_log *log = fake_find(ft, SXCL_AUTH_URL_MC_LOGIN, 0);
    t_check(log != NULL, "记下了这次请求");
    if (log != NULL) {
        t_check_contains(log->body, "\"identityToken\":\"XBL3.0 x=u0000000000000001;XSTS-TOKEN-QQQ\"",
                         "identityToken 格式必须是 XBL3.0 x=<uhs>;<xsts>");
    }
    fake_free(ft);
}

static void test_mc_login_failure(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_MC_LOGIN, 401, "mc_login_401.json");
    sxcl_auth_minecraft_tokens mc;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_minecraft_login(&ft->pub, "uhs", "xsts", 5000, &mc, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_HTTP, "401 → HTTP 错误码");
    t_check_contains(err, "401", "err 里带上状态码");
    t_check_contains(err, "Invalid app registration", "**服务端原文一并带出**(不带就查不出原因)");
    fake_free(ft);
}

static void test_entitlements(void)
{
    {
        fake_transport *ft = fake_create();
        fake_route1(ft, SXCL_AUTH_URL_MC_ENTITLEMENTS, 200, "mcstore_ok.json");
        int count = -1;
        char err[SXCL_AUTH_ERROR_MAX];
        err[0] = '\0';
        t_check(sxcl_auth_minecraft_entitlements(&ft->pub, "MC-TOKEN", 5000, &count, err, sizeof(err)) ==
                    SXCL_AUTH_OK,
                "mcstore 查询成功");
        t_check(count == 2, "数出 2 条权益(product_minecraft / game_minecraft)");
        const fake_log *log = fake_find(ft, SXCL_AUTH_URL_MC_ENTITLEMENTS, 0);
        t_check(log != NULL && strcmp(log->method, "GET") == 0, "权益接口用 GET");
        if (log != NULL) {
            t_check_contains(log->authorization, "Bearer MC-TOKEN", "带 Authorization: Bearer <MC token>");
        }
        fake_free(ft);
    }
    {
        fake_transport *ft = fake_create();
        fake_route1(ft, SXCL_AUTH_URL_MC_ENTITLEMENTS, 404, "mcstore_404.json");
        int count = -1;
        char err[SXCL_AUTH_ERROR_MAX];
        err[0] = '\0';
        t_check(sxcl_auth_minecraft_entitlements(&ft->pub, "MC-TOKEN", 5000, &count, err, sizeof(err)) ==
                    SXCL_AUTH_OK,
                "404 = “确实没买”,不算查询失败");
        t_check(count == 0, "条目数 0");
        t_check_contains(err, "没有", "人话说明“没有任何 Java 版权益”");
        fake_free(ft);
    }
}

static void test_profile(void)
{
    {
        fake_transport *ft = fake_create();
        fake_route1(ft, SXCL_AUTH_URL_MC_PROFILE, 200, "mc_profile_ok.json");
        char uuid[SXCL_AUTH_UUID_MAX];
        char name[SXCL_AUTH_NAME_MAX];
        char err[SXCL_AUTH_ERROR_MAX];
        err[0] = '\0';
        t_check(sxcl_auth_minecraft_profile(&ft->pub, "MC", 5000, uuid, sizeof(uuid), name,
                                            sizeof(name), err, sizeof(err)) == SXCL_AUTH_OK,
                "档案查询成功");
        t_check_str(name, "SXCLPlayer", "取到玩家名");
        t_check_str(uuid, "00000000000000000000000000000000", "取到 uuid");
        fake_free(ft);
    }
    {
        /* **关键分支**:404 = 没买 Java 版,必须报错 */
        fake_transport *ft = fake_create();
        fake_route1(ft, SXCL_AUTH_URL_MC_PROFILE, 404, "mc_profile_404.json");
        char uuid[SXCL_AUTH_UUID_MAX];
        char name[SXCL_AUTH_NAME_MAX];
        char err[SXCL_AUTH_ERROR_MAX];
        err[0] = '\0';
        const int rc = sxcl_auth_minecraft_profile(&ft->pub, "MC", 5000, uuid, sizeof(uuid), name,
                                                   sizeof(name), err, sizeof(err));
        t_check(rc == SXCL_AUTH_ERR_NO_PROFILE, "profile 404 → ERR_NO_PROFILE(没买 Java 版)");
        t_check(name[0] == '\0', "名字保持为空(不编造默认名)");
        t_check_contains(err, "Java 版", "人话说明“没有 Java 版档案”");
        fake_free(ft);
    }
    {
        /* HTTP 200 但 name 为空:同样必须报错 */
        fake_transport *ft = fake_create();
        fake_route1(ft, SXCL_AUTH_URL_MC_PROFILE, 200, "mc_profile_empty_name.json");
        char uuid[SXCL_AUTH_UUID_MAX];
        char name[SXCL_AUTH_NAME_MAX];
        char err[SXCL_AUTH_ERROR_MAX];
        err[0] = '\0';
        const int rc = sxcl_auth_minecraft_profile(&ft->pub, "MC", 5000, uuid, sizeof(uuid), name,
                                                   sizeof(name), err, sizeof(err));
        t_check(rc == SXCL_AUTH_ERR_NO_PROFILE, "name 为空 → ERR_NO_PROFILE(**不许继续**)");
        t_check_contains(err, "name", "人话点出“name 是空的”");
        fake_free(ft);
    }
}

static void test_exchange_code(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_exchange_code_tenant(&ft->pub, CID, TENANT, "THE-CODE",
                                                  "http://localhost:51234/callback", "THE-VERIFIER",
                                                  5000, &tok, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "授权码换 token 成功");
    t_check(tok.access_token[0] != '\0', "拿到 access_token");
    t_check(tok.refresh_token[0] != '\0', "拿到 refresh_token(offline_access 生效)");
    t_check(tok.expires_at > tok.issued_at, "过期时间 = 签发时间 + expires_in");
    t_check_str(tok.token_type, "Bearer", "token_type 解析正确");
    const fake_log *log = fake_find(ft, SXCL_AUTH_URL_TOKEN, 0);
    t_check(log != NULL, "记下了换 token 请求");
    if (log != NULL) {
        t_check_str(log->method, "POST", "换 token 用 POST");
        t_check_contains(log->content_type, "application/x-www-form-urlencoded",
                         "token 端点用表单编码(微软要求)");
        t_check_contains(log->body, "grant_type=authorization_code", "grant_type 正确");
        t_check_contains(log->body, "code=THE-CODE", "带上授权码");
        t_check_contains(log->body, "code_verifier=THE-VERIFIER", "带上 PKCE verifier");
        t_check_contains(log->body, "redirect_uri=http%3A%2F%2Flocalhost%3A51234%2Fcallback",
                         "redirect_uri 与授权时逐字一致(URL 编码后)");
        t_check_contains(log->body, "client_id=", "带 client_id");
        t_check(strstr(log->body, "client_secret") == NULL, "表单里没有 client_secret");
    }
    fake_free(ft);
}

static void test_refresh(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    t_check(sxcl_auth_refresh_ms_tenant(&ft->pub, CID, TENANT, "OLD-REFRESH", 5000, &tok, err,
                                        sizeof(err)) == SXCL_AUTH_OK,
            "refresh_token 续期成功(免密)");
    const fake_log *log = fake_find(ft, SXCL_AUTH_URL_TOKEN, 0);
    if (log != NULL) {
        t_check_contains(log->body, "grant_type=refresh_token", "grant_type=refresh_token");
        t_check_contains(log->body, "refresh_token=OLD-REFRESH", "带上旧 refresh token");
    }
    fake_free(ft);
}

static void test_refresh_expired(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 400, "ms_token_error_invalid_grant.json");
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_refresh_ms_tenant(&ft->pub, CID, TENANT, "DEAD-REFRESH", 5000, &tok,
                                               err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_EXPIRED, "refresh token 过期 → ERR_EXPIRED(要重新登录)");
    t_check_contains(err, "失效", "人话说明“已失效”");
    t_check_contains(err, "AADSTS700082", "**原始 AADSTS 码带出**");
    t_check_contains(err, "重新登录", "给出中文排查建议");
    fake_free(ft);
}

static void test_tenant_mismatch_error(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 400, "ms_token_error_tenant_mismatch.json");
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_refresh_ms_tenant(&ft->pub, CID, TENANT, "R", 5000, &tok, err,
                                               sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_ARG, "租户段没配对(50059)→ 归类成“配置/参数问题”");
    t_check_contains(err, "AADSTS50059", "带出 AADSTS 码");
    t_check_contains(err, "受支持的帐户类型", "**指出这是应用注册配置问题,不是代码问题**");
    fake_free(ft);
}

/* ── 设备码流 ── */
typedef struct device_cb_state {
    int calls;
    char user_code[64];
    char uri[128];
} device_cb_state;

static void on_user_code_cb(void *ud, const char *user_code, const char *verification_uri, const char *message)
{
    device_cb_state *st = (device_cb_state *)ud;
    st->calls++;
    (void)snprintf(st->user_code, sizeof(st->user_code), "%s", user_code ? user_code : "");
    (void)snprintf(st->uri, sizeof(st->uri), "%s", verification_uri ? verification_uri : "");
    (void)message;
}

static void test_device_code_success(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01); /* 5 秒 → 50ms,别让测试等十几秒 */
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    device_cb_state st;
    memset(&st, 0, sizeof(st));
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.poll_timeout_ms = 20000;
    opts.cb.on_user_code = on_user_code_cb;
    opts.cb.userdata = &st;
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_device_code_login_tenant(&ft->pub, &opts, CID, TENANT, &tok, err,
                                                      sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "设备码流:拿到 token");
    t_check(st.calls == 1, "user_code 回调被调用一次(把代码交给前端)");
    t_check_str(st.user_code, "K7QP4M2XZ", "回调里的 user_code 正确");
    t_check_str(st.uri, "https://microsoft.com/devicelogin", "回调里的 verification_uri 正确");
    t_check(tok.access_token[0] != '\0', "换到 access_token");
    const fake_log *dc = fake_find(ft, SXCL_AUTH_URL_DEVICECODE, 0);
    if (dc != NULL) {
        t_check_contains(dc->body, "scope=XboxLive.signin%20offline_access",
                         "设备码申请带 scope(含 offline_access)");
    }
    const fake_log *poll = fake_find(ft, SXCL_AUTH_URL_TOKEN, 0);
    if (poll != NULL) {
        t_check_contains(poll->body,
                         "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code",
                         "轮询用 device_code 的 grant_type(URN 已编码)");
        t_check_contains(poll->body, "device_code=", "带上 device_code");
    }
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

static void test_device_code_pending_then_success(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    const fake_step steps[] = {
        {400, "ms_devicecode_pending.json"},
        {400, "ms_devicecode_pending.json"},
        {400, "ms_devicecode_slow_down.json"},
        {200, "ms_token_success.json"},
    };
    fake_route_seq(ft, SXCL_AUTH_URL_TOKEN, steps, 4);
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.poll_timeout_ms = 20000;
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_device_code_login_tenant(&ft->pub, &opts, CID, TENANT, &tok, err,
                                                      sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "pending → pending → slow_down → 成功:整串都按流程控制走对了");
    size_t polls = 0;
    for (size_t i = 0; i < ft->log_count; ++i) {
        if (strcmp(ft->log[i].url, SXCL_AUTH_URL_TOKEN) == 0) {
            ++polls;
        }
    }
    t_check(polls == 4, "一共轮询了 4 次(没有提前退出、也没有多轮)");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

static void test_device_code_expired(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 400, "ms_devicecode_expired.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.poll_timeout_ms = 20000;
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_device_code_login_tenant(&ft->pub, &opts, CID, TENANT, &tok, err,
                                                      sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_EXPIRED, "expired_token → ERR_EXPIRED");
    t_check_contains(err, "过期", "人话说明设备码过期");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

static void test_device_code_declined(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 400, "ms_devicecode_declined.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.poll_timeout_ms = 20000;
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_device_code_login_tenant(&ft->pub, &opts, CID, TENANT, &tok, err,
                                                      sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_DENIED, "authorization_declined → ERR_DENIED(用户拒绝)");
    t_check_contains(err, "拒绝", "人话说明用户点了拒绝");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

static void test_device_code_cancel(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 400, "ms_devicecode_pending.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    /* 一直 pending:轮询到 poll_timeout_ms 就该收工报过期(不能让用户干等)。
     * 时间要给得小 —— 这是**真实墙钟**(不随 poll_scale 缩放),不然测试白等 20 秒。 */
    opts.poll_timeout_ms = 600;
    sxcl_auth_ms_tokens tok;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_device_code_login_tenant(&ft->pub, &opts, CID, TENANT, &tok, err,
                                                      sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_EXPIRED, "一直 pending → 到 poll_timeout 就报过期(不无限等)");
    size_t polls = 0;
    for (size_t i = 0; i < ft->log_count; ++i) {
        if (strcmp(ft->log[i].url, SXCL_AUTH_URL_TOKEN) == 0) {
            ++polls;
        }
    }
    t_check(polls >= 3, "超时之前确实轮询了多次(pending 是在“继续等”,不是“失败”)");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

/* ── 整链 ── */
static void test_full_login_device_code(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    fake_setup_java_chain(ft);
    device_cb_state st;
    memset(&st, 0, sizeof(st));
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = CID;
    opts.device_code = 1;
    opts.poll_timeout_ms = 20000;
    opts.cb.on_user_code = on_user_code_cb;
    opts.cb.userdata = &st;
    sxcl_auth_session session;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_login(&ft->pub, &opts, &session, err, sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "整链(设备码流)登录成功");
    t_check_str(session.client_id, CID, "会话记下了 client_id");
    t_check_str(session.tenant, TENANT, "会话记下了租户段");
    t_check_str(session.mc.name, "SXCLPlayer", "会话里有玩家名");
    t_check_str(session.mc.uuid, "00000000000000000000000000000000", "会话里有 uuid");
    t_check(session.mc.entitlement_count == 2, "会话里有权益条数");
    t_check(session.mc.entitlements_checked == 1, "记下“权益真的查过了”");
    t_check(session.mc.profile_checked == 1, "记下“档案真的查过了”");
    t_check_str(session.account_name, "SXCLPlayer", "展示名回落到玩家名");
    t_check(session.ms.refresh_token[0] != '\0', "有 refresh token(可免密续期)");
    /* 五跳都要真的发生过,而且顺序不能乱 */
    const char *order[] = {SXCL_AUTH_URL_DEVICECODE, SXCL_AUTH_URL_TOKEN, SXCL_AUTH_URL_XBL_USER,
                           SXCL_AUTH_URL_XSTS, SXCL_AUTH_URL_MC_LOGIN,
                           SXCL_AUTH_URL_MC_ENTITLEMENTS, SXCL_AUTH_URL_MC_PROFILE};
    size_t found = 0;
    for (size_t i = 0; i < ft->log_count; ++i) {
        if (found < sizeof(order) / sizeof(order[0]) && strcmp(ft->log[i].url, order[found]) == 0) {
            ++found;
        }
    }
    t_check(found == sizeof(order) / sizeof(order[0]),
            "七次请求的类型与顺序完全符合链路(devicecode→token→xbl→xsts→mc→mcstore→profile)");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

static void test_full_login_no_java_edition(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_LOGIN, 200, "mc_login_success.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_ENTITLEMENTS, 404, "mcstore_404.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_PROFILE, 404, "mc_profile_404.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = CID;
    opts.device_code = 1;
    opts.poll_timeout_ms = 20000;
    sxcl_auth_session session;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_login(&ft->pub, &opts, &session, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_NO_PROFILE,
            "整链:没买 Java 版 → 登录**失败**(不会拿空名字继续)");
    t_check_contains(err, "Java 版", "人话说明没有 Java 版");
    t_check(session.mc.name[0] == '\0', "名字依然为空");
    t_check(session.mc.entitlement_count == 0, "权益条数为 0 并被记住");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

/* **这条是实测踩出来的缺陷的回归测试**:
 * 用户辛辛苦苦输完设备码、微软也给了 refresh token,结果第 ⑥ 跳 login_with_xbox 回 403 →
 * 第一版实现把整次登录判失败、**连已经到手的微软凭据一起丢掉**,害得用户要再输一次码。
 * 现在:不管后面哪一跳失败,会话里的微软令牌必须还在,调用方才能"存下来、修好配置再重试"。 */
static void test_ms_tokens_survive_downstream_failure(void)
{
    sxcl_auth_set_poll_scale_for_test(0.01);
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_DEVICECODE, 200, "ms_devicecode.json");
    fake_route1(ft, SXCL_AUTH_URL_TOKEN, 200, "ms_token_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    /* 第 ⑥ 跳:403 Invalid app registration(就是实测遇到的那一条) */
    fake_route1(ft, SXCL_AUTH_URL_MC_LOGIN, 403, "mc_login_403_app_not_registered.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = CID;
    opts.device_code = 1;
    opts.poll_timeout_ms = 20000;
    sxcl_auth_session session;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = 0;
    const int rc = sxcl_auth_login(&ft->pub, &opts, &session, err, sizeof(err));
    t_check(rc != SXCL_AUTH_OK, "第 ⑥ 跳失败 → 整次登录判失败(不假装成功)");
    t_check_contains(err, "Invalid app registration", "把服务端的原始原因带出来");
    t_check(session.ms.access_token[0] != 0, "**失败但微软 access_token 仍在会话里**(不能丢)");
    t_check(session.ms.refresh_token[0] != 0,
            "**失败但 refresh_token 仍在会话里**(用户不用再输一次设备码)");
    t_check(session.ms.expires_at > 0, "过期时间也在(续期时用得上)");
    t_check(session.xbox.user_hash[0] != 0, "已经跑通的前几跳结果也留着(便于排障)");
    t_check_str(session.client_id, CID, "client_id 记在会话里");
    sxcl_auth_set_poll_scale_for_test(1.0);
    fake_free(ft);
}

static void test_login_bad_client_id(void)
{
    fake_transport *ft = fake_create();
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = "bad id with spaces";
    sxcl_auth_session session;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_login(&ft->pub, &opts, &session, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_ARG, "client_id 形状不对 → 发请求之前就报错");
    t_check_contains(err, "client_id", "人话点出是 client_id 的问题");
    t_check(ft->log_count == 0, "**一个请求都没发出去**(错误在本地就被挡住)");
    fake_free(ft);
}

static void test_login_bad_tenant(void)
{
    fake_transport *ft = fake_create();
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = CID;
    opts.tenant = "bad/../tenant";
    sxcl_auth_session session;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_login(&ft->pub, &opts, &session, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_ARG, "租户段含 / → 本地拒绝(不能改写端点路径)");
    t_check(ft->log_count == 0, "一个请求都没发");
    fake_free(ft);
}

int main(void)
{
    test_xbox_user_auth();
    test_xsts_success();
    test_xsts_error_codes();
    test_mc_login();
    test_mc_login_failure();
    test_entitlements();
    test_profile();
    test_exchange_code();
    test_refresh();
    test_refresh_expired();
    test_tenant_mismatch_error();
    test_device_code_success();
    test_device_code_pending_then_success();
    test_device_code_expired();
    test_device_code_declined();
    test_device_code_cancel();
    test_full_login_device_code();
    test_full_login_no_java_edition();
    test_ms_tokens_survive_downstream_failure();
    test_login_bad_client_id();
    test_login_bad_tenant();
    return t_report("auth_flow_test");
}
