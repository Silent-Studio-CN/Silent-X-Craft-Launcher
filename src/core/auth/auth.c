/* 登录编排:一次调用把整条链跑完(微软 → XBL → XSTS → Minecraft → 权益/档案)。
 *
 * 授权码主路(默认):
 *   PKCE 生成 verifier/challenge → 起 127.0.0.1 环回服务 → 拼授权 URL → 打开浏览器
 *   → 等 /callback?code=…(校验 state)→ 换 token → 跑后面的链
 * 设备码兜底(--device-code / 没有浏览器 / 环回端口起不来):
 *   devicecode 拿 user_code 交给前端 → 轮询 → 换到 token → 跑后面的链
 * 续期(refresh):
 *   用落盘的 refresh_token 直接换新 token → 跑后面的链(全程不要用户参与)
 *
 * 铁律:任何一跳失败都**如实上报**,绝不"用上一次的结果接着往下走" ——
 * 尤其是 profile 的 name 为空(没买 Java 版)必须直接失败,不能拿默认名字去启动游戏。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#elif !defined(__APPLE__)
#  include <unistd.h>
#endif

int sxcl_auth_open_browser(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return -1;
    }
#if defined(_WIN32)
    const int need = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    if (need <= 0) {
        return -1;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (wide == NULL) {
        return -1;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, need) != need) {
        free(wide);
        return -1;
    }
    /* ShellExecuteW 不阻塞(不等浏览器退出),而且不会闪黑框 —— 比 system("start") 干净 */
    const HINSTANCE rc = ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL);
    free(wide);
    return ((INT_PTR)rc > 32) ? 0 : -1;
#else
    /* fork + execlp:不用 system(),避免 URL 里的字符被 shell 解释 */
    const char *prog = NULL;
#  if defined(__APPLE__)
    prog = "/usr/bin/open";
#  else
    prog = "xdg-open";
#  endif
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        (void)execlp(prog, prog, url, (char *)NULL);
        _exit(127);
    }
    return 0;
#endif
}

static int64_t http_timeout_of(const sxcl_auth_opts *opts)
{
    return (opts != NULL && opts->http_timeout_ms > 0) ? opts->http_timeout_ms : 30000;
}

static void status(const sxcl_auth_opts *opts, const char *message)
{
    if (opts != NULL && opts->cb.on_status != NULL) {
        opts->cb.on_status(opts->cb.userdata, message);
    }
}

/* 会话里的展示名:优先 gamertag,其次 Java 版名字 */
static void refresh_account_name(sxcl_auth_session *s)
{
    if (s->xbox.gamertag[0] != '\0') {
        (void)sxcl_auth_copy(s->account_name, sizeof(s->account_name), s->xbox.gamertag);
    } else if (s->mc.name[0] != '\0') {
        (void)sxcl_auth_copy(s->account_name, sizeof(s->account_name), s->mc.name);
    }
}

