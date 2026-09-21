/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "sxcl/limiter.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_near(double got, double want, double tol, const char *what) {
    if (fabs(got - want) <= tol) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %.6f want %.6f (tol %.6f)\n", what, got, want, tol);
    }
}

static void check_range(double got, double lo, double hi, const char *what) {
    if (got >= lo && got <= hi) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %.4f 期望 [%.4f, %.4f]\n", what, got, lo, hi);
    }
}

/* 按 take() 回报的等待时间推进,模拟引擎逐块取令牌 */
static double drain(sxcl_limiter *lim, unsigned long long total, unsigned long long chunk) {
    const double t0 = sxcl_limiter_now();
    unsigned long long done = 0;
    while (done < total) {
        unsigned long long want = total - done;
        if (want > chunk) {
            want = chunk;
        }
        const double wait = sxcl_limiter_take(lim, want);
        if (wait > 0.0) {
            sxcl_limiter_consume(lim, want);
        }
        done += want;
    }
    return sxcl_limiter_now() - t0;
}

static void test_unlimited(void) {
    sxcl_limiter *lim = sxcl_limiter_create(SXCL_LIMITER_UNLIMITED);
    check(lim != NULL, "create unlimited");
    if (!lim) {
        return;
    }
    check_near(sxcl_limiter_rate(lim), 0.0, 1e-9, "不限速 rate=0");
    check_near(sxcl_limiter_take(lim, 1ULL << 20), 0.0, 1e-9, "不限速 take 立即放行");
    const double t0 = sxcl_limiter_now();
    sxcl_limiter_consume(lim, 8ULL << 20);
    check_range(sxcl_limiter_now() - t0, 0.0, 0.05, "不限速 consume 不阻塞");
    sxcl_limiter_destroy(lim);
}

static void test_burst(void) {
    sxcl_limiter *small = sxcl_limiter_create(100.0);
    sxcl_limiter *mid = sxcl_limiter_create(1024.0 * 1024.0);
    sxcl_limiter *big = sxcl_limiter_create(1024.0 * 1024.0 * 1024.0);
    check_near(sxcl_limiter_burst(small), 64.0 * 1024.0, 1.0, "桶容量下限 64KiB");
    check_near(sxcl_limiter_burst(mid), 256.0 * 1024.0, 1.0, "桶容量 = rate*0.25s");
    check_near(sxcl_limiter_burst(big), 8.0 * 1024.0 * 1024.0, 1.0, "桶容量上限 8MiB");
    sxcl_limiter_destroy(small);
    sxcl_limiter_destroy(mid);
    sxcl_limiter_destroy(big);
}

static void test_full_bucket(void) {
    sxcl_limiter *lim = sxcl_limiter_create(1024.0 * 1024.0);
    if (!lim) {
        check(0, "create 1MiB");
        return;
    }
    check_near(sxcl_limiter_take(lim, 256 * 1024), 0.0, 1e-9, "满桶一次取走");
    const double wait = sxcl_limiter_take(lim, 4096);
    check_range(wait, 0.002, 0.02, "取空后等待时间 ≈ 4096/1MiB");
    sxcl_limiter_destroy(lim);
}

static void test_oversized(void) {
    sxcl_limiter *lim = sxcl_limiter_create(1024.0 * 1024.0);
    if (!lim) {
        check(0, "create 1MiB");
        return;
    }
    check_near(sxcl_limiter_take(lim, 4ULL << 20), 4.0, 1e-6, "n>桶容量:按 n/rate 等待");
    sxcl_limiter_destroy(lim);
}

/* 回归:令牌不足时不得清零(清零会多等一轮,把 5MB/s 限成 2MB/s) */
static void test_no_token_loss(void) {
    const double rate = 2.0 * 1024.0 * 1024.0;
    sxcl_limiter *lim = sxcl_limiter_create(rate);
    if (!lim) {
        check(0, "create 2MiB");
        return;
    }
    sxcl_limiter_take(lim, 512 * 1024); /* 先抽干初始满桶 */
    const unsigned long long total = 512ULL * 1024;
    const double elapsed = drain(lim, total, 64ULL * 1024);
    const double expected = (double)total / rate; /* 0.25s */
    check_range(elapsed, expected * 0.7, expected * 1.6,
                "512KiB @2MiB/s ≈ 0.25s(清零缺陷会到 ~0.5s)");
    sxcl_limiter_destroy(lim);
}

