/* auth 内部工具实现:动态缓冲、编码、随机数、错误人话、时间。纯 C11,零依赖。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* snprintf/time 在 MSVC 下会报 C4996;/WX 下会打断构建 */
#endif

#include "auth_internal.h"

#include "sxcl/hash.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>   /* BCryptGenRandom —— 系统 CSPRNG(不用自己攒熵) */
#elif defined(__APPLE__)
#  include <Security/SecRandom.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/random.h> /* getrandom(2) */
#  endif
#endif

/* ── 时间 ── */
static int64_t g_auth_fixed_now = -1;

int64_t sxcl_auth_now(void)
{
    if (g_auth_fixed_now >= 0) {
        return g_auth_fixed_now;
    }
    return (int64_t)time(NULL);
}

void sxcl_auth_set_now_for_test(int64_t fixed_now)
{
    g_auth_fixed_now = fixed_now;
}

/* ── 人话错误 ── */
int sxcl_auth_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return SXCL_AUTH_ERR_ARG;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
    err[err_len - 1] = '\0';
    return SXCL_AUTH_ERR_ARG;
}

/* ── 字符串 ── */
int sxcl_auth_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return -1;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }
    const size_t n = strlen(src);
    const size_t take = (n < dst_len - 1) ? n : dst_len - 1;
    if (take > 0) {
        memcpy(dst, src, take);
    }
    dst[take] = '\0';
    return (take == n) ? (int)take : -1;
}

void sxcl_auth_secure_zero(void *p, size_t n)
{
    if (p == NULL || n == 0) {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(p, n); /* 语义就是"保证不被优化掉" */
#else
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n-- > 0) {
        *v++ = 0;
    }
#endif
}

int sxcl_auth_ct_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    const size_t la = strlen(a);
    const size_t lb = strlen(b);
    if (la != lb) {
        return 0;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < la; ++i) {
        diff = (unsigned char)(diff | (unsigned char)(a[i] ^ b[i]));
    }
    return diff == 0 ? 1 : 0;
}