/* 第 3~6 跳。成功返回 0;任何一跳失败就把 err 塞满人话后返回。 */
static int run_java_chain(sxcl_transport *tr, const sxcl_auth_opts *opts, sxcl_auth_session *s,
                          char *err, size_t err_len)
{
    const int64_t timeout = http_timeout_of(opts);
    char xbl[SXCL_AUTH_TOKEN_MAX];
    char uhs[SXCL_AUTH_NAME_MAX];
    int64_t xbl_expires = 0;
    memset(xbl, 0, sizeof(xbl));
    memset(uhs, 0, sizeof(uhs));

    status(opts, "正在做 Xbox Live 用户认证…");
    int rc = sxcl_auth_xbox_user_authenticate(tr, s->ms.access_token, timeout, xbl, sizeof(xbl),
                                              uhs, sizeof(uhs), &xbl_expires, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        sxcl_auth_secure_zero(xbl, sizeof(xbl));
        return rc;
    }

    status(opts, "正在做 XSTS 授权（中继方 = Minecraft Java 版）…");
    uint64_t xerr = 0;
    /* 自己重新取一次 XSTS:上一次会话里的 XSTS 可能已经过期,不能复用 */
    rc = sxcl_auth_xsts_authorize(tr, xbl, SXCL_AUTH_RP_MINECRAFT, timeout, &s->xbox, &xerr,
                                  err, err_len);
    s->last_error_xerr = (int64_t)xerr;
    sxcl_auth_secure_zero(xbl, sizeof(xbl));
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (s->xbox.user_hash[0] == '\0' && uhs[0] != '\0') {
        (void)sxcl_auth_copy(s->xbox.user_hash, sizeof(s->xbox.user_hash), uhs);
    }

    status(opts, "正在换取 Minecraft 访问令牌…");
    rc = sxcl_auth_minecraft_login(tr, s->xbox.user_hash, s->xbox.xsts_token, timeout, &s->mc,
                                   err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }

    /* 权益:查询失败不阻断登录(某些账号/接口抽风),但要如实记录"查没查过、几条" */
    status(opts, "正在查询 Java 版权益（mcstore）…");
    {
        int count = 0;
        char eerr[SXCL_AUTH_ERROR_MAX];
        eerr[0] = '\0';
        const int erc = sxcl_auth_minecraft_entitlements(tr, s->mc.access_token, timeout, &count,
                                                         eerr, sizeof(eerr));
        if (erc == SXCL_AUTH_OK) {
            s->mc.entitlements_checked = 1;
            s->mc.entitlement_count = count;
            if (count == 0) {
                /* 不在这里失败:档案接口才是"能不能玩"的最终判据(见下),但结论要留住 */
                (void)sxcl_auth_copy(err, err_len, eerr);
            }
        }
    }

    status(opts, "正在读取 Minecraft 档案…");
    char uuid[SXCL_AUTH_UUID_MAX];
    char name[SXCL_AUTH_NAME_MAX];
    uuid[0] = '\0';
    name[0] = '\0';
    rc = sxcl_auth_minecraft_profile(tr, s->mc.access_token, timeout, uuid, sizeof(uuid), name,
                                     sizeof(name), err, err_len);
    s->mc.profile_checked = 1;
    if (rc == SXCL_AUTH_ERR_NO_PROFILE) {
        /* **没买 Java 版**,不许继续:把账号名留在会话里(界面能显示"哪个账号没买") */
        return rc;
    }
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    (void)sxcl_auth_copy(s->mc.uuid, sizeof(s->mc.uuid), uuid);
    (void)sxcl_auth_copy(s->mc.name, sizeof(s->mc.name), name);
    refresh_account_name(s);
    return SXCL_AUTH_OK;
}

