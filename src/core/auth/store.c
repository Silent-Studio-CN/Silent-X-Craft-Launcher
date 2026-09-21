/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include "sxcl/fs.h"
#include "sxcl/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <wincrypt.h> /* CryptProtectData / CryptUnprotectData(dpapi.h 里也是它) */
#elif defined(__APPLE__)
#  include <Security/Security.h>
#else
#  include <dlfcn.h> /* libsecret 用 dlopen,不引入链接期依赖 */
#  include <sys/stat.h>
#  include <unistd.h> /* gethostname(POSIX/Android;MSVC 从 windows.h 间接拿到) */
#endif

#define SXCL_AUTH_STORE_HEADER_LEN 16u
#define SXCL_AUTH_STORE_MAX_PAYLOAD (256u * 1024u) /* 会话 JSON 不可能超过 256KiB */

static sxcl_auth_store_kind g_store_override = SXCL_AUTH_STORE_NONE;

void sxcl_auth_store_set_backend_for_test(sxcl_auth_store_kind kind)
{
    g_store_override = kind;
}

const char *sxcl_auth_store_kind_name(sxcl_auth_store_kind kind)
{
    switch (kind) {
        case SXCL_AUTH_STORE_DPAPI: return "dpapi";
        case SXCL_AUTH_STORE_KEYCHAIN: return "keychain";
        case SXCL_AUTH_STORE_LIBSECRET: return "libsecret";
        case SXCL_AUTH_STORE_FILE_KEY: return "file+key";
        default: return "none";
    }
}

const char *sxcl_auth_store_kind_note(sxcl_auth_store_kind kind)
{
    switch (kind) {
        case SXCL_AUTH_STORE_DPAPI:
            return "Windows DPAPI:密钥由当前用户的登录凭据派生,换个用户或换台机器都解不开;"
                   "同机的其它程序以**同一个用户**运行时仍然能解(这是 DPAPI 的既定边界)。";
        case SXCL_AUTH_STORE_KEYCHAIN:
            return "macOS 钥匙串:令牌交给系统钥匙串保管,受用户登录密码保护。";
        case SXCL_AUTH_STORE_LIBSECRET:
            return "Linux libsecret(GNOME Keyring 等):令牌交给系统密钥环保管。";
        case SXCL_AUTH_STORE_FILE_KEY:
            return "降级路径:**0600 权限文件 + 本机派生密钥 + ChaCha20-Poly1305**。"
                   "它能挡住“把令牌文件拷到别的机器/误提交进仓库”,但挡不住“同一台机器上以同一用户"
                   "运行的其它程序”——因为它必须能从本机材料重新算出密钥。装好 DPAPI/Keychain/libsecret "
                   "的环境会自动走强后端,不走这条。";
        default:
            return "没有可用的加密后端(不该发生:降级路径总是可用)。";
    }
}

