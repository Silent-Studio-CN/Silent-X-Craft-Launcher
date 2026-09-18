/* 会话小工具 + 配置解析(client_id / 租户段)+ 令牌打码。
 *
 * 这些函数看着零碎,但它们决定了"用户在哪配置、代码认哪一个"的唯一口径:
 *   client_id:显式参数 > SXCL_AUTH_CLIENT_ID 环境变量 > settings["auth.client_id"] > 内置默认
 *   租户段   :显式参数 > SXCL_AUTH_TENANT    环境变量 > settings["auth.tenant"]    > consumers
 * 别在别的文件里再写一套 if(getenv(...)) —— 两个口径迟早会分叉。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sxcl_auth_session_has_ms(const sxcl_auth_session *s)
{
    return (s != NULL && s->ms.access_token[0] != '\0') ? 1 : 0;
}

int sxcl_auth_session_has_mc(const sxcl_auth_session *s)
{
    return (s != NULL && s->mc.access_token[0] != '\0') ? 1 : 0;
}

int sxcl_auth_ms_expired(const sxcl_auth_session *s, int64_t now, int64_t skew_seconds)
{
    if (s == NULL || s->ms.access_token[0] == '\0') {
        return 1; /* 没有令牌 = 必须重新登录 */
    }
    if (s->ms.expires_at <= 0) {
        return 0; /* 拿不到 expires_in 时不擅自判过期(宁可让服务端回 401) */
    }
    return (now + skew_seconds >= s->ms.expires_at) ? 1 : 0;
}

int sxcl_auth_mc_expired(const sxcl_auth_session *s, int64_t now, int64_t skew_seconds)
{
    if (s == NULL || s->mc.access_token[0] == '\0') {
        return 1;
    }
    if (s->mc.expires_at <= 0) {
        return 0;
    }
    return (now + skew_seconds >= s->mc.expires_at) ? 1 : 0;
}

int sxcl_auth_mask_token(const char *token, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }
    if (token == NULL || token[0] == '\0') {
        return sxcl_auth_copy(out, out_len, "(无)") < 0 ? 0 : (int)strlen(out);
    }
    const size_t len = strlen(token);
    const size_t keep = (len < 6u) ? len : 6u;
    char head[8];
    memcpy(head, token, keep);
    head[keep] = '\0';
    (void)snprintf(out, out_len, "%s…（共 %llu 字节，不打印全文）", head, (unsigned long long)len);
    out[out_len - 1] = '\0';
    return (int)strlen(out);
}

int sxcl_auth_resolve_client_id(const char *explicit_id, sxcl_settings *settings,
                                char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    out[0] = '\0';
    if (explicit_id != NULL && explicit_id[0] != '\0') {
        (void)sxcl_auth_copy(out, out_len, explicit_id);
        return SXCL_AUTH_OK;
    }
    const char *env = getenv(SXCL_AUTH_ENV_CLIENT_ID);
    if (env != NULL && env[0] != '\0') {
        (void)sxcl_auth_copy(out, out_len, env);
        return SXCL_AUTH_OK;
    }
    if (settings != NULL) {
        const char *v = sxcl_settings_get(settings, SXCL_AUTH_SETTINGS_CLIENT_ID, "");
        if (v != NULL && v[0] != '\0') {
            (void)sxcl_auth_copy(out, out_len, v);
            return SXCL_AUTH_OK;
        }
    }
    (void)sxcl_auth_copy(out, out_len, SXCL_AUTH_DEFAULT_CLIENT_ID);
    return SXCL_AUTH_OK;
}

int sxcl_auth_resolve_tenant(const char *explicit_tenant, sxcl_settings *settings,
                             char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    out[0] = '\0';
    if (explicit_tenant != NULL && explicit_tenant[0] != '\0') {
        if (!sxcl_auth_tenant_valid(explicit_tenant)) {
            return SXCL_AUTH_ERR_ARG;
        }
        (void)sxcl_auth_copy(out, out_len, explicit_tenant);
        return SXCL_AUTH_OK;
    }
    const char *env = getenv(SXCL_AUTH_ENV_TENANT);
    if (env != NULL && env[0] != '\0') {
        if (!sxcl_auth_tenant_valid(env)) {
            return SXCL_AUTH_ERR_ARG;
        }
        (void)sxcl_auth_copy(out, out_len, env);
        return SXCL_AUTH_OK;
    }
    if (settings != NULL) {
        const char *v = sxcl_settings_get(settings, SXCL_AUTH_SETTINGS_TENANT, "");
        if (v != NULL && v[0] != '\0') {
            if (!sxcl_auth_tenant_valid(v)) {
                return SXCL_AUTH_ERR_ARG;
            }
            (void)sxcl_auth_copy(out, out_len, v);
            return SXCL_AUTH_OK;
        }
    }
    (void)sxcl_auth_copy(out, out_len, SXCL_AUTH_TENANT_DEFAULT);
    return SXCL_AUTH_OK;
}

/* ── 默认配置目录(与 keymap_store 的 platform.py 分支一致:
 *    Windows %APPDATA%/SilentXCraftLauncher(没有 APPDATA 时 %USERPROFILE%/AppData/Roaming/…)
 *    macOS   ~/Library/Application Support/SilentXCraftLauncher
 *    Linux   $XDG_CONFIG_HOME/silentxcraftlauncher 或 ~/.config/silentxcraftlauncher) ── */
int sxcl_auth_default_config_dir(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
#if defined(_WIN32)
    const char *appdata = getenv("APPDATA");
    if (appdata != NULL && appdata[0] != '\0') {
        return (snprintf(out, out_len, "%s\\SilentXCraftLauncher", appdata) > 0) ? 0 : -1;
    }
    const char *home = getenv("USERPROFILE");
    if (home != NULL && home[0] != '\0') {
        return (snprintf(out, out_len, "%s\\AppData\\Roaming\\SilentXCraftLauncher", home) > 0) ? 0 : -1;
    }
    return -1;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return (snprintf(out, out_len, "%s/Library/Application Support/SilentXCraftLauncher", home) > 0)
                   ? 0
                   : -1;
    }
    return -1;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        return (snprintf(out, out_len, "%s/silentxcraftlauncher", xdg) > 0) ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return (snprintf(out, out_len, "%s/.config/silentxcraftlauncher", home) > 0) ? 0 : -1;
    }
    return -1;
#endif
}
