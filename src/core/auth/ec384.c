/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "auth_internal.h"

#include <string.h>

#define EC_LIMBS 12 /* 12 × 32 = 384 位 */

typedef struct fe {
    uint32_t v[EC_LIMBS];
} fe;

/* p(小端肢体;RFC 5903 §3.2 的 MSB-first 写法倒过来就是它):
 *   FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE
 *   FFFFFFFF 00000000 00000000 FFFFFFFF */
/* **注意字节序**:下面这些常量一律是"小端肢体"(v[0] 是最低 32 位)。
 * RFC 5903 印出来的顺序是**大端**(最高位在前),直接照抄会把 p 抄反 ——
 * 抄反的后果不是崩溃,而是"标量乘法算出一个看起来像公钥的乱码",只有官方向量能发现(实测踩过)。 */
static const fe EC_P = {{
    0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
}};

/* 群阶 n(RFC 5903 §3.2)*/
static const fe EC_N = {{
    0xCCC52973u, 0xECEC196Au, 0x48B0A77Au, 0x581A0DB2u, 0xF4372DDFu, 0xC7634D81u,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
}};

/* 曲线常数 b(RFC 5903 §3.2)*/
static const fe EC_B = {{
    0xD3EC2AEFu, 0x2A85C8EDu, 0x8A2ED19Du, 0xC656398Du, 0x5013875Au, 0x0314088Fu,
    0xFE814112u, 0x181D9C6Eu, 0xE3F82D19u, 0x988E056Bu, 0xE23EE7E4u, 0xB3312FA7u
}};

/* 基点 G(RFC 5903 §3.2)*/
static const fe EC_GX = {{
    0x72760AB7u, 0x3A545E38u, 0xBF55296Cu, 0x5502F25Du, 0x82542A38u, 0x59F741E0u,
    0x8BA79B98u, 0x6E1D3B62u, 0xF320AD74u, 0x8EB1C71Eu, 0xBE8B0537u, 0xAA87CA22u
}};
static const fe EC_GY = {{
    0x90EA0E5Fu, 0x7A431D7Cu, 0x1D7E819Du, 0x0A60B1CEu, 0xB5F0B8C0u, 0xE9DA3113u,
    0x289A147Cu, 0xF8F41DBDu, 0x9292DC29u, 0x5D9E98BFu, 0x96262C6Fu, 0x3617DE4Au
}};

/* ── 基本比较 / 加减 ── */
static int fe_cmp(const fe *a, const fe *b)
{
    for (int i = EC_LIMBS - 1; i >= 0; --i) {
        if (a->v[i] != b->v[i]) {
            return (a->v[i] > b->v[i]) ? 1 : -1;
        }
    }
    return 0;
}

static int fe_is_zero(const fe *a)
{
    uint32_t acc = 0;
    for (int i = 0; i < EC_LIMBS; ++i) {
        acc |= a->v[i];
    }
    return acc == 0;
}

static void fe_sub_raw(fe *r, const fe *a, const fe *b)
{
    uint32_t borrow = 0;
    for (int i = 0; i < EC_LIMBS; ++i) {
        const uint64_t diff = (uint64_t)a->v[i] - b->v[i] - borrow;
        r->v[i] = (uint32_t)diff;
        borrow = (uint32_t)((diff >> 32) & 1u);
    }
}

static void fe_add_raw(fe *r, const fe *a, const fe *b, uint32_t *carry_out)
{
    uint32_t carry = 0;
    for (int i = 0; i < EC_LIMBS; ++i) {
        const uint64_t sum = (uint64_t)a->v[i] + b->v[i] + carry;
        r->v[i] = (uint32_t)sum;
        carry = (uint32_t)(sum >> 32);
    }
    if (carry_out != NULL) {
        *carry_out = carry;
    }
}

static void fe_add(fe *r, const fe *a, const fe *b)
{
    fe t;
    uint32_t carry = 0;
    fe_add_raw(&t, a, b, &carry);
    if (carry != 0 || fe_cmp(&t, &EC_P) >= 0) {
        fe_sub_raw(&t, &t, &EC_P);
    }
    *r = t;
}

