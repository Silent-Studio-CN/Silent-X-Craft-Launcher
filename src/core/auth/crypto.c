/* ChaCha20-Poly1305 AEAD(RFC 8439)—— 令牌加密存储用的加密原语,自研零依赖。
 *
 * 为什么自己写而不用系统库:
 *   - Windows 有 DPAPI、macOS 有 Keychain,那两条路**不需要**这个;
 *   - 但 Linux 上 libsecret 可能没有(GNOME Keyring 没装/无会话总线),这时只剩
 *     "0600 文件 + 本机派生密钥"这一条降级路。只用 0600 挡不住"文件被拷走",
 *     所以密钥之外还得有真正的加密 —— 于是有了这个 AEAD。
 *   - 自研要能被验证:实现直接对拍 RFC 8439 §2.3.2(块函数)/§2.5.2(Poly1305)/
 *     §2.8.2(AEAD)的官方测试向量,见 tests/auth_crypto_test.c。
 *
 * 只实现需要的:256 位密钥、96 位 nonce、16 字节 tag、单条消息(不分块)。
 * 不做常数时间承诺之外的侧信道加固(密钥来自本机文件,威胁模型是"拷走文件"而不是"同机计时攻击")。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <string.h>

#define ROTL32(v, c) ((uint32_t)(((v) << (c)) | ((v) >> (32 - (c)))))

static uint32_t load32_le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32_le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

#define QR(a, b, c, d)                    \
    do {                                  \
        (a) += (b); (d) ^= (a); (d) = ROTL32((d), 16); \
        (c) += (d); (b) ^= (c); (b) = ROTL32((b), 12); \
        (a) += (b); (d) ^= (a); (d) = ROTL32((d), 8);  \
        (c) += (d); (b) ^= (c); (b) = ROTL32((b), 7);  \
    } while (0)

static void chacha20_rounds(uint32_t x[16])
{
    for (int i = 0; i < 10; ++i) { /* 20 轮 = 10 次双轮 */
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
}

void sxcl_auth_chacha20_block(const unsigned char key[32], uint32_t counter,
                              const unsigned char nonce[12], unsigned char out[64])
{
    static const char sigma[17] = "expand 32-byte k"; /* 16 字节常量 + NUL:MSVC /W4 会为"数组装不下 NUL"报 C4295 */
    uint32_t st[16];
    st[0] = load32_le((const unsigned char *)sigma + 0);
    st[1] = load32_le((const unsigned char *)sigma + 4);
    st[2] = load32_le((const unsigned char *)sigma + 8);
    st[3] = load32_le((const unsigned char *)sigma + 12);
    for (int i = 0; i < 8; ++i) {
        st[4 + i] = load32_le(key + i * 4);
    }
    st[12] = counter;
    st[13] = load32_le(nonce + 0);
    st[14] = load32_le(nonce + 4);
    st[15] = load32_le(nonce + 8);

    uint32_t w[16];
    memcpy(w, st, sizeof(w));
    chacha20_rounds(w);
    for (int i = 0; i < 16; ++i) {
        store32_le(out + i * 4, w[i] + st[i]);
    }
    sxcl_auth_secure_zero(w, sizeof(w));
    sxcl_auth_secure_zero(st, sizeof(st));
}

void sxcl_auth_chacha20_xor(const unsigned char key[32], uint32_t counter,
                            const unsigned char nonce[12],
                            const unsigned char *in, unsigned char *out, size_t len)
{
    unsigned char block[64];
    size_t done = 0;
    while (done < len) {
        sxcl_auth_chacha20_block(key, counter, nonce, block);
        ++counter;
        const size_t n = (len - done < 64u) ? (len - done) : 64u;
        for (size_t i = 0; i < n; ++i) {
            out[done + i] = (unsigned char)(in[done + i] ^ block[i]);
        }
        done += n;
    }
    sxcl_auth_secure_zero(block, sizeof(block));
}

/* ── Poly1305(RFC 8439 §2.5) ──
 * 用 5×26 位肢体(和参考实现一致):32 位平台也能算,不依赖 64 位乘法取模。 */
