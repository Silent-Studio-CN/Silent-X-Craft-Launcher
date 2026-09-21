/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "auth_internal.h"

#include "auth_fake.h"

static void hex_to_bytes(const char *hex, unsigned char *out, size_t out_len)
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

/* RFC 8439 §2.3.2 —— ChaCha20 块函数测试向量 */
static void test_chacha20_block(void)
{
    const char *key_hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const char *nonce_hex = "000000090000004a00000000";
    const char *want_hex =
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e";
    unsigned char key[32], nonce[12], out[64], want[64];
    char got_hex[129];
    hex_to_bytes(key_hex, key, 32);
    hex_to_bytes(nonce_hex, nonce, 12);
    hex_to_bytes(want_hex, want, 64);
    sxcl_auth_chacha20_block(key, 1, nonce, out);
    bytes_to_hex(out, 64, got_hex);
    t_check(memcmp(out, want, 64) == 0, "RFC 8439 2.3.2 ChaCha20 块函数逐字节一致");
    if (memcmp(out, want, 64) != 0) {
        printf("       期望 %s\n       实际 %s\n", want_hex, got_hex);
    }
}

/* RFC 8439 §2.4.2 —— ChaCha20 加密测试向量(带 counter=1 的流) */
static void test_chacha20_encrypt(void)
{
    const char *key_hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const char *nonce_hex = "000000000000004a00000000";
    const char *plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    const char *want_hex =
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42874d";
    unsigned char key[32], nonce[12], want[114];
    unsigned char out[114];
    char got_hex[229];
    hex_to_bytes(key_hex, key, 32);
    hex_to_bytes(nonce_hex, nonce, 12);
    hex_to_bytes(want_hex, want, 114);
    sxcl_auth_chacha20_xor(key, 1, nonce, (const unsigned char *)plain, out, 114);
    bytes_to_hex(out, 114, got_hex);
    t_check(memcmp(out, want, 114) == 0, "RFC 8439 2.4.2 ChaCha20 加密逐字节一致");
    if (memcmp(out, want, 114) != 0) {
        printf("       期望 %s\n       实际 %s\n", want_hex, got_hex);
    }
    unsigned char back[114];
    sxcl_auth_chacha20_xor(key, 1, nonce, out, back, 114);
    t_check(memcmp(back, plain, 114) == 0, "ChaCha20 再异或一次还原明文");
}

/* RFC 8439 §2.5.2 —— Poly1305 测试向量 */
static void test_poly1305(void)
{
    const char *key_hex = "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b";
    const char *msg = "Cryptographic Forum Research Group";
    const char *want_hex = "a8061dc1305136c6c22b8baf0c0127a9";
    unsigned char key[32], tag[16], want[16];
    char got_hex[33];
    hex_to_bytes(key_hex, key, 32);
    hex_to_bytes(want_hex, want, 16);
    sxcl_auth_poly1305(key, (const unsigned char *)msg, strlen(msg), tag);
    bytes_to_hex(tag, 16, got_hex);
    t_check(memcmp(tag, want, 16) == 0, "RFC 8439 2.5.2 Poly1305 逐字节一致");
    if (memcmp(tag, want, 16) != 0) {
        printf("       期望 %s\n       实际 %s\n", want_hex, got_hex);
    }
    /* 空消息的 Poly1305 = s(key[16..31]):能单独验"收尾与加 s"这一段 */
    unsigned char empty_tag[16];
    sxcl_auth_poly1305(key, (const unsigned char *)"", 0, empty_tag);
    t_check(memcmp(empty_tag, key + 16, 16) == 0, "空消息的 Poly1305 等于 key 的后 16 字节(s)");
}