static void fe_sub(fe *r, const fe *a, const fe *b)
{
    fe t;
    fe_sub_raw(&t, a, b);
    if (fe_cmp(a, b) < 0) {
        fe_add_raw(&t, &t, &EC_P, NULL);
    }
    *r = t;
}

static void fe_double(fe *r, const fe *a)
{
    fe_add(r, a, a);
}

static void fe_mul_small(fe *r, const fe *a, uint32_t k)
{
    /* 小整数乘:重复加(只用在 k<=8 的场合,够快也够简单) */
    fe acc;
    memset(&acc, 0, sizeof(acc));
    for (uint32_t i = 0; i < k; ++i) {
        fe_add(&acc, &acc, a);
    }
    *r = acc;
}

/* ── Montgomery 乘法 ── */
static uint32_t g_n0inv = 0; /* -p^{-1} mod 2^32 */
static fe g_r2;              /* R² mod p,R = 2^384 */
static fe g_one_mont;        /* 1 的 Montgomery 形式 = R mod p */
static fe g_one_plain;       /* 普通域的 1(转出蒙哥马利域时要用它,不是用 R) */
static int g_mont_ready = 0;

static uint32_t fe_mont_n0inv(void)
{
    /* Newton 迭代求 p^{-1} mod 2^32,再取负 */
    uint32_t inv = 1;
    for (int i = 0; i < 5; ++i) {
        inv = inv * (2u - EC_P.v[0] * inv);
    }
    return (uint32_t)(0u - inv);
}

static void mont_mul(fe *r, const fe *a, const fe *b)
{
    /* CIOS(n=12,t[14]) */
    uint32_t t[EC_LIMBS + 2];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < EC_LIMBS; ++i) {
        uint32_t carry = 0;
        for (int j = 0; j < EC_LIMBS; ++j) {
            const uint64_t prod = (uint64_t)a->v[j] * b->v[i] + t[j] + carry;
            t[j] = (uint32_t)prod;
            carry = (uint32_t)(prod >> 32);
        }
        uint64_t sum = (uint64_t)t[EC_LIMBS] + carry;
        t[EC_LIMBS] = (uint32_t)sum;
        t[EC_LIMBS + 1] = (uint32_t)(sum >> 32);

        const uint32_t m = t[0] * g_n0inv;
        uint64_t prod = (uint64_t)m * EC_P.v[0] + t[0];
        carry = (uint32_t)(prod >> 32);
        for (int j = 1; j < EC_LIMBS; ++j) {
            prod = (uint64_t)m * EC_P.v[j] + t[j] + carry;
            t[j - 1] = (uint32_t)prod;
            carry = (uint32_t)(prod >> 32);
        }
        sum = (uint64_t)t[EC_LIMBS] + carry;
        t[EC_LIMBS - 1] = (uint32_t)sum;
        t[EC_LIMBS] = t[EC_LIMBS + 1] + (uint32_t)(sum >> 32);
        t[EC_LIMBS + 1] = 0;
    }
    fe res;
    for (int i = 0; i < EC_LIMBS; ++i) {
        res.v[i] = t[i];
    }
    if (t[EC_LIMBS] != 0 || fe_cmp(&res, &EC_P) >= 0) {
        fe_sub_raw(&res, &res, &EC_P);
    }
    *r = res;
}

/* 转出蒙哥马利域:mont_mul(a, 1) = a * 1 * R^{-1} = a/R。
 * **不是** mont_mul(a, R) —— 那是恒等变换(a*R*R^{-1} = a),会让人以为"转换过了",
 * 实际把蒙哥马利域的值当普通域的值交出去(实测:公钥变成一串看起来像样的乱码)。 */
static void fe_from_mont(fe *r, const fe *a)
{
    mont_mul(r, a, &g_one_plain);
}

static void fe_to_mont(fe *r, const fe *a)
{
    mont_mul(r, a, &g_r2);
}

static void mont_init(void)
{
    if (g_mont_ready) {
        return;
    }
    g_n0inv = fe_mont_n0inv();
    memset(&g_one_plain, 0, sizeof(g_one_plain));
    g_one_plain.v[0] = 1;
    /* R mod p = 2^384 mod p:从 1 开始"翻倍 384 次"最直接,也顺便验证了加法/约减是自洽的 */
    fe r;
    memset(&r, 0, sizeof(r));
    r.v[0] = 1;
    for (int i = 0; i < 384; ++i) {
        fe_double(&r, &r);
    }
    g_one_mont = r;
    /* R² mod p:继续翻倍 384 次 */
    for (int i = 0; i < 384; ++i) {
        fe_double(&r, &r);
    }
    g_r2 = r;
    g_mont_ready = 1;
}