/* ── 动态缓冲 ── */
void sxcl_auth_buf_init(sxcl_auth_buf *b)
{
    if (b == NULL) {
        return;
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

void sxcl_auth_buf_free(sxcl_auth_buf *b)
{
    if (b == NULL) {
        return;
    }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

int sxcl_auth_buf_reserve(sxcl_auth_buf *b, size_t extra)
{
    if (b == NULL || b->oom) {
        return -1;
    }
    if (b->len + extra + 1u <= b->cap) {
        return 0;
    }
    size_t want = b->cap ? b->cap : 128u;
    while (want < b->len + extra + 1u) {
        if (want > (size_t)1 << 24) { /* 16MiB 上限:登录链的正文不可能这么大,防跑飞 */
            b->oom = 1;
            return -1;
        }
        want *= 2u;
    }
    char *grown = (char *)realloc(b->data, want);
    if (grown == NULL) {
        b->oom = 1;
        return -1;
    }
    b->data = grown;
    b->cap = want;
    if (b->len == 0) {
        b->data[0] = '\0';
    }
    return 0;
}

int sxcl_auth_buf_append_n(sxcl_auth_buf *b, const char *s, size_t n)
{
    if (b == NULL || s == NULL || b->oom) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (sxcl_auth_buf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int sxcl_auth_buf_append(sxcl_auth_buf *b, const char *s)
{
    if (s == NULL) {
        return 0;
    }
    return sxcl_auth_buf_append_n(b, s, strlen(s));
}

int sxcl_auth_buf_appendc(sxcl_auth_buf *b, char c)
{
    return sxcl_auth_buf_append_n(b, &c, 1u);
}

int sxcl_auth_buf_printf(sxcl_auth_buf *b, const char *fmt, ...)
{
    if (b == NULL || fmt == NULL || b->oom) {
        return -1;
    }
    char stack[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = 1;
        return -1;
    }
    if ((size_t)n < sizeof(stack)) {
        return sxcl_auth_buf_append_n(b, stack, (size_t)n);
    }
    /* 太长:按需再来一次(登录链里只有 URL 会走到这儿) */
    if (sxcl_auth_buf_reserve(b, (size_t)n) != 0) {
        return -1;
    }
    va_start(ap, fmt);
    const int m = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (m < 0 || m != n) {
        b->oom = 1;
        return -1;
    }
    b->len += (size_t)n;
    b->data[b->len] = '\0';
    return 0;
}

const char *sxcl_auth_buf_cstr(sxcl_auth_buf *b)
{
    if (b == NULL || b->data == NULL) {
        return "";
    }
    b->data[b->len] = '\0';
    return b->data;
}

char *sxcl_auth_buf_take(sxcl_auth_buf *b)
{
    if (b == NULL || b->data == NULL) {
        return NULL;
    }
    char *out = b->data;
    out[b->len] = '\0';
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
    return out;
}

int sxcl_auth_iequal(const char *a, const char *b)
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

void sxcl_auth_sleep_ms(int64_t ms)
{
    if (ms <= 0) {
        return;
    }
#if defined(_WIN32)
    Sleep((DWORD)((ms > 3600000) ? 3600000 : ms));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    while (nanosleep(&ts, &ts) != 0) {
        /* 只处理"被信号打断后继续睡剩下的一段",其余情况直接返回 */
    }
#endif
}

/* ── 编码 ── */
int sxcl_auth_json_escape(sxcl_auth_buf *b, const char *s)
{
    if (b == NULL || s == NULL) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; ++p) {
        const unsigned char c = *p;
        switch (c) {
            case '"':  if (sxcl_auth_buf_append(b, "\\\"") != 0) return -1; break;
            case '\\': if (sxcl_auth_buf_append(b, "\\\\") != 0) return -1; break;
            case '\n': if (sxcl_auth_buf_append(b, "\\n") != 0) return -1; break;
            case '\r': if (sxcl_auth_buf_append(b, "\\r") != 0) return -1; break;
            case '\t': if (sxcl_auth_buf_append(b, "\\t") != 0) return -1; break;
            case '\b': if (sxcl_auth_buf_append(b, "\\b") != 0) return -1; break;
            case '\f': if (sxcl_auth_buf_append(b, "\\f") != 0) return -1; break;
            case '/':  /* "/" 不必转义,但 JSON 里 \/ 合法;这里不转,少一处特殊 */
            default:
                if (c < 0x20) {
                    if (sxcl_auth_buf_printf(b, "\\u%04x", (unsigned)c) != 0) return -1;
                } else if (sxcl_auth_buf_appendc(b, (char)c) != 0) {
                    return -1;
                }
                break;
        }
    }
    return 0;
}

int sxcl_auth_url_encode(sxcl_auth_buf *b, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (b == NULL) {
        return -1;
    }
    if (value == NULL) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; ++p) {
        const unsigned char c = *p;
        const int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (sxcl_auth_buf_appendc(b, (char)c) != 0) return -1;
        } else {
            char enc[3];
            enc[0] = '%';
            enc[1] = hex[c >> 4];
            enc[2] = hex[c & 0x0f];
            if (sxcl_auth_buf_append_n(b, enc, 3) != 0) return -1;
        }
    }
    return 0;
}

int sxcl_auth_form_append(sxcl_auth_buf *b, const char *key, const char *value)
{
    if (b == NULL || key == NULL) {
        return -1;
    }
    if (b->len > 0 && sxcl_auth_buf_appendc(b, '&') != 0) {
        return -1;
    }
    if (sxcl_auth_url_encode(b, key) != 0) {
        return -1;
    }
    if (sxcl_auth_buf_appendc(b, '=') != 0) {
        return -1;
    }
    return sxcl_auth_url_encode(b, value);
}

static int base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_len,
                         int url_safe, int pad)
{
    static const char std_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char url_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char *tab = url_safe ? url_tab : std_tab;
    if (out == NULL || out_len == 0) {
        return -1;
    }
    const size_t need = pad ? ((in_len + 2u) / 3u) * 4u : (in_len * 4u + 2u) / 3u;
    if (need + 1u > out_len) {
        out[0] = '\0';
        return -1;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= in_len) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[o++] = tab[(v >> 18) & 0x3f];
        out[o++] = tab[(v >> 12) & 0x3f];
        out[o++] = tab[(v >> 6) & 0x3f];
        out[o++] = tab[v & 0x3f];
        i += 3u;
    }
    const size_t rem = in_len - i;
    if (rem == 1u) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = tab[(v >> 18) & 0x3f];
        out[o++] = tab[(v >> 12) & 0x3f];
        if (pad) {
            out[o++] = '=';
            out[o++] = '=';
        }
    } else if (rem == 2u) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = tab[(v >> 18) & 0x3f];
        out[o++] = tab[(v >> 12) & 0x3f];
        out[o++] = tab[(v >> 6) & 0x3f];
        if (pad) {
            out[o++] = '=';
        }
    }
    out[o] = '\0';
    return (int)o;
}