/* 授权码主路(PKCE + 环回) */
static int login_interactive(sxcl_transport *tr, const sxcl_auth_opts *opts, sxcl_auth_session *s,
                             char *err, size_t err_len)
{
    char verifier[SXCL_AUTH_PKCE_VERIFIER_MAX];
    char challenge[96];
    char state[48];
    verifier[0] = '\0';
    challenge[0] = '\0';
    if (sxcl_auth_pkce_generate(verifier, sizeof(verifier), challenge, sizeof(challenge)) !=
        SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len, "生成 PKCE 校验串失败（系统随机数不可用）");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    if (sxcl_auth_random_string(state, sizeof(state)) != 0) {
        sxcl_auth_secure_zero(verifier, sizeof(verifier));
        sxcl_auth_err(err, err_len, "生成 state 失败（系统随机数不可用）");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }

    sxcl_auth_loopback *lb = NULL;
    int rc = sxcl_auth_loopback_start(&lb, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        sxcl_auth_secure_zero(verifier, sizeof(verifier));
        /* 环回起不来(防火墙/策略)→ 明确提示可改用设备码流,而不是含糊地失败 */
        char why[SXCL_AUTH_ERROR_MAX];
        (void)sxcl_auth_copy(why, sizeof(why), err);
        sxcl_auth_err(err, err_len,
                      "%s；如果这台机器不允许本地监听，请改用设备码登录（--device-code）", why);
        return rc;
    }

    /* redirect_uri 必须和换 token 时**逐字一致**(微软会比对),而环回服务一旦销毁端口就没了,
     * 所以先把它抄下来,再去关服务。 */
    char redirect_uri[SXCL_AUTH_URL_MAX];
    (void)sxcl_auth_copy(redirect_uri, sizeof(redirect_uri), sxcl_auth_loopback_redirect_uri(lb));

    char url[SXCL_AUTH_URL_MAX];
    rc = sxcl_auth_build_authorize_url_tenant(s->client_id, s->tenant, redirect_uri, challenge,
                                              state, url, sizeof(url), err, err_len);
    if (rc != SXCL_AUTH_OK) {
        sxcl_auth_loopback_destroy(lb);
        sxcl_auth_secure_zero(verifier, sizeof(verifier));
        return rc;
    }

    /* 前端拿得到链接(CLI 会打出来给用户手点;UI 可以自己弹窗) */
    if (opts != NULL && opts->cb.on_open_url != NULL) {
        opts->cb.on_open_url(opts->cb.userdata, url);
    }
    if (opts == NULL || !opts->no_browser) {
        if (sxcl_auth_open_browser(url) != 0) {
            status(opts, "没能自动打开浏览器，请手动打开上面的链接");
        }
    }
    status(opts, "已打开浏览器，请完成微软账号登录并同意授权…");

    const int64_t wait_ms = (opts != NULL && opts->loopback_timeout_ms > 0) ? opts->loopback_timeout_ms
                                                                           : 300000;
    char code[SXCL_AUTH_TOKEN_MAX];
    char got_error[128];
    code[0] = '\0';
    got_error[0] = '\0';
    rc = sxcl_auth_loopback_wait(lb, state, code, sizeof(code), NULL, 0, got_error,
                                 sizeof(got_error), wait_ms,
                                 (opts != NULL) ? opts->should_cancel : NULL,
                                 (opts != NULL) ? opts->cb.userdata : NULL, err, err_len);
    sxcl_auth_loopback_destroy(lb);
    if (rc != SXCL_AUTH_OK) {
        sxcl_auth_secure_zero(verifier, sizeof(verifier));
        return rc;
    }

    status(opts, "已拿到授权码，正在换取令牌…");
    rc = sxcl_auth_exchange_code_tenant(tr, s->client_id, s->tenant, code, redirect_uri, verifier,
                                        http_timeout_of(opts), &s->ms, err, err_len);
    sxcl_auth_secure_zero(code, sizeof(code));
    sxcl_auth_secure_zero(verifier, sizeof(verifier));
    return rc;
}

int sxcl_auth_login(sxcl_transport *tr, const sxcl_auth_opts *opts, sxcl_auth_session *out,
                    char *err, size_t err_len)
{
    if (tr == NULL || out == NULL) {
        sxcl_auth_err(err, err_len, "内部错误:登录参数为空");
        return SXCL_AUTH_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    sxcl_settings *settings = (opts != NULL) ? opts->settings : NULL;
    if (sxcl_auth_resolve_client_id((opts != NULL) ? opts->client_id : NULL, settings,
                                    out->client_id, sizeof(out->client_id)) != SXCL_AUTH_OK ||
        !sxcl_auth_client_id_valid(out->client_id)) {
        sxcl_auth_err(err, err_len,
                      "client_id 不合法：%s（用 SXCL_AUTH_CLIENT_ID 环境变量或设置项 %s 覆盖）",
                      out->client_id[0] ? out->client_id : "(空)", SXCL_AUTH_SETTINGS_CLIENT_ID);
        return SXCL_AUTH_ERR_ARG;
    }
    if (sxcl_auth_resolve_tenant((opts != NULL) ? opts->tenant : NULL, settings, out->tenant,
                                 sizeof(out->tenant)) != SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len,
                      "租户段不合法（只允许字母数字与 . -）：用 %s 环境变量或设置项 %s 覆盖",
                      SXCL_AUTH_ENV_TENANT, SXCL_AUTH_SETTINGS_TENANT);
        return SXCL_AUTH_ERR_ARG;
    }
    int rc;
    if (opts != NULL && opts->device_code) {
        rc = sxcl_auth_device_code_login_tenant(tr, opts, out->client_id, out->tenant, &out->ms,
                                                err, err_len);
    } else {
        rc = login_interactive(tr, opts, out, err, err_len);
    }
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    if (out->ms.refresh_token[0] == '\0') {
        /* 没有 refresh token = 不能免密续期(几乎总是 scope 里少了 offline_access)。
         * 不算致命(这次登录是成功的),但必须让用户知道。 */
        status(opts, "注意：本次没有拿到 refresh token，下次还得重新登录一次");
    }
    return run_java_chain(tr, opts, out, err, err_len);
}