/* 求逆(费马小定理:a^(p-2)),**输入输出都在蒙哥马利域**。
 *
 * 为什么不再"进域/出域":蒙哥马利域是一个同构的乘法群,幂运算在里面做完全自洽
 * (mont_mul(x,x) 仍然是 x² 的蒙哥马利形式)。反过来,如果按"普通域进、普通域出"写,
 * 就会在拿 acc.z(它已经是蒙哥马利形式)当普通值用时静默算错 —— 实测:公钥变成一串
 * 看起来很像样的乱码,而且只有官方向量能发现。少一次域转换,就少一处犯错的地方。 */
static void fe_inv_mont(fe *r, const fe *a)
{
    fe acc = g_one_mont; /* 1 的蒙哥马利形式 */
    fe exp_m1 = EC_P;
    exp_m1.v[0] -= 2u; /* 指数 = p - 2 */
    for (int i = EC_LIMBS - 1; i >= 0; --i) {
        for (int bit = 31; bit >= 0; --bit) {
            mont_mul(&acc, &acc, &acc);
            if ((exp_m1.v[i] >> bit) & 1u) {
                mont_mul(&acc, &acc, a);
            }
        }
    }
    *r = acc;
}

/* ── Jacobian 点 ── */
typedef struct ec_point {
    fe x, y, z; /* 蒙哥马利域 */
    int infinity;
} ec_point;

static void point_set_infinity(ec_point *p)
{
    memset(p, 0, sizeof(*p));
    p->infinity = 1;
}

static void point_from_affine(ec_point *p, const fe *x, const fe *y)
{
    fe_to_mont(&p->x, x);
    fe_to_mont(&p->y, y);
    p->z = g_one_mont;
    p->infinity = 0;
}

/* 倍点(EFD dbl-2007-bl 的 a=-3 变体:因为 a = -3,M = 3*(XX − ZZ²)) */
static void point_double(ec_point *r, const ec_point *p)
{
    if (p->infinity || fe_is_zero(&p->y)) {
        point_set_infinity(r);
        return;
    }
    fe xx, yy, yyyy, zz, zzzz, s, m, t, tmp;
    mont_mul(&xx, &p->x, &p->x);
    mont_mul(&yy, &p->y, &p->y);
    mont_mul(&yyyy, &yy, &yy);
    mont_mul(&zz, &p->z, &p->z);
    mont_mul(&zzzz, &zz, &zz);

    /* S = 2*((X+YY)² − XX − YYYY) */
    fe_add(&tmp, &p->x, &yy);
    mont_mul(&s, &tmp, &tmp);
    fe_sub(&s, &s, &xx);
    fe_sub(&s, &s, &yyyy);
    fe_double(&s, &s);

    /* M = 3*(XX − ZZZZ)  (a = -3) */
    fe_sub(&m, &xx, &zzzz);
    fe t3;
    fe_double(&t3, &m);
    fe_add(&m, &t3, &m);

    /* X3 = M² − 2S */
    mont_mul(&t, &m, &m);
    fe s2;
    fe_double(&s2, &s);
    fe_sub(&r->x, &t, &s2);

    /* Y3 = M*(S − X3) − 8*YYYY */
    fe_sub(&tmp, &s, &r->x);
    mont_mul(&tmp, &m, &tmp);
    fe y8;
    fe_double(&y8, &yyyy);
    fe_double(&y8, &y8);
    fe_double(&y8, &y8);
    fe_sub(&r->y, &tmp, &y8);

    /* Z3 = 2*Y*Z */
    mont_mul(&tmp, &p->y, &p->z);
    fe_double(&r->z, &tmp);
    r->infinity = 0;
}