/* RFC 8439 §2.8.2 —— ChaCha20-Poly1305 AEAD 测试向量(这正是我们存储用的那条路) */
static void test_aead(void)
{
    const char *plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    const char *aad_hex = "50515253c0c1c2c3c4c5c6c7";
    const char *key_hex = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
    const char *nonce_hex = "070000004041424344454647";
    const char *want_ct_hex =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116";
    const char *want_tag_hex = "1ae10b594f09e26a7e902ecbd0600691";

    unsigned char key[32], nonce[12], aad[12];
    unsigned char want_ct[114], want_tag[16];
    unsigned char ct[114], tag[16], back[114];
    char got_hex[240];

    const size_t pt_len = strlen(plain);
    hex_to_bytes(key_hex, key, 32);
    hex_to_bytes(nonce_hex, nonce, 12);
    hex_to_bytes(aad_hex, aad, 12);
    hex_to_bytes(want_ct_hex, want_ct, 114);
    hex_to_bytes(want_tag_hex, want_tag, 16);

    t_check(pt_len == 114, "测试明文是 114 字节(与官方向量一致)");
    t_check(sxcl_auth_aead_seal(key, nonce, aad, 12, (const unsigned char *)plain, pt_len, ct,
                                tag) == 0,
            "AEAD seal 返回成功");
    bytes_to_hex(ct, pt_len, got_hex);
    t_check(memcmp(ct, want_ct, pt_len) == 0, "RFC 8439 2.8.2 AEAD 密文逐字节一致");
    if (memcmp(ct, want_ct, pt_len) != 0) {
        printf("       期望 %s\n       实际 %s\n", want_ct_hex, got_hex);
    }
    bytes_to_hex(tag, 16, got_hex);
    t_check(memcmp(tag, want_tag, 16) == 0, "RFC 8439 2.8.2 AEAD tag 逐字节一致");
    if (memcmp(tag, want_tag, 16) != 0) {
        printf("       tag 期望 %s\n       tag 实际 %s\n", want_tag_hex, got_hex);
    }

    /* 交叉验证:AEAD 的 tag 必须等于"对 mac_data 直接算 Poly1305"的结果。
     * mac_data = aad || pad16(aad) || ct || pad16(ct) || le64(12) || le64(114)(RFC 8439 2.8)
     * 这条断言把"AEAD 组装错了"和"Poly1305 算错了"分开 —— 不然两边一起红就查不出是哪边。 */
    {
        unsigned char mac_data[256];
        size_t n = 0;
        memcpy(mac_data + n, aad, 12);
        n += 12u;
        for (int i = 0; i < 4; ++i) {
            mac_data[n++] = 0; /* pad16(aad) */
        }
        memcpy(mac_data + n, ct, pt_len);
        n += pt_len;
        for (int i = 0; i < 14; ++i) {
            mac_data[n++] = 0; /* pad16(ct) */
        }
        const uint64_t aad_len = 12;
        const uint64_t ct_len = (uint64_t)pt_len;
        for (int i = 0; i < 8; ++i) {
            mac_data[n++] = (unsigned char)((aad_len >> (8 * i)) & 0xffu);
        }
        for (int i = 0; i < 8; ++i) {
            mac_data[n++] = (unsigned char)((ct_len >> (8 * i)) & 0xffu);
        }
        t_check(n == 160u, "mac_data 长度 = 160 字节(10 个整块)");
        unsigned char poly_key[32];
        unsigned char block0[64];
        sxcl_auth_chacha20_block(key, 0, nonce, block0);
        memcpy(poly_key, block0, 32);
        unsigned char tag2[16];
        sxcl_auth_poly1305(poly_key, mac_data, n, tag2);
        char hex2[33];
        bytes_to_hex(tag2, 16, hex2);
        t_check(memcmp(tag2, want_tag, 16) == 0,
                "对 mac_data 直接算 Poly1305 = 官方 tag(组装与算法都对)");
        if (memcmp(tag2, want_tag, 16) != 0) {
            printf("       mac_data 期望 %s\n       mac_data 实际 %s\n", want_tag_hex, hex2);
        }
    }

    t_check(sxcl_auth_aead_open(key, nonce, aad, 12, ct, pt_len, tag, back) == 0,
            "AEAD open 返回成功");
    t_check(memcmp(back, plain, pt_len) == 0, "AEAD 解出来和原文一致");

    /* 被改过就必须失败:这是"文件里没有明文"之外的第二道保证 */
    unsigned char bad_tag[16];
    memcpy(bad_tag, tag, 16);
    bad_tag[0] ^= 0x01;
    t_check(sxcl_auth_aead_open(key, nonce, aad, 12, ct, pt_len, bad_tag, back) != 0,
            "tag 改一位 -> 解密失败(认证不通过)");
    unsigned char bad_ct[114];
    memcpy(bad_ct, ct, pt_len);
    bad_ct[5] ^= 0x01;
    t_check(sxcl_auth_aead_open(key, nonce, aad, 12, bad_ct, pt_len, tag, back) != 0,
            "密文改一位 -> 解密失败");
    unsigned char bad_aad[12];
    memcpy(bad_aad, aad, 12);
    bad_aad[1] ^= 0x01;
    t_check(sxcl_auth_aead_open(key, nonce, bad_aad, 12, ct, pt_len, tag, back) != 0,
            "附加数据改一位 -> 解密失败(文件头被改也算)");
    unsigned char bad_key[32];
    memcpy(bad_key, key, 32);
    bad_key[31] ^= 0x01;
    t_check(sxcl_auth_aead_open(bad_key, nonce, aad, 12, ct, pt_len, tag, back) != 0,
            "换一把密钥 -> 解密失败(换台机器/换个用户解不开)");
}

/* 编解码往返(基岩链的 identityPublicKey 走 base64) */
static void test_base64(void)
{
    const unsigned char raw[5] = {0x00, 0x01, 0xfe, 0xff, 0x7f};
    char b64[32];
    char b64url[32];
    unsigned char back[8];
    size_t back_len = 0;
    t_check(sxcl_auth_base64(raw, sizeof(raw), b64, sizeof(b64)) == 8, "标准 base64 长度正确");
    t_check_str(b64, "AAH+/38=", "标准 base64(带填充)");
    t_check(sxcl_auth_base64url(raw, sizeof(raw), b64url, sizeof(b64url)) == 7, "base64url 长度正确");
    t_check_str(b64url, "AAH-_38", "base64url(无填充)");
    t_check(sxcl_auth_base64_decode(b64url, back, sizeof(back), &back_len) == 0 && back_len == 5,
            "base64url 解码长度正确");
    t_check(memcmp(back, raw, 5) == 0, "base64url 解码内容一致");
    t_check(sxcl_auth_base64_decode(b64, back, sizeof(back), &back_len) == 0 && back_len == 5,
            "标准 base64(带填充)也能解");
    t_check(sxcl_auth_base64_decode("!!!!", back, sizeof(back), &back_len) != 0,
            "非法字符 -> 解码失败");
}

int main(void)
{
    test_chacha20_block();
    test_chacha20_encrypt();
    test_poly1305();
    test_aead();
    test_base64();
    return t_report("auth_crypto_test");
}