int sxcl_auth_base64url(const unsigned char *in, size_t in_len, char *out, size_t out_len)
{
    if (in == NULL && in_len != 0) {
        return -1;
    }
    return base64_encode(in, in_len, out, out_len, 1, 0);
}

int sxcl_auth_base64(const unsigned char *in, size_t in_len, char *out, size_t out_len)
{
    if (in == NULL && in_len != 0) {
        return -1;
    }
    return base64_encode(in, in_len, out, out_len, 0, 1);
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62; /* 同时认标准与 url-safe 变体 */
    if (c == '/' || c == '_') return 63;
    return -1;
}

int sxcl_auth_base64_decode(const char *in, unsigned char *out, size_t out_cap, size_t *out_len)
{
    if (in == NULL || out == NULL) {
        return -1;
    }
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = in; *p != 0; ++p) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') {
            continue;
        }
        const int v = b64_value(*p);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) {
                return -1;
            }
            out[o++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    if (out_len != NULL) {
        *out_len = o;
    }
    return 0;
}

int sxcl_auth_hex_decode(const char *hex, unsigned char *out, size_t out_cap, size_t *out_len)
{
    if (hex == NULL || out == NULL) {
        return -1;
    }
    const size_t n = strlen(hex);
    if ((n % 2u) != 0 || n / 2u > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < n / 2u; ++i) {
        int hi = -1;
        int lo = -1;
        const char a = hex[i * 2u];
        const char b = hex[i * 2u + 1u];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if ((a | 0x20) >= 'a' && (a | 0x20) <= 'f') hi = (a | 0x20) - 'a' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if ((b | 0x20) >= 'a' && (b | 0x20) <= 'f') lo = (b | 0x20) - 'a' + 10;
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    if (out_len != NULL) {
        *out_len = n / 2u;
    }
    return 0;
}

/* ── 随机数 ── */
int sxcl_auth_random(void *buf, size_t len)
{
    if (buf == NULL && len != 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
#if defined(_WIN32)
    const NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) {
        return -1;
    }
    return 0;
#elif defined(__APPLE__)
    return SecRandomCopyBytes(kSecRandomDefault, len, buf) == errSecSuccess ? 0 : -1;
#else
#  if defined(__linux__)
    {
        size_t got = 0;
        while (got < len) {
            const ssize_t n = getrandom((char *)buf + got, len - got, 0);
            if (n > 0) {
                got += (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            break; /* ENOSYS(老内核)→ 退回 /dev/urandom */
        }
        if (got == len) {
            return 0;
        }
    }
#  endif
    {
        const int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) {
            return -1;
        }
        size_t got = 0;
        while (got < len) {
            const ssize_t n = read(fd, (char *)buf + got, len - got);
            if (n > 0) {
                got += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                (void)close(fd);
                return -1;
            }
        }
        (void)close(fd);
        return 0;
    }
#endif
}

int sxcl_auth_random_string(char *out, size_t out_len)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (out == NULL || out_len < 2u) {
        return -1;
    }
    unsigned char raw[64];
    const size_t want = out_len - 1u;
    if (want > sizeof(raw)) {
        return -1;
    }
    if (sxcl_auth_random(raw, want) != 0) {
        out[0] = '\0';
        return -1;
    }
    for (size_t i = 0; i < want; ++i) {
        out[i] = tab[raw[i] & 0x3f];
    }
    out[want] = '\0';
    sxcl_auth_secure_zero(raw, sizeof(raw));
    return 0;
}