/* 一般加点(EFD add-2007-bl);a、b 都是 Jacobian */
static void point_add(ec_point *r, const ec_point *a, const ec_point *b)
{
    if (a->infinity) {
        *r = *b;
        return;
    }
    if (b->infinity) {
        *r = *a;
        return;
    }
    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, tmp, tmp2;
    mont_mul(&z1z1, &a->z, &a->z);
    mont_mul(&z2z2, &b->z, &b->z);
    mont_mul(&u1, &a->x, &z2z2);
    mont_mul(&u2, &b->x, &z1z1);
    mont_mul(&tmp, &b->z, &z2z2);
    mont_mul(&s1, &a->y, &tmp);
    mont_mul(&tmp, &a->z, &z1z1);
    mont_mul(&s2, &b->y, &tmp);

    if (fe_cmp(&u1, &u2) == 0) {
        if (fe_cmp(&s1, &s2) == 0) {
            point_double(r, a); /* 同一个点:加法公式会退化(Z3=0),直接走倍点 */
            return;
        }
        point_set_infinity(r); /* 互为逆元:P + (−P) = 无穷远点 */
        return;
    }

    fe_sub(&h, &u2, &u1);
    fe_double(&tmp, &h);
    mont_mul(&i, &tmp, &tmp);
    mont_mul(&j, &h, &i);
    fe_sub(&tmp, &s2, &s1);
    fe_double(&rr, &tmp);
    mont_mul(&v, &u1, &i);

    mont_mul(&tmp, &rr, &rr);
    fe_sub(&tmp, &tmp, &j);
    fe v2;
    fe_double(&v2, &v);
    fe_sub(&r->x, &tmp, &v2);

    fe_sub(&tmp, &v, &r->x);
    mont_mul(&tmp, &rr, &tmp);
    mont_mul(&tmp2, &s1, &j);
    fe_double(&tmp2, &tmp2);
    fe_sub(&r->y, &tmp, &tmp2);

    fe_add(&tmp, &a->z, &b->z);
    mont_mul(&tmp, &tmp, &tmp);
    fe_sub(&tmp, &tmp, &z1z1);
    fe_sub(&tmp, &tmp, &z2z2);
    mont_mul(&r->z, &tmp, &h);
    r->infinity = 0;
}

/* 标量乘:k 为 48 字节大端;结果写进仿射坐标 out_x/out_y(普通域,非蒙哥马利) */
static int scalar_mul(const unsigned char k[48], fe *out_x, fe *out_y)
{
    mont_init();
    ec_point acc;
    point_set_infinity(&acc);
    ec_point base;
    point_from_affine(&base, &EC_GX, &EC_GY);
    int seen = 0;
    for (int i = 0; i < 48; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            if (seen) {
                ec_point dbl;
                point_double(&dbl, &acc);
                acc = dbl;
            }
            if (((k[i] >> bit) & 1u) != 0) {
                if (!seen) {
                    acc = base;
                    seen = 1;
                } else {
                    ec_point sum;
                    point_add(&sum, &acc, &base);
                    acc = sum;
                }
            }
        }
    }
    if (!seen || acc.infinity) {
        return -1; /* k = 0 或无穷远:非法标量 */
    }
    /* 仿射化:x = X/Z²,y = Y/Z³(全程留在蒙哥马利域里算) */
    fe zinv_m, zinv2, zinv3;
    fe_inv_mont(&zinv_m, &acc.z);
    mont_mul(&zinv2, &zinv_m, &zinv_m);
    mont_mul(&zinv3, &zinv2, &zinv_m);
    fe xm, ym;
    mont_mul(&xm, &acc.x, &zinv2);
    mont_mul(&ym, &acc.y, &zinv3);
    fe_from_mont(out_x, &xm);
    fe_from_mont(out_y, &ym);
    return 0;
}

/* ── 字节序与导出 ── */
static void fe_from_be_bytes(fe *r, const unsigned char *be, size_t len)
{
    memset(r, 0, sizeof(*r));
    /* be 是 MSB-first 的 len 字节;放进 12 肢体的低位 */
    for (size_t i = 0; i < len; ++i) {
        const size_t pos = len - 1u - i;            /* 从最低字节往高数 */
        const size_t limb = pos / 4u;
        const size_t shift = (pos % 4u) * 8u;
        if (limb < EC_LIMBS) {
            r->v[limb] |= (uint32_t)be[i] << shift;
        }
    }
}