int sxcl_auth_refresh(sxcl_transport *tr, const sxcl_auth_opts *opts, sxcl_auth_session *session,
                      char *err, size_t err_len)
{
    if (tr == NULL || session == NULL) {
        sxcl_auth_err(err, err_len, "内部错误:续期参数为空");
        return SXCL_AUTH_ERR_ARG;
    }
    if (session->client_id[0] == '\0') {
        (void)sxcl_auth_resolve_client_id((opts != NULL) ? opts->client_id : NULL,
                                          (opts != NULL) ? opts->settings : NULL, session->client_id,
                                          sizeof(session->client_id));
    }
    if (session->tenant[0] == '\0') {
        (void)sxcl_auth_resolve_tenant((opts != NULL) ? opts->tenant : NULL,
                                       (opts != NULL) ? opts->settings : NULL, session->tenant,
                                       sizeof(session->tenant));
    }
    if (session->ms.refresh_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "没有 refresh token，免密续期不可能：请重新登录一次");
        return SXCL_AUTH_ERR_EXPIRED;
    }
    status(opts, "正在用 refresh token 续期（免密）…");
    sxcl_auth_ms_tokens fresh;
    memset(&fresh, 0, sizeof(fresh));
    const int rc = sxcl_auth_refresh_ms_tenant(tr, session->client_id, session->tenant,
                                               session->ms.refresh_token, http_timeout_of(opts),
                                               &fresh, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    /* 新 token 覆盖旧的;refresh token 缺失时 refresh_ms 已经沿用旧值 */
    session->ms = fresh;
    return run_java_chain(tr, opts, session, err, err_len);
}

int sxcl_auth_session_bedrock(sxcl_transport *tr, const sxcl_auth_opts *opts,
                              sxcl_auth_session *session, char *err, size_t err_len)
{
    if (tr == NULL || session == NULL) {
        sxcl_auth_err(err, err_len, "内部错误:基岩登录参数为空");
        return SXCL_AUTH_ERR_ARG;
    }
    if (session->ms.access_token[0] == '\0') {
        sxcl_auth_err(err, err_len, "还没有微软登录结果，先登录再取基岩链");
        return SXCL_AUTH_ERR_ARG;
    }
    /* 微软令牌过期就先续期(基岩与 Java 共用同一份微软令牌,但各自保留自己的 XSTS/权益) */
    if (sxcl_auth_ms_expired(session, sxcl_auth_now(), 60)) {
        if (session->ms.refresh_token[0] == '\0') {
            sxcl_auth_err(err, err_len, "微软令牌已过期且没有 refresh token：请重新登录");
            return SXCL_AUTH_ERR_EXPIRED;
        }
        status(opts, "微软令牌已过期，先免密续期…");
        sxcl_auth_ms_tokens fresh;
        memset(&fresh, 0, sizeof(fresh));
        const int rc = sxcl_auth_refresh_ms_tenant(tr, session->client_id, session->tenant,
                                                   session->ms.refresh_token,
                                                   http_timeout_of(opts), &fresh, err, err_len);
        if (rc != SXCL_AUTH_OK) {
            return rc;
        }
        session->ms = fresh;
    }
    status(opts, "正在跑基岩链（XSTS 中继方 = multiplayer.minecraft.net）…");
    return sxcl_auth_bedrock_authorize(tr, opts, session->ms.access_token, &session->xbox_bedrock,
                                       &session->bedrock, err, err_len);
}
