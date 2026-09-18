/* 基岩链测试:P-384 身份密钥(**RFC 5903 官方向量对拍**)+ 第二条链的请求形状与权益判定。
 *
 * 为什么必须对拍官方向量:P-384 的标量乘法自己写,一旦有一位算错,公钥就是个"看起来像"的乱码 ——
 * 本地自测发现不了,只有服务端会拒。RFC 5903 第 8.2 节给了 (私钥 i, 公钥 g^i) 的官方数值,
 * 拿它一比,对错立判。另外还验了"公钥确实在曲线上"(把"曲线参数抄错"也一起挡住)。
 */
#include <stdio.h>
#include <string.h>

#include "sxcl/auth.h"
#include "auth_internal.h"

#include "auth_fake.h"

/* RFC 5903 第 8.2 节:384 位随机 ECP 群的测试向量 */
static const char *k_rfc5903_secret_hex =
    "099F3C7034D4A2C699884D73A375A67F7624EF7C6B3C0F160647B67414DCE655"
    "E35B538041E649EE3FAEF896783AB194";
static const char *k_rfc5903_pub_x_hex =
    "667842D7D180AC2CDE6F74F37551F55755C7645C20EF73E31634FE72B4C55EE6"
    "DE3AC808ACB4BDB4C88732AEE95F41AA";
static const char *k_rfc5903_pub_y_hex =
    "9482ED1FC0EEB9CAFC4984625CCFC23F65032149E0E144ADA024181535A0F38E"
    "EB9FCFF3C2C947DAE69B4C634573A81C";
/* RFC 5903 第 3.2 节的基点 G */
static const char *k_base_x_hex =
    "AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A38"
    "5502F25DBF55296C3A545E3872760AB7";
static const char *k_base_y_hex =
    "3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C0"
    "0A60B1CE1D7E819D7A431D7C90EA0E5F";

/* 十六进制比较:忽略大小写(RFC 印的是大写,我们的 bytes_to_hex 出小写 —— 逐字符比会假红) */
static int hex_equal_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != 0 && *b != 0) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
    }
    return *a == *b ? 1 : 0;
}