static void fe_to_be_bytes(const fe *a, unsigned char *out, size_t len)
{
    memset(out, 0, len);
    for (size_t i = 0; i < len; ++i) {
        const size_t pos = len - 1u - i;
        const size_t limb = pos / 4u;
        const size_t shift = (pos % 4u) * 8u;
        if (limb < EC_LIMBS) {
            out[i] = (unsigned char)((a->v[limb] >> shift) & 0xffu);
        }
    }
}

static void fe_set_small(fe *r, uint32_t value)
{
    memset(r, 0, sizeof(*r));
    r->v[0] = value;
}

int sxcl_auth_ec384_field_selftest(char *err, size_t err_len)
{
    mont_init();
    fe one, two, three, six;
    fe_set_small(&one, 1);
    fe_set_small(&two, 2);
    fe_set_small(&three, 3);
    fe_set_small(&six, 6);
    fe one_m, two_m, three_m, six_m, t;
    fe_to_mont(&one_m, &one);
    fe_to_mont(&two_m, &two);
    fe_to_mont(&three_m, &three);
    fe_to_mont(&six_m, &six);
    if (fe_cmp(&one_m, &g_one_mont) != 0) {
        sxcl_auth_err(err, err_len, "域运算自检失败:R mod p 与 to_mont(1) 不一致");
        return -1;
    }
    /* R = 2^384 mod p 有闭式解:因为 p = 2^384 - 2^128 - 2^96 + 2^32 - 1,
     * R = 2^128 + 2^96 - 2^32 + 1 = 0x1_00000000_FFFFFFFF_0000000000000001。
     * 把它按肢体直接对出来,能一次性验证"翻倍 384 次的 R"与 mod p 约减都是对的。 */
    {
        fe r_expect;
        memset(&r_expect, 0, sizeof(r_expect));
        /* R = 2^128 + (2^96 - 2^32) + 1:置位的是 bit 0、bit 32..95、bit 128,
         * 所以按 32 位肢体是 {1, FFFFFFFF, FFFFFFFF, 0, 1, 0, ...}
         * (手推这里错了两次 —— 所以它必须由程序验,不能靠人眼。) */
        r_expect.v[0] = 0x00000001u;
        r_expect.v[1] = 0xFFFFFFFFu;
        r_expect.v[2] = 0xFFFFFFFFu;
        r_expect.v[3] = 0x00000000u;
        r_expect.v[4] = 0x00000001u;
        if (fe_cmp(&r_expect, &g_one_mont) != 0) {
            sxcl_auth_err(err, err_len,
                          "域运算自检失败:R mod p != 2^128 + 2^96 - 2^32 + 1(模约减坏了)");
            return -1;
        }
    }
    mont_mul(&t, &one_m, &one_m);
    if (fe_cmp(&t, &one_m) != 0) {
        sxcl_auth_err(err, err_len, "域运算自检失败:1*1 != 1(蒙哥马利乘法坏了)");
        return -1;
    }
    mont_mul(&t, &two_m, &three_m);
    if (fe_cmp(&t, &six_m) != 0) {
        sxcl_auth_err(err, err_len, "域运算自检失败:2*3 != 6(蒙哥马利乘法坏了)");
        return -1;
    }
    /* 大数乘法:mont_mul(R, R²) = R*R²*R^-1 = R² —— 这条会真正压到进位链(小数字测不出来) */
    {
        fe big;
        mont_mul(&big, &g_one_mont, &g_r2);
        if (fe_cmp(&big, &g_r2) != 0) {
            sxcl_auth_err(err, err_len, "域运算自检失败:R * R² / R != R²(蒙哥马利乘法进位出错)");
            return -1;
        }
    }
    /* 转出/转入蒙哥马利域必须是互逆的 */
    {
        fe v, m, back;
        fe_set_small(&v, 0x12345678u);
        v.v[3] = 0x9ABCDEF0u; /* 放一个大一点的值 */
        fe_to_mont(&m, &v);
        fe_from_mont(&back, &m);
        if (fe_cmp(&back, &v) != 0) {
            sxcl_auth_err(err, err_len, "域运算自检失败:to_mont/from_mont 不是互逆的");
            return -1;
        }
    }
    /* a * a^-1 = 1:顺带把求逆(费马)也验了(两边都在蒙哥马利域) */
    fe inv_three_m, prod;
    fe_inv_mont(&inv_three_m, &three_m);
    mont_mul(&prod, &three_m, &inv_three_m);
    if (fe_cmp(&prod, &one_m) != 0) {
        sxcl_auth_err(err, err_len, "域运算自检失败:3 * 3^-1 != 1(求逆坏了)");
        return -1;
    }
    /* (p-1) * 1 = p-1;再用加法验证 mod p 约减:p-1 + 1 = 0 */
    fe pm1 = EC_P;
    pm1.v[0] -= 1u;
    fe pm1_m, sum;
    fe_to_mont(&pm1_m, &pm1);
    fe_add(&sum, &pm1_m, &one_m);
    if (!fe_is_zero(&sum)) {
        sxcl_auth_err(err, err_len, "域运算自检失败:(p-1) + 1 != 0(mod p 约减坏了)");
        return -1;
    }
    return 0;
}