typedef struct poly1305_state {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
} poly1305_state;

static void poly1305_blocks(poly1305_state *st, const unsigned char *m, size_t len, uint32_t hibit)
{
    const uint32_t mask = 0x3ffffffu;
    while (len >= 16u) {
        uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
        h0 += load32_le(m + 0) & mask;
        h1 += (load32_le(m + 3) >> 2) & mask;
        h2 += (load32_le(m + 6) >> 4) & mask;
        h3 += (load32_le(m + 9) >> 6) & mask;
        h4 += (load32_le(m + 12) >> 8) | hibit;

        const uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
        const uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * (r4 * 5u) + (uint64_t)h2 * (r3 * 5u) +
                            (uint64_t)h3 * (r2 * 5u) + (uint64_t)h4 * (r1 * 5u);
        const uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * (r4 * 5u) +
                            (uint64_t)h3 * (r3 * 5u) + (uint64_t)h4 * (r2 * 5u);
        const uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                            (uint64_t)h3 * (r4 * 5u) + (uint64_t)h4 * (r3 * 5u);
        const uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                            (uint64_t)h3 * r0 + (uint64_t)h4 * (r4 * 5u);
        const uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                            (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & mask;
        uint64_t t = d1 + c;
        c = (uint32_t)(t >> 26);
        h1 = (uint32_t)t & mask;
        t = d2 + c;
        c = (uint32_t)(t >> 26);
        h2 = (uint32_t)t & mask;
        t = d3 + c;
        c = (uint32_t)(t >> 26);
        h3 = (uint32_t)t & mask;
        t = d4 + c;
        c = (uint32_t)(t >> 26);
        h4 = (uint32_t)t & mask;
        h0 += c * 5u;
        c = h0 >> 26;
        h0 &= mask;
        h1 += c;

        st->h[0] = h0;
        st->h[1] = h1;
        st->h[2] = h2;
        st->h[3] = h3;
        st->h[4] = h4;
        m += 16;
        len -= 16u;
    }
}

static void poly1305_init(poly1305_state *st, const unsigned char key[32])
{
    /* r 的"clamping"(RFC 8439 §2.5:r &= 0x0ffffffc0ffffffc0ffffffc0fffffff)。
     * 在 5×26 位肢体里,clamping 不是"统一低 26 位" —— 每个肢体的掩码都不一样,
     * 写成一个 mask 会让 Poly1305 算错(官方测试向量立刻红)。
     * 下面的掩码取自 poly1305-donna 的参考实现,与 RFC 的 clamping 逐位等价。 */
    st->r[0] = load32_le(key + 0) & 0x3ffffffu;
    st->r[1] = (load32_le(key + 3) >> 2) & 0x3ffff03u;
    st->r[2] = (load32_le(key + 6) >> 4) & 0x3ffc0ffu;
    st->r[3] = (load32_le(key + 9) >> 6) & 0x3f03fffu;
    st->r[4] = (load32_le(key + 12) >> 8) & 0x00fffffu;
    st->h[0] = st->h[1] = st->h[2] = st->h[3] = st->h[4] = 0;
    st->pad[0] = load32_le(key + 16);
    st->pad[1] = load32_le(key + 20);
    st->pad[2] = load32_le(key + 24);
    st->pad[3] = load32_le(key + 28);
}

static void poly1305_finish(poly1305_state *st, unsigned char tag[16])
{
    const uint32_t mask = 0x3ffffffu;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];

    uint32_t c = h1 >> 26; h1 &= mask; h2 += c;
    c = h2 >> 26; h2 &= mask; h3 += c;
    c = h3 >> 26; h3 &= mask; h4 += c;
    c = h4 >> 26; h4 &= mask; h0 += c * 5u;
    c = h0 >> 26; h0 &= mask; h1 += c;

    uint32_t g0 = h0 + 5u; c = g0 >> 26; g0 &= mask;
    uint32_t g1 = h1 + c;  c = g1 >> 26; g1 &= mask;
    uint32_t g2 = h2 + c;  c = g2 >> 26; g2 &= mask;
    uint32_t g3 = h3 + c;  c = g3 >> 26; g3 &= mask;
    uint32_t g4 = h4 + c - (1u << 26);

    /* 若 g = h + 5 - 2^130 没"借位"(g4 最高位为 0),说明 h >= p,该用 g(h-p);否则用 h。
     * 掩码方向写反过一次 —— 表现就是 Poly1305 与官方向量对不上(测试抓到的)。 */
    const uint32_t pick_g = (g4 >> 31) - 1u; /* 无借位 → 全 1(选 g) */
    const uint32_t pick_h = ~pick_g;         /* 有借位 → 全 1(选 h) */
    h0 = (g0 & pick_g) | (h0 & pick_h);
    h1 = (g1 & pick_g) | (h1 & pick_h);
    h2 = (g2 & pick_g) | (h2 & pick_h);
    h3 = (g3 & pick_g) | (h3 & pick_h);
    h4 = (g4 & pick_g) | (h4 & pick_h);

    /* 把 5×26 位"重新打包"成 4 个 32 位字,再加 s(pad),只保留低 32 位并进位。
     * 每个打包结果都必须 &0xffffffff:多余的高位属于**下一个字**,留在这里会把 pad 的加法带偏。
     * (这里也写错过一次,同样是官方向量抓出来的。) */
    const uint32_t w0 = (uint32_t)(((uint64_t)h0 | ((uint64_t)h1 << 26)) & 0xffffffffu);
    const uint32_t w1 = (uint32_t)(((uint64_t)(h1 >> 6) | ((uint64_t)h2 << 20)) & 0xffffffffu);
    const uint32_t w2 = (uint32_t)(((uint64_t)(h2 >> 12) | ((uint64_t)h3 << 14)) & 0xffffffffu);
    const uint32_t w3 = (uint32_t)(((uint64_t)(h3 >> 18) | ((uint64_t)h4 << 8)) & 0xffffffffu);

    uint64_t f;
    f = (uint64_t)w0 + (uint64_t)st->pad[0];
    store32_le(tag + 0, (uint32_t)f);
    f = (uint64_t)w1 + (uint64_t)st->pad[1] + (f >> 32);
    store32_le(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + (uint64_t)st->pad[2] + (f >> 32);
    store32_le(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + (uint64_t)st->pad[3] + (f >> 32);
    store32_le(tag + 12, (uint32_t)f);
}

void sxcl_auth_poly1305(const unsigned char key[32], const unsigned char *msg, size_t len,
                        unsigned char tag[16])
{
    poly1305_state st;
    poly1305_init(&st, key);
    poly1305_blocks(&st, msg, len, 1u << 24);
    /* 不足 16 字节的尾巴:补 0x01 再算一块 */
    if (len % 16u != 0) {
        unsigned char tail[16];
        memset(tail, 0, sizeof(tail));
        memcpy(tail, msg + (len - len % 16u), len % 16u);
        tail[len % 16u] = 1;
        poly1305_blocks(&st, tail, 16u, 0);
        sxcl_auth_secure_zero(tail, sizeof(tail));
    }
    poly1305_finish(&st, tag);
    sxcl_auth_secure_zero(&st, sizeof(st));
}

/* ── AEAD(RFC 8439 §2.8) ──
 * 注意 AEAD 里的 Poly1305 **不是**"尾巴补 0x01"那一套:
 * mac_data = aad || pad16(aad) || ct || pad16(ct) || le64(len_aad) || le64(len_ct),
 * 全长必然是 16 的倍数,于是**每个** 16 字节块(含补 0 的那块)都带 2^128 位(hibit=1<<24)。
 * 这里和 sxcl_auth_poly1305(原始 Poly1305,尾部补 0x01)是两条不同的规矩,别混。 */
static void poly1305_update_padded(poly1305_state *st, const unsigned char *data, size_t len)
{
    if (len > 0) {
        poly1305_blocks(st, data, len, 1u << 24);
        const size_t rem = len % 16u;
        if (rem != 0) {
            unsigned char pad[16];
            memset(pad, 0, sizeof(pad));
            memcpy(pad, data + (len - rem), rem);
            poly1305_blocks(st, pad, 16u, 1u << 24); /* 补 0 的块同样带 hibit */
            sxcl_auth_secure_zero(pad, sizeof(pad));
        }
    }
}

/* mac_data 的结尾是 le64(len_aad) || le64(len_ct) —— **两个长度合起来才是一个 16 字节块**。
 * 把每个长度各自补成一个块(某些老实现是那么干的)算出来的是另一种 AEAD,官方向量会对不上:
 * 实测就是这里红了一条 tag(靠"对 mac_data 直接算 Poly1305"那条交叉断言定位到的)。 */
static void poly1305_update_lengths(poly1305_state *st, uint64_t aad_len, uint64_t ct_len)
{
    unsigned char buf[16];
    for (int i = 0; i < 8; ++i) {
        buf[i] = (unsigned char)((aad_len >> (8 * i)) & 0xffu);
        buf[8 + i] = (unsigned char)((ct_len >> (8 * i)) & 0xffu);
    }
    poly1305_blocks(st, buf, 16u, 1u << 24);
    sxcl_auth_secure_zero(buf, sizeof(buf));
}

static void aead_mac(const unsigned char key[32], const unsigned char nonce[12],
                     const unsigned char *aad, size_t aad_len,
                     const unsigned char *ct, size_t ct_len, unsigned char tag[16])
{
    unsigned char block0[64];
    unsigned char poly_key[64];
    sxcl_auth_chacha20_block(key, 0, nonce, block0);
    memcpy(poly_key, block0, 32); /* 第一块的 32 字节当 Poly1305 的 r||s */

    poly1305_state st;
    poly1305_init(&st, poly_key);
    poly1305_update_padded(&st, aad, aad_len);
    poly1305_update_padded(&st, ct, ct_len);
    poly1305_update_lengths(&st, (uint64_t)aad_len, (uint64_t)ct_len);
    poly1305_finish(&st, tag);

    sxcl_auth_secure_zero(block0, sizeof(block0));
    sxcl_auth_secure_zero(poly_key, sizeof(poly_key));
    sxcl_auth_secure_zero(&st, sizeof(st));
}

int sxcl_auth_aead_seal(const unsigned char key[32], const unsigned char nonce[12],
                        const unsigned char *aad, size_t aad_len,
                        const unsigned char *pt, size_t pt_len,
                        unsigned char *ct, unsigned char tag[16])
{
    if (key == NULL || nonce == NULL || ct == NULL || tag == NULL) {
        return -1;
    }
    if (pt == NULL && pt_len != 0) {
        return -1;
    }
    if (pt_len > 0) {
        sxcl_auth_chacha20_xor(key, 1u, nonce, pt, ct, pt_len);
    }
    aead_mac(key, nonce, aad, aad_len, ct, pt_len, tag);
    return 0;
}

int sxcl_auth_aead_open(const unsigned char key[32], const unsigned char nonce[12],
                        const unsigned char *aad, size_t aad_len,
                        const unsigned char *ct, size_t ct_len,
                        const unsigned char tag[16], unsigned char *pt)
{
    if (key == NULL || nonce == NULL || ct == NULL || tag == NULL || pt == NULL) {
        return -1;
    }
    unsigned char expect[16];
    aead_mac(key, nonce, aad, aad_len, ct, ct_len, expect);
    unsigned char diff = 0;
    for (int i = 0; i < 16; ++i) {
        diff = (unsigned char)(diff | (expect[i] ^ tag[i]));
    }
    sxcl_auth_secure_zero(expect, sizeof(expect));
    if (diff != 0) {
        return -1; /* 认证失败:要么密钥不对,要么文件被动过 */
    }
    if (ct_len > 0) {
        sxcl_auth_chacha20_xor(key, 1u, nonce, ct, pt, ct_len);
    }
    return 0;
}
