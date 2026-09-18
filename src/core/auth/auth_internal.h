/* auth 模块内部共用的小工具(不对外公开;测试可以直接 include 这个文件)。
 *
 * 这里只放"每个 .c 都要用、又不值得单开一个模块"的东西:
 *   动态缓冲、JSON 转义、表单编码、base64(url)、随机数、人话错误、安全清零。
 * 全部纯 C11、无第三方依赖,不分配全局状态。
 */
#ifndef SXCL_AUTH_INTERNAL_H
#define SXCL_AUTH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/auth.h"
#include "sxcl/auth_store.h"
#include "sxcl/http.h"
#include "sxcl/json.h"
#include "sxcl/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 动态缓冲(NUL 结尾始终保证;oom 一旦置 1 后续 append 全是 no-op) ── */
typedef struct sxcl_auth_buf {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} sxcl_auth_buf;

void sxcl_auth_buf_init(sxcl_auth_buf *b);
void sxcl_auth_buf_free(sxcl_auth_buf *b);
int sxcl_auth_buf_reserve(sxcl_auth_buf *b, size_t extra);
int sxcl_auth_buf_append(sxcl_auth_buf *b, const char *s);
int sxcl_auth_buf_append_n(sxcl_auth_buf *b, const char *s, size_t n);
int sxcl_auth_buf_appendc(sxcl_auth_buf *b, char c);
int sxcl_auth_buf_printf(sxcl_auth_buf *b, const char *fmt, ...);
/** 保证 NUL 结尾并返回指针(buf 为空/oom 时返回 "")。 */
const char *sxcl_auth_buf_cstr(sxcl_auth_buf *b);
/** 交出内部缓冲的所有权(调用方 free);buf 复位。 */
char *sxcl_auth_buf_take(sxcl_auth_buf *b);

/* ── 字符串 ── */
/** 拷贝并 NUL 结尾。装不下就截断并返回 -1,否则返回拷贝的字节数。dst_len 为 0 时什么都不做。 */
int sxcl_auth_copy(char *dst, size_t dst_len, const char *src);
/** 安全清零(不会被优化掉)。 */
void sxcl_auth_secure_zero(void *p, size_t n);
/** 常量时间比较(长度不同直接 0)。 */
int sxcl_auth_ct_equal(const char *a, const char *b);

/* ── 人话错误(printf 风格;err 可空) ── */
int sxcl_auth_err(char *err, size_t err_len, const char *fmt, ...);

/* ── 编码 ── */
/** JSON 字符串内容转义(不含两端的引号)。 */
int sxcl_auth_json_escape(sxcl_auth_buf *b, const char *s);
/** application/x-www-form-urlencoded 追加 "k=v"(自动补 & )。value 可空 = 空串。 */
int sxcl_auth_form_append(sxcl_auth_buf *b, const char *key, const char *value);
/** 对单个值做表单编码(不追加 key)。 */
int sxcl_auth_url_encode(sxcl_auth_buf *b, const char *value);
/** base64url(无填充)。out 装不下返回 -1;成功返回写入长度(不含 NUL)。 */
int sxcl_auth_base64url(const unsigned char *in, size_t in_len, char *out, size_t out_len);
/** 标准 base64(带填充)。 */
int sxcl_auth_base64(const unsigned char *in, size_t in_len, char *out, size_t out_len);
/** 解码(自动认 base64url 与标准 base64,忽略空白与填充)。成功返回 0。 */
int sxcl_auth_base64_decode(const char *in, unsigned char *out, size_t out_cap, size_t *out_len);
/** 十六进制串 → 字节(大小写都认)。长度必须是偶数且都合法,否则返回 -1。 */
int sxcl_auth_hex_decode(const char *hex, unsigned char *out, size_t out_cap, size_t *out_len);

/** 睡一会儿(毫秒;可被切成小片,调用方负责检查取消)。 */
void sxcl_auth_sleep_ms(int64_t ms);
/** ASCII 大小写不敏感比较(响应头名字用)。 */
int sxcl_auth_iequal(const char *a, const char *b);