int sxcl_auth_ec384_point_on_curve(const unsigned char pub[97])
{
    if (pub == NULL || pub[0] != 0x04u) {
        return 0;
    }
    mont_init();
    fe x, y;
    fe_from_be_bytes(&x, pub + 1, 48);
    fe_from_be_bytes(&y, pub + 49, 48);
    if (fe_cmp(&x, &EC_P) >= 0 || fe_cmp(&y, &EC_P) >= 0) {
        return 0;
    }
    fe xm, ym, l, rr;
    fe_to_mont(&xm, &x);
    fe_to_mont(&ym, &y);
    /* 左边 = y² */
    mont_mul(&l, &ym, &ym);
    /* 右边 = x³ − 3x + b */
    mont_mul(&rr, &xm, &xm);
    mont_mul(&rr, &rr, &xm);
    fe bx;
    fe_to_mont(&bx, &EC_B);
    fe xm3;
    fe_double(&xm3, &xm);
    fe_add(&xm3, &xm3, &xm);
    fe_sub(&rr, &rr, &xm3);
    fe_add(&rr, &rr, &bx);
    /* 比较(都在蒙哥马利域,可以直接比) */
    return (fe_cmp(&l, &rr) == 0) ? 1 : 0;
}

int sxcl_auth_ec384_public_from_secret(const unsigned char secret[48], unsigned char out[97])
{
    if (secret == NULL || out == NULL) {
        return -1;
    }
    fe d;
    fe_from_be_bytes(&d, secret, 48);
    if (fe_is_zero(&d) || fe_cmp(&d, &EC_N) >= 0) {
        return -1; /* 私钥必须是 [1, n-1] */
    }
    fe x, y;
    if (scalar_mul(secret, &x, &y) != 0) {
        return -1;
    }
    out[0] = 0x04u; /* 未压缩点:0x04 || X || Y */
    fe_to_be_bytes(&x, out + 1, 48);
    fe_to_be_bytes(&y, out + 49, 48);
    return 0;
}

int sxcl_auth_ec384_keygen(char *public_b64, size_t public_b64_len, unsigned char secret[48])
{
    if (public_b64 == NULL || public_b64_len < 133u || secret == NULL) {
        return SXCL_AUTH_ERR_ARG;
    }
    unsigned char pub[97];
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (sxcl_auth_random(secret, 48) != 0) {
            sxcl_auth_secure_zero(secret, 48);
            return SXCL_AUTH_ERR_UNSUPPORTED;
        }
        if (sxcl_auth_ec384_public_from_secret(secret, pub) != 0) {
            continue; /* d 落在 [1, n-1] 之外(概率 ~2^-190):换一个 */
        }
        const int n = sxcl_auth_base64(pub, sizeof(pub), public_b64, public_b64_len);
        sxcl_auth_secure_zero(pub, sizeof(pub));
        if (n < 0) {
            sxcl_auth_secure_zero(secret, 48);
            return SXCL_AUTH_ERR_ARG;
        }
        return SXCL_AUTH_OK;
    }
    sxcl_auth_secure_zero(secret, 48);
    return SXCL_AUTH_ERR_UNSUPPORTED;
}