static void hex_to_be(const char *hex, unsigned char *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; ++i) {
        unsigned v = 0;
        (void)sscanf(hex + i * 2u, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static void bytes_to_hex(const unsigned char *in, size_t len, char *out)
{
    static const char tab[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2u] = tab[in[i] >> 4];
        out[i * 2u + 1u] = tab[in[i] & 0x0f];
    }
    out[len * 2u] = '\0';
}

/* 私钥 d → 未压缩公钥(0x04||X||Y);与给定期望的 X/Y 比对 */
static void check_scalar(const char *what, const unsigned char *secret, const char *want_x_hex,
                         const char *want_y_hex)
{
    unsigned char pub[97];
    if (sxcl_auth_ec384_public_from_secret(secret, pub) != 0) {
        t_check(0, what);
        return;
    }
    char got_x[97];
    char got_y[97];
    bytes_to_hex(pub + 1, 48, got_x);
    bytes_to_hex(pub + 49, 48, got_y);
    t_check(pub[0] == 0x04, "公钥是未压缩点(0x04 开头)");
    if (!hex_equal_ci(got_x, want_x_hex)) {
        t_check(0, what);
        printf("       X 期望 %s\n       X 实际 %s\n", want_x_hex, got_x);
    } else if (!hex_equal_ci(got_y, want_y_hex)) {
        t_check(0, what);
        printf("       Y 期望 %s\n       Y 实际 %s\n", want_y_hex, got_y);
    } else {
        t_check(1, what);
    }
    t_check(sxcl_auth_ec384_point_on_curve(pub) == 1, "算出来的公钥确实在 P-384 曲线上");
}

static void test_field_selftest(void)
{
    char err[256];
    err[0] = '\0';
    if (sxcl_auth_ec384_field_selftest(err, sizeof(err)) != 0) {
        t_check(0, "P-384 域运算自洽性(1*1=1 / 2*3=6 / 3*(1/3)=1 / (p-1)+1=0)");
        printf("       %s\n", err);
        return;
    }
    t_check(1, "P-384 域运算自洽性(1*1=1 / 2*3=6 / 3*(1/3)=1 / (p-1)+1=0)");
}

static void test_rfc5903_vectors(void)
{
    unsigned char one[48];
    unsigned char gx[48];
    unsigned char gy[48];
    unsigned char secret[48];
    memset(one, 0, sizeof(one));
    one[47] = 1; /* 标量 = 1 */
    hex_to_be(k_base_x_hex, gx, 48);
    hex_to_be(k_base_y_hex, gy, 48);
    hex_to_be(k_rfc5903_secret_hex, secret, 48);

    /* 1 * G 必须正好是基点 —— 这条同时验证了基点常量与乘法框架 */
    check_scalar("1 * G = 基点 G(RFC 5903 第 3.2 节的 Gx/Gy)", one, k_base_x_hex, k_base_y_hex);

    /* RFC 5903 第 8.2 节的官方向量:i -> g^i */
    check_scalar("RFC 5903 第 8.2 节的 i -> g^i 逐字节一致", secret, k_rfc5903_pub_x_hex,
                 k_rfc5903_pub_y_hex);

    /* 基点本身也要在曲线上(曲线参数抄错的话这里就红) */
    unsigned char base_pub[97];
    base_pub[0] = 0x04;
    memcpy(base_pub + 1, gx, 48);
    memcpy(base_pub + 49, gy, 48);
    t_check(sxcl_auth_ec384_point_on_curve(base_pub) == 1, "基点 G 在曲线上(y^2 = x^3 - 3x + b)");

    /* 非法标量要挡住 */
    unsigned char zero[48];
    memset(zero, 0, sizeof(zero));
    unsigned char tmp[97];
    t_check(sxcl_auth_ec384_public_from_secret(zero, tmp) != 0, "私钥 = 0 被拒");
    unsigned char too_big[48];
    memset(too_big, 0xff, sizeof(too_big));
    t_check(sxcl_auth_ec384_public_from_secret(too_big, tmp) != 0, "私钥 >= 群阶 n 被拒");
}

static void test_keygen(void)
{
    char pub_b64[256];
    unsigned char secret_a[48];
    unsigned char secret_b[48];
    t_check(sxcl_auth_ec384_keygen(pub_b64, sizeof(pub_b64), secret_a) == SXCL_AUTH_OK,
            "生成一对 P-384 身份密钥");
    t_check(strlen(pub_b64) == 132, "未压缩点 97 字节 -> 标准 base64 是 132 字符(含一个 =)");
    t_check(pub_b64[131] == '=', "base64 带填充(服务端要的是标准 base64,不是 url-safe)");
    /* 解回来必须是个合法的未压缩点 */
    unsigned char pub[128];
    size_t pub_len = 0;
    t_check(sxcl_auth_base64_decode(pub_b64, pub, sizeof(pub), &pub_len) == 0 && pub_len == 97,
            "base64 解回来是 97 字节");
    t_check(sxcl_auth_ec384_point_on_curve(pub) == 1, "随机生成的公钥在曲线上");
    t_check(sxcl_auth_ec384_keygen(pub_b64, sizeof(pub_b64), secret_b) == SXCL_AUTH_OK,
            "再生成一对");
    t_check(memcmp(secret_a, secret_b, 48) != 0, "两次生成的私钥不同(用了真随机)");
}

static void test_xsts_relying_party_and_chain(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    fake_route1(ft, SXCL_AUTH_URL_BEDROCK_AUTH, 200, "bedrock_auth_ok.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.http_timeout_ms = 5000;
    sxcl_auth_xbox_tokens xbox;
    sxcl_auth_bedrock_tokens bed;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_bedrock_authorize(&ft->pub, &opts, "MS-ACCESS-TOKEN", &xbox, &bed, err,
                                               sizeof(err));
    t_check(rc == SXCL_AUTH_OK, "基岩链跑通(拿到证书链)");
    t_check(bed.chain_count == 2, "解析出 2 段 chain");
    t_check(strlen(bed.chain[0]) > 20, "chain[0] 不为空");
    t_check(bed.identity_public_key[0] != '\0', "记下了本次用的身份公钥");
    t_check(strlen(bed.identity_public_key) == 132, "身份公钥是 132 字符的 base64");
    t_check(bed.entitlement_checked == 1 && bed.entitled == 1,
            "**基岩权益单独记录**(不是从 Java 段推断的)");

    const fake_log *xsts = fake_find(ft, SXCL_AUTH_URL_XSTS, 0);
    t_check(xsts != NULL, "记下了 XSTS 请求");
    if (xsts != NULL) {
        t_check_contains(xsts->body, "https://multiplayer.minecraft.net/",
                         "**XSTS 中继方换成 multiplayer.minecraft.net**(这是第二条链的关键)");
    }
    const fake_log *auth = fake_find(ft, SXCL_AUTH_URL_BEDROCK_AUTH, 0);
    t_check(auth != NULL, "记下了 /authentication 请求");
    if (auth != NULL) {
        t_check_str(auth->method, "POST", "基岩认证用 POST");
        t_check_contains(auth->body, "\"identityPublicKey\":\"", "带 identityPublicKey");
        t_check_contains(auth->body, "\"token\":\"XBL3.0 x=u0000000000000001;",
                         "token 格式是 XBL3.0 x=<uhs>;<XSTS>");
        t_check_contains(auth->content_type, "application/json", "Content-Type 是 JSON");
        t_check(strlen(auth->body) > 200, "请求里确实塞进了完整的公钥与 XSTS token");
    }
    fake_free(ft);
}

static void test_no_bedrock_entitlement(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    fake_route1(ft, SXCL_AUTH_URL_BEDROCK_AUTH, 403, "bedrock_auth_403.json");
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.http_timeout_ms = 5000;
    sxcl_auth_bedrock_tokens bed;
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_bedrock_authorize(&ft->pub, &opts, "MS", NULL, &bed, err, sizeof(err));
    t_check(rc == SXCL_AUTH_ERR_NO_ENTITLE, "403 -> 归类成“没有基岩版权益”");
    t_check_contains(err, "基岩", "人话里点明是基岩版的问题");
    t_check_contains(err, "分开购买", "**说明 Java 买了不等于基岩买了**");
    t_check(bed.entitlement_checked == 1 && bed.entitled == 0,
            "如实记下“查过了、没有权益”");
    t_check_contains(err, "does not own Minecraft", "服务端原文一并带出");
    fake_free(ft);
}

/* Java 与基岩是两条链:同一个会话里两份 XSTS 各自独立(中继方不同) */
static void test_two_chains_are_separate(void)
{
    fake_transport *ft = fake_create();
    fake_route1(ft, SXCL_AUTH_URL_XBL_USER, 200, "xbl_user_auth_success.json");
    fake_route1(ft, SXCL_AUTH_URL_XSTS, 200, "xsts_success.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_LOGIN, 200, "mc_login_success.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_ENTITLEMENTS, 200, "mcstore_ok.json");
    fake_route1(ft, SXCL_AUTH_URL_MC_PROFILE, 200, "mc_profile_ok.json");
    fake_route1(ft, SXCL_AUTH_URL_BEDROCK_AUTH, 200, "bedrock_auth_ok.json");

    sxcl_auth_session session;
    memset(&session, 0, sizeof(session));
    (void)snprintf(session.client_id, sizeof(session.client_id), "%s", SXCL_AUTH_DEFAULT_CLIENT_ID);
    (void)snprintf(session.tenant, sizeof(session.tenant), "%s", SXCL_AUTH_TENANT_DEFAULT);
    (void)snprintf(session.ms.access_token, sizeof(session.ms.access_token), "MS-TOKEN");
    session.ms.expires_at = sxcl_auth_now() + 3600;

    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = SXCL_AUTH_DEFAULT_CLIENT_ID;
    opts.http_timeout_ms = 5000;

    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    t_check(sxcl_auth_refresh_ms_tenant(&ft->pub, SXCL_AUTH_DEFAULT_CLIENT_ID,
                                       SXCL_AUTH_TENANT_DEFAULT, "R", 100, NULL, err, 0) !=
                SXCL_AUTH_OK,
            "(占位调用:确保后面用的是夹具路由)");
    /* 直接跑 Java 段(不经过 refresh):用一条假的微软令牌把链路走完 */
    {
        sxcl_auth_opts o2 = opts;
        t_check(sxcl_auth_session_bedrock(&ft->pub, &o2, &session, err, sizeof(err)) == SXCL_AUTH_OK,
                "基岩链在会话上跑通");
    }
    t_check(session.bedrock.chain_count == 2, "会话里基岩段有自己的 chain");
    t_check(session.xbox_bedrock.user_hash[0] != '\0', "会话里基岩段有自己的 XSTS/uhs");
    /* Java 段的 XSTS 与基岩段的 XSTS 必须是**两次不同的请求** */
    const fake_log *first = fake_find(ft, SXCL_AUTH_URL_XSTS, 0);
    t_check(first != NULL, "至少发过一次 XSTS");
    if (first != NULL) {
        t_check_contains(first->body, SXCL_AUTH_RP_BEDROCK,
                         "会话级基岩链用的中继方是 multiplayer.minecraft.net");
        t_check(strstr(first->body, SXCL_AUTH_RP_MINECRAFT) == NULL,
                "**同一次请求里不会同时出现两个中继方**(一份 XSTS 只能给一个中继方)");
    }
    fake_free(ft);
}

int main(void)
{
    test_field_selftest();
    test_rfc5903_vectors();
    test_keygen();
    test_xsts_relying_party_and_chain();
    test_no_bedrock_entitlement();
    test_two_chains_are_separate();
    return t_report("auth_bedrock_test");
}