/* ── 随机数(平台 CSPRNG;失败返回 -1) ── */
int sxcl_auth_random(void *buf, size_t len);
/** 随机 URL-safe 串(用于 state;长度 = out_len-1 字节)。 */
int sxcl_auth_random_string(char *out, size_t out_len);

/* ── PKCE(RFC 7636) ── */
#define SXCL_AUTH_PKCE_VERIFIER_MAX 128 /* 43..128,我们用 64 */
/** 随机生成 verifier 与 S256 challenge(两串的缓冲都建议 >= 96/64)。 */
int sxcl_auth_pkce_generate(char *verifier, size_t verifier_len,
                            char *challenge, size_t challenge_len);
/** 由已有 verifier 算 S256 challenge(= BASE64URL(SHA256(ASCII(verifier))))。 */
int sxcl_auth_pkce_challenge(const char *verifier, char *challenge, size_t challenge_len);
/** 测试用:用给定熵生成 verifier(便于对拍 RFC 7636 的确定性向量)。 */
int sxcl_auth_pkce_from_entropy(const unsigned char *entropy, size_t entropy_len,
                                char *verifier, size_t verifier_len,
                                char *challenge, size_t challenge_len);

/* ── 一次 HTTP 交换(带状态码与正文,非 2xx 也把正文读回来) ── */
typedef struct sxcl_auth_http {
    int status;   /* 0 = 连响应都没拿到 */
    char *body;   /* malloc,NUL 结尾;可能为 NULL */
    size_t body_len;
    int64_t retry_after_ms; /* 响应头 Retry-After 折算的毫秒;没给为 -1 */
    char content_type[128];
} sxcl_auth_http;

void sxcl_auth_http_free(sxcl_auth_http *r);
/** 发一次请求。content_type/body 可空;bearer 非空则加 Authorization: Bearer。
 *  返回 SXCL_AUTH_OK(拿到了响应,不管状态码)/ SXCL_AUTH_ERR_NET / …(负错误码)。 */
int sxcl_auth_http_call(sxcl_transport *tr, const char *method, const char *url,
                        const char *content_type, const char *body, size_t body_len,
                        const char *bearer, int64_t timeout_ms,
                        sxcl_auth_http *out, char *err, size_t err_len);

/** OAuth 端点(4xx 正文是 {"error":…,"error_description":…})→ 返回码 + 人话。
 *  认识 authorization_pending / slow_down / expired_token / authorization_declined /
 *  invalid_grant / bad_verification_code / interaction_required 等。raw_error 可空(收原始 error 串)。 */
int sxcl_auth_oauth_error(const sxcl_auth_http *r, char *raw_error, size_t raw_error_len,
                          char *err, size_t err_len);

/** 解析 "expires_in":秒 → 绝对过期时间。取不到按 def_seconds 算。 */
int64_t sxcl_auth_expiry_from(const sxcl_json_value *obj, int64_t now, int64_t def_seconds);

/* ── ChaCha20-Poly1305(RFC 8439 AEAD;降级存储用的加密原语,自己实现零依赖) ── */
#define SXCL_AUTH_AEAD_KEY_LEN  32
#define SXCL_AUTH_AEAD_NONCE_LEN 12
#define SXCL_AUTH_AEAD_TAG_LEN  16
/** 加密:ct 至少 pt_len 字节(可以与 pt 同一块)。成功返回 0。 */
int sxcl_auth_aead_seal(const unsigned char key[SXCL_AUTH_AEAD_KEY_LEN],
                        const unsigned char nonce[SXCL_AUTH_AEAD_NONCE_LEN],
                        const unsigned char *aad, size_t aad_len,
                        const unsigned char *pt, size_t pt_len,
                        unsigned char *ct, unsigned char tag[SXCL_AUTH_AEAD_TAG_LEN]);