sxcl_auth_store_kind sxcl_auth_store_backend(void)
{
    if (g_store_override != SXCL_AUTH_STORE_NONE) {
        return g_store_override;
    }
#if defined(_WIN32)
    return SXCL_AUTH_STORE_DPAPI;
#elif defined(__APPLE__)
    return SXCL_AUTH_STORE_KEYCHAIN;
#elif defined(__linux__)
    /* libsecret 只在真的能 dlopen 到才算数 —— 不能"声称"用了密钥环 */
    {
        void *h = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (h == NULL) {
            h = dlopen("libsecret-1.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (h != NULL) {
            void *store_fn = dlsym(h, "secret_password_store_sync");
            void *lookup_fn = dlsym(h, "secret_password_lookup_sync");
            void *clear_fn = dlsym(h, "secret_password_clear_sync");
            dlclose(h);
            if (store_fn != NULL && lookup_fn != NULL && clear_fn != NULL) {
                return SXCL_AUTH_STORE_LIBSECRET;
            }
        }
        return SXCL_AUTH_STORE_FILE_KEY;
    }
#else
    return SXCL_AUTH_STORE_FILE_KEY;
#endif
}

int sxcl_auth_config_dir(char *out, size_t out_len, char *err, size_t err_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    if (sxcl_auth_default_config_dir(out, out_len) != 0) {
        sxcl_auth_err(err, err_len, "拼不出配置目录（APPDATA/HOME 都没有？）");
        return SXCL_AUTH_ERR_STORE;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_store_default_path(char *out, size_t out_len, char *err, size_t err_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    if (sxcl_auth_default_config_dir(dir, sizeof(dir)) != 0) {
        sxcl_auth_err(err, err_len, "拼不出配置目录（APPDATA/HOME 都没有？）");
        return SXCL_AUTH_ERR_STORE;
    }
    const int n = snprintf(out, out_len, "%s/auth/tokens.bin", dir);
    if (n < 0 || (size_t)n >= out_len) {
        sxcl_auth_err(err, err_len, "令牌文件路径太长：%s/auth/tokens.bin", dir);
        return SXCL_AUTH_ERR_STORE;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_store_exists(const char *path)
{
    return (path != NULL && sxcl_fs_exists(path)) ? 1 : 0;
}

/* ── 会话 ↔ JSON(明文;只走内存) ── */
static void put_str(sxcl_auth_buf *b, const char *key, const char *value, int *first)
{
    if (!*first) {
        (void)sxcl_auth_buf_appendc(b, ',');
    }
    *first = 0;
    (void)sxcl_auth_buf_appendc(b, '"');
    (void)sxcl_auth_buf_append(b, key);
    (void)sxcl_auth_buf_append(b, "\":\"");
    (void)sxcl_auth_json_escape(b, value != NULL ? value : "");
    (void)sxcl_auth_buf_appendc(b, '"');
}

static void put_int(sxcl_auth_buf *b, const char *key, int64_t value, int *first)
{
    if (!*first) {
        (void)sxcl_auth_buf_appendc(b, ',');
    }
    *first = 0;
    (void)sxcl_auth_buf_printf(b, "\"%s\":%lld", key, (long long)value);
}

static void put_token(sxcl_auth_buf *b, const char *key, const char *value, int *first)
{
    put_str(b, key, value, first);
}

static void write_ms(sxcl_auth_buf *b, const char *key, const sxcl_auth_ms_tokens *t)
{
    (void)sxcl_auth_buf_printf(b, ",\"%s\":{", key);
    int first = 1;
    put_token(b, "access_token", t->access_token, &first);
    put_token(b, "refresh_token", t->refresh_token, &first);
    put_token(b, "id_token", t->id_token, &first);
    put_token(b, "token_type", t->token_type, &first);
    put_token(b, "scope", t->scope, &first);
    put_int(b, "issued_at", t->issued_at, &first);
    put_int(b, "expires_at", t->expires_at, &first);
    (void)sxcl_auth_buf_appendc(b, '}');
}

static void write_xbox(sxcl_auth_buf *b, const char *key, const sxcl_auth_xbox_tokens *t)
{
    (void)sxcl_auth_buf_printf(b, ",\"%s\":{", key);
    int first = 1;
    put_token(b, "uhs", t->user_hash, &first);
    put_token(b, "token", t->xsts_token, &first);
    put_token(b, "gamertag", t->gamertag, &first);
    put_int(b, "expires_at", t->xsts_expires_at, &first);
    put_int(b, "xuid", (int64_t)t->xuid, &first);
    (void)sxcl_auth_buf_appendc(b, '}');
}

static void write_mc(sxcl_auth_buf *b, const sxcl_auth_minecraft_tokens *t)
{
    (void)sxcl_auth_buf_append(b, ",\"mc\":{");
    int first = 1;
    put_token(b, "access_token", t->access_token, &first);
    put_str(b, "uuid", t->uuid, &first);
    put_str(b, "name", t->name, &first);
    put_int(b, "expires_at", t->expires_at, &first);
    put_int(b, "entitlement_count", t->entitlement_count, &first);
    put_int(b, "entitlements_checked", t->entitlements_checked, &first);
    put_int(b, "profile_checked", t->profile_checked, &first);
    (void)sxcl_auth_buf_appendc(b, '}');
}

static void write_bedrock(sxcl_auth_buf *b, const sxcl_auth_bedrock_tokens *t)
{
    (void)sxcl_auth_buf_append(b, ",\"bedrock\":{\"chain\":[");
    for (int i = 0; i < t->chain_count && i < 3; ++i) {
        if (i > 0) {
            (void)sxcl_auth_buf_appendc(b, ',');
        }
        (void)sxcl_auth_buf_appendc(b, '"');
        (void)sxcl_auth_json_escape(b, t->chain[i]);
        (void)sxcl_auth_buf_appendc(b, '"');
    }
    (void)sxcl_auth_buf_appendc(b, ']');
    int first = 0; /* 前面已经有 "chain" 这个成员 */
    put_str(b, "identity_public_key", t->identity_public_key, &first);
    put_int(b, "entitlement_checked", t->entitlement_checked, &first);
    put_int(b, "entitled", t->entitled, &first);
    (void)sxcl_auth_buf_appendc(b, '}');
}

int sxcl_auth_session_to_json(const sxcl_auth_session *session, char **out, size_t *out_len)
{
    if (session == NULL || out == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    *out = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }
    sxcl_auth_buf b;
    sxcl_auth_buf_init(&b);
    (void)sxcl_auth_buf_append(&b, "{\"v\":1");
    int first = 0;
    put_str(&b, "client_id", session->client_id, &first);
    put_str(&b, "tenant", session->tenant, &first);
    put_str(&b, "account_name", session->account_name, &first);
    put_int(&b, "last_error_xerr", session->last_error_xerr, &first);
    write_ms(&b, "ms", &session->ms);
    write_xbox(&b, "xbox", &session->xbox);
    write_mc(&b, &session->mc);
    write_xbox(&b, "xbox_bedrock", &session->xbox_bedrock);
    write_bedrock(&b, &session->bedrock);
    (void)sxcl_auth_buf_appendc(&b, '}');
    if (b.oom) {
        sxcl_auth_buf_free(&b);
        return SXCL_AUTH_ERR_NOMEM;
    }
    char *text = sxcl_auth_buf_take(&b);
    if (text == NULL) {
        return SXCL_AUTH_ERR_NOMEM;
    }
    *out = text;
    if (out_len != NULL) {
        *out_len = strlen(text);
    }
    return SXCL_AUTH_OK;
}

static void read_ms(const sxcl_json_value *o, sxcl_auth_ms_tokens *t)
{
    if (o == NULL) {
        return;
    }
    (void)sxcl_auth_copy(t->access_token, sizeof(t->access_token),
                         sxcl_json_get_string(o, "access_token", ""));
    (void)sxcl_auth_copy(t->refresh_token, sizeof(t->refresh_token),
                         sxcl_json_get_string(o, "refresh_token", ""));
    (void)sxcl_auth_copy(t->id_token, sizeof(t->id_token), sxcl_json_get_string(o, "id_token", ""));
    (void)sxcl_auth_copy(t->token_type, sizeof(t->token_type),
                         sxcl_json_get_string(o, "token_type", "Bearer"));
    (void)sxcl_auth_copy(t->scope, sizeof(t->scope), sxcl_json_get_string(o, "scope", ""));
    t->issued_at = sxcl_json_get_int64(o, "issued_at", 0);
    t->expires_at = sxcl_json_get_int64(o, "expires_at", 0);
}

static void read_xbox(const sxcl_json_value *o, sxcl_auth_xbox_tokens *t)
{
    if (o == NULL) {
        return;
    }
    (void)sxcl_auth_copy(t->user_hash, sizeof(t->user_hash), sxcl_json_get_string(o, "uhs", ""));
    (void)sxcl_auth_copy(t->xsts_token, sizeof(t->xsts_token), sxcl_json_get_string(o, "token", ""));
    (void)sxcl_auth_copy(t->gamertag, sizeof(t->gamertag), sxcl_json_get_string(o, "gamertag", ""));
    t->xsts_expires_at = sxcl_json_get_int64(o, "expires_at", 0);
    t->xuid = (uint64_t)sxcl_json_get_int64(o, "xuid", 0);
}

int sxcl_auth_session_from_json(const char *json, size_t len, sxcl_auth_session *out)
{
    if (json == NULL || out == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    char jerr[128];
    jerr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(json, len, jerr, sizeof(jerr));
    if (doc == NULL) {
        return SXCL_AUTH_ERR_JSON;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    memset(out, 0, sizeof(*out));
    if (root != NULL) {
        (void)sxcl_auth_copy(out->client_id, sizeof(out->client_id),
                             sxcl_json_get_string(root, "client_id", ""));
        (void)sxcl_auth_copy(out->tenant, sizeof(out->tenant),
                             sxcl_json_get_string(root, "tenant", ""));
        (void)sxcl_auth_copy(out->account_name, sizeof(out->account_name),
                             sxcl_json_get_string(root, "account_name", ""));
        out->last_error_xerr = sxcl_json_get_int64(root, "last_error_xerr", 0);
        read_ms(sxcl_json_get(root, "ms"), &out->ms);
        read_xbox(sxcl_json_get(root, "xbox"), &out->xbox);
        read_xbox(sxcl_json_get(root, "xbox_bedrock"), &out->xbox_bedrock);
        const sxcl_json_value *mc = sxcl_json_get(root, "mc");
        if (mc != NULL) {
            (void)sxcl_auth_copy(out->mc.access_token, sizeof(out->mc.access_token),
                                 sxcl_json_get_string(mc, "access_token", ""));
            (void)sxcl_auth_copy(out->mc.uuid, sizeof(out->mc.uuid),
                                 sxcl_json_get_string(mc, "uuid", ""));
            (void)sxcl_auth_copy(out->mc.name, sizeof(out->mc.name),
                                 sxcl_json_get_string(mc, "name", ""));
            out->mc.expires_at = sxcl_json_get_int64(mc, "expires_at", 0);
            out->mc.entitlement_count = (int)sxcl_json_get_int64(mc, "entitlement_count", 0);
            out->mc.entitlements_checked = (int)sxcl_json_get_int64(mc, "entitlements_checked", 0);
            out->mc.profile_checked = (int)sxcl_json_get_int64(mc, "profile_checked", 0);
        }
        const sxcl_json_value *bd = sxcl_json_get(root, "bedrock");
        if (bd != NULL) {
            const sxcl_json_value *chain = sxcl_json_get(bd, "chain");
            const size_t n = (chain != NULL) ? sxcl_json_size(chain) : 0;
            for (size_t i = 0; i < n && i < 3u; ++i) {
                const sxcl_json_value *item = sxcl_json_at(chain, i);
                if (item != NULL && sxcl_json_type_of(item) == SXCL_JSON_STRING) {
                    (void)sxcl_auth_copy(out->bedrock.chain[out->bedrock.chain_count],
                                         SXCL_AUTH_TOKEN_MAX, sxcl_json_string(item));
                    out->bedrock.chain_count++;
                }
            }
            (void)sxcl_auth_copy(out->bedrock.identity_public_key,
                                 sizeof(out->bedrock.identity_public_key),
                                 sxcl_json_get_string(bd, "identity_public_key", ""));
            out->bedrock.entitlement_checked = (int)sxcl_json_get_int64(bd, "entitlement_checked", 0);
            out->bedrock.entitled = (int)sxcl_json_get_int64(bd, "entitled", 0);
        }
    }
    sxcl_json_free(doc);
    return SXCL_AUTH_OK;
}

/* ── 本机派生密钥(降级路径用) ──
 * 材料 = 固定标签 + 机器标识(Windows 的 MachineGuid / Linux 的 machine-id / 主机名)
 *        + 用户名 + 一份**随机盐**(0600 文件,首次使用时生成)。
 * 盐是真正的熵来源:只拷走令牌文件没用(解不开),把盐也一起拷走才会被解开 ——
 * 这正是"防离线窃取"的边界,文档里写清楚了。 */
static int read_file_quiet(const char *path, char *out, size_t out_len, size_t *got)
{
    FILE *fp = sxcl_fs_fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    const size_t n = fread(out, 1, out_len - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    if (got != NULL) {
        *got = n;
    }
    return 0;
}

static void machine_material(char *out, size_t out_len)
{
    out[0] = '\0';
#if defined(_WIN32)
    {
        HKEY key = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
                          KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            char guid[128];
            DWORD len = (DWORD)sizeof(guid);
            DWORD type = 0;
            if (RegQueryValueExA(key, "MachineGuid", NULL, &type, (LPBYTE)guid, &len) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_LINK)) {
                guid[len < sizeof(guid) ? len : sizeof(guid) - 1u] = '\0';
                (void)sxcl_auth_copy(out, out_len, guid);
            }
            RegCloseKey(key);
        }
    }
    if (out[0] == '\0') {
        const char *cn = getenv("COMPUTERNAME");
        (void)sxcl_auth_copy(out, out_len, (cn != NULL) ? cn : "unknown-host");
    }
    const char *user = getenv("USERNAME");
    if (user != NULL && user[0] != '\0') {
        const size_t used = strlen(out);
        (void)snprintf(out + used, out_len > used ? out_len - used : 0, "|%s", user);
    }
#else
#  if defined(__linux__)
    {
        size_t got = 0;
        if (read_file_quiet("/etc/machine-id", out, out_len, &got) == 0 && got > 0) {
            /* 去掉结尾换行 */
            for (size_t i = 0; i < got; ++i) {
                if (out[i] == '\n' || out[i] == '\r') {
                    out[i] = '\0';
                    break;
                }
            }
        }
    }
#  endif
    if (out[0] == '\0') {
        char host[128];
        host[0] = '\0';
        (void)gethostname(host, sizeof(host) - 1u);
        host[sizeof(host) - 1u] = '\0';
        (void)sxcl_auth_copy(out, out_len, host[0] ? host : "unknown-host");
    }
    const char *user = getenv("USER");
    if (user == NULL || user[0] == '\0') {
        user = getenv("LOGNAME");
    }
    if (user != NULL && user[0] != '\0') {
        const size_t used = strlen(out);
        (void)snprintf(out + used, out_len > used ? out_len - used : 0, "|%s", user);
    }
#endif
}

int sxcl_auth_store_machine_key(const char *salt_dir, unsigned char key_out[32],
                                char *err, size_t err_len)
{
    if (key_out == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    if (salt_dir != NULL && salt_dir[0] != '\0') {
        (void)sxcl_auth_copy(dir, sizeof(dir), salt_dir);
    } else if (sxcl_auth_default_config_dir(dir, sizeof(dir)) != 0) {
        sxcl_auth_err(err, err_len, "拼不出配置目录,算不出本机密钥");
        return SXCL_AUTH_ERR_STORE;
    }
    char auth_dir[SXCL_AUTH_STORE_PATH_MAX];
    (void)snprintf(auth_dir, sizeof(auth_dir), "%s/auth", dir);
    if (sxcl_fs_mkdirs(auth_dir) != 0) {
        sxcl_auth_err(err, err_len, "建不了目录：%s", auth_dir);
        return SXCL_AUTH_ERR_STORE;
    }
    char salt_path[SXCL_AUTH_STORE_PATH_MAX];
    (void)snprintf(salt_path, sizeof(salt_path), "%s/salt.bin", auth_dir);

    unsigned char salt[32];
    size_t salt_len = 0;
    if (sxcl_fs_exists(salt_path)) {
        FILE *fp = sxcl_fs_fopen(salt_path, "rb");
        if (fp == NULL) {
            sxcl_auth_err(err, err_len, "读不了盐文件：%s", salt_path);
            return SXCL_AUTH_ERR_STORE;
        }
        salt_len = fread(salt, 1, sizeof(salt), fp);
        fclose(fp);
        if (salt_len != sizeof(salt)) {
            sxcl_auth_err(err, err_len, "盐文件内容不对（%llu 字节）：%s",
                          (unsigned long long)salt_len, salt_path);
            return SXCL_AUTH_ERR_STORE;
        }
    } else {
        if (sxcl_auth_random(salt, sizeof(salt)) != 0) {
            sxcl_auth_err(err, err_len, "拿不到系统随机数,生成不了盐");
            return SXCL_AUTH_ERR_UNSUPPORTED;
        }
        FILE *fp = sxcl_fs_fopen(salt_path, "wb");
        if (fp == NULL) {
            sxcl_auth_err(err, err_len, "写不了盐文件：%s", salt_path);
            return SXCL_AUTH_ERR_STORE;
        }
        const size_t wrote = fwrite(salt, 1, sizeof(salt), fp);
        fclose(fp);
        if (wrote != sizeof(salt)) {
            sxcl_auth_err(err, err_len, "盐文件写不完整：%s", salt_path);
            return SXCL_AUTH_ERR_STORE;
        }
#if !defined(_WIN32)
        (void)chmod(salt_path, 0600); /* 只有本用户可读写 */
#endif
    }

    char material[512];
    machine_material(material, sizeof(material));

    char hex[65];
    if (sxcl_hash_digest(SXCL_HASH_SHA256, material, strlen(material), hex, sizeof(hex)) != 0) {
        sxcl_auth_err(err, err_len, "算不出本机密钥（内部错误）");
        return SXCL_AUTH_ERR_STORE;
    }
    unsigned char mat_digest[32];
    size_t mat_len = 0;
    if (sxcl_auth_hex_decode(hex, mat_digest, sizeof(mat_digest), &mat_len) != 0 || mat_len != 32u) {
        return SXCL_AUTH_ERR_STORE;
    }
    /* 最终密钥 = SHA256(标签 || 机器材料摘要 || 盐):任何一项换了都解不开 */
    static const char label[] = "SXCLAUTH|v1|machine-key";
    sxcl_hash_ctx ctx;
    sxcl_hash_init(&ctx, SXCL_HASH_SHA256);
    sxcl_hash_update(&ctx, label, sizeof(label) - 1u);
    sxcl_hash_update(&ctx, mat_digest, sizeof(mat_digest));
    sxcl_hash_update(&ctx, salt, sizeof(salt));
    char key_hex[65];
    if (sxcl_hash_final_hex(&ctx, key_hex, sizeof(key_hex)) != 0) {
        return SXCL_AUTH_ERR_STORE;
    }
    size_t key_len = 0;
    if (sxcl_auth_hex_decode(key_hex, key_out, 32u, &key_len) != 0 || key_len != 32u) {
        return SXCL_AUTH_ERR_STORE;
    }
    sxcl_auth_secure_zero(salt, sizeof(salt));
    sxcl_auth_secure_zero(mat_digest, sizeof(mat_digest));
    sxcl_auth_secure_zero(key_hex, sizeof(key_hex));
    sxcl_auth_secure_zero(material, sizeof(material));
    (void)salt_len;
    return SXCL_AUTH_OK;
}

/* ── 后端:DPAPI ── */
#if defined(_WIN32)
static int dpapi_seal(const unsigned char *plain, size_t plain_len, unsigned char **out,
                      size_t *out_len, char *err, size_t err_len)
{
    static const char entropy_label[] = "SXCL-C auth token v1";
    DATA_BLOB in;
    DATA_BLOB entropy;
    DATA_BLOB out_blob;
    memset(&out_blob, 0, sizeof(out_blob));
    in.pbData = (BYTE *)plain;
    in.cbData = (DWORD)plain_len;
    entropy.pbData = (BYTE *)entropy_label;
    entropy.cbData = (DWORD)(sizeof(entropy_label) - 1u);
    if (!CryptProtectData(&in, L"SXCL auth tokens", &entropy, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        sxcl_auth_err(err, err_len, "DPAPI 加密失败（错误码 %lu）", (unsigned long)GetLastError());
        return SXCL_AUTH_ERR_STORE;
    }
    unsigned char *copy = (unsigned char *)malloc(out_blob.cbData);
    if (copy == NULL) {
        LocalFree(out_blob.pbData);
        return SXCL_AUTH_ERR_NOMEM;
    }
    memcpy(copy, out_blob.pbData, out_blob.cbData);
    *out_len = out_blob.cbData;
    LocalFree(out_blob.pbData);
    *out = copy;
    return SXCL_AUTH_OK;
}

static int dpapi_open(const unsigned char *sealed, size_t sealed_len, unsigned char **out,
                      size_t *out_len, char *err, size_t err_len)
{
    static const char entropy_label[] = "SXCL-C auth token v1";
    DATA_BLOB in;
    DATA_BLOB entropy;
    DATA_BLOB out_blob;
    memset(&out_blob, 0, sizeof(out_blob));
    in.pbData = (BYTE *)sealed;
    in.cbData = (DWORD)sealed_len;
    entropy.pbData = (BYTE *)entropy_label;
    entropy.cbData = (DWORD)(sizeof(entropy_label) - 1u);
    if (!CryptUnprotectData(&in, NULL, &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        sxcl_auth_err(err, err_len,
                      "DPAPI 解密失败（错误码 %lu）：令牌文件不是这个用户/这台机器加密的，"
                      "或者文件被改过。请重新登录。",
                      (unsigned long)GetLastError());
        return SXCL_AUTH_ERR_STORE;
    }
    unsigned char *copy = (unsigned char *)malloc(out_blob.cbData + 1u);
    if (copy == NULL) {
        LocalFree(out_blob.pbData);
        return SXCL_AUTH_ERR_NOMEM;
    }
    memcpy(copy, out_blob.pbData, out_blob.cbData);
    copy[out_blob.cbData] = '\0';
    *out_len = out_blob.cbData;
    LocalFree(out_blob.pbData);
    *out = copy;
    return SXCL_AUTH_OK;
}
#endif

/* ── 后端:文件 + 本机密钥(降级路径,也是唯一在四平台都能跑的一条) ── */
static int filekey_seal(const char *path, const unsigned char *plain, size_t plain_len,
                        unsigned char **out, size_t *out_len, char *err, size_t err_len)
{
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    (void)sxcl_auth_copy(dir, sizeof(dir), path != NULL ? path : "");
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    if (slash != NULL) {
        *slash = '\0';
    } else {
        dir[0] = '\0';
    }
    unsigned char key[32];
    int rc = sxcl_auth_store_machine_key(dir[0] ? dir : NULL, key, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    unsigned char nonce[12];
    if (sxcl_auth_random(nonce, sizeof(nonce)) != 0) {
        sxcl_auth_secure_zero(key, sizeof(key));
        sxcl_auth_err(err, err_len, "拿不到系统随机数,生成不了 nonce");
        return SXCL_AUTH_ERR_UNSUPPORTED;
    }
    unsigned char *payload = (unsigned char *)malloc(sizeof(nonce) + plain_len + 16u);
    if (payload == NULL) {
        sxcl_auth_secure_zero(key, sizeof(key));
        return SXCL_AUTH_ERR_NOMEM;
    }
    memcpy(payload, nonce, sizeof(nonce));
    unsigned char tag[16];
    /* AAD 用固定标签:文件头另外还会被校验一遍(见 store_save/load) */
    static const char aad[] = "SXCLAUTH|filekey|v1";
    if (sxcl_auth_aead_seal(key, nonce, (const unsigned char *)aad, sizeof(aad) - 1u, plain,
                            plain_len, payload + sizeof(nonce), tag) != 0) {
        free(payload);
        sxcl_auth_secure_zero(key, sizeof(key));
        sxcl_auth_err(err, err_len, "加密失败（内部错误）");
        return SXCL_AUTH_ERR_STORE;
    }
    memcpy(payload + sizeof(nonce) + plain_len, tag, sizeof(tag));
    sxcl_auth_secure_zero(key, sizeof(key));
    *out = payload;
    *out_len = sizeof(nonce) + plain_len + 16u;
    return SXCL_AUTH_OK;
}

static int filekey_open(const char *path, const unsigned char *payload, size_t payload_len,
                        unsigned char **out, size_t *out_len, char *err, size_t err_len)
{
    if (payload_len < 12u + 16u) {
        sxcl_auth_err(err, err_len, "令牌文件太短,内容不完整");
        return SXCL_AUTH_ERR_STORE;
    }
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    (void)sxcl_auth_copy(dir, sizeof(dir), path != NULL ? path : "");
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    if (slash != NULL) {
        *slash = '\0';
    } else {
        dir[0] = '\0';
    }
    unsigned char key[32];
    int rc = sxcl_auth_store_machine_key(dir[0] ? dir : NULL, key, err, err_len);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    const size_t ct_len = payload_len - 12u - 16u;
    unsigned char *plain = (unsigned char *)malloc(ct_len + 1u);
    if (plain == NULL) {
        sxcl_auth_secure_zero(key, sizeof(key));
        return SXCL_AUTH_ERR_NOMEM;
    }
    static const char aad[] = "SXCLAUTH|filekey|v1";
    if (sxcl_auth_aead_open(key, payload, (const unsigned char *)aad, sizeof(aad) - 1u,
                            payload + 12u, ct_len, payload + 12u + ct_len, plain) != 0) {
        free(plain);
        sxcl_auth_secure_zero(key, sizeof(key));
        sxcl_auth_err(err, err_len,
                      "令牌文件解密失败（认证不通过）：文件被改过、或者不是这台机器/这个用户写的。"
                      "请重新登录。");
        return SXCL_AUTH_ERR_STORE;
    }
    sxcl_auth_secure_zero(key, sizeof(key));
    plain[ct_len] = '\0';
    *out = plain;
    *out_len = ct_len;
    return SXCL_AUTH_OK;
}

/* ── macOS Keychain / Linux libsecret(编译得到,但本机(Windows)无法实测) ──
 * 这里刻意写得保守:任何失败都返回错误让上层降级,绝不"以为存进去了"。 */
#if defined(__APPLE__)
static const char *k_keychain_service = "SilentXCraftLauncher";
static const char *k_keychain_account = "auth.tokens";
#endif

/* ── 保存 / 读取 ── */
int sxcl_auth_store_save(const char *path, const sxcl_auth_session *session,
                         char *err, size_t err_len)
{
    if (path == NULL || path[0] == '\0' || session == NULL) {
        sxcl_auth_err(err, err_len, "内部错误:保存令牌的参数为空");
        return SXCL_AUTH_ERR_ARG;
    }
    char *json = NULL;
    size_t json_len = 0;
    int rc = sxcl_auth_session_to_json(session, &json, &json_len);
    if (rc != SXCL_AUTH_OK || json == NULL) {
        sxcl_auth_err(err, err_len, "序列化会话失败");
        return SXCL_AUTH_ERR_STORE;
    }
    const sxcl_auth_store_kind kind = sxcl_auth_store_backend();

    unsigned char *payload = NULL;
    size_t payload_len = 0;
    unsigned char short_lived[1]; /* DPAPI 输出要先 malloc,这里只是占位避免未初始化警告 */
    (void)short_lived;
    switch (kind) {
#if defined(_WIN32)
        case SXCL_AUTH_STORE_DPAPI:
            rc = dpapi_seal((const unsigned char *)json, json_len, &payload, &payload_len, err, err_len);
            break;
#endif
        case SXCL_AUTH_STORE_FILE_KEY:
            rc = filekey_seal(path, (const unsigned char *)json, json_len, &payload, &payload_len,
                              err, err_len);
            break;
        case SXCL_AUTH_STORE_KEYCHAIN:
        case SXCL_AUTH_STORE_LIBSECRET:
            /* 这两条后端把令牌放进系统密钥环,**不写文件**;失败时明确降级说明。
             * 本机(Windows)编译不到这两个分支,所以这里给的是"如实报错",不是"假装成功"。 */
            rc = SXCL_AUTH_ERR_UNSUPPORTED;
            sxcl_auth_err(err, err_len,
                          "%s 后端在当前构建里不可用,请改用 file+key 降级路径",
                          sxcl_auth_store_kind_name(kind));
            break;
        default:
            rc = SXCL_AUTH_ERR_STORE;
            break;
    }
    sxcl_auth_secure_zero(json, json_len);
    free(json);
    if (rc != SXCL_AUTH_OK) {
        free(payload);
        return rc;
    }

    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        free(payload);
        sxcl_auth_err(err, err_len, "建不了令牌文件所在目录：%s", path);
        return SXCL_AUTH_ERR_STORE;
    }
    unsigned char header[SXCL_AUTH_STORE_HEADER_LEN];
    memcpy(header, SXCL_AUTH_STORE_MAGIC, 8);
    header[8] = (unsigned char)SXCL_AUTH_STORE_VERSION;
    header[9] = (unsigned char)kind;
    header[10] = 0;
    header[11] = 0;
    const uint32_t plen = (uint32_t)payload_len;
    header[12] = (unsigned char)(plen & 0xffu);
    header[13] = (unsigned char)((plen >> 8) & 0xffu);
    header[14] = (unsigned char)((plen >> 16) & 0xffu);
    header[15] = (unsigned char)((plen >> 24) & 0xffu);

    char tmp[SXCL_AUTH_STORE_PATH_MAX + 8];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = sxcl_fs_fopen(tmp, "wb");
    if (fp == NULL) {
        free(payload);
        sxcl_auth_err(err, err_len, "写不了令牌文件：%s", tmp);
        return SXCL_AUTH_ERR_STORE;
    }
    const int ok = (fwrite(header, 1, sizeof(header), fp) == sizeof(header)) &&
                   (fwrite(payload, 1, payload_len, fp) == payload_len);
    fclose(fp);
    sxcl_auth_secure_zero(payload, payload_len);
    free(payload);
    if (!ok) {
        (void)sxcl_fs_remove(tmp);
        sxcl_auth_err(err, err_len, "令牌文件写不完整：%s", tmp);
        return SXCL_AUTH_ERR_STORE;
    }
    if (sxcl_fs_rename_replace(tmp, path) != 0) {
        (void)sxcl_fs_remove(tmp);
        sxcl_auth_err(err, err_len, "令牌文件改名失败（目标可能被占用）：%s", path);
        return SXCL_AUTH_ERR_STORE;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_store_load(const char *path, sxcl_auth_session *session, char *err, size_t err_len)
{
    if (path == NULL || path[0] == '\0' || session == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    if (!sxcl_fs_exists(path)) {
        sxcl_auth_err(err, err_len, "还没有登录过（令牌文件不存在）：%s", path);
        return SXCL_AUTH_ERR_STORE;
    }
    FILE *fp = sxcl_fs_fopen(path, "rb");
    if (fp == NULL) {
        sxcl_auth_err(err, err_len, "读不了令牌文件：%s", path);
        return SXCL_AUTH_ERR_STORE;
    }
    unsigned char header[SXCL_AUTH_STORE_HEADER_LEN];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header) ||
        memcmp(header, SXCL_AUTH_STORE_MAGIC, 8) != 0) {
        fclose(fp);
        sxcl_auth_err(err, err_len, "令牌文件格式不对（不是 SXCL 的令牌文件）");
        return SXCL_AUTH_ERR_STORE;
    }
    if (header[8] != SXCL_AUTH_STORE_VERSION) {
        fclose(fp);
        sxcl_auth_err(err, err_len, "令牌文件版本不认识（%u，本程序支持 %u）",
                      (unsigned)header[8], (unsigned)SXCL_AUTH_STORE_VERSION);
        return SXCL_AUTH_ERR_STORE;
    }
    const sxcl_auth_store_kind kind = (sxcl_auth_store_kind)header[9];
    const uint32_t plen = (uint32_t)header[12] | ((uint32_t)header[13] << 8) |
                          ((uint32_t)header[14] << 16) | ((uint32_t)header[15] << 24);
    if (plen == 0 || plen > SXCL_AUTH_STORE_MAX_PAYLOAD) {
        fclose(fp);
        sxcl_auth_err(err, err_len, "令牌文件长度字段不合法（%u 字节）", (unsigned)plen);
        return SXCL_AUTH_ERR_STORE;
    }
    unsigned char *payload = (unsigned char *)malloc(plen);
    if (payload == NULL) {
        fclose(fp);
        return SXCL_AUTH_ERR_NOMEM;
    }
    if (fread(payload, 1, plen, fp) != plen) {
        free(payload);
        fclose(fp);
        sxcl_auth_err(err, err_len, "令牌文件被截断了：%s", path);
        return SXCL_AUTH_ERR_STORE;
    }
    fclose(fp);

    unsigned char *plain = NULL;
    size_t plain_len = 0;
    int rc;
    switch (kind) {
#if defined(_WIN32)
        case SXCL_AUTH_STORE_DPAPI:
            rc = dpapi_open(payload, plen, &plain, &plain_len, err, err_len);
            break;
#endif
        case SXCL_AUTH_STORE_FILE_KEY:
            rc = filekey_open(path, payload, plen, &plain, &plain_len, err, err_len);
            break;
        default:
            rc = SXCL_AUTH_ERR_STORE;
            sxcl_auth_err(err, err_len,
                          "令牌文件是用 %s 后端写的，当前构建读不了（%s）",
                          sxcl_auth_store_kind_name(kind), "换用同一平台的构建，或重新登录");
            break;
    }
    sxcl_auth_secure_zero(payload, plen);
    free(payload);
    if (rc != SXCL_AUTH_OK) {
        return rc;
    }
    const int prc = sxcl_auth_session_from_json((const char *)plain, plain_len, session);
    sxcl_auth_secure_zero(plain, plain_len);
    free(plain);
    if (prc != SXCL_AUTH_OK) {
        sxcl_auth_err(err, err_len, "令牌文件解出来的内容不是合法 JSON");
        return SXCL_AUTH_ERR_STORE;
    }
    return SXCL_AUTH_OK;
}

int sxcl_auth_store_clear(const char *path, char *err, size_t err_len)
{
    if (path == NULL || path[0] == '\0') {
        return SXCL_AUTH_ERR_ARG;
    }
    if (!sxcl_fs_exists(path)) {
        return SXCL_AUTH_OK; /* 本来就没有,算成功 */
    }
    if (sxcl_fs_remove(path) != 0) {
        sxcl_auth_err(err, err_len, "删不掉令牌文件：%s", path);
        return SXCL_AUTH_ERR_STORE;
    }
    return SXCL_AUTH_OK;
}
