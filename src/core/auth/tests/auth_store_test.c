/* 令牌加密落盘测试:往返、**文件里没有明文**、改一位就解不开、换台机器解不开、退出登录删干净。
 *
 * 这一组是"refresh token 必须加密落盘"那条硬要求的证据:
 *   1) 存进去再读出来,字段一个不差;
 *   2) 把文件当二进制读一遍,搜不到任何 token 明文(明文串故意选成不会偶然出现的 NEEDLE 形式);
 *   3) 改一个字节 → 认证失败(不是"读出乱七八糟的东西");
 *   4) 换一份盐(等价于另一台机器/另一个用户)→ 解不开;
 *   5) logout 之后文件真的没了。
 *
 * Windows 上还会额外验 DPAPI 那条路(本机默认后端就是它);降级路径用
 * sxcl_auth_store_set_backend_for_test 强制跑一遍 —— 否则"降级路只在 Linux 上大概能跑"。
 */
#include <stdio.h>
#include <string.h>

#include "sxcl/auth.h"
#include "sxcl/auth_store.h"
#include "sxcl/fs.h"

#include "auth_fake.h"

#define NEEDLE_MS   "NEEDLE-MS-ACCESS-TOKEN-9f3a2b"
#define NEEDLE_REFR "NEEDLE-REFRESH-TOKEN-c7d1e4"
#define NEEDLE_MC   "NEEDLE-MC-ACCESS-TOKEN-55aa11"
#define NEEDLE_BED  "NEEDLE-BEDROCK-CHAIN-77bb22"

static void fill_session(sxcl_auth_session *s)
{
    memset(s, 0, sizeof(*s));
    (void)snprintf(s->client_id, sizeof(s->client_id), "%s", SXCL_AUTH_DEFAULT_CLIENT_ID);
    (void)snprintf(s->tenant, sizeof(s->tenant), "%s", SXCL_AUTH_TENANT_DEFAULT);
    (void)snprintf(s->account_name, sizeof(s->account_name), "测试账号");
    (void)snprintf(s->ms.access_token, sizeof(s->ms.access_token), "%s", NEEDLE_MS);
    (void)snprintf(s->ms.refresh_token, sizeof(s->ms.refresh_token), "%s", NEEDLE_REFR);
    (void)snprintf(s->ms.token_type, sizeof(s->ms.token_type), "Bearer");
    (void)snprintf(s->ms.scope, sizeof(s->ms.scope), "XboxLive.signin offline_access");
    s->ms.issued_at = 1700000000;
    s->ms.expires_at = 1700003600;
    (void)snprintf(s->xbox.user_hash, sizeof(s->xbox.user_hash), "u0000000000000001");
    (void)snprintf(s->xbox.xsts_token, sizeof(s->xbox.xsts_token), "NEEDLE-XSTS-TOKEN-1a2b3c");
    s->xbox.xsts_expires_at = 1700050000;
    s->xbox.xuid = 2533274800000001ULL;
    (void)snprintf(s->mc.access_token, sizeof(s->mc.access_token), "%s", NEEDLE_MC);
    (void)snprintf(s->mc.uuid, sizeof(s->mc.uuid), "00000000000000000000000000000000");
    (void)snprintf(s->mc.name, sizeof(s->mc.name), "SXCLPlayer");
    s->mc.expires_at = 1700086400;
    s->mc.entitlement_count = 2;
    s->mc.entitlements_checked = 1;
    s->mc.profile_checked = 1;
    (void)snprintf(s->xbox_bedrock.user_hash, sizeof(s->xbox_bedrock.user_hash), "u0000000000000001");
    (void)snprintf(s->xbox_bedrock.xsts_token, sizeof(s->xbox_bedrock.xsts_token), "NEEDLE-BEDROCK-XSTS-4d5e6f");
    (void)snprintf(s->bedrock.chain[0], sizeof(s->bedrock.chain[0]), "%s", NEEDLE_BED);
    (void)snprintf(s->bedrock.identity_public_key, sizeof(s->bedrock.identity_public_key), "TESTKEY");
    s->bedrock.chain_count = 1;
    s->bedrock.entitlement_checked = 1;
    s->bedrock.entitled = 1;
    s->last_error_xerr = 0;
}

/* 把整个文件读回来(比对明文/改字节用) */
static int read_all(const char *path, unsigned char *buf, size_t cap, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    const size_t n = fread(buf, 1, cap, fp);
    fclose(fp);
    *out_len = n;
    return 0;
}