/** 解密:验证失败返回 -1(此时 pt 内容无意义)。 */
int sxcl_auth_aead_open(const unsigned char key[SXCL_AUTH_AEAD_KEY_LEN],
                        const unsigned char nonce[SXCL_AUTH_AEAD_NONCE_LEN],
                        const unsigned char *aad, size_t aad_len,
                        const unsigned char *ct, size_t ct_len,
                        const unsigned char tag[SXCL_AUTH_AEAD_TAG_LEN],
                        unsigned char *pt);
/** 底层原语(测试用 RFC 8439 官方向量直接对拍)。 */
void sxcl_auth_chacha20_block(const unsigned char key[32], uint32_t counter,
                              const unsigned char nonce[12], unsigned char out[64]);
void sxcl_auth_chacha20_xor(const unsigned char key[32], uint32_t counter,
                            const unsigned char nonce[12],
                            const unsigned char *in, unsigned char *out, size_t len);
void sxcl_auth_poly1305(const unsigned char key[32], const unsigned char *msg, size_t len,
                        unsigned char tag[16]);

/* ── P-384 身份密钥(基岩链的 identityPublicKey) ── */
/** 由 48 字节私钥标量算未压缩公钥(0x04||X||Y,97 字节)。成功返回 0,标量非法返回 -1。 */
int sxcl_auth_ec384_public_from_secret(const unsigned char secret[48], unsigned char out[97]);
/** 随机生成密钥对。public_b64 至少 133 字节缓冲。 */
int sxcl_auth_ec384_keygen(char *public_b64, size_t public_b64_len, unsigned char secret[48]);
/** **只给测试用**:P-384 域运算自洽性检查(1*1=1、2*3=6、3*(1/3)=1、R 的一致性)。
 *  曲线算错时,它能先把"域运算坏了"和"曲线参数抄错了"分开 —— 不然只能看到一个乱码公钥。 */
int sxcl_auth_ec384_field_selftest(char *err, size_t err_len);

/** 点是否在曲线上(未压缩 97 字节)。1 = 在;0 = 不在或格式不对。
 *  用来把"标量乘法算错了"和"曲线参数抄错了"分开报错 —— 测试里两个向量都要过。 */
int sxcl_auth_ec384_point_on_curve(const unsigned char pub[97]);

/** **只给测试用**:把设备码轮询的间隔按比例缩放(默认 1.0)。
 *  生产语义不变(微软给的 interval 是多少就等多久);测试设 0.01 就能把 5 秒压成 50ms,
 *  否则"pending → slow_down → 成功"这一串要跑十几秒。传 <=0 恢复 1.0。 */
void sxcl_auth_set_poll_scale_for_test(double scale);

/* ── 环回重定向服务器(只监听 127.0.0.1;授权码流的接收端) ── */
typedef struct sxcl_auth_loopback sxcl_auth_loopback;
/** 起服务(bind 127.0.0.1 的随机端口)。成功后 redirect_uri 已拼好(http://localhost:<port>/callback)。 */
int sxcl_auth_loopback_start(sxcl_auth_loopback **out, char *err, size_t err_len);
int sxcl_auth_loopback_port(const sxcl_auth_loopback *lb);
const char *sxcl_auth_loopback_redirect_uri(const sxcl_auth_loopback *lb);
/** 等一次 /callback 回调。校验 state;拿到 code 返回 SXCL_AUTH_OK,用户拒绝返回 ERR_DENIED,
 *  超时返回 ERR_TIMEOUT,取消返回 ERR_CANCELLED。 */
int sxcl_auth_loopback_wait(sxcl_auth_loopback *lb, const char *expect_state,
                            char *code, size_t code_len, char *state, size_t state_len,
                            char *error_code, size_t error_code_len,
                            int64_t timeout_ms, int (*should_cancel)(void *), void *cancel_ud,
                            char *err, size_t err_len);
void sxcl_auth_loopback_destroy(sxcl_auth_loopback *lb);

/** 用系统默认浏览器打开 URL(尽力而为:失败只是返回 -1,不影响"把链接打给用户看")。 */
int sxcl_auth_open_browser(const char *url);

/* ── 存储内部:默认目录(与 keymap_store 的 platform.py 分支一致) ── */
int sxcl_auth_default_config_dir(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_AUTH_INTERNAL_H */