static void test_throughput(void) {
    const double rate = 4.0 * 1024.0 * 1024.0;
    sxcl_limiter *lim = sxcl_limiter_create(rate);
    if (!lim) {
        check(0, "create 4MiB");
        return;
    }
    /* 初始桶是满的,先抽干,否则首发突发额度会让人误判限速失效 */
    sxcl_limiter_take(lim, (unsigned long long)sxcl_limiter_burst(lim));
    const unsigned long long total = 2ULL * 1024 * 1024;
    const double elapsed = drain(lim, total, 16ULL * 1024);
    const double expected = (double)total / rate; /* 0.5s */
    check_range(elapsed, expected * 0.75, expected * 1.5, "2MiB @4MiB/s ≈ 0.5s");
    sxcl_limiter_destroy(lim);
}

static void test_set_rate(void) {
    sxcl_limiter *lim = sxcl_limiter_create(1024.0 * 1024.0);
    if (!lim) {
        check(0, "create");
        return;
    }
    sxcl_limiter_take(lim, 256 * 1024);
    sxcl_limiter_set_rate(lim, 8.0 * 1024.0 * 1024.0);
    check_near(sxcl_limiter_rate(lim), 8.0 * 1024.0 * 1024.0, 1.0, "改速生效");
    check_near(sxcl_limiter_burst(lim), 2.0 * 1024.0 * 1024.0, 1.0, "改速重算桶容量");
    check(sxcl_limiter_take(lim, 1024 * 1024) > 0.0, "下调后令牌不凭空增加");
    sxcl_limiter_set_rate(lim, SXCL_LIMITER_UNLIMITED);
    check_near(sxcl_limiter_take(lim, 1ULL << 30), 0.0, 1e-9, "改为不限速后立即放行");
    sxcl_limiter_destroy(lim);
}

static void test_parse(void) {
    check_near(sxcl_limiter_parse_rate("10 MB/s"), 10.0 * 1024 * 1024, 1.0, "10 MB/s");
    check_near(sxcl_limiter_parse_rate("512kb"), 512.0 * 1024, 1.0, "512kb");
    check_near(sxcl_limiter_parse_rate("2mibps"), 2.0 * 1024 * 1024, 1.0, "2mibps");
    check_near(sxcl_limiter_parse_rate("1.5g"), 1.5 * 1024.0 * 1024 * 1024, 1.0, "1.5g");
    check_near(sxcl_limiter_parse_rate("  4096  "), 4096.0, 1e-9, "纯数字(字节/秒)");
    check_near(sxcl_limiter_parse_rate("0"), 0.0, 1e-9, "0 = 不限速");
    check_near(sxcl_limiter_parse_rate("unlimited"), 0.0, 1e-9, "unlimited");
    check_near(sxcl_limiter_parse_rate(""), 0.0, 1e-9, "空串");
    check_near(sxcl_limiter_parse_rate("abc"), 0.0, 1e-9, "非法输入");
    check_near(sxcl_limiter_parse_rate("1.2.3mb"), 0.0, 1e-9, "非法数字段");
    check_near(sxcl_limiter_parse_rate("5 xyz"), 5.0, 1e-9, "未知单位按 1 倍(python _UNITS.get(u,1))");
}

int main(void) {
    test_unlimited();
    test_burst();
    test_full_bucket();
    test_oversized();
    test_no_token_loss();
    test_throughput();
    test_set_rate();
    test_parse();
    printf("限速器测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("[PASS] 限速器语义与 Python 版一致\n");
    }
    return g_fail == 0 ? 0 : 1;
}