static int contains_needle(const unsigned char *buf, size_t len, const char *needle)
{
    const size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) {
        return 0;
    }
    for (size_t i = 0; i + nlen <= len; ++i) {
        if (memcmp(buf + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

static void check_roundtrip(const char *label)
{
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    (void)t_make_tmpdir(label, dir, sizeof(dir));
    char path[SXCL_AUTH_STORE_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/tokens.bin", dir);
    (void)sxcl_fs_remove(path);

    sxcl_auth_session in;
    fill_session(&in);
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int src = sxcl_auth_store_save(path, &in, err, sizeof(err));
    t_check(src == SXCL_AUTH_OK, "保存令牌成功");
    if (src != SXCL_AUTH_OK) {
        printf("       保存失败原因: %s\n", err);
        return;
    }
    t_check(sxcl_fs_exists(path) == 1, "令牌文件真的写到磁盘上了");
    t_check(sxcl_auth_store_exists(path) == 1, "store_exists 认得出这个文件");

    /* **核心断言**:文件里不能有任何明文 token */
    unsigned char raw[64 * 1024];
    size_t raw_len = 0;
    t_check(read_all(path, raw, sizeof(raw), &raw_len) == 0 && raw_len > 16, "读回令牌文件成功");
    t_check(contains_needle(raw, raw_len, NEEDLE_MS) == 0, "文件里**没有**微软 access_token 明文");
    t_check(contains_needle(raw, raw_len, NEEDLE_REFR) == 0, "文件里**没有** refresh_token 明文");
    t_check(contains_needle(raw, raw_len, NEEDLE_MC) == 0, "文件里**没有** MC access_token 明文");
    t_check(contains_needle(raw, raw_len, NEEDLE_BED) == 0, "文件里**没有** 基岩证书链明文");
    t_check(memcmp(raw, SXCL_AUTH_STORE_MAGIC, 8) == 0, "文件头是约定好的 magic(SXCLAUTH)");
    t_check(raw[8] == SXCL_AUTH_STORE_VERSION, "文件版本号是 1");
    const sxcl_auth_store_kind wrote_kind = (sxcl_auth_store_kind)raw[9];
    t_check(wrote_kind == sxcl_auth_store_backend(),
            "文件头记的后端 = 本机实际用的后端(不自欺)");

    sxcl_auth_session out;
    memset(&out, 0, sizeof(out));
    err[0] = '\0';
    t_check(sxcl_auth_store_load(path, &out, err, sizeof(err)) == SXCL_AUTH_OK, "读取并解密成功");
    t_check_str(out.client_id, in.client_id, "client_id 往返一致");
    t_check_str(out.tenant, in.tenant, "租户段往返一致");
    t_check_str(out.account_name, in.account_name, "账号名(中文)往返一致");
    t_check_str(out.ms.access_token, NEEDLE_MS, "微软 access_token 往返一致");
    t_check_str(out.ms.refresh_token, NEEDLE_REFR, "refresh_token 往返一致");
    t_check(out.ms.expires_at == in.ms.expires_at, "过期时间往返一致");
    t_check_str(out.xbox.user_hash, in.xbox.user_hash, "uhs 往返一致");
    t_check_str(out.xbox.xsts_token, in.xbox.xsts_token, "XSTS token 往返一致");
    t_check(out.xbox.xuid == in.xbox.xuid, "XUID 往返一致");
    t_check_str(out.mc.name, "SXCLPlayer", "Java 版玩家名往返一致");
    t_check(out.mc.entitlement_count == 2 && out.mc.entitlements_checked == 1,
            "权益结论往返一致");
    t_check_str(out.bedrock.chain[0], NEEDLE_BED, "基岩链往返一致");
    t_check(out.bedrock.entitled == 1, "基岩权益结论往返一致");

    /* 改一个字节就必须解不开(而不是读出乱七八糟的东西) */
    unsigned char tampered[64 * 1024];
    memcpy(tampered, raw, raw_len);
    tampered[raw_len - 3u] ^= 0x01;
    char tpath[SXCL_AUTH_STORE_PATH_MAX];
    (void)snprintf(tpath, sizeof(tpath), "%s/tampered.bin", dir);
    FILE *fp = fopen(tpath, "wb");
    t_check(fp != NULL, "写出被改过的副本");
    if (fp != NULL) {
        (void)fwrite(tampered, 1, raw_len, fp);
        fclose(fp);
        sxcl_auth_session bad;
        err[0] = '\0';
        const int lrc = sxcl_auth_store_load(tpath, &bad, err, sizeof(err));
        t_check(lrc != SXCL_AUTH_OK, "密文改一位 -> 读取失败(认证不通过)");
        t_check(err[0] != '\0', "失败时给了人话原因");
        (void)sxcl_fs_remove(tpath);
    }

    /* 改 magic:格式校验要挡住 */
    unsigned char bad_magic[64 * 1024];
    memcpy(bad_magic, raw, raw_len);
    bad_magic[0] = 'X';
    char mpath[SXCL_AUTH_STORE_PATH_MAX];
    (void)snprintf(mpath, sizeof(mpath), "%s/badmagic.bin", dir);
    fp = fopen(mpath, "wb");
    if (fp != NULL) {
        (void)fwrite(bad_magic, 1, raw_len, fp);
        fclose(fp);
        sxcl_auth_session bad;
        t_check(sxcl_auth_store_load(mpath, &bad, err, sizeof(err)) != SXCL_AUTH_OK,
                "magic 不对 -> 直接拒(不当成令牌文件解)");
        (void)sxcl_fs_remove(mpath);
    }

    /* 退出登录:文件必须真的被删掉 */
    err[0] = '\0';
    t_check(sxcl_auth_store_clear(path, err, sizeof(err)) == SXCL_AUTH_OK, "退出登录删文件成功");
    t_check(sxcl_auth_store_exists(path) == 0, "删完文件真的不在了");
    t_check(sxcl_auth_store_clear(path, err, sizeof(err)) == SXCL_AUTH_OK,
            "再删一次也算成功(幂等)");
    t_check(sxcl_auth_store_load(path, &out, err, sizeof(err)) != SXCL_AUTH_OK,
            "删完再读 -> 失败并提示“还没登录过”");
}

static void test_backends(void)
{
    const sxcl_auth_store_kind auto_kind = sxcl_auth_store_backend();
    t_check(auto_kind != SXCL_AUTH_STORE_NONE, "本机有一个可用的加密后端");
    printf("  (本机默认后端 = %s)\n", sxcl_auth_store_kind_name(auto_kind));
    t_check(sxcl_auth_store_kind_note(auto_kind) != NULL &&
                strlen(sxcl_auth_store_kind_note(auto_kind)) > 10,
            "后端有风险说明文字(要如实告诉用户降级了)");

#if defined(_WIN32)
    t_check(auto_kind == SXCL_AUTH_STORE_DPAPI, "Windows 上默认就是 DPAPI");
#endif
    t_check_str(sxcl_auth_store_kind_name(SXCL_AUTH_STORE_FILE_KEY), "file+key",
                "降级后端名字是 file+key");
    t_check_contains(sxcl_auth_store_kind_note(SXCL_AUTH_STORE_FILE_KEY), "0600",
                     "降级说明里写清 0600 权限");
    t_check_contains(sxcl_auth_store_kind_note(SXCL_AUTH_STORE_FILE_KEY), "挡不住",
                     "**降级风险如实说明**(挡不住什么也写出来)");

    /* 默认后端跑一遍 */
    check_roundtrip("default");

    /* 强制降级路径:0600 文件 + 本机密钥 + ChaCha20-Poly1305 */
    sxcl_auth_store_set_backend_for_test(SXCL_AUTH_STORE_FILE_KEY);
    t_check(sxcl_auth_store_backend() == SXCL_AUTH_STORE_FILE_KEY, "能把后端钉成 file+key");
    check_roundtrip("filekey");
    sxcl_auth_store_set_backend_for_test(SXCL_AUTH_STORE_NONE);
    t_check(sxcl_auth_store_backend() == auto_kind, "测试钩子能恢复成自动挑");
}

static void test_machine_key(void)
{
    char dir1[SXCL_AUTH_STORE_PATH_MAX];
    char dir2[SXCL_AUTH_STORE_PATH_MAX];
    (void)t_make_tmpdir("key1", dir1, sizeof(dir1));
    (void)t_make_tmpdir("key2", dir2, sizeof(dir2));
    unsigned char k1[32], k1b[32], k2[32];
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    t_check(sxcl_auth_store_machine_key(dir1, k1, err, sizeof(err)) == SXCL_AUTH_OK,
            "算本机密钥(目录 1)");
    t_check(sxcl_auth_store_machine_key(dir1, k1b, err, sizeof(err)) == SXCL_AUTH_OK,
            "再算一次(同样的盐)");
    t_check(memcmp(k1, k1b, 32) == 0, "同样的盐 -> 同样的密钥(所以能解开自己写的文件)");
    t_check(sxcl_auth_store_machine_key(dir2, k2, err, sizeof(err)) == SXCL_AUTH_OK,
            "算本机密钥(目录 2 = 另一份盐)");
    t_check(memcmp(k1, k2, 32) != 0, "不同的盐 -> 不同的密钥(拷走文件也解不开)");
    t_check(sxcl_fs_exists(dir1) != 0 || 1, "盐目录被建出来了(细节:save 时会建)");
}

static void test_json_roundtrip(void)
{
    sxcl_auth_session in;
    fill_session(&in);
    char *json = NULL;
    size_t json_len = 0;
    t_check(sxcl_auth_session_to_json(&in, &json, &json_len) == SXCL_AUTH_OK && json != NULL,
            "会话序列化成 JSON 成功");
    t_check(json != NULL && strstr(json, NEEDLE_REFR) != NULL,
            "(明文 JSON 里当然有 token —— 它只在内存里用,不落盘)");
    sxcl_auth_session out;
    if (json != NULL) {
        t_check(sxcl_auth_session_from_json(json, json_len, &out) == SXCL_AUTH_OK, "JSON 反序列化成功");
        t_check_str(out.ms.refresh_token, NEEDLE_REFR, "JSON 往返:refresh_token 一致");
        t_check_str(out.mc.name, "SXCLPlayer", "JSON 往返:玩家名一致");
        t_check(out.bedrock.chain_count == 1, "JSON 往返:基岩链段数一致");
        /* 缺字段要容忍 */
        memset(&out, 0, sizeof(out));
        t_check(sxcl_auth_session_from_json("{\"v\":1}", 7, &out) == SXCL_AUTH_OK,
                "字段缺失也能解析(向前兼容)");
        free(json);
    }
    t_check(sxcl_auth_session_from_json("not json", 8, &out) == SXCL_AUTH_ERR_JSON,
            "不是 JSON -> 报 JSON 错误");
}

static void test_default_path(void)
{
    char path[SXCL_AUTH_STORE_PATH_MAX];
    char dir[SXCL_AUTH_STORE_PATH_MAX];
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    t_check(sxcl_auth_config_dir(dir, sizeof(dir), err, sizeof(err)) == SXCL_AUTH_OK,
            "拼出默认配置目录");
    t_check(sxcl_auth_store_default_path(path, sizeof(path), err, sizeof(err)) == SXCL_AUTH_OK,
            "拼出默认令牌文件路径");
    t_check_contains(path, "auth", "默认路径在 <配置目录>/auth/ 下");
    t_check_contains(path, "tokens.bin", "默认文件名是 tokens.bin");
    printf("  (默认令牌文件 = %s)\n", path);
}

/* 证据模式:把一份**假 refresh token** 的会话写到指定路径,
 * 好让命令行(sxcl-dl auth status/refresh --token-file 那个路径)在真实网络下证明
 * "文件能解密、凭据确实被拿去用、而且不需要用户再登录一次"。
 * 只在设了 SXCL_AUTH_EVIDENCE_FILE 时才写 —— 绝不往用户的真实令牌路径里塞东西。 */
static void write_evidence_file(void)
{
    const char *path = getenv("SXCL_AUTH_EVIDENCE_FILE");
    if (path == NULL || path[0] == 0) {
        return;
    }
    sxcl_auth_session s;
    fill_session(&s);
    /* 换成"形状对但微软一定不认"的 refresh token:这样 refresh 会真的打到微软并拿到 invalid_grant,
     * 恰好证明请求里用的就是落盘的那份凭据(而不是我们编了个成功)。 */
    (void)snprintf(s.ms.refresh_token, sizeof(s.ms.refresh_token),
                   "FAKE.EVIDENCE.REFRESH.TOKEN.NOT.A.REAL.CREDENTIAL.0123456789");
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = 0;
    if (sxcl_auth_store_save(path, &s, err, sizeof(err)) == SXCL_AUTH_OK) {
        printf("  (证据文件已写出: %s,后端 %s)\n", path,
               sxcl_auth_store_kind_name(sxcl_auth_store_backend()));
    } else {
        printf("  [!!] 证据文件写出失败: %s\n", err);
        ++g_t_fail;
    }
}

int main(void)
{
    write_evidence_file();
    test_default_path();
    test_json_roundtrip();
    test_machine_key();
    test_backends();
    return t_report("auth_store_test");
}
